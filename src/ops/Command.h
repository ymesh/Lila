#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lila
{
using json = nlohmann::json;

/** Thrown by a command to fail with a message for the user. */
struct CommandError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/** What execute throws for a command the user aborted from the progress card ("importFbx: cancelled"). */
struct CommandCancelled : CommandError
{
    using CommandError::CommandError;
};

enum class ArgType : uint8_t
{
    Bool,
    Int,
    Float,
    String,
    Vec3,
    List,
    Any,
};

/**
 * One flag. `doc` is read by the API reference, the generated Python stubs (completion, hover and signature help in
 * the script editor) and runtime help(), so write it for someone typing the call.
 */
struct FlagDef
{
    std::string name;
    std::string shortName;
    ArgType type = ArgType::Any;
    json defaultValue; // null = not passed unless given
    std::string doc;
    /** Allowed values of a String flag, for menus and combo boxes; empty means free text. */
    std::vector<std::string> choices;
    /** A short Python call showing the flag in use, e.g. `xform("cube1", worldSpace=True, query=True, translation=True)`. */
    std::string example;
    /** Bounds of an Int or Float flag: calls are clamped to them and option boxes use them as the field's range. */
    std::optional<double> min;
    std::optional<double> max;
    /**
     * The node type a String or List flag names ("mesh", "curve", "joint"; "node" for any), so option boxes give it a
     * node picker. Set it with nodeFlag().
     */
    std::string nodeType;
    /** Values an Int or Float flag's option box field offers in a menu and steps through on the wheel; set it with presetFlag(). */
    std::vector<double> presets;
    /**
     * What leaving a null-default flag out means (the node attr's own default), shown by option boxes. Unlike
     * defaultValue it never fills calls, so commands still see the flag as not given.
     */
    json shownDefault;
};

/** `flag` as one naming a scene node of `type` (see FlagDef::nodeType). */
inline FlagDef nodeFlag(FlagDef flag, std::string type)
{
    flag.nodeType = std::move(type);
    return flag;
}

/** `flag` with preset values (see FlagDef::presets). */
inline FlagDef presetFlag(FlagDef flag, std::vector<double> presets)
{
    flag.presets = std::move(presets);
    return flag;
}

/** Power-of-two texture sizes, the presets of every texture size field. */
inline const std::vector<double> kTextureSizePresets = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};

/** Normalized call: positional args plus long-name flags, defaults filled in. */
struct LILA_FOUNDATION_API Args
{
    json positional = json::array();
    json flags = json::object();

    bool has(std::string_view flag) const { return flags.contains(flag) && !flags[std::string(flag)].is_null(); }

    template <class T> T get(std::string_view flag, const T& fallback = T{}) const
    {
        auto it = flags.find(flag);
        if (it == flags.end() || it->is_null())
            return fallback;
        return it->template get<T>();
    }

    /** Positional args, or the selection when none were given. */
    std::vector<std::string> objects() const;
};

enum class CallSource : uint8_t
{
    Script, // Python, MCP
    UI,     // menus, hotkeys, tools: echoed to the script editor
};

/**
 * A registered command. `doc` is the summary line, then optionally a blank line and a longer description. It feeds the
 * API reference, the generated Python stubs and runtime help(), together with the flag docs and `examples`.
 */
struct CommandDef
{
    std::string name;
    std::string doc;
    std::string positionalDoc; // empty = takes none
    std::vector<FlagDef> flags;
    bool undoable = true;
    std::function<json(const Args&)> run;
    std::string owner; // extension that registered it; empty = built in
    /** Re-running it with different flags after undoing it is safe, so the UI may offer to adjust the last call live. */
    bool adjustable = false;
    /** Python snippets, each one or more lines of runnable source. */
    std::vector<std::string> examples;
    /** Registered from Python at runtime (a user script or a Python extension), whatever its owner. */
    bool scripted = false;
};

/** One top-level execute, reported before the command runs and again after it finished or failed. */
struct ExecuteEvent
{
    std::string_view name;
    const json& call;
    CallSource source;
    bool done = false; // false: about to run
    bool ok = false;   // with done: it returned without throwing
};

namespace ops
{
/** Every command, owner-tagged; registerCommand and friends go through it. */
LILA_FOUNDATION_API Registry<CommandDef>& registry();
LILA_FOUNDATION_API void registerCommand(CommandDef def);
LILA_FOUNDATION_API void unregisterCommand(std::string_view name);
LILA_FOUNDATION_API const CommandDef* find(std::string_view name);
LILA_FOUNDATION_API std::vector<const CommandDef*> all();

/** `call` is {"args": [...], flag: value, ...}, flags by long or short name. Main thread only. Throws CommandError. */
LILA_FOUNDATION_API json execute(std::string_view name, const json& call, CallSource source = CallSource::Script);

/** The arguments of the innermost command running now, or null outside any. */
LILA_FOUNDATION_API const Args* currentArgs();

/** Marks an already registered command adjustable (see CommandDef::adjustable). False if it isn't registered. */
LILA_FOUNDATION_API bool setAdjustable(std::string_view name, bool on = true);

/** Called around top-level executes only; commands run by other commands aren't reported. Returns an id. */
LILA_FOUNDATION_API int addExecuteListener(std::function<void(const ExecuteEvent&)> fn);
LILA_FOUNDATION_API void removeExecuteListener(int id);

/**
 * Asked before every command runs, nested ones too but not previews; throwing CommandError refuses it. One gate at a
 * time; an empty function removes it.
 */
LILA_FOUNDATION_API void setExecuteGate(std::function<void(std::string_view name)> gate);

/** Whether an agent's MCP request is being handled; commands run meanwhile are the agent's. */
LILA_FOUNDATION_API bool agentCalling();
LILA_FOUNDATION_API void setAgentCalling(bool on);

/**
 * True while commands run only to show what they would do and are reverted straight after (marking menu previews).
 * Execute listeners are not told about them; graph listeners that record changes for others should ignore them.
 */
LILA_FOUNDATION_API bool previewing();
LILA_FOUNDATION_API void setPreviewing(bool on);

/** Marks an agent's request for its lifetime. */
struct AgentScope
{
    AgentScope() : outer(agentCalling()) { setAgentCalling(true); }
    ~AgentScope() { setAgentCalling(outer); }
    AgentScope(const AgentScope&) = delete;
    AgentScope& operator=(const AgentScope&) = delete;
    bool outer;
};

/** Python source equivalent of a call, for echo. */
LILA_FOUNDATION_API std::string toPython(std::string_view name, const json& call);

/** Selection provider for Args::objects(); installed by the scene module. */
LILA_FOUNDATION_API void setSelectionProvider(std::function<std::vector<std::string>()> fn);

LILA_FOUNDATION_API void registerBuiltins();
} // namespace ops
} // namespace lila
