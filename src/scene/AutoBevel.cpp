#include "scene/AutoBevel.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <set>

namespace lila::scene
{
namespace
{
using geo::Index;

enum Attr
{
    kInMesh,
    kEnabled,
    kMode,
    kMinAngle,
    kMaxAngle,
    kSide,
    kThin,
    kMinLength,
    kCompleteCorners,
    kWeightThreshold,
    kOffset,
    kSegments,
    kProfile,
    kClamp,
    kSmoothingAngle,
    kShowEdges,
    kEditing,
    kOverrides,
    kEditBackup,
    kOutMesh,
    kChosen,
    kEdgeCount,
    kLostOverrides,
};

enum Mode : int64_t
{
    kAuto,
    kAutoOverrides,
    kManual,
    kAngle,
    kWeight,
};

const std::vector<std::string> kModeNames = {"auto", "autoOverrides", "manual", "angle", "weight"};
const std::vector<std::string> kBevelSideNames = {"both", "outside", "inside"};

/** Opaque json payload: the selection to restore when Edit Edges ends. */
struct JsonData final : Data
{
    json value;
    const char* kind() const override { return "bevelEditBackup"; }
    json toJson() const override { return {{"kind", kind()}, {"value", value}}; }
};

geo::Mesh bevelChosen(const geo::Mesh& in, const Node& n, std::span<const Index> edges, BevelEdgeSet* set = nullptr);
std::vector<Index> chooseEdges(const geo::Mesh& m, const Node& n, int* lost);

class AutoBevelNode final : public ModifierNode
{
public:
    AutoBevelNode()
    {
        inAttr = kInMesh;
        enabledAttr = kEnabled;
        outAttr = kOutMesh;
        // Re-runs (exports, uv solves on the exported mesh) bevel whatever input they give.
        apply = [](const geo::Mesh& in, const Node& n) { return bevelChosen(in, n, chooseEdges(in, n, nullptr)); };
    }

    bool editingInput() const override { return as<bool>(kEditing); }
    void compute(const EvalContext& ctx) override;
};

json refToJson(const geo::EdgeRef& r)
{
    return json::array({r.edge, r.p0, r.p1, r.a.x, r.a.y, r.a.z, r.b.x, r.b.y, r.b.z});
}

geo::EdgeRef refFromJson(const json& j)
{
    geo::EdgeRef r;
    if (!j.is_array() || j.size() != 9)
        return r;
    r.edge = j[0].get<Index>();
    r.p0 = j[1].get<Index>();
    r.p1 = j[2].get<Index>();
    r.a = {j[3].get<double>(), j[4].get<double>(), j[5].get<double>()};
    r.b = {j[6].get<double>(), j[7].get<double>(), j[8].get<double>()};
    return r;
}

geo::FoldEdgeOptions foldOptions(const Node& n)
{
    geo::FoldEdgeOptions o;
    o.minAngle = n.as<double>(kMinAngle);
    o.maxAngle = n.as<double>(kMaxAngle);
    o.side = geo::FoldSide(std::clamp<int64_t>(n.as<int64_t>(kSide), 0, 2));
    o.thin = n.as<double>(kThin);
    o.minLength = n.as<double>(kMinLength);
    o.completeCorners = n.as<bool>(kCompleteCorners);
    return o;
}

/** The edges the mode's rule picks, before overrides. */
std::vector<Index> ruleEdges(const geo::Mesh& m, const Node& n)
{
    switch (n.as<int64_t>(kMode))
    {
    case kAuto:
    case kAutoOverrides: return geo::foldEdges(m, foldOptions(n));
    case kAngle: return geo::angleEdges(m, n.as<double>(kMinAngle));
    case kWeight: return geo::weightedEdges(m, n.as<double>(kWeightThreshold));
    default: return {};
    }
}

bool sameTopology(const geo::Mesh& m, const BevelOverrides& o)
{
    return m.pointCapacity() == o.points && m.edgeCapacity() == o.edges && m.faceCapacity() == o.faces;
}

/** Rule plus overrides; plain auto ignores the overrides. */
std::vector<Index> chooseEdges(const geo::Mesh& m, const Node& n, int* lost)
{
    LILA_ZONE();

    std::vector<Index> rule = ruleEdges(m, n);
    auto* o = dynamic_cast<const BevelOverrides*>(n.as<DataPtr>(kOverrides).get());
    if (lost)
        *lost = 0;
    if (!o || n.as<int64_t>(kMode) == kAuto || (o->add.empty() && o->remove.empty()))
        return rule;
    std::vector<uint8_t> on(m.edgeCapacity(), 0);
    for (Index e : rule)
        on[e] = 1;
    geo::EdgeMatcher matcher(m);
    const bool same = sameTopology(m, *o);
    for (auto [refs, value] : {std::pair{&o->remove, uint8_t(0)}, {&o->add, uint8_t(1)}})
        for (const geo::EdgeRef& r : *refs)
        {
            Index e = matcher.find(r, same);
            if (e == geo::kInvalid)
            {
                if (lost)
                    ++*lost;
                continue;
            }
            on[e] = value;
        }
    std::vector<Index> out;
    for (Index e = 0; e < Index(on.size()); ++e)
        if (on[e])
            out.push_back(e);
    return out;
}

geo::Mesh bevelChosen(const geo::Mesh& in, const Node& n, std::span<const Index> edges, BevelEdgeSet* set)
{
    geo::Mesh m = in;
    if (edges.empty())
        return m;
    geo::BevelOptions o;
    if (set)
    {
        o.appliedOffset = &set->applied;
        o.edgeLimits = &set->limits;
    }
    o.offset = n.as<double>(kOffset);
    o.segments = int(std::clamp<int64_t>(n.as<int64_t>(kSegments), 1, 32));
    o.profile = n.as<double>(kProfile);
    o.clamp = n.as<bool>(kClamp);
    o.smoothingAngle = n.as<double>(kSmoothingAngle);
    if (set)
        set->offset = o.offset;
    geo::bevelEdges(m, edges, o);
    m.compact();
    return m;
}

void AutoBevelNode::compute(const EvalContext&)
{
    LILA_ZONE();

    const MeshPtr& in = as<MeshPtr>(kInMesh);
    auto set = std::make_shared<BevelEdgeSet>();
    set->mesh = in;
    setOutput(kOutMesh, in);
    setOutput(kEdgeCount, int64_t(0));
    setOutput(kLostOverrides, int64_t(0));
    if (!in || (!as<bool>(kEnabled) && !as<bool>(kEditing)) || modifiers::bypassed(in))
    {
        setOutput(kChosen, DataPtr(set));
        return;
    }
    try
    {
        set->edges = chooseEdges(*in, *this, &set->lost);
        setOutput(kEdgeCount, int64_t(set->edges.size()));
        setOutput(kLostOverrides, int64_t(set->lost));
        if (as<bool>(kEditing))
        {
            // Bevelled only for its limits, so the edges that hold the offset back show while editing.
            if (!set->edges.empty())
                bevelChosen(*modifiers::stackInput(*this, in), *this, set->edges, set.get());
            auto shown = std::make_shared<geo::Mesh>(*in);
            shown->pointAttrs().add(modifiers::kBypassLayer, geo::AttrType::Bool);
            setOutput(kOutMesh, MeshPtr(std::move(shown)));
        }
        else if (!set->edges.empty())
            setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(bevelChosen(*modifiers::stackInput(*this, in), *this, set->edges, set.get()))));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        setOutput(kOutMesh, in);
    }
    setOutput(kChosen, DataPtr(set));
}

AttrDef bevelParam(std::string name, PlugType type, PlugValue def, std::string doc, uint32_t extra = AttrKeyable, std::optional<double> lo = {},
              std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | extra,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

/** A named autoBevelModifier, or the one editing (else the topmost) in a named or selected mesh's stack. */
Node& modifierArg(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n)
            continue;
        if (autoBevel::isAutoBevel(*n))
            return *n;
        if (n->type().isA("mesh"))
        {
            std::vector<NodeId> s = modifiers::stack(g, n->id());
            for (NodeId id : s)
                if (autoBevel::editing(*g.node(id)))
                    return *g.node(id);
            for (auto it = s.rbegin(); it != s.rend(); ++it)
                if (autoBevel::isAutoBevel(*g.node(*it)))
                    return *g.node(*it);
        }
    }
    throw CommandError(std::string(cmd) + ": give an autoBevel modifier or a mesh that has one");
}

std::shared_ptr<const geo::Mesh> bevelInput(Graph& g, Node& mod)
{
    g.evaluate();
    return mod.as<MeshPtr>(kInMesh);
}

void endEdit(Graph& g, Node& mod)
{
    NodeId mesh = autoBevel::meshOf(g, mod.id());
    json backup;
    if (auto* d = dynamic_cast<const JsonData*>(mod.as<DataPtr>(kEditBackup).get()))
        backup = d->value;
    g.setValue({mod.id(), kEditing, -1}, false);
    g.setValue({mod.id(), kEditBackup, -1}, DataPtr());
    if (mesh != kNoNode)
        modifiers::restoreSelection(g, mesh, backup);
}

void selectChosen(Graph& g, Node& mod, NodeId mesh)
{
    auto set = autoBevel::chosen(g, mod.id());
    g.select({mesh});
    components::select(mesh, ComponentMode::Edge, set ? set->edges : components::Ids{});
    components::setMode(ComponentMode::Edge);
}

std::shared_ptr<BevelOverrides> overridesFor(const geo::Mesh& m, const std::vector<Index>& add, const std::vector<Index>& remove)
{
    auto o = std::make_shared<BevelOverrides>();
    o->points = m.pointCapacity();
    o->edges = m.edgeCapacity();
    o->faces = m.faceCapacity();
    for (Index e : add)
        o->add.push_back(geo::edgeRef(m, e));
    for (Index e : remove)
        o->remove.push_back(geo::edgeRef(m, e));
    return o;
}

int64_t enumArg(const Args& a, const char* flag, const std::vector<std::string>& names, const char* cmd)
{
    std::string v = a.get<std::string>(flag);
    auto it = std::find(names.begin(), names.end(), v);
    if (it == names.end())
        throw CommandError(std::string(cmd) + ": unknown " + flag + " '" + v + "'");
    return int64_t(it - names.begin());
}
} // namespace

json BevelOverrides::toJson() const
{
    json added = json::array(), removed = json::array();
    for (const geo::EdgeRef& r : add)
        added.push_back(refToJson(r));
    for (const geo::EdgeRef& r : remove)
        removed.push_back(refToJson(r));
    return {{"kind", kind()}, {"points", points}, {"edges", edges}, {"faces", faces}, {"add", added}, {"remove", removed}};
}

DataPtr BevelOverrides::fromJson(const json& j)
{
    auto o = std::make_shared<BevelOverrides>();
    o->points = j.value("points", Index(0));
    o->edges = j.value("edges", Index(0));
    o->faces = j.value("faces", Index(0));
    for (const json& r : j.value("add", json::array()))
        o->add.push_back(refFromJson(r));
    for (const json& r : j.value("remove", json::array()))
        o->remove.push_back(refFromJson(r));
    return o;
}

namespace autoBevel
{
bool isAutoBevel(const Node& n) { return dynamic_cast<const AutoBevelNode*>(&n) != nullptr; }

bool editing(const Node& modifier) { return isAutoBevel(modifier) && modifier.as<bool>(kEditing); }

std::shared_ptr<const BevelEdgeSet> chosen(Graph& g, NodeId modifier)
{
    Node* n = g.node(modifier);
    if (!n || !isAutoBevel(*n))
        return nullptr;
    g.evaluate();
    return std::dynamic_pointer_cast<const BevelEdgeSet>(n->as<DataPtr>(kChosen));
}

NodeId meshOf(Graph& g, NodeId modifier)
{
    NodeId cur = modifier;
    for (int guard = 0; guard < 4096; ++guard)
    {
        Node* n = g.node(cur);
        auto* mod = dynamic_cast<ModifierNode*>(n);
        if (!mod)
            return kNoNode;
        auto dsts = g.destinations({cur, mod->outAttr, -1});
        if (dsts.empty())
            return kNoNode;
        Node* d = g.node(dsts[0].node);
        if (d->type().isA("mesh"))
            return d->id();
        cur = d->id();
    }
    return kNoNode;
}

void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base)
{
    std::vector<NodeId> s = modifiers::stack(g, mesh);
    if (s.empty() || !isAutoBevel(*g.node(s.front())))
        return;
    Node& mod = *g.node(s.front());
    auto* o = dynamic_cast<const BevelOverrides*>(mod.as<DataPtr>(kOverrides).get());
    if (!o || (o->add.empty() && o->remove.empty()))
        return;
    auto next = std::make_shared<BevelOverrides>(*o);
    next->points = base.pointCapacity();
    next->edges = base.edgeCapacity();
    next->faces = base.faceCapacity();
    for (auto* refs : {&next->add, &next->remove})
        for (geo::EdgeRef& r : *refs)
        {
            Index e = r.edge < remap.edges.size() ? remap.edges[r.edge] : geo::kInvalid;
            // Gone: keep the old ids out of the way so only its position can find it again.
            if (e == geo::kInvalid || !base.edgeValid(e))
                r.edge = r.p0 = r.p1 = geo::kInvalid;
            else
                r = geo::edgeRef(base, e);
        }
    g.setValue({mod.id(), kOverrides, -1}, DataPtr(next));
}

void registerTypes()
{
    if (types::find("autoBevelModifier"))
        return;
    data::registerKind("bevelOverrides", &BevelOverrides::fromJson);
    data::registerKind("bevelEditBackup", [](const json& j) -> DataPtr {
        auto d = std::make_shared<JsonData>();
        d->value = j.value("value", json());
        return d;
    });

    NodeType t;
    t.name = "autoBevelModifier";
    t.icon = "stack-2";
    t.doc = "Bevels the fold edges it finds live on its input by angle, side and length rules, plus edges picked by hand. Use "
            "it for game-art bevels that keep following the mesh as you edit it.";
    auto modeAttr = bevelParam("mode", PlugType::Enum, int64_t(kAuto),
                          "auto: fold edges by the rule. autoOverrides: the rule plus edges added and removed by hand. manual: only "
                          "the hand-picked edges. angle: every edge sharper than minAngle (Blender's angle limit). weight: edges "
                          "whose bevelWeight is above weightThreshold.");
    modeAttr.enumNames = kModeNames;
    auto sideAttr = bevelParam("side", PlugType::Enum, int64_t(0), "Which folds: both, outside (convex) or inside (concave).");
    sideAttr.enumNames = kBevelSideNames;
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        bevelParam("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        modeAttr,
        bevelParam("minAngle", PlugType::Float, 30.0, "Smallest fold to bevel, degrees between the face normals.", AttrKeyable, 0.0, 180.0),
        bevelParam("maxAngle", PlugType::Float, 180.0, "Largest fold to bevel, degrees.", AttrKeyable, 0.0, 180.0),
        sideAttr,
        bevelParam("thin", PlugType::Float, 0.0, "Skip thickness edges: shorter than this or beside a face narrower than this, cm. 0 is off.", AttrKeyable, 0.0),
        bevelParam("minLength", PlugType::Float, 0.0, "Skip edges shorter than this, cm.", AttrKeyable, 0.0),
        bevelParam("completeCorners", PlugType::Bool, true, "Where folds meet at an angle, add the quad loops that join them."),
        bevelParam("weightThreshold", PlugType::Float, 0.0, "weight mode: bevel edges whose bevelWeight is above this.", AttrKeyable, 0.0, 1.0),
        bevelParam("offset", PlugType::Float, 1.0, "Bevel width, cm.", AttrKeyable, 0.0),
        bevelParam("segments", PlugType::Int, int64_t(2), "Edges across each bevel, 1 to 32. More is rounder.", AttrKeyable, 1.0, 32.0),
        bevelParam("profile", PlugType::Float, 0.5, "Profile shape: 0.5 is a round arc, 0.25 a straight chamfer.", AttrKeyable, 0.0, 1.0),
        bevelParam("clamp", PlugType::Bool, true, "Keep the width under half the shortest neighbouring edge."),
        bevelParam("smoothingAngle", PlugType::Float, 30.0, "New edges sharper than this are hard; negative leaves them soft.", AttrKeyable, {}, 180.0),
        bevelParam("showEdges", PlugType::Bool, false, "Highlight the picked edges in the viewport even when the modifier isn't selected.", 0),
        bevelParam("editing", PlugType::Bool, false, "Edit Edges is in progress.", AttrHidden),
        {.name = "overrides", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Edges added and removed by hand."},
        {.name = "editBackup", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Selection to restore when Edit Edges ends."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "chosen", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "edgeCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Edges bevelled."},
        {.name = "lostOverrides", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput,
         .doc = "Hand-picked edges no longer found on the input mesh."},
    };
    t.create = [] { return std::make_unique<AutoBevelNode>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    std::vector<FlagDef> ruleFlags = {
        {"minAngle", "", T::Float, nullptr, "Smallest fold, degrees between the face normals.", {}, {}, 0.0, 180.0},
        {"maxAngle", "", T::Float, nullptr, "Largest fold, degrees.", {}, {}, 0.0, 180.0},
        {"side", "", T::String, nullptr, "both, outside (convex) or inside (concave)."},
        {"thin", "", T::Float, nullptr, "Skip thickness edges narrower than this, cm.", {}, {}, 0.0},
        {"minLength", "", T::Float, nullptr, "Skip edges shorter than this, cm.", {}, {}, 0.0},
        {"completeCorners", "", T::Bool, nullptr, "Add the quad loops joining folds that meet at an angle."},
    };

    std::vector<FlagDef> createFlags = ruleFlags;
    for (FlagDef f : std::vector<FlagDef>{
             {"mode", "", T::String, nullptr, "auto, autoOverrides, manual, angle or weight."},
             {"weightThreshold", "", T::Float, nullptr, "weight mode threshold.", {}, {}, 0.0, 1.0},
             {"offset", "", T::Float, nullptr, "Width, cm.", {}, {}, 0.0},
             {"segments", "", T::Int, nullptr, "Edges across each bevel, 1 to 32.", {}, {}, 1.0, 32.0},
             {"profile", "", T::Float, nullptr, "0.5 round, 0.25 straight.", {}, {}, 0.0, 1.0},
             {"clamp", "", T::Bool, nullptr, "Clamp the width."},
             {"smoothingAngle", "", T::Float, nullptr, "Hard-edge angle for the new edges.", {}, {}, {}, 180.0},
         })
        createFlags.push_back(f);

    ops::registerCommand({
        .name = "autoBevel",
        .doc = "Add an autoBevel modifier to the top of each mesh's stack: bevels fold edges found live on its input mesh. Returns the "
               "modifier names.",
        .positionalDoc = "meshes",
        .flags = createFlags,
        .run = [createFlags](const Args& a) -> json {
            Graph& g = Graph::current();
            json params = json::object();
            for (const FlagDef& f : createFlags)
                if (a.has(f.name))
                    params[f.name] = a.flags[f.name];
            for (auto [flag, names] : {std::pair{"mode", &kModeNames}, {"side", &kBevelSideNames}})
                if (a.has(flag))
                    params[flag] = enumArg(a, flag, *names, "autoBevel");
            json out = json::array();
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && n->type().isA("mesh"))
                    out.push_back(ops::execute("addModifier", {{"args", {name}}, {"kind", "autoBevel"}, {"params", params}}));
            if (out.empty())
                throw CommandError("autoBevel: select or name a mesh");
            return out;
        },
    });

    ops::registerCommand({
        .name = "bevelEdgeInfo",
        .doc = "The edges an autoBevel modifier picks on its input mesh: {edges, count, lost, editing, offset, applied, limiting}. "
               "applied is the offset after the clamp; limiting lists the edges holding it there.",
        .positionalDoc = "modifier or mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& mod = modifierArg(a, "bevelEdgeInfo");
            auto set = chosen(Graph::current(), mod.id());
            json edges = set ? json(set->edges) : json::array();
            json limiting = json::array();
            if (set && !set->limits.empty())
                for (Index e : set->edges)
                    if (e < set->limits.size() && set->limits[e] <= set->applied * 1.05)
                        limiting.push_back(e);
            return {{"edges", edges},
                    {"count", edges.size()},
                    {"lost", set ? set->lost : 0},
                    {"editing", editing(mod)},
                    {"offset", set ? set->offset : 0.0},
                    {"applied", set ? set->applied : 0.0},
                    {"limiting", limiting}};
        },
    });

    ops::registerCommand({
        .name = "bevelEditEdges",
        .doc = "Show an autoBevel modifier's input mesh (it and later modifiers pass through) with its edges selected, so they can "
               "be changed with the normal selection tools. Finish with bevelApplyEdges or bevelCancelEdges.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& mod = modifierArg(a, "bevelEditEdges");
            NodeId mesh = meshOf(g, mod.id());
            if (mesh == kNoNode)
                throw CommandError("bevelEditEdges: " + mod.path() + " feeds no mesh");
            for (NodeId id : modifiers::stack(g, mesh))
                if (id != mod.id() && static_cast<ModifierNode*>(g.node(id))->editingInput())
                    throw CommandError("bevelEditEdges: " + g.node(id)->path() + " is already editing its edges");
            if (!editing(mod))
            {
                g.evaluate();
                auto backup = std::make_shared<JsonData>();
                backup->value = modifiers::selectionSnapshot(g, mesh);
                g.setValue({mod.id(), kEditBackup, -1}, DataPtr(backup));
                g.setValue({mod.id(), kEditing, -1}, true);
            }
            selectChosen(g, mod, mesh);
            return mod.path();
        },
    });

    ops::registerCommand({
        .name = "bevelApplyEdges",
        .doc = "Store the selected edges as an autoBevel modifier's edge set (as overrides of its rule) and end Edit Edges. Returns "
               "{added, removed}.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& mod = modifierArg(a, "bevelApplyEdges");
            if (!editing(mod))
                throw CommandError("bevelApplyEdges: " + mod.path() + " is not editing its edges");
            NodeId mesh = meshOf(g, mod.id());
            MeshPtr in = bevelInput(g, mod);
            if (!in || mesh == kNoNode)
                throw CommandError("bevelApplyEdges: " + mod.path() + " has no input mesh");
            const geo::Mesh& m = *in;
            std::vector<uint8_t> want(m.edgeCapacity(), 0), rule(m.edgeCapacity(), 0);
            for (uint32_t e : components::selected(mesh, ComponentMode::Edge))
                if (m.edgeValid(e))
                    want[e] = 1;
            int64_t mode = mod.as<int64_t>(kMode);
            if (mode != kManual)
                for (Index e : ruleEdges(m, mod))
                    rule[e] = 1;
            std::vector<Index> add, remove;
            for (Index e = 0; e < m.edgeCapacity(); ++e)
            {
                if (want[e] && !rule[e])
                    add.push_back(e);
                else if (!want[e] && rule[e])
                    remove.push_back(e);
            }
            g.setValue({mod.id(), kOverrides, -1}, DataPtr(overridesFor(m, add, remove)));
            if (mode == kAuto && (!add.empty() || !remove.empty()))
                g.setValue({mod.id(), kMode, -1}, int64_t(kAutoOverrides));
            endEdit(g, mod);
            return {{"added", add.size()}, {"removed", remove.size()}};
        },
    });

    ops::registerCommand({
        .name = "bevelCancelEdges",
        .doc = "End an autoBevel modifier's Edit Edges without changing its edge set.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& mod = modifierArg(a, "bevelCancelEdges");
            if (!editing(mod))
                throw CommandError("bevelCancelEdges: " + mod.path() + " is not editing its edges");
            endEdit(g, mod);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "bevelClearOverrides",
        .doc = "Drop an autoBevel modifier's hand-picked edges, going back to its rule.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Node& mod = modifierArg(a, "bevelClearOverrides");
            Graph::current().setValue({mod.id(), kOverrides, -1}, DataPtr());
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "bevelMeasureThin",
        .doc = "Set an autoBevel modifier's thin to 1.5 times the longest selected edge (pick a thickness edge). Returns it.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& mod = modifierArg(a, "bevelMeasureThin");
            NodeId mesh = meshOf(g, mod.id());
            MeshPtr cage = mesh != kNoNode ? modifiers::cage(g, mesh) : nullptr;
            double longest = 0.0;
            if (cage)
                for (uint32_t e : components::selected(mesh, ComponentMode::Edge))
                    if (cage->edgeValid(e))
                        longest = std::max(longest, cage->edgeLength(e));
            if (longest <= 0.0)
                throw CommandError("bevelMeasureThin: select a thickness edge on " + (mesh ? g.node(mesh)->path() : mod.path()));
            g.setValue({mod.id(), kThin, -1}, longest * 1.5);
            return longest * 1.5;
        },
    });

    std::vector<FlagDef> selectFlags = {
        {"add", "", T::Bool, false, "Add to the edge selection instead of replacing it."},
        {"minAngle", "", T::Float, 30.0, "Smallest fold, degrees between the face normals.", {}, {}, 0.0, 180.0},
        {"maxAngle", "", T::Float, 180.0, "Largest fold, degrees.", {}, {}, 0.0, 180.0},
        {"side", "", T::String, "both", "both, outside (convex) or inside (concave)."},
        {"thin", "", T::Float, 0.0, "Skip thickness edges narrower than this, cm.", {}, {}, 0.0},
        {"minLength", "", T::Float, 0.0, "Skip edges shorter than this, cm.", {}, {}, 0.0},
        {"completeCorners", "", T::Bool, true, "Add the quad loops joining folds that meet at an angle."},
    };
    ops::registerCommand({
        .name = "selectFoldEdges",
        .doc = "Select fold edges of meshes by the autoBevel rule (on the editable cage). Returns the number selected.",
        .positionalDoc = "meshes",
        .flags = selectFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            geo::FoldEdgeOptions o;
            o.minAngle = a.get<double>("minAngle");
            o.maxAngle = a.get<double>("maxAngle");
            o.side = geo::FoldSide(enumArg(a, "side", kBevelSideNames, "selectFoldEdges"));
            o.thin = a.get<double>("thin");
            o.minLength = a.get<double>("minLength");
            o.completeCorners = a.get<bool>("completeCorners");
            bool first = !a.get<bool>("add");
            size_t total = 0;
            bool any = false;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                    continue;
                MeshPtr cage = modifiers::cage(g, n->id());
                if (!cage)
                    continue;
                any = true;
                std::vector<Index> edges = geo::foldEdges(*cage, o);
                total += edges.size();
                components::select(n->id(), ComponentMode::Edge, edges, first ? components::Op::Replace : components::Op::Add);
                first = false;
            }
            if (!any)
                throw CommandError("selectFoldEdges: select or name a mesh");
            components::setMode(ComponentMode::Edge);
            return total;
        },
    });
}
} // namespace autoBevel
} // namespace lila::scene
