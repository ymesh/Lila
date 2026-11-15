#include "ops/Command.h"

#include "ops/Undo.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "core/Progress.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>

namespace lila
{
namespace
{
std::function<std::vector<std::string>()>& selectionProvider()
{
    static std::function<std::vector<std::string>()> fn;
    return fn;
}

std::map<int, std::function<void(const ExecuteEvent&)>>& executeListeners()
{
    static std::map<int, std::function<void(const ExecuteEvent&)>> l;
    return l;
}

std::function<void(std::string_view)>& executeGate()
{
    static std::function<void(std::string_view)> gate;
    return gate;
}

int gExecuteDepth = 0;
bool sPreviewing = false;
std::vector<const Args*> gRunning;

void notifyExecute(const ExecuteEvent& e)
{
    auto copy = executeListeners();
    for (auto& [id, fn] : copy)
        fn(e);
}

bool matches(ArgType type, const json& v)
{
    switch (type)
    {
    case ArgType::Bool: return v.is_boolean() || v.is_number_integer();
    case ArgType::Int: return v.is_number_integer() || v.is_boolean();
    case ArgType::Float: return v.is_number();
    case ArgType::String: return v.is_string();
    case ArgType::Vec3: return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number();
    case ArgType::List: return v.is_array();
    case ArgType::Any: return true;
    }
    return true;
}

json coerce(const FlagDef& flag, const json& v)
{
    if (flag.type == ArgType::Bool && v.is_number_integer())
        return v.get<int64_t>() != 0;
    if (flag.type == ArgType::Int && v.is_boolean())
        return v.get<bool>() ? 1 : 0;
    if (flag.type == ArgType::Int && v.is_number_integer() && (flag.min || flag.max))
        return std::clamp(v.get<int64_t>(), flag.min ? int64_t(std::ceil(*flag.min)) : INT64_MIN,
                          flag.max ? int64_t(std::floor(*flag.max)) : INT64_MAX);
    if (flag.type == ArgType::Float && v.is_number() && (flag.min || flag.max))
        return std::clamp(v.get<double>(), flag.min.value_or(-HUGE_VAL), flag.max.value_or(HUGE_VAL));
    return v;
}

Args normalize(const CommandDef& def, const json& call)
{
    Args a;
    if (!call.is_object())
        throw CommandError(def.name + ": call must be an object");

    for (auto& [key, v] : call.items())
    {
        if (key == "args")
        {
            a.positional = v.is_array() ? v : json::array({v});
            continue;
        }
        const FlagDef* flag = nullptr;
        for (const FlagDef& f : def.flags)
            if (f.name == key || (!f.shortName.empty() && f.shortName == key))
                flag = &f;
        if (!flag)
            throw CommandError(def.name + ": unknown flag '" + key + "'");
        if (!v.is_null() && !matches(flag->type, v))
            throw CommandError(def.name + ": bad value for '" + flag->name + "': " + v.dump());
        if (v.is_string() && !flag->choices.empty() && std::ranges::find(flag->choices, v.get<std::string>()) == flag->choices.end())
        {
            std::string allowed;
            for (const std::string& c : flag->choices)
                allowed += (allowed.empty() ? "" : ", ") + c;
            throw CommandError(def.name + ": bad value for '" + flag->name + "': " + v.dump() + " (one of " + allowed + ")");
        }
        a.flags[flag->name] = coerce(*flag, v);
    }

    if (def.positionalDoc.empty() && !a.positional.empty())
        throw CommandError(def.name + " takes no positional arguments");

    for (const FlagDef& f : def.flags)
        if (!a.flags.contains(f.name) && !f.defaultValue.is_null())
            a.flags[f.name] = f.defaultValue;
    return a;
}

std::string pyLiteral(const json& v)
{
    if (v.is_boolean())
        return v.get<bool>() ? "True" : "False";
    if (v.is_null())
        return "None";
    if (v.is_array())
    {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i)
            s += (i ? ", " : "") + pyLiteral(v[i]);
        return s + "]";
    }
    if (v.is_object())
    {
        std::string s = "{";
        bool first = true;
        for (auto& [k, x] : v.items())
        {
            s += (first ? "" : ", ") + json(k).dump() + ": " + pyLiteral(x);
            first = false;
        }
        return s + "}";
    }
    return v.dump();
}
} // namespace

std::vector<std::string> Args::objects() const
{
    std::vector<std::string> out;
    for (const json& v : positional)
        if (v.is_string())
            out.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(x.get<std::string>());
    if (out.empty() && selectionProvider())
        out = selectionProvider()();
    return out;
}

namespace ops
{
Registry<CommandDef>& registry()
{
    static Registry<CommandDef> r;
    return r;
}

void registerCommand(CommandDef def)
{
    if (const std::string* attributed = owners::attributedOwner())
        def.owner = *attributed;
    std::string name = def.name;
    std::string owner = def.owner;
    registry().put(std::move(name), std::move(def), std::move(owner));
}

void unregisterCommand(std::string_view name)
{
    registry().remove(name);
}

const CommandDef* find(std::string_view name)
{
    return registry().find(name);
}

std::vector<const CommandDef*> all()
{
    std::vector<CommandDef*> defs = registry().all();
    return {defs.begin(), defs.end()};
}

namespace
{
std::string sCoalesceKey;
struct LastCoalesced
{
    std::string key;
    std::string command;
    uint64_t step = 0;
} sLastCoalesced;
} // namespace

void setCoalesceKey(std::string key)
{
    sCoalesceKey = std::move(key);
}

json execute(std::string_view name, const json& call, CallSource source)
{
    LILA_ZONE();
    LILA_ZONE_TEXT(name.data(), name.size());
    LILA_MESSAGE(name.data(), name.size());
    assert(mainq::isMainThread());

    const CommandDef* def = find(name);
    if (!def)
        throw CommandError("unknown command '" + std::string(name) + "'");

    Args args = normalize(*def, call.is_null() ? json::object() : call);

    if (!sPreviewing && executeGate())
        executeGate()(def->name);

    // A preview run is reverted at once, so nothing watching executes should see it.
    bool top = gExecuteDepth == 0 && !sPreviewing;
    if (top && !executeListeners().empty())
        notifyExecute({name, call, source});
    json result;
    ++gExecuteDepth;
    gRunning.push_back(&args);
    const uint64_t cancelsBefore = progress::cancelledCount();
    bool coalesced = false;
    const uint64_t stepBefore = UndoStack::get().newestId();
    try
    {
        if (def->undoable)
        {
            UndoStack& undo = UndoStack::get();
            // The same scrolled control running the same command again replaces its last step instead of adding one.
            const bool coalesce = top && source == CallSource::UI && !sCoalesceKey.empty();
            const bool reopened = coalesce && sLastCoalesced.key == sCoalesceKey && sLastCoalesced.command == def->name &&
                                  undo.newestId() == sLastCoalesced.step && !undo.chunkOpen() && undo.reopenNewest(def->name);
            struct CloseReopened
            {
                bool on;
                ~CloseReopened()
                {
                    if (on)
                        UndoStack::get().closeChunk();
                }
            } closeReopened{reopened};
            UndoChunk chunk(def->name);
            const size_t mark = UndoStack::get().openEdits();
            try
            {
                result = def->run(args);
                coalesced = coalesce;
            }
            catch (...)
            {
                // An aborted import or conversion leaves nothing half-built behind.
                if (progress::cancelledCount() != cancelsBefore || progress::cancelRequested())
                    UndoStack::get().rollbackTo(mark);
                throw;
            }
        }
        else
        {
            result = def->run(args);
        }
    }
    catch (...)
    {
        gRunning.pop_back();
        --gExecuteDepth;
        if (top && !executeListeners().empty())
            notifyExecute({name, call, source, true, false});
        if (progress::cancelledCount() != cancelsBefore)
            throw CommandCancelled(std::string(name) + ": cancelled");
        throw;
    }
    gRunning.pop_back();
    --gExecuteDepth;
    // Only a step this run made can be folded into next time: a run that changed nothing leaves an older step newest.
    if (coalesced)
        sLastCoalesced = UndoStack::get().newestId() > stepBefore ? LastCoalesced{sCoalesceKey, def->name, UndoStack::get().newestId()} : LastCoalesced{};
    if (top && !executeListeners().empty())
        notifyExecute({name, call, source, true, true});

    if (source == CallSource::UI)
        log::write(LogLevel::Echo, toPython(name, call) + "\n");
    return result;
}

void setExecuteGate(std::function<void(std::string_view name)> gate) { executeGate() = std::move(gate); }

const Args* currentArgs() { return gRunning.empty() ? nullptr : gRunning.back(); }

bool setAdjustable(std::string_view name, bool on)
{
    CommandDef* def = registry().find(name);
    if (!def)
        return false;
    def->adjustable = on;
    return true;
}

namespace
{
bool sAgentCalling = false;
} // namespace

bool agentCalling()
{
    return sAgentCalling;
}

void setAgentCalling(bool on)
{
    sAgentCalling = on;
}

bool previewing()
{
    return sPreviewing;
}

void setPreviewing(bool on)
{
    sPreviewing = on;
}

int addExecuteListener(std::function<void(const ExecuteEvent&)> fn)
{
    static int next = 1;
    executeListeners()[next] = std::move(fn);
    return next++;
}

void removeExecuteListener(int id)
{
    executeListeners().erase(id);
}

std::string toPython(std::string_view name, const json& call)
{
    // Short form: commands are bare names in the script editor's namespace.
    std::string s = std::string(name) + "(";
    bool first = true;
    if (call.is_object())
    {
        if (auto it = call.find("args"); it != call.end())
        {
            const json& pos = *it;
            for (const json& v : pos.is_array() ? pos : json::array({pos}))
            {
                s += (first ? "" : ", ") + pyLiteral(v);
                first = false;
            }
        }
        for (auto& [k, v] : call.items())
        {
            if (k == "args")
                continue;
            s += (first ? "" : ", ") + k + "=" + pyLiteral(v);
            first = false;
        }
    }
    return s + ")";
}

void setSelectionProvider(std::function<std::vector<std::string>()> fn)
{
    selectionProvider() = std::move(fn);
}

void registerBuiltins()
{
    registerCommand({
        .name = "undo",
        .doc = "Undo the last command.",
        .undoable = false,
        .run = [](const Args&) -> json {
            std::string label = UndoStack::get().undoLabel();
            if (!UndoStack::get().undo())
                throw CommandError("nothing to undo");
            log::info("undo: {}", label);
            return label;
        },
    });
    registerCommand({
        .name = "redo",
        .doc = "Redo the last undone command.",
        .undoable = false,
        .run = [](const Args&) -> json {
            std::string label = UndoStack::get().redoLabel();
            if (!UndoStack::get().redo())
                throw CommandError("nothing to redo");
            log::info("redo: {}", label);
            return label;
        },
    });
    registerCommand({
        .name = "flushUndo",
        .doc = "Clear undo history. which='past' drops the steps undo would revert, so undo cannot step back past this "
               "point; 'future' drops the redo steps; 'all' drops both.",
        .flags = {{"which", "w", ArgType::String, "past", "past, future or all.", {"past", "future", "all"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string which = a.get<std::string>("which", "past");
            UndoStack& u = UndoStack::get();
            if (which == "past")
                u.flushPast();
            else if (which == "future")
                u.flushFuture();
            else if (which == "all")
                u.flush();
            else
                throw CommandError("flushUndo: which must be past, future or all");
            log::info("undo history flushed ({})", which);
            return nullptr;
        },
    });
    registerCommand({
        .name = "undoJump",
        .doc = "Undo or redo in one go until the given number of undo steps remain, as listed by undoInfo(history=True).",
        .flags = {{"position", "p", ArgType::Int, nullptr, "Undo steps left afterwards: 0 undoes everything.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            UndoStack& u = UndoStack::get();
            int64_t pos = a.get<int64_t>("position", int64_t(u.undoCount()));
            if (pos < 0 || !u.jumpTo(size_t(pos)))
                throw CommandError(std::format("undoJump: position must be 0 to {}", u.undoCount() + u.redoCount()));
            return int64_t(u.undoCount());
        },
    });
    registerCommand({
        .name = "undoInfo",
        .doc = "Query or control the undo queue. openChunk/closeChunk group everything in between into one undo step.",
        .flags = {
            {"openChunk", "ock", ArgType::Bool, nullptr, "Start a chunk."},
            {"closeChunk", "cck", ArgType::Bool, nullptr, "End a chunk."},
            {"chunkName", "cn", ArgType::String, nullptr, "Label for openChunk."},
            {"state", "st", ArgType::Bool, nullptr, "Turn undo recording on or off."},
            {"limit", "l", ArgType::Int, nullptr, "Max undo steps, 0 for infinite.", {}, {}, 0.0},
            {"memoryBudget", "mb", ArgType::Float, nullptr, "Drop the oldest steps past this many MB, 0 for unlimited.", {}, {}, 0.0},
            {"history", "h", ArgType::Bool, nullptr, "Also return every step's label, oldest first, and the current position."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            UndoStack& u = UndoStack::get();
            if (a.get<bool>("openChunk"))
                u.openChunk(a.get<std::string>("chunkName", "chunk"));
            if (a.get<bool>("closeChunk"))
                u.closeChunk();
            if (a.has("state"))
                u.setEnabled(a.get<bool>("state"));
            if (a.has("limit"))
                u.setLimit(size_t(std::max<int64_t>(0, a.get<int64_t>("limit"))));
            if (a.has("memoryBudget"))
                u.setByteBudget(size_t(std::max(0.0, a.get<double>("memoryBudget")) * 1024.0 * 1024.0));
            json out = {{"undo", u.undoLabel()}, {"redo", u.redoLabel()}, {"state", u.enabled()}, {"memory", u.memoryBytes()}};
            if (a.get<bool>("history"))
            {
                json steps = json::array();
                for (const UndoStack::Step& s : u.steps())
                    steps.push_back({{"label", s.label}, {"selection", s.selectionOnly}});
                out["steps"] = std::move(steps);
                out["position"] = u.undoCount();
            }
            return out;
        },
    });
}
} // namespace ops
} // namespace lila
