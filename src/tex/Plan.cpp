#include "tex/Plan.h"

#include "core/Profile.h"
#include "tex/Expressions.h"
#include "tex/Ports.h"
#include "tex/Prelude.h"
#include "tex/TextRaster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

/**
 * Scheduling: every flattened node output is either fused into the passes that read it (re-evaluated per sample, as a
 * function call) or cached in a buffer by a pass of its own. An output is buffered when it is a native node's or a
 * Buffer node's, feeds a native node, is read by several inputs and is not cheap (cheapCost), is sampled by a multi-tap
 * input at a cost over tapBudget, or when a pass would otherwise cost more than passBudget per texel (then its most
 * expensive fused output is buffered, repeatedly). Material values are never buffered: they don't fit one texel.
 */
namespace lila::tex
{
namespace
{
struct Ref
{
    int node = -1;
    std::string socket;
    PortType type = PortType::Float;
    /** Constants: the gU slot holding the value (-1 for a material, which reads as the default material). */
    int slot = -1;
    /** An unlinked input reading the texture coordinate (PortDecl::implicit). */
    bool uv = false;

    bool constant() const { return node < 0; }
    /** Identifies what a buffer would hold. */
    std::string key() const
    {
        if (uv)
            return "uv";
        return constant() ? "c" + std::to_string(slot) : std::to_string(node) + "." + socket;
    }
};

struct FlatNode
{
    std::string path;
    const NodeDef* def = nullptr;
    NodePorts ports;
    /** Resolved param values by id. */
    std::map<std::string, json> values;
    std::vector<Ref> inputs;
    std::map<std::string, int> paramSlot;
    /** Node settings: its buffers are the plan's size times 2^sizeShift; bits 0 keeps the plan's format. */
    int sizeShift = 0;
    int bits = 0;

    bool ownBuffers() const { return sizeShift != 0 || bits != 0; }
};

struct Root
{
    std::string name;
    Ref ref;
    PortType store = PortType::Color;
    std::string field;
    /** The ref is already in a buffer of another type: the pass converts what it reads from there. */
    bool fromBuffer = false;
    /** Stores 8-bit steps. */
    bool quantize = false;
};

const std::vector<NodeSetting> kNodeSettings = {
    {"outputSize", "Size", {"eighth", "quarter", "half", "set", "double", "quadruple"}, "set", "Resolution of the node's output against the set's."},
    {"bitDepth", "Bit Depth", {"set", "8", "16", "32"}, "set", "Bits per channel the node's output is stored at; set keeps the set's."},
};

/** The option index a node's value picks for a setting, -1 when it has none or an unknown one. */
int settingIndex(const json& values, const char* id)
{
    const NodeSetting* s = nodeSetting(id);
    auto it = values.find(id);
    if (!s || it == values.end() || !it->is_string())
        return -1;
    auto at = std::find(s->options.begin(), s->options.end(), it->get<std::string>());
    return at == s->options.end() ? -1 : int(at - s->options.begin());
}

int scaledSize(int size, int shift) { return std::clamp(int(std::ldexp(double(size), shift)), 1, std::max(size, 16384)); }

struct Scope
{
    const Graph* g = nullptr;
    std::unique_ptr<Graph> owned;
    std::string prefix;
    std::map<std::string, Ref> groupInputs;
    std::map<std::string, json> overrides;
    /** The graph's exposed param inputs by name, as expressions read them (input.name). */
    std::map<std::string, json> inputValues;
    /** node.param expressions being evaluated, to catch one that reads itself. */
    std::set<std::string> evaluating;
    std::map<std::string, int> flatOf;
    std::map<std::string, std::unique_ptr<Scope>> children;
    std::set<std::string> visiting;
    int depth = 0;
    /** Appended to the paths of nodes flattened here: a repeat pass's "#k". */
    std::string suffix;
    /** A repeat pass: its zone's body resolves here, every other node in parent (once for all passes). */
    Scope* parent = nullptr;
    std::set<std::string> body;
    std::string repeatInput;
    /** The zone input's outputs during this pass. */
    std::map<std::string, Ref> lanes;
    /** Unrolled zones by Repeat Output, with their passes' scopes. */
    std::map<std::string, std::map<std::string, Ref>> repeats;
    std::vector<std::unique_ptr<Scope>> passes;
};

constexpr int kRepeatPassesPerBuffer = 8;

const std::vector<std::pair<std::string, PortType>> kStandard = {
    {"baseColor", PortType::Color}, {"roughness", PortType::Gray},   {"metallic", PortType::Gray}, {"normal", PortType::Color},
    {"height", PortType::Gray},     {"ao", PortType::Gray},          {"emissive", PortType::Color}, {"opacity", PortType::Gray},
};

const char* swizzle(PortType t)
{
    switch (t)
    {
    case PortType::Color: return "";
    case PortType::Vector: return ".xyz";
    default: return ".x";
    }
}

const char* convFn(PortType to)
{
    switch (to)
    {
    case PortType::Color: return "texToColor";
    case PortType::Vector: return "texToVector";
    case PortType::Material: return "texToMaterial";
    default: return "texToFloat";
    }
}

bool scalar(PortType t) { return t == PortType::Float || t == PortType::Gray || t == PortType::Sdf2d; }

/** Whether a buffer storing `from` reads back unchanged as `to`. */
bool storageCompatible(PortType from, PortType to) { return from == to || (scalar(from) && scalar(to)); }

void packFloat4(std::vector<float>& u, int slot, const float v[4])
{
    std::memcpy(&u[size_t(slot) * 4], v, sizeof(float) * 4);
}

class Builder
{
public:
    Builder(const PlanRequest& req) : mReq(req) {}

    Plan run(const Graph& g)
    {
        LILA_ZONE();
        Scope top;
        top.g = &g;
        mSeed = g.seed;
        mTime = g.time;
        for (const GraphInput& gi : g.inputs)
            if (gi.param)
                top.inputValues[gi.name] = gi.defaultValue;
        mPlan.width = std::max(1, mReq.width);
        mPlan.height = std::max(1, mReq.height);
        try
        {
            if (mReq.outputs.empty())
                throw std::invalid_argument("nothing to evaluate: no outputs requested");
            for (const OutputRequest& o : mReq.outputs)
                mRoots.push_back(resolveRequest(top, o));
            schedule();
            emit();
        }
        catch (const std::exception& e)
        {
            Plan failed;
            failed.width = mPlan.width;
            failed.height = mPlan.height;
            failed.error = e.what();
            failed.warnings = mPlan.warnings;
            return failed;
        }
        finish();
        return std::move(mPlan);
    }

private:
    // Flattening -------------------------------------------------------------------------------------------------

    int newSlots(const std::string& node, const std::string& id, int count)
    {
        int slot = mPlan.uniformSlots;
        mPlan.uniformSlots += count;
        mPlan.uniforms.resize(size_t(mPlan.uniformSlots) * 4, 0.0f);
        mPlan.params.push_back({node, id, slot, count});
        return slot;
    }

    Ref constantRef(const std::string& node, const std::string& id, PortType type, const json& value)
    {
        Ref r;
        r.type = type;
        if (type == PortType::Material)
            return r;
        r.slot = newSlots(node, id, 1);
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (!value.is_null() && !valueToFloat4(value, type, v))
            mPlan.warnings.push_back(node + "." + id + ": unreadable value " + value.dump() + ", using 0");
        packFloat4(mPlan.uniforms, r.slot, v);
        return r;
    }

    json valueFor(Scope& s, const GNode& n, const std::string& id, const json& fallback)
    {
        if (auto it = s.overrides.find(n.name + "." + id); it != s.overrides.end())
            return it->second;
        json stored = fallback;
        if (auto it = n.values.find(id); it != n.values.end())
            stored = *it;
        if (const std::string text = expressionOf(n, id); !text.empty())
            return expressionValue(s, n, id, text, stored);
        return stored;
    }

    /** An expression's value; a broken one warns and keeps the stored value. */
    json expressionValue(Scope& s, const GNode& n, const std::string& id, const std::string& text, const json& stored)
    {
        const std::string key = n.name + "." + id;
        if (!s.evaluating.insert(key).second)
            throw std::invalid_argument(key + " reads itself through its expression");
        auto read = [&](const std::string& name) -> double {
            const size_t dot = name.find('.');
            const std::string node = name.substr(0, dot), what = name.substr(dot + 1);
            if (node == "input" && !s.g->node(node))
            {
                auto it = s.inputValues.find(what);
                if (it == s.inputValues.end())
                    throw std::invalid_argument("the graph has no input " + what);
                return expressionNumber(it->second, name);
            }
            const GNode* other = s.g->node(node);
            if (!other)
                throw std::invalid_argument("no node " + node);
            const NodePorts ports = portsOf(*s.g, *other);
            json fallback;
            if (const ParamDecl* p = ports.param(what))
                fallback = p->defaultValue;
            else if (const PortDecl* in = ports.input(what))
                fallback = in->defaultValue;
            else
                throw std::invalid_argument(node + " has no parameter " + what);
            return expressionNumber(valueFor(s, *other, what, fallback), name);
        };
        json out = stored;
        try
        {
            out = evaluateExpression(text, read, mTime);
        }
        catch (const std::invalid_argument& e)
        {
            const std::string warning = s.prefix + key + ": expression " + text + ": " + e.what() + ", using " + stored.dump();
            if (std::find(mPlan.warnings.begin(), mPlan.warnings.end(), warning) == mPlan.warnings.end())
                mPlan.warnings.push_back(warning);
        }
        s.evaluating.erase(key);
        return out;
    }

    /** implicitUv: the reader is a fused Slang node, which can read the coordinate in place of a constant. */
    Ref resolveInput(Scope& s, const GNode& n, const PortDecl& in, bool implicitUv = false)
    {
        std::vector<GLink> links = s.g->linksInto(n.name, in.id);
        if (!links.empty())
        {
            Ref r = resolveOutput(s, links.front().fromNode, links.front().fromSocket);
            if (!linkable(r.type, in.type))
                throw std::invalid_argument(s.prefix + n.name + "." + in.id + ": can't take a " + portTypeName(r.type));
            return r;
        }
        const json value = valueFor(s, n, in.id, json());
        if (implicitUv && in.implicit == "uv" && value.is_null())
        {
            Ref r;
            r.type = in.type;
            r.uv = true;
            return r;
        }
        return constantRef(s.prefix + n.name, in.id, in.type, value.is_null() ? in.defaultValue : value);
    }

    Scope& childScope(Scope& s, const GNode& n, const NodePorts& ports)
    {
        auto& slot = s.children[n.name];
        if (slot)
            return *slot;
        if (s.depth > 32)
            throw std::invalid_argument(s.prefix + n.name + ": groups nest too deep (a group using itself?)");
        auto child = std::make_unique<Scope>();
        child->owned = std::make_unique<Graph>(Graph::fromJson(ports.graph));
        resolveRouting(*child->owned);
        child->g = child->owned.get();
        child->prefix = s.prefix + n.name + s.suffix + "/";
        child->depth = s.depth + 1;
        // A graph node's own subgroups resolve inside it; a Group node shares its parent's.
        if (child->owned->subgroups.empty())
            child->owned->subgroups = s.g->subgroups;
        for (const GraphInput& gi : child->g->inputs)
        {
            if (gi.param)
            {
                json v = valueFor(s, n, gi.name, gi.defaultValue);
                for (const std::string& t : gi.targets)
                    child->overrides[t] = v;
                child->inputValues[gi.name] = v;
                continue;
            }
            const PortDecl* decl = ports.input(gi.name);
            child->groupInputs[gi.name] = resolveInput(s, n, *decl);
        }
        slot = std::move(child);
        return *slot;
    }

    /** Unrolls a repeat zone: its body flattened once per pass, each pass reading the last one's results. */
    const std::map<std::string, Ref>& unroll(Scope& s, const GNode& out, const NodePorts& outPorts)
    {
        if (auto it = s.repeats.find(out.name); it != s.repeats.end())
            return it->second;
        const GNode* in = repeatInputOf(*s.g, out);
        if (!in)
            throw std::invalid_argument(s.prefix + out.name + ": no Repeat Input pairs with it");
        const NodePorts inPorts = portsOf(*s.g, *in);
        if (!inPorts.error.empty())
            throw std::invalid_argument(s.prefix + in->name + ": " + inPorts.error);
        const json count = valueFor(s, *in, "iterations", inPorts.param("iterations")->defaultValue);
        int passes = count.is_number() ? int(std::lround(count.get<double>())) : 0;
        if (passes > kMaxRepeatIterations)
            mPlan.warnings.push_back(s.prefix + in->name + ": " + std::to_string(passes) + " iterations, running the most, " +
                                     std::to_string(kMaxRepeatIterations));
        passes = std::clamp(passes, 0, kMaxRepeatIterations);

        std::map<std::string, Ref> lanes;
        for (const PortDecl& lane : inPorts.inputs)
            lanes[lane.id] = resolveInput(s, *in, lane);
        const std::vector<std::string> bodyNames = repeatBody(*s.g, in->name, out.name);
        for (int k = 0; k < passes; ++k)
        {
            auto pass = std::make_unique<Scope>();
            pass->g = s.g;
            pass->prefix = s.prefix;
            pass->suffix = s.suffix + "#" + std::to_string(k);
            pass->groupInputs = s.groupInputs;
            pass->overrides = s.overrides;
            pass->inputValues = s.inputValues;
            pass->depth = s.depth;
            pass->parent = &s;
            pass->body = {bodyNames.begin(), bodyNames.end()};
            pass->repeatInput = in->name;
            pass->lanes = lanes;
            pass->lanes["iteration"] = constantRef(s.prefix + in->name + pass->suffix, "iteration", PortType::Float, json(k));
            for (const PortDecl& lane : outPorts.inputs)
                if (!s.g->linksInto(out.name, lane.id).empty())
                    lanes[lane.id] = resolveInput(*pass, out, lane);
            // Every few passes the lanes go to buffers, so no pass fuses more than that many copies of the body.
            if ((k + 1) % kRepeatPassesPerBuffer == 0 && k + 1 < passes)
                for (const auto& [id, r] : lanes)
                    if (!r.constant() && !r.uv)
                        mRepeatCuts.push_back(r);
            s.passes.push_back(std::move(pass));
        }
        return s.repeats[out.name] = lanes;
    }

    Ref resolveOutput(Scope& s, const std::string& nodeName, const std::string& socket)
    {
        if (s.parent && nodeName != s.repeatInput && !s.body.contains(nodeName))
            return resolveOutput(*s.parent, nodeName, socket);
        const GNode* n = s.g->node(nodeName);
        if (!n)
            throw std::invalid_argument(s.prefix + ": no node " + nodeName);
        NodePorts ports = portsOf(*s.g, *n);
        if (!ports.error.empty())
            throw std::invalid_argument(s.prefix + nodeName + ": " + ports.error);
        const PortDecl* out = ports.output(socket);
        if (!out)
            throw std::invalid_argument(s.prefix + nodeName + " has no output " + socket);
        const NodeKind kind = ports.def->kind;
        if (kind == NodeKind::GroupInput)
        {
            auto it = s.groupInputs.find(socket);
            if (it != s.groupInputs.end())
                return it->second;
            const GraphInput* gi = s.g->input(socket);
            return constantRef(s.prefix + nodeName, socket, out->type, gi ? gi->defaultValue : json());
        }
        if (kind == NodeKind::Group || kind == NodeKind::Graph)
        {
            Scope& inner = childScope(s, *n, ports);
            const GNode* go = inner.g->firstOfType(kGroupOutputType);
            std::vector<GLink> links = go ? inner.g->linksInto(go->name, socket) : std::vector<GLink>{};
            if (links.empty())
                return constantRef(inner.prefix + "GroupOutput", socket, out->type, json());
            Ref r = resolveOutput(inner, links.front().fromNode, links.front().fromSocket);
            if (!linkable(r.type, out->type))
                throw std::invalid_argument(inner.prefix + "GroupOutput." + socket + ": can't take a " + portTypeName(r.type));
            return r;
        }
        if (kind == NodeKind::Output || kind == NodeKind::GroupOutput)
            throw std::invalid_argument(s.prefix + nodeName + " has no outputs");
        if (kind == NodeKind::RepeatInput)
        {
            if (nodeName == s.repeatInput)
                return s.lanes.at(socket);
            // Read outside its zone, or with no zone: the values it starts from, at pass 0.
            if (const PortDecl* lane = ports.input(socket))
                return resolveInput(s, *n, *lane);
            return constantRef(s.prefix + nodeName + s.suffix, socket, out->type, json(0.0));
        }
        if (kind == NodeKind::RepeatOutput)
            return unroll(s, *n, ports).at(socket);
        return {flatten(s, *n, ports), socket, out->type, -1};
    }

    int flatten(Scope& s, const GNode& n, const NodePorts& ports)
    {
        if (auto it = s.flatOf.find(n.name); it != s.flatOf.end())
            return it->second;
        if (!s.visiting.insert(n.name).second)
            throw std::invalid_argument(s.prefix + n.name + ": the graph has a cycle through it");
        FlatNode f;
        f.path = s.prefix + n.name + s.suffix;
        f.def = ports.def;
        f.ports = ports;
        for (const PortDecl& in : ports.inputs)
            f.inputs.push_back(resolveInput(s, n, in, ports.def && ports.def->kind == NodeKind::Slang));
        for (const ParamDecl& p : ports.params)
        {
            json v = valueFor(s, n, p.id, p.defaultValue);
            if ((p.type == ParamType::Int || p.type == ParamType::Seed || p.type == ParamType::Enum) && v.is_number_float())
                v = std::llround(v.get<double>());
            if (!paramValueValid(p.type, v, p.options))
            {
                mPlan.warnings.push_back(f.path + "." + p.id + ": " + v.dump() + " doesn't fit a " + paramTypeName(p.type) + ", using the default");
                v = p.defaultValue;
            }
            if (p.type == ParamType::Seed && mSeed != 0 && v.is_number())
                v = std::llround(v.get<double>()) + mSeed;
            f.values[p.id] = v;
            f.paramSlot[p.id] = newSlots(f.path, p.id, paramSlots(p.type));
        }
        if (f.def && (f.def->kind == NodeKind::Slang || f.def->kind == NodeKind::Native || f.def->kind == NodeKind::Buffer))
        {
            if (const int i = settingIndex(n.values, "outputSize"); i >= 0)
                f.sizeShift = i - 3;
            if (const int i = settingIndex(n.values, "bitDepth"); i > 0)
                f.bits = std::stoi(kNodeSettings[1].options[size_t(i)]);
            for (const NodeSetting& ns : kNodeSettings)
                if (n.values.contains(ns.id) && settingIndex(n.values, ns.id.c_str()) < 0)
                    mPlan.warnings.push_back(f.path + "." + ns.id + ": " + n.values[ns.id].dump() + " isn't one of its options, using " + ns.defaultValue);
        }
        s.visiting.erase(n.name);
        int index = int(mNodes.size());
        mNodes.push_back(std::move(f));
        s.flatOf[n.name] = index;
        return index;
    }

    Root resolveRequest(Scope& top, const OutputRequest& o)
    {
        const GNode* n = top.g->node(o.node);
        if (!n)
            throw std::invalid_argument("no node " + o.node);
        const NodeDef* d = findNode(n->type);
        if (!d)
            throw std::invalid_argument(o.node + ": unknown node type " + n->type);
        Root r{o.name, {}, o.type, o.field};
        if (d->kind == NodeKind::Output || d->kind == NodeKind::GroupOutput)
        {
            ChannelList extra = {{o.socket, o.type}};
            NodePorts ports = portsOf(*top.g, *n, &extra);
            const PortDecl* in = ports.input(o.socket);
            const bool linked = !top.g->linksInto(n->name, o.socket).empty();
            const PortDecl* mat = ports.input("material");
            if (!linked && mat && mat->type == PortType::Material && !top.g->linksInto(n->name, "material").empty() && o.field.empty() &&
                std::any_of(kStandard.begin(), kStandard.end(), [&](const auto& c) { return c.first == o.socket; }))
            {
                r.ref = resolveInput(top, *n, *mat);
                r.field = o.socket;
                return r;
            }
            r.ref = resolveInput(top, *n, *in);
            return r;
        }
        r.ref = resolveOutput(top, o.node, o.socket);
        if (r.ref.type == PortType::Material && r.field.empty())
            throw std::invalid_argument(o.name + ": a material output needs a field (baseColor, roughness...)");
        return r;
    }

    // Scheduling -------------------------------------------------------------------------------------------------

    double fusedCost(const Ref& r)
    {
        if (r.constant())
            return 0.0;
        if (mBuffered.contains(r.key()))
            return 4.0;
        auto it = mCost.find(r.key());
        if (it != mCost.end())
            return it->second;
        const FlatNode& n = mNodes[size_t(r.node)];
        double c = n.def->cost;
        for (size_t i = 0; i < n.inputs.size(); ++i)
            c += n.ports.inputs[i].taps * fusedCost(n.inputs[i]);
        for (const ParamDecl& p : n.ports.params)
            if (p.type == ParamType::Image)
                c += 4.0;
        c += 4.0 * double(n.def->externals.size());
        mCost[r.key()] = c;
        return c;
    }

    double rootCost(const Ref& r)
    {
        // The root itself is computed by the pass, not read from its own buffer.
        if (r.constant())
            return 0.0;
        bool was = mBuffered.erase(r.key()) > 0;
        mCost.erase(r.key());
        double c = fusedCost(r);
        mCost.erase(r.key());
        if (was)
            mBuffered.insert(r.key());
        return c;
    }

    void buffer(const Ref& r, const char* why)
    {
        if (r.type == PortType::Material)
        {
            mPlan.warnings.push_back(mNodes[size_t(r.node)].path + "." + r.socket + ": a material can't be buffered (" + why + "), it is re-evaluated");
            return;
        }
        if (mBuffered.insert(r.key()).second)
        {
            mBufferRefs[r.key()] = r;
            mCost.clear();
        }
    }

    void schedule()
    {
        std::map<std::string, int> readers;
        std::map<std::string, Ref> refs;
        for (const FlatNode& n : mNodes)
            for (const Ref& r : n.inputs)
                if (!r.constant())
                {
                    ++readers[r.key()];
                    refs[r.key()] = r;
                }
        for (const Root& r : mRoots)
            if (!r.ref.constant())
            {
                ++readers[r.ref.key()];
                refs[r.ref.key()] = r.ref;
            }

        for (const Ref& r : mRepeatCuts)
            buffer(r, "repeat passes");
        for (size_t i = 0; i < mNodes.size(); ++i)
        {
            const FlatNode& n = mNodes[i];
            if (n.def->kind == NodeKind::Native || n.def->kind == NodeKind::Buffer)
                for (const PortDecl& o : n.ports.outputs)
                    buffer({int(i), o.id, o.type, -1}, "native");
            if (n.ownBuffers())
                for (const PortDecl& o : n.ports.outputs)
                    if (o.type != PortType::Material)
                        buffer({int(i), o.id, o.type, -1}, "node settings");
            for (size_t k = 0; k < n.inputs.size(); ++k)
            {
                const Ref& src = n.inputs[k];
                if (n.def->kind == NodeKind::Native)
                {
                    if (src.constant())
                        mBufferRefs[src.key()] = src, mBuffered.insert(src.key());
                    else
                        buffer(src, "read by a native node");
                    continue;
                }
                if (src.constant())
                    continue;
                const int taps = n.ports.inputs[k].taps;
                if (taps > 1 && taps * fusedCost(src) > mReq.tapBudget && fusedCost(src) > mReq.cheapCost)
                    buffer(src, "sampled many times");
            }
            for (const PortDecl& o : n.ports.outputs)
            {
                Ref r{int(i), o.id, o.type, -1};
                if (readers[r.key()] > 1 && fusedCost(r) > mReq.cheapCost)
                    buffer(r, "read by several inputs");
            }
        }

        // Split passes that would cost too much per texel.
        for (int guard = 0; guard < 256; ++guard)
        {
            std::vector<Ref> roots;
            for (auto& [k, r] : mBufferRefs)
                roots.push_back(r);
            for (const Root& r : mRoots)
                roots.push_back(r.ref);
            bool changed = false;
            for (const Ref& root : roots)
            {
                if (root.constant() || rootCost(root) <= mReq.passBudget)
                    continue;
                Ref worst;
                double worstCost = mReq.cheapCost;
                std::set<std::string> seen;
                std::vector<Ref> stack;
                const FlatNode& rn = mNodes[size_t(root.node)];
                for (const Ref& in : rn.inputs)
                    stack.push_back(in);
                while (!stack.empty())
                {
                    Ref r = stack.back();
                    stack.pop_back();
                    if (r.constant() || mBuffered.contains(r.key()) || !seen.insert(r.key()).second)
                        continue;
                    double c = fusedCost(r);
                    if (r.type != PortType::Material && c > worstCost)
                    {
                        worst = r;
                        worstCost = c;
                    }
                    for (const Ref& in : mNodes[size_t(r.node)].inputs)
                        stack.push_back(in);
                }
                if (worst.node >= 0)
                {
                    buffer(worst, "pass budget");
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }

    // Emission ---------------------------------------------------------------------------------------------------

    int addBuffer(BufferKind kind, std::string name, PortType type, int w, int h)
    {
        BufferDesc b;
        b.id = int(mPlan.buffers.size());
        b.kind = kind;
        b.name = std::move(name);
        b.type = type;
        b.width = w;
        b.height = h;
        b.format = mReq.bufferFormat;
        b.slot = newSlots("", "buffer" + std::to_string(b.id), 1);
        mPlan.buffers.push_back(b);
        return b.id;
    }

    std::string helperOrder(const std::vector<std::string>& wanted, std::set<std::string>& done)
    {
        std::string out;
        std::function<void(const std::string&, int)> visit = [&](const std::string& name, int depth) {
            if (done.contains(name) || depth > 32)
                return;
            const HelperDef* h = findHelper(name);
            if (!h)
                throw std::invalid_argument("no texture helper named " + name);
            done.insert(name);
            for (const std::string& r : h->needs)
                visit(r, depth + 1);
            out += "// helper " + name + "\n" + h->code + "\n";
        };
        for (const std::string& w : wanted)
            visit(w, 0);
        return out;
    }

    std::string paramField(const ParamDecl& p)
    {
        switch (p.type)
        {
        case ParamType::Float: return "float " + p.id;
        case ParamType::Int:
        case ParamType::Enum:
        case ParamType::Seed: return "int " + p.id;
        case ParamType::Bool: return "bool " + p.id;
        case ParamType::Color: return "float4 " + p.id;
        case ParamType::Gradient: return "TexGradient " + p.id;
        case ParamType::Curve: return "TexCurve " + p.id;
        case ParamType::Image:
        case ParamType::Code:
        case ParamType::Node:
        case ParamType::String: return {};
        }
        return {};
    }

    /** The generated name of a node's type: per instance for nodes with code params, since their source differs. */
    std::string typeKey(const FlatNode& n)
    {
        std::string code;
        for (const ParamDecl& p : n.ports.params)
            if (p.type == ParamType::Code)
                code += p.id + "=" + n.values.at(p.id).get<std::string>() + ";";
        if (code.empty())
            return n.def->type;
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash64(code)));
        return n.def->type + "_" + hex;
    }

    /** Text with every `$id` of a code param replaced by its value. */
    std::string expandCode(const FlatNode& n, std::string text)
    {
        for (const ParamDecl& p : n.ports.params)
        {
            if (p.type != ParamType::Code)
                continue;
            const std::string token = "$" + p.id;
            const std::string value = n.values.at(p.id).get<std::string>();
            for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
                text.replace(at, token.size(), value);
        }
        return text;
    }

    /** The generic sampler arguments of a node type with their interfaces: its inputs, then its image params. */
    std::vector<std::pair<std::string, std::string>> samplerArgs(const NodePorts& ports)
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const PortDecl& in : ports.inputs)
            out.emplace_back(in.id, samplerInterface(in.type));
        for (const ParamDecl& p : ports.params)
            if (p.type == ParamType::Image)
                out.emplace_back(p.id, "ITexImage");
        if (ports.def)
            for (const std::string& e : ports.def->externals)
                out.emplace_back(e, samplerInterface(PortType::Color));
        return out;
    }

    std::string typeCode(const FlatNode& n)
    {
        const NodeDef& d = *n.def;
        const std::string key = typeKey(n);
        std::ostringstream s;
        s << "// type " << key << "\nstruct TexP_" << key << "\n{\n    int unused_;\n";
        for (const ParamDecl& p : d.params)
            if (std::string f = paramField(p); !f.empty())
                s << "    " << f << ";\n";
        s << "};\n";
        if (!d.code.empty())
            s << expandCode(n, d.code) << "\n";
        auto args = samplerArgs(n.ports);
        for (const PortDecl& o : d.outputs)
        {
            s << slangType(o.type) << " tex_" << key << "_" << o.id;
            if (!args.empty())
            {
                s << "<";
                for (size_t i = 0; i < args.size(); ++i)
                    s << (i ? ", " : "") << "S_" << args[i].first << " : " << args[i].second;
                s << ">";
            }
            s << "(float2 uv, TexP_" << key << " p";
            for (const auto& [id, t] : args)
                s << ", S_" << id << " " << id;
            s << ")\n{\n" << expandCode(n, d.body.at(o.id)) << "\n}\n";
        }
        return s.str();
    }

    /** Expression for a ref's value at `uv` inside a pass whose own root is root. */
    std::string refExpr(const Ref& r, const std::string& rootKey)
    {
        if (r.uv)
            return r.type == PortType::Vector ? "float3(uv, 0.0)" : r.type == PortType::Color ? "float4(uv, 0.0, 1.0)" : "uv.x";
        if (r.constant())
            return r.type == PortType::Material ? std::string("texDefaultMaterial()") : "gU[" + std::to_string(r.slot) + "]" + swizzle(r.type);
        if (r.key() != rootKey && mBuffered.contains(r.key()))
            return "texReadBuf(" + std::to_string(mPlan.buffers[size_t(mBufferOf.at(r.key()))].slot) + ", uv)" + swizzle(r.type);
        return "n" + std::to_string(r.node) + "_" + r.socket + "(uv)";
    }

    void collect(const Ref& r, const std::string& rootKey, std::set<int>& nodes, std::set<std::string>& reads)
    {
        if (r.constant())
            return;
        if (r.key() != rootKey && mBuffered.contains(r.key()))
        {
            reads.insert(r.key());
            return;
        }
        if (!nodes.insert(r.node).second)
            return;
        for (const Ref& in : mNodes[size_t(r.node)].inputs)
            collect(in, rootKey, nodes, reads);
    }

    std::string paramBuilder(int index, const FlatNode& n)
    {
        std::ostringstream s;
        const std::string key = typeKey(n);
        s << "TexP_" << key << " p" << index << "()\n{\n    TexP_" << key << " p;\n    p.unused_ = 0;\n";
        for (const ParamDecl& p : n.ports.params)
        {
            const int slot = n.paramSlot.at(p.id);
            const std::string u = "gU[" + std::to_string(slot) + "]";
            switch (p.type)
            {
            case ParamType::Float: s << "    p." << p.id << " = " << u << ".x;\n"; break;
            case ParamType::Int:
            case ParamType::Enum:
            case ParamType::Seed: s << "    p." << p.id << " = int(" << u << ".x);\n"; break;
            case ParamType::Bool: s << "    p." << p.id << " = " << u << ".x != 0.0;\n"; break;
            case ParamType::Color: s << "    p." << p.id << " = " << u << ";\n"; break;
            case ParamType::Gradient:
            case ParamType::Curve: s << "    p." << p.id << ".base = " << slot << "u;\n"; break;
            case ParamType::Image:
            case ParamType::Code:
            case ParamType::Node:
            case ParamType::String: break;
            }
        }
        s << "    return p;\n}\n";
        return s.str();
    }

    std::vector<int> valueSlots(const std::set<int>& nodes, const Ref& root)
    {
        std::set<int> out;
        auto constant = [&](const Ref& r) {
            if (r.constant() && r.slot >= 0)
                out.insert(r.slot);
        };
        constant(root);
        for (int i : nodes)
        {
            const FlatNode& n = mNodes[size_t(i)];
            for (const Ref& r : n.inputs)
                constant(r);
            for (const ParamDecl& p : n.ports.params)
                for (int k = 0; k < paramSlots(p.type); ++k)
                    out.insert(n.paramSlot.at(p.id) + k);
        }
        return {out.begin(), out.end()};
    }

    /**
     * Whether a node's code samples the sampler argument id only at its own uv: every use is `id.sample(uv)` (or
     * `id.sample(uv * float(p.k))` with k at 1, or the same as `id.sampleImage(..., wrap, filter)` without cubic filtering)
     * and uv is never reassigned. Anything else, such as passing the sampler to a
     * helper, counts as non-local.
     */
    static bool samplesAtUv(const std::string& text, const std::string& id, const FlatNode& n)
    {
        auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (size_t at = text.find("uv"); at != std::string::npos; at = text.find("uv", at + 2))
        {
            if ((at > 0 && (word(text[at - 1]) || text[at - 1] == '.')) || (at + 2 < text.size() && word(text[at + 2])))
                continue;
            size_t k = at + 2;
            while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k])))
                ++k;
            if (k + 1 < text.size() && std::strchr("+-*/", text[k]) && text[k + 1] == '=')
                return false;
            if (k < text.size() && text[k] == '=' && (k + 1 >= text.size() || text[k + 1] != '='))
                return false;
        }
        auto unitParam = [&](const std::string& name) {
            auto it = n.values.find(name);
            return it != n.values.end() && it->second.is_number() && it->second.get<double>() == 1.0;
        };
        for (size_t at = text.find(id); at != std::string::npos; at = text.find(id, at + id.size()))
        {
            if ((at > 0 && (word(text[at - 1]) || text[at - 1] == '.')) || (at + id.size() < text.size() && word(text[at + id.size()])))
                continue;
            const std::string rest = text.substr(at + id.size(), 64);
            // An image read's wrap, filter and footprint args leave it reading the texels at uv, unless cubic filtering widens it.
            const bool image = rest.starts_with(".sampleImage(");
            if (image)
            {
                auto it = n.values.find("filter");
                if (it != n.values.end() && ((it->second.is_string() && it->second.get<std::string>() == "cubic") ||
                                             (it->second.is_number() && it->second.get<int>() == 2)))
                    return false;
            }
            const std::string method = image ? ".sampleImage(" : ".sample(";
            const std::string end = image ? "," : ")";
            if (rest.starts_with(method + "uv" + end))
                continue;
            bool scaled = false;
            for (const std::string scale : {"uv * float(p.", "uv * p."})
                if (const std::string form = method + scale; rest.starts_with(form))
                {
                    const size_t from = form.size();
                    size_t to = from;
                    while (to < rest.size() && word(rest[to]))
                        ++to;
                    const std::string close = (scale.find("float(") != std::string::npos ? ")" : "") + end;
                    scaled = rest.compare(to, close.size(), close) == 0 && unitParam(rest.substr(from, to - from));
                }
            if (!scaled)
                return false;
        }
        return true;
    }

    /**
     * Buffers a pass reads anywhere but at its own uv (see Pass::local); those read only within a known texel distance of
     * it (inputs with a reach, through nodes sampled at uv) go in nearby instead, with that distance.
     */
    void nonLocalReads(const Ref& r, const std::string& rootKey, bool identity, int reach, std::set<std::tuple<int, bool, int>>& seen,
                       std::set<int>& out, std::map<int, int>& nearby)
    {
        if (r.constant())
            return;
        if (r.key() != rootKey && mBuffered.contains(r.key()))
        {
            const int b = mBufferOf.at(r.key());
            if (!identity)
                out.insert(b);
            else if (reach > 0)
                nearby[b] = std::max(nearby[b], reach);
            return;
        }
        if (!seen.insert({r.node, identity, reach}).second)
            return;
        const FlatNode& n = mNodes[size_t(r.node)];
        if (n.def->kind == NodeKind::Buffer)
        {
            nonLocalReads(n.inputs[0], rootKey, identity, reach, seen, out, nearby);
            return;
        }
        std::string text = expandCode(n, n.def->code);
        for (const auto& [id, body] : n.def->body)
            text += "\n" + expandCode(n, body);
        // An unlinked coordinate input reads the node's own uv, so what is sampled through it stays local.
        for (size_t k = 0; k < n.inputs.size(); ++k)
            if (n.inputs[k].uv)
                for (const std::string& read : {n.ports.inputs[k].id + ".sample(uv).xy", n.ports.inputs[k].id + ".sample(uv)"})
                    for (size_t at = text.find(read); at != std::string::npos; at = text.find(read, at))
                        text.replace(at, read.size(), "uv");
        for (size_t k = 0; k < n.inputs.size(); ++k)
        {
            const bool atUv = samplesAtUv(text, n.ports.inputs[k].id, n);
            const int inputReach = n.ports.inputs[k].reach;
            if (identity && !atUv && inputReach > 0)
                nonLocalReads(n.inputs[k], rootKey, true, reach + inputReach, seen, out, nearby);
            else
                nonLocalReads(n.inputs[k], rootKey, identity && atUv, reach, seen, out, nearby);
        }
        const std::string index = std::to_string(r.node);
        for (const ParamDecl& p : n.ports.params)
            if (p.type == ParamType::Image && !(identity && samplesAtUv(text, p.id, n)))
                out.insert(mImageBuffer.at(index + "." + p.id));
        for (const std::string& e : n.def->externals)
            if (!(identity && samplesAtUv(text, e, n)))
                out.insert(mExternalBuffer.at(e));
    }

    std::string passSource(const Root& root, std::vector<int>& readsOut, std::vector<int>* slotsOut = nullptr, std::vector<int>* localOut = nullptr,
                           std::map<int, int>* reachOut = nullptr)
    {
        const std::string rootKey = root.fromBuffer ? std::string() : root.ref.key();
        std::set<int> nodes;
        std::set<std::string> reads;
        collect(root.ref, rootKey, nodes, reads);
        if (slotsOut)
            *slotsOut = valueSlots(nodes, root.ref);

        std::ostringstream src;
        src << "// Lila texture graph pass\n" << kPrelude;

        std::vector<std::string> helperNames;
        std::vector<std::string> types;
        for (int i : nodes)
        {
            const NodeDef& d = *mNodes[size_t(i)].def;
            for (const std::string& h : d.helpers)
                if (std::find(helperNames.begin(), helperNames.end(), h) == helperNames.end())
                    helperNames.push_back(h);
        }
        std::set<std::string> done;
        src << helperOrder(helperNames, done);
        for (int i : nodes)
        {
            const FlatNode& n = mNodes[size_t(i)];
            const std::string key = typeKey(n);
            if (std::find(types.begin(), types.end(), key) != types.end())
                continue;
            types.push_back(key);
            if (n.def->kind == NodeKind::Slang)
                src << typeCode(n);
        }

        for (int i : nodes)
        {
            const FlatNode& n = mNodes[size_t(i)];
            const std::string tag = "n" + std::to_string(i);
            src << "// node " << i << "\n";
            if (n.def->kind == NodeKind::Buffer)
            {
                const Ref& in = n.inputs[0];
                const PortDecl& o = n.ports.outputs[0];
                src << slangType(o.type) << " " << tag << "_" << o.id << "(float2 uv) { return " << convFn(o.type) << "(" << refExpr(in, rootKey)
                    << "); }\n";
                continue;
            }
            if (n.def->kind != NodeKind::Slang)
                throw std::invalid_argument(n.path + ": a " + nodeKindName(n.def->kind) + " node can't be fused");
            src << paramBuilder(i, n);
            auto args = samplerArgs(n.ports);
            for (size_t k = 0; k < n.inputs.size(); ++k)
            {
                const PortDecl& decl = n.ports.inputs[k];
                src << "struct " << tag << "_s_" << decl.id << " : " << samplerInterface(decl.type) << "\n{\n    " << slangType(decl.type)
                    << " sample(float2 uv) { return " << convFn(decl.type) << "(" << refExpr(n.inputs[k], rootKey) << "); }\n};\n";
            }
            for (const ParamDecl& p : n.ports.params)
                if (p.type == ParamType::Image)
                {
                    int b = mImageBuffer.at(std::to_string(i) + "." + p.id);
                    const std::string slot = std::to_string(mPlan.buffers[size_t(b)].slot);
                    src << "struct " << tag << "_s_" << p.id << " : ITexImage\n{\n    float4 sample(float2 uv) { return texReadBuf(" << slot
                        << ", uv); }\n    float4 sampleImage(float2 uv, int wrap, int filter, float2 dx, float2 dy) { return texReadImage(" << slot
                        << ", uv, wrap, filter, dx, dy); }\n};\n";
                    readsOut.push_back(b);
                }
            for (const std::string& e : n.def->externals)
            {
                int b = mExternalBuffer.at(e);
                src << "struct " << tag << "_s_" << e << " : ITexColor\n{\n    float4 sample(float2 uv) { return texReadBuf("
                    << mPlan.buffers[size_t(b)].slot << ", uv); }\n};\n";
                readsOut.push_back(b);
            }
            for (const PortDecl& o : n.ports.outputs)
            {
                src << slangType(o.type) << " " << tag << "_" << o.id << "(float2 uv)\n{\n";
                for (const auto& [id, t] : args)
                    src << "    " << tag << "_s_" << id << " s_" << id << ";\n";
                src << "    return tex_" << typeKey(n) << "_" << o.id;
                if (!args.empty())
                {
                    src << "<";
                    for (size_t a = 0; a < args.size(); ++a)
                        src << (a ? ", " : "") << tag << "_s_" << args[a].first;
                    src << ">";
                }
                src << "(uv, p" << i << "()";
                for (const auto& [id, t] : args)
                    src << ", s_" << id;
                src << ");\n}\n";
            }
        }

        std::string value = refExpr(root.ref, rootKey);
        PortType valueType = root.ref.type;
        if (!root.field.empty())
        {
            auto it = std::find_if(kStandard.begin(), kStandard.end(), [&](const auto& c) { return c.first == root.field; });
            if (it == kStandard.end())
                throw std::invalid_argument(root.name + ": a material has no field " + root.field);
            value = "(" + value + ")." + root.field;
            valueType = it->second;
        }
        if (!linkable(valueType, root.store))
            throw std::invalid_argument(root.name + ": a " + std::string(portTypeName(valueType)) + " can't be stored as " + portTypeName(root.store));
        src << "\n[shader(\"compute\")]\n[numthreads(8, 8, 1)]\nvoid texMain(uint3 id : SV_DispatchThreadID)\n{\n"
            << "    uint w = uint(gPass.res.x);\n    uint h = uint(gPass.res.y);\n    if (id.x >= w || id.y >= h)\n        return;\n"
            << "    float2 uv = gPass.uv.xy + float2((float(id.x) + 0.5) * gPass.res.z, 1.0 - (float(id.y) + 0.5) * gPass.res.w) * gPass.uv.zw;\n"
            << "    gOut[id.y * w + id.x] = " << (root.quantize ? "texQuantize8(" : "") << "texStore(" << convFn(root.store) << "(" << value << "))"
            << (root.quantize ? ")" : "") << ";\n}\n";

        for (const std::string& k : reads)
            readsOut.push_back(mBufferOf.at(k));
        std::sort(readsOut.begin(), readsOut.end());
        readsOut.erase(std::unique(readsOut.begin(), readsOut.end()), readsOut.end());
        if (localOut)
        {
            std::set<std::tuple<int, bool, int>> seen;
            std::set<int> distant;
            std::map<int, int> nearby;
            nonLocalReads(root.ref, rootKey, true, 0, seen, distant, nearby);
            localOut->clear();
            if (reachOut)
                reachOut->clear();
            for (int b : readsOut)
                if (distant.contains(b))
                    continue;
                else if (auto it = nearby.find(b); it != nearby.end())
                {
                    if (reachOut)
                        (*reachOut)[b] = it->second;
                }
                else
                    localOut->push_back(b);
        }
        return src.str();
    }

    Pass nativePass(const Ref& r)
    {
        const FlatNode& n = mNodes[size_t(r.node)];
        Pass p;
        p.kind = PassKind::Native;
        p.nativeType = n.def->type;
        p.node = n.path;
        for (const auto& [id, slot] : n.paramSlot)
            p.paramSlots[id] = slot;
        for (size_t k = 0; k < n.inputs.size(); ++k)
        {
            int b = mBufferOf.at(n.inputs[k].key());
            p.inputBuffers[n.ports.inputs[k].id] = b;
            p.reads.push_back(b);
        }
        p.slots = valueSlots({r.node}, Ref{});
        json key = {{"native", p.nativeType}, {"params", p.paramSlots}, {"inputs", p.inputBuffers}};
        p.hash = hash64(key.dump());
        p.cost = n.def->cost;
        return p;
    }

    void emit()
    {
        // Image parameters become buffers, and every value lands in its slots.
        for (size_t i = 0; i < mNodes.size(); ++i)
        {
            FlatNode& n = mNodes[i];
            for (const ParamDecl& p : n.ports.params)
            {
                const int slot = n.paramSlot[p.id];
                const json& v = n.values[p.id];
                if (p.type == ParamType::Image)
                {
                    const std::string path = p.source == "text"  ? textSourceName(n.values, mPlan.width, mPlan.height)
                                             : v.is_string() ? v.get<std::string>()
                                                             : std::string();
                    // "external:<name>" reads a caller-supplied map (a paint buffer) instead of a file.
                    int b;
                    if (path.starts_with("external:"))
                    {
                        const std::string name = path.substr(9);
                        if (!mExternalBuffer.contains(name))
                            mExternalBuffer[name] = addBuffer(BufferKind::External, name, PortType::Color, 0, 0);
                        b = mExternalBuffer.at(name);
                    }
                    else
                        b = addBuffer(BufferKind::Image, path, PortType::Color, 0, 0);
                    mImageBuffer[std::to_string(i) + "." + p.id] = b;
                    continue;
                }
                packParam(p, v, slot);
            }
            for (const std::string& e : n.def->externals)
                if (!mExternalBuffer.contains(e))
                    mExternalBuffer[e] = addBuffer(BufferKind::External, e, PortType::Color, 0, 0);
        }

        // Buffered outputs in dependency order: constants first, then by node (flattening is post-order).
        std::vector<Ref> order;
        for (auto& [k, r] : mBufferRefs)
            order.push_back(r);
        std::sort(order.begin(), order.end(), [](const Ref& a, const Ref& b) {
            if (a.node != b.node)
                return a.node < b.node;
            return a.constant() ? a.slot < b.slot : a.socket < b.socket;
        });
        for (const Ref& r : order)
        {
            const FlatNode* n = r.constant() ? nullptr : &mNodes[size_t(r.node)];
            std::string name = n ? n->path + "." + r.socket : "constant" + std::to_string(r.slot);
            const int shift = n ? n->sizeShift : 0;
            const int b = addBuffer(BufferKind::Intermediate, name, r.type, scaledSize(mPlan.width, shift), scaledSize(mPlan.height, shift));
            if (n && n->bits)
                mPlan.buffers[size_t(b)].format = n->bits == 32 ? "rgba32f" : "rgba16f";
            if (n && n->bits == 8 && n->def->kind == NodeKind::Native)
                mPlan.warnings.push_back(name + ": a native node's output keeps 16 bits; 8-bit steps come from fused nodes only");
            mBufferOf[r.key()] = b;
        }
        for (const Ref& r : order)
        {
            Pass p;
            if (!r.constant() && mNodes[size_t(r.node)].def->kind == NodeKind::Native)
                p = nativePass(r);
            else
            {
                Root root{"", r, r.type, {}};
                root.quantize = !r.constant() && mNodes[size_t(r.node)].bits == 8;
                p.source = passSource(root, p.reads, &p.slots, &p.local, &p.reach);
                p.hash = hash64(p.source);
                p.node = r.constant() ? "constant" : mNodes[size_t(r.node)].path;
                p.cost = rootCost(r);
            }
            p.output = mBufferOf.at(r.key());
            mPlan.passes.push_back(std::move(p));
        }

        std::set<int> claimed;
        for (const Root& root : mRoots)
        {
            auto it = mBufferOf.find(root.ref.key());
            if (it != mBufferOf.end() && root.field.empty() && storageCompatible(root.ref.type, root.store) && !claimed.contains(it->second) &&
                mPlan.buffers[size_t(it->second)].width == mPlan.width && mPlan.buffers[size_t(it->second)].height == mPlan.height)
            {
                BufferDesc& b = mPlan.buffers[size_t(it->second)];
                b.kind = BufferKind::Output;
                b.name = root.name;
                b.type = root.store;
                claimed.insert(it->second);
                mPlan.outputs[root.name] = it->second;
                continue;
            }
            int b = addBuffer(BufferKind::Output, root.name, root.store, mPlan.width, mPlan.height);
            Root converted = root;
            converted.fromBuffer = it != mBufferOf.end();
            Pass p;
            p.source = passSource(converted, p.reads, &p.slots, &p.local, &p.reach);
            p.hash = hash64(p.source);
            p.node = root.ref.constant() ? "constant" : mNodes[size_t(root.ref.node)].path;
            p.cost = rootCost(root.ref);
            p.output = b;
            mPlan.passes.push_back(std::move(p));
            mPlan.outputs[root.name] = b;
        }

        // Mip levels pack below level 0, so only buffers every node reads through sampleImage (which knows the layout) get them.
        std::set<int> plainReads;
        for (const auto& [key, b] : mImageBuffer)
        {
            const FlatNode& n = mNodes[size_t(std::stoi(key.substr(0, key.find('.'))))];
            const std::string id = key.substr(key.find('.') + 1);
            for (const auto& [out, text] : n.def->body)
                for (size_t at = text.find(id + ".sample("); at != std::string::npos; at = text.find(id + ".sample(", at + 1))
                    if (at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_' || text[at - 1] == '.'))
                        plainReads.insert(b);
        }
        for (const Pass& p : mPlan.passes)
            for (int b : p.reads)
                if (BufferDesc& d = mPlan.buffers[size_t(b)]; d.kind == BufferKind::Image && p.kind == PassKind::Fused && !plainReads.contains(b) &&
                                                              std::find(p.local.begin(), p.local.end(), b) == p.local.end() && !p.reach.contains(b))
                    d.mips = true;
    }

    void packParam(const ParamDecl& p, const json& v, int slot)
    {
        std::vector<float>& u = mPlan.uniforms;
        float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        switch (p.type)
        {
        case ParamType::Float:
        case ParamType::Int:
        case ParamType::Seed: f[0] = v.get<float>(); break;
        case ParamType::Bool: f[0] = (v.is_boolean() ? v.get<bool>() : v.get<double>() != 0.0) ? 1.0f : 0.0f; break;
        case ParamType::Enum:
            if (v.is_number())
                f[0] = v.get<float>();
            else
                f[0] = float(std::find(p.options.begin(), p.options.end(), v.get<std::string>()) - p.options.begin());
            break;
        case ParamType::Color: valueToFloat4(v, PortType::Color, f); break;
        case ParamType::Gradient:
        {
            const json stops = v.is_object() ? v.value("stops", json::array()) : v;
            std::string interp = v.is_object() ? v.value("interpolation", "linear") : "linear";
            std::vector<std::pair<float, std::array<float, 4>>> sorted;
            for (const json& s : stops)
            {
                std::array<float, 4> c{0.0f, 0.0f, 0.0f, 1.0f};
                valueToFloat4(s[1], PortType::Color, c.data());
                sorted.emplace_back(s[0].get<float>(), c);
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            if (sorted.size() > size_t(kMaxGradientStops))
            {
                mPlan.warnings.push_back(p.id + ": only the first " + std::to_string(kMaxGradientStops) + " gradient stops are used");
                sorted.resize(size_t(kMaxGradientStops));
            }
            f[0] = float(sorted.size());
            f[1] = interp == "constant" ? 1.0f : interp == "smooth" ? 2.0f : 0.0f;
            packFloat4(u, slot, f);
            for (size_t k = 0; k < sorted.size(); ++k)
            {
                float pos[4] = {sorted[k].first, 0.0f, 0.0f, 0.0f};
                packFloat4(u, slot + 1 + int(2 * k), pos);
                packFloat4(u, slot + 2 + int(2 * k), sorted[k].second.data());
            }
            return;
        }
        case ParamType::Curve:
        {
            const json points = v.is_object() ? v.value("points", json::array()) : v;
            bool smooth = v.is_object() ? v.value("smooth", true) : true;
            std::vector<std::pair<float, float>> sorted;
            for (const json& pt : points)
                sorted.emplace_back(pt[0].get<float>(), pt[1].get<float>());
            std::stable_sort(sorted.begin(), sorted.end());
            if (sorted.size() > size_t(kMaxCurvePoints))
            {
                mPlan.warnings.push_back(p.id + ": only the first " + std::to_string(kMaxCurvePoints) + " curve points are used");
                sorted.resize(size_t(kMaxCurvePoints));
            }
            f[0] = float(sorted.size());
            f[1] = smooth ? 1.0f : 0.0f;
            packFloat4(u, slot, f);
            for (size_t k = 0; k < sorted.size(); ++k)
            {
                u[size_t(slot + 1 + int(k / 2)) * 4 + (k % 2) * 2] = sorted[k].first;
                u[size_t(slot + 1 + int(k / 2)) * 4 + (k % 2) * 2 + 1] = sorted[k].second;
            }
            return;
        }
        case ParamType::Image:
        case ParamType::Code:
        case ParamType::Node:
        case ParamType::String: return;
        }
        packFloat4(u, slot, f);
    }

    void finish()
    {
        json structure = json::array();
        for (const Pass& p : mPlan.passes)
            structure.push_back({p.hash, p.output, p.reads});
        json bufs = json::array();
        for (const BufferDesc& b : mPlan.buffers)
            bufs.push_back({int(b.kind), b.kind == BufferKind::Image ? std::string() : b.name, int(b.type), b.width, b.height, b.format, b.slot, b.mips});
        json all = {{"passes", structure}, {"buffers", bufs}, {"slots", mPlan.uniformSlots}, {"outputs", mPlan.outputs}};
        mPlan.structureHash = hash64(all.dump());
    }

    const PlanRequest& mReq;
    double mTime = 0.0;
    Plan mPlan;
    /** The graph seed, added to every seed parameter. */
    int64_t mSeed = 0;
    std::vector<FlatNode> mNodes;
    std::vector<Root> mRoots;
    std::set<std::string> mBuffered;
    std::map<std::string, Ref> mBufferRefs;
    std::map<std::string, double> mCost;
    std::map<std::string, int> mBufferOf;
    std::map<std::string, int> mImageBuffer;
    std::map<std::string, int> mExternalBuffer;
    /** Repeat lanes buffered between groups of passes. */
    std::vector<Ref> mRepeatCuts;
};
} // namespace

uint64_t hash64(std::string_view data, uint64_t seed)
{
    uint64_t h = seed;
    for (unsigned char c : data)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

const std::vector<std::pair<std::string, PortType>>& standardChannels() { return kStandard; }

const std::vector<NodeSetting>& nodeSettings() { return kNodeSettings; }

const NodeSetting* nodeSetting(std::string_view id)
{
    for (const NodeSetting& s : kNodeSettings)
        if (s.id == id)
            return &s;
    return nullptr;
}

std::vector<OutputRequest> materialRequests(const Graph& g, const std::vector<std::pair<std::string, PortType>>& channels)
{
    std::vector<OutputRequest> out;
    const GNode* n = g.firstOfType(kMaterialOutputType);
    if (!n)
        return out;
    for (const auto& [name, type] : channels)
        out.push_back({name, n->name, name, type, {}});
    return out;
}

Plan buildPlan(const Graph& g, const PlanRequest& request)
{
    Graph resolved = g;
    resolveRouting(resolved);
    Builder b(request);
    return b.run(resolved);
}

json Plan::summary() const
{
    json passesJ = json::array();
    for (const Pass& p : passes)
        passesJ.push_back({{"kind", p.kind == PassKind::Fused ? "fused" : "native"},
                           {"node", p.node},
                           {"cost", p.cost},
                           {"reads", p.reads},
                           {"output", p.output},
                           {"native", p.nativeType},
                           {"hash", std::to_string(p.hash)}});
    json buffersJ = json::array();
    static const char* kinds[] = {"intermediate", "output", "image", "external"};
    for (const BufferDesc& b : buffers)
        buffersJ.push_back({{"id", b.id}, {"kind", kinds[int(b.kind)]}, {"name", b.name}, {"type", portTypeName(b.type)}, {"width", b.width},
                            {"height", b.height}, {"format", b.format}, {"mips", b.mips}});
    json out = {{"passes", passesJ},  {"buffers", buffersJ}, {"uniformSlots", uniformSlots}, {"structureHash", std::to_string(structureHash)},
                {"outputs", outputs}, {"width", width},      {"height", height}};
    if (!error.empty())
        out["error"] = error;
    if (!warnings.empty())
        out["warnings"] = warnings;
    return out;
}
} // namespace lila::tex
