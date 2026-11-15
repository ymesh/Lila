#include "UsdStage.h"

#include "UsdAttributes.h"
#include "UsdBackend.h"
#include "UsdCommon.h"
#include "UsdExport.h"
#include "UsdImport.h"

#include <lila/Anim.h>
#include <lila/Deformer.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Rig.h>
#include <lila/SceneFile.h>
#include <lila/Transform.h>
#include <lila/Undo.h>

#include <pxr/base/gf/quath.h>
#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/primIndex.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageLoadRules.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/tokens.h>
#include <pxr/usd/usdSkel/topology.h>
#include <pxr/usd/usdSkel/utils.h>
#include <pxr/usd/usdUtils/stitch.h>

#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd::stage
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::kNoNode;
using scene::Node;
using scene::NodeId;

/** Hash per property group of one prim: xform, visibility, shape, binding, skin, blendShapes. */
using Groups = std::map<std::string, uint64_t>;
/** By composed prim path, $mat:<slot> or $skel:<path>. */
using Baseline = std::map<std::string, Groups>;
using PathMap = std::unordered_map<SdfPath, NodeId, SdfPath::Hash>;

const Usd_PrimFlagsPredicate kPresent = UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/** The composed stage's path for a link path, which names the LOD variant of a prim inside a level (/Chair{LOD=LOD1}Mesh). */
SdfPath stagePath(const SdfPath& p) { return p.ContainsPrimVariantSelection() ? p.StripAllVariantSelections() : p; }

/** True when the link path is below the composed prim q, not q itself. */
bool insideOf(const SdfPath& link, const SdfPath& q)
{
    const SdfPath p = stagePath(link);
    return p != q && p.HasPrefix(q);
}

/** Baseline keys of an instance opened for edits, and of each linked prim's baseline from before it was. */
const std::string kOpen = "$open:", kOrigin = "$origin:";
/**
 * Baseline keys of a point instance taken out of its instancer ("$extract:<xform>|<instancer>", its index), and of the
 * holder's and copies' baselines as they were taken out, by their link paths.
 */
const std::string kExtract = "$extract:", kFrom = "$from:";

/** Above zero while Lila changes the scene for a stage itself; the graph listener ignores those changes. */
int& quietDepth()
{
    static int depth = 0;
    return depth;
}

struct Stage : TfWeakBase
{
    NodeId node = kNoNode;
    /** The node object itself: a scene loaded since reuses ids, never live objects. */
    const Node* object = nullptr;
    /** The filePath and editLayer attrs this was composed from. */
    std::string source, editSpec;
    enum class Mode
    {
        Embedded, // anonymous, kept as text in the .lila
        File,     // a .usda of its own
        Sublayer, // one of the source's sublayers, edited in place
    } mode = Mode::Embedded;
    SdfLayerRefPtr sourceLayer, root, edit;
    UsdStageRefPtr stage;
    TfNotice::Key key;
    /** Whether stage has a material with a MaterialX surface, worked out once per composed stage. */
    const UsdStage* mtlxScanned = nullptr;
    bool usesMtlx = false;

    /** By link path (stagePath). */
    std::unordered_map<NodeId, SdfPath> primOf;
    std::map<SdfPath, NodeId> nodeOf;
    /** Nodes a prim's build made besides its own (joints, point instances), with that prim. */
    std::unordered_map<NodeId, SdfPath> partOf;
    /** Point instance copies (in partOf too): each node built from a prototype prim, with that prim. */
    std::unordered_map<NodeId, SdfPath> copyOf;
    /** The transform of each point instance (in partOf too): its instancer and index. */
    std::unordered_map<NodeId, std::pair<SdfPath, size_t>> instanceOf;
    /** The joints of Skeleton prims (in partOf too): each one's token in its skeleton's joints. */
    std::unordered_map<NodeId, TfToken> jointOf;
    /** Linked nodes deleted this session, so an undo that brings one back links it again. */
    std::unordered_map<NodeId, SdfPath> deleted;
    struct Part
    {
        SdfPath owner;
        std::optional<SdfPath> copy;
        std::optional<std::pair<SdfPath, size_t>> instance;
        std::optional<TfToken> joint;
        /** Where the joint stood in its skeleton's joints. */
        std::optional<size_t> jointAt;
    };
    /** Parts deleted this session (joints, point instances, copies), so an undo that brings one back makes it a part again. */
    std::unordered_map<NodeId, Part> deletedParts;
    Baseline baseline;
    /** The material prim each Lila material came from, or was written to. */
    std::map<std::string, SdfPath> materialOrigin;
    std::map<std::string, std::pair<int64_t, uint64_t>> stamps;
    /** Seconds the animation is compared over; NaN takes the playback range. */
    double start = std::numeric_limits<double>::quiet_NaN(), end = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::string> warnings;

    /** Above zero while Lila itself changes the stage, whose notices it then ignores. */
    int busy = 0;
    std::set<SdfPath> resynced, changed;

    /** Nodes the scene changed since the last comparison; full when anything may have (a batch, an undo, a fresh state). */
    std::set<NodeId> pending;
    bool full = true;
    /** The playback range the last comparison sampled, when the stage takes its range from playback. */
    double comparedStart = kNaN, comparedEnd = kNaN;
    /** For tests: the prims the last comparison exported, and how many full and partial comparisons ran. */
    size_t lastExported = 0, fullDiffs = 0, partialDiffs = 0;

    ~Stage()
    {
        if (key.IsValid())
            TfNotice::Revoke(key);
    }

    void link(NodeId n, const SdfPath& p)
    {
        if (auto old = nodeOf.find(p); old != nodeOf.end() && old->second != n)
            primOf.erase(old->second);
        if (auto was = primOf.find(n); was != primOf.end() && was->second != p)
            nodeOf.erase(was->second);
        primOf[n] = p;
        nodeOf[p] = n;
        partOf.erase(n);
        copyOf.erase(n);
        instanceOf.erase(n);
        jointOf.erase(n);
        deleted.erase(n);
        deletedParts.erase(n);
    }
    void unlink(NodeId n)
    {
        if (auto it = primOf.find(n); it != primOf.end())
        {
            nodeOf.erase(it->second);
            primOf.erase(it);
        }
        partOf.erase(n);
        copyOf.erase(n);
        instanceOf.erase(n);
        jointOf.erase(n);
    }

    void onChanged(const UsdNotice::ObjectsChanged& notice, const UsdStagePtr&)
    {
        if (busy)
            return;
        auto add = [&](const SdfPath& p, bool resync) {
            if (p.IsAbsoluteRootPath())
            {
                if (resync)
                    resynced.insert(p);
                return;
            }
            const SdfPath prim = p.GetPrimPath();
            // A prototype's prims stand for every instance of it: rebuild everything.
            if (TfStringStartsWith(prim.GetString(), "/__Prototype"))
                resynced.insert(SdfPath::AbsoluteRootPath());
            else if (resync && p.IsPrimPath())
                resynced.insert(prim);
            else
                changed.insert(prim);
        };
        for (const SdfPath& p : notice.GetResyncedPaths())
            add(p, true);
        for (const SdfPath& p : notice.GetChangedInfoOnlyPaths())
            add(p, false);
    }
};

std::map<NodeId, std::unique_ptr<Stage>>& stages()
{
    // Never destroyed: revoking notices while Pixar's libraries shut down at exit is unsafe.
    static auto* all = new std::map<NodeId, std::unique_ptr<Stage>>();
    return *all;
}

Stage* findStage(NodeId id)
{
    auto it = stages().find(id);
    return it == stages().end() ? nullptr : it->second.get();
}

/** Hands every scene change to the stages, for the next comparison to export only what it touched. */
void listen(Graph& g)
{
    static Graph* listening = nullptr;
    if (listening == &g)
        return;
    listening = &g;
    g.addListener([](scene::Change c, NodeId id) {
        if (quietDepth() > 0 || c == scene::Change::Selection)
            return;
        for (auto& [node, s] : stages())
        {
            if (c == scene::Change::Reset || c == scene::Change::Cleared)
                s->full = true;
            else if (!s->full)
                s->pending.insert(id);
        }
    });
}

/**
 * While a link path names LOD variants, those variants are selected in the session layer, so the composed stage holds
 * that level's prims; the selections the session layer had come back after.
 */
class VariantScope
{
public:
    VariantScope(Stage& s, const SdfPath& link) : mStage(s)
    {
        if (!link.ContainsPrimVariantSelection())
            return;
        ++s.busy;
        ++quietDepth();
        mActive = true;
        for (const SdfPath& q : link.GetPrefixes())
        {
            if (!q.IsPrimVariantSelectionPath())
                continue;
            const auto [set, sel] = q.GetVariantSelection();
            const SdfPath prim = q.GetPrimPath().StripAllVariantSelections();
            UsdPrim up = s.stage->GetPrimAtPath(prim);
            if (!up || up.GetVariantSets().GetVariantSet(set).GetVariantSelection() == sel)
                continue;
            const SdfPrimSpecHandle spec = s.stage->GetSessionLayer()->GetPrimAtPath(prim);
            std::optional<std::string> had;
            if (spec && spec->GetVariantSelections().count(set))
                had = spec->GetVariantSelections().find(set)->second;
            mUndo.push_back({prim, set, had});
            UsdEditContext session(s.stage, s.stage->GetSessionLayer());
            up.GetVariantSets().GetVariantSet(set).SetVariantSelection(sel);
        }
    }
    ~VariantScope()
    {
        if (!mActive)
            return;
        for (auto it = mUndo.rbegin(); it != mUndo.rend(); ++it)
            if (UsdPrim up = mStage.stage->GetPrimAtPath(it->prim))
            {
                UsdEditContext session(mStage.stage, mStage.stage->GetSessionLayer());
                UsdVariantSet vs = up.GetVariantSets().GetVariantSet(it->set);
                if (it->had)
                    vs.SetVariantSelection(*it->had);
                else
                    vs.ClearVariantSelection();
            }
        --mStage.busy;
        --quietDepth();
    }
    VariantScope(const VariantScope&) = delete;
    VariantScope& operator=(const VariantScope&) = delete;

private:
    struct Was
    {
        SdfPath prim;
        std::string set;
        std::optional<std::string> had;
    };
    Stage& mStage;
    std::vector<Was> mUndo;
    bool mActive = false;
};

/** The variant selection prefix of a link path inside an LOD level (/Chair{LOD=LOD1}), or empty. */
SdfPath variantPrefix(const SdfPath& link)
{
    SdfPath out;
    for (const SdfPath& q : link.GetPrefixes())
        if (q.IsPrimVariantSelectionPath())
            out = q;
    return out;
}

/** The outermost LOD group a link path inside a level runs through, which builds every level below it; empty outside levels. */
SdfPath lodGroupOf(const SdfPath& link)
{
    for (const SdfPath& q : link.GetPrefixes())
        if (q.IsPrimVariantSelectionPath())
            return stagePath(q);
    return {};
}

/** The link path of a composed prim while level (a variant selection path, or empty for none) is selected. */
SdfPath levelLink(const SdfPath& level, const SdfPath& composed)
{
    if (level.IsEmpty())
        return composed;
    SdfPath out = level;
    for (const SdfPath& p : composed.MakeRelativePath(stagePath(level)).GetPrefixes())
        out = out.AppendChild(p.GetNameToken());
    return out;
}

/**
 * Calls f(prim, link) on every present prim. Prims inside LOD levels are visited once per level with that level
 * selected, their link paths naming it; prims are only valid during the call.
 */
template <class F> void eachPrim(Stage& s, F&& f)
{
    std::vector<SdfPath> levels{SdfPath()};
    for (size_t i = 0; i < levels.size(); ++i)
    {
        const SdfPath level = levels[i];
        VariantScope scope(s, level);
        const SdfPath top = level.IsEmpty() ? SdfPath::AbsoluteRootPath() : stagePath(level);
        const UsdPrim root = s.stage->GetPrimAtPath(top);
        if (!root)
            continue;
        UsdPrimRange range(root, kPresent);
        for (auto it = range.begin(); it != range.end(); ++it)
        {
            const UsdPrim& p = *it;
            if (p.GetPath() == top)
                continue;
            const SdfPath link = levelLink(level, p.GetPath());
            f(p, link);
            const std::vector<std::string> lods = usd::lodLevels(p);
            if (lods.empty())
                continue;
            for (const std::string& v : lods)
                levels.push_back(link.AppendVariantSelection("LOD", v));
            it.PruneChildren();
        }
    }
}

/** Selects a variant on the prim at link; inside an LOD level, with the level selected and the selection written into it. */
void selectVariant(Stage& s, const SdfPath& link, const std::string& set, const std::string& variant)
{
    const SdfPath level = variantPrefix(link);
    if (level.IsEmpty())
    {
        if (const UsdPrim p = s.stage->GetPrimAtPath(link))
            p.GetVariantSets().GetVariantSet(set).SetVariantSelection(variant);
        return;
    }
    {
        VariantScope scope(s, link);
        UsdEditContext target(s.stage, UsdEditTarget::ForLocalDirectVariant(s.edit, level));
        if (const UsdPrim p = s.stage->GetPrimAtPath(stagePath(link)))
            p.GetVariantSets().GetVariantSet(set).SetVariantSelection(variant);
    }
    // Notices go unheard while a level is selected: its LOD group is built again instead.
    s.changed.insert(lodGroupOf(link));
}

struct Busy
{
    Stage& s;
    explicit Busy(Stage& st) : s(st)
    {
        ++s.busy;
        ++quietDepth();
    }
    ~Busy()
    {
        --s.busy;
        --quietDepth();
    }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
};

/** Bookkeeping writes and updates from outside Lila are not steps anyone undoes. */
struct NoUndo
{
    bool was = UndoStack::get().enabled();
    NoUndo() { UndoStack::get().setEnabled(false); }
    ~NoUndo() { UndoStack::get().setEnabled(was); }
    NoUndo(const NoUndo&) = delete;
    NoUndo& operator=(const NoUndo&) = delete;
};

// Scene helpers

std::string text(const Node& n, const char* attr)
{
    const int a = n.type().attrIndex(attr);
    return a >= 0 ? n.as<std::string>(a) : std::string();
}

void setText(Graph& g, NodeId id, const char* attr, const std::string& v)
{
    const Node* n = g.node(id);
    const int a = n ? n->type().attrIndex(attr) : -1;
    if (a >= 0 && n->as<std::string>(a) != v)
        g.setValue({id, a, -1}, v);
}

json parseJson(const std::string& s)
{
    if (s.empty())
        return json();
    try
    {
        return json::parse(s);
    }
    catch (const std::exception&)
    {
        return json();
    }
}

bool isStage(const Node* n) { return n && n->type().isA(kStageType); }

NodeId stageOf(const Graph& g, NodeId n)
{
    for (NodeId p = g.parent(n); p != kNoNode; p = g.parent(p))
        if (isStage(g.node(p)))
            return p;
    return kNoNode;
}

bool under(const Graph& g, NodeId n, NodeId ancestor)
{
    for (NodeId p = g.parent(n); p != kNoNode; p = g.parent(p))
        if (p == ancestor)
            return true;
    return false;
}

/** "A|B|C" from the stage node down; empty when n isn't under it. */
std::string relPath(const Graph& g, NodeId stage, NodeId n)
{
    std::vector<std::string> names;
    NodeId p = n;
    for (; p != stage && p != kNoNode && g.node(p); p = g.parent(p))
        names.push_back(g.node(p)->name());
    if (p != stage)
        return {};
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
        out += (out.empty() ? "" : "|") + *it;
    return out;
}

NodeId resolveRel(const Graph& g, NodeId stage, const std::string& rel)
{
    NodeId cur = stage;
    size_t pos = 0;
    while (pos <= rel.size() && cur != kNoNode)
    {
        const size_t bar = rel.find('|', pos);
        const std::string name = rel.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        NodeId next = kNoNode;
        for (NodeId c : g.children(cur))
            if (g.node(c)->name() == name)
                next = c;
        cur = next;
        if (bar == std::string::npos)
            break;
        pos = bar + 1;
    }
    return cur;
}

/** A path from a flag or attr: relative ones against the scene's folder. */
fs::path resolveFile(const std::string& p)
{
    fs::path f = utf8Path(p);
    if (f.is_relative())
    {
        const fs::path& scene = io::currentPath();
        std::error_code ec;
        f = (scene.empty() ? fs::current_path(ec) : scene.parent_path()) / f;
    }
    std::error_code ec;
    fs::path w = fs::weakly_canonical(f, ec);
    return ec ? f.lexically_normal() : w;
}

std::string generic(const fs::path& p)
{
    std::string s = utf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool samePath(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::equivalent(a, b, ec))
        return true;
    std::string x = generic(a.lexically_normal()), y = generic(b.lexically_normal());
#if defined(_WIN32)
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return x == y;
}

/** The file a layer was read from; a layer inside a package is the package. */
std::string fileOf(const SdfLayerHandle& l)
{
    std::string real = l->GetRealPath();
    if (const size_t b = real.find('['); b != std::string::npos)
        real.resize(b);
    return real;
}

// Hashing. Stable across sessions (the baseline is saved with the scene), so tokens and paths hash by their text.

struct Hasher
{
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n)
    {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
    }
    void num(uint64_t v) { bytes(&v, sizeof v); }
    void str(std::string_view s)
    {
        bytes(s.data(), s.size());
        num(s.size());
    }
    template <class T, class F> void listOp(const SdfListOp<T>& op, F&& item)
    {
        num(op.IsExplicit() ? 1 : 0);
        for (const auto* items : {&op.GetExplicitItems(), &op.GetPrependedItems(), &op.GetAppendedItems(), &op.GetDeletedItems(), &op.GetOrderedItems()})
        {
            num(items->size());
            for (const T& v : *items)
                item(v);
        }
    }
    void value(const VtValue& v)
    {
        str(v.GetTypeName());
        if (v.IsHolding<TfToken>())
            str(v.UncheckedGet<TfToken>().GetString());
        else if (v.IsHolding<VtTokenArray>())
            for (const TfToken& t : v.UncheckedGet<VtTokenArray>())
                str(t.GetString());
        else if (v.IsHolding<TfTokenVector>())
            for (const TfToken& t : v.UncheckedGet<TfTokenVector>())
                str(t.GetString());
        else if (v.IsHolding<std::string>())
            str(v.UncheckedGet<std::string>());
        else if (v.IsHolding<SdfAssetPath>())
            str(v.UncheckedGet<SdfAssetPath>().GetAssetPath());
        else if (v.IsHolding<SdfPath>())
            str(v.UncheckedGet<SdfPath>().GetString());
        else if (v.IsHolding<SdfPathListOp>())
            listOp(v.UncheckedGet<SdfPathListOp>(), [&](const SdfPath& p) { str(p.GetString()); });
        else if (v.IsHolding<SdfTokenListOp>())
            listOp(v.UncheckedGet<SdfTokenListOp>(), [&](const TfToken& t) { str(t.GetString()); });
        else if (v.IsHolding<SdfStringListOp>())
            listOp(v.UncheckedGet<SdfStringListOp>(), [&](const std::string& s) { str(s); });
        else if (v.IsHolding<VtDictionary>())
            str(TfStringify(v));
        else if (v.CanHash())
            num(v.GetHash());
        else
            str(TfStringify(v));
    }
};

/** Every field of one spec and its time samples. customData only when asked: Lila keeps bookkeeping there. */
uint64_t hashSpec(const SdfLayerHandle& layer, const SdfPath& path, bool customData)
{
    Hasher h;
    std::vector<TfToken> fields = layer->ListFields(path);
    std::sort(fields.begin(), fields.end(), [](const TfToken& a, const TfToken& b) { return a.GetString() < b.GetString(); });
    for (const TfToken& f : fields)
    {
        if (f == SdfFieldKeys->TimeSamples || f == SdfFieldKeys->Documentation || (!customData && f == SdfFieldKeys->CustomData))
            continue;
        h.str(f.GetString());
        h.value(layer->GetField(path, f));
    }
    for (double t : layer->ListTimeSamplesForPath(path))
    {
        VtValue v;
        layer->QueryTimeSample(path, t, &v);
        h.bytes(&t, sizeof t);
        h.value(v);
    }
    return h.h;
}

/** A spec and everything under it, by path relative to it. */
uint64_t hashTree(const SdfLayerHandle& layer, const SdfPath& root, bool customData)
{
    std::vector<SdfPath> paths;
    layer->Traverse(root, [&](const SdfPath& p) { paths.push_back(p); });
    std::sort(paths.begin(), paths.end(), [](const SdfPath& a, const SdfPath& b) { return a.GetString() < b.GetString(); });
    Hasher h;
    for (const SdfPath& p : paths)
    {
        h.str(p.MakeRelativePath(root).GetString());
        h.num(hashSpec(layer, p, customData));
    }
    return h.h;
}

/** Which group a prim property is written with, or empty for one Lila never writes. components: the prim's component attributes. */
std::string groupOf(const std::string& name, const std::set<std::string>& components)
{
    if (components.count(name))
        return "attrs";
    if (name == "xformOpOrder" || name.starts_with("xformOp:"))
        return "xform";
    if (name == "visibility")
        return "visibility";
    if (name.starts_with("material:binding"))
        return "binding";
    if (name.starts_with("skel:blendShape"))
        return "blendShapes";
    if (name.starts_with("primvars:skel:") || name.starts_with("skel:"))
        return "skin";
    if (name == "purpose" || name == "proxyPrim")
        return {};
    return "shape";
}

/** Shape attributes a rewritten mesh would leave stale behind it, blocked when Lila's version has none. */
bool blockable(const std::string& name)
{
    static const std::set<std::string> kNames{"normals",           "velocities",      "accelerations",    "holeIndices",
                                              "cornerIndices",     "cornerSharpnesses", "creaseIndices",  "creaseLengths",
                                              "creaseSharpnesses", "orientation",     "doubleSided"};
    return (name.starts_with("primvars:") && !name.starts_with("primvars:skel:")) || kNames.count(name);
}

Groups hashPrim(const SdfLayerHandle& layer, const SdfPath& path)
{
    Groups out;
    const SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
    if (!spec)
        return out;
    std::map<std::string, Hasher> hs;
    const std::set<std::string> components = componentAttributes(spec);
    for (const TfToken& api : specComponentSchemas(spec))
        hs["attrs"].str("apiSchemas:" + api.GetString());
    auto props = [&](const SdfPrimSpecHandle& p, const std::string& prefix, bool subset) {
        std::vector<std::pair<std::string, SdfPath>> list;
        for (const SdfPropertySpecHandle& prop : p->GetProperties())
            list.push_back({prop->GetName(), prop->GetPath()});
        std::sort(list.begin(), list.end());
        for (const auto& [name, propPath] : list)
        {
            const std::string group = subset ? (name.starts_with("material:binding") ? "binding" : "shape") : groupOf(name, components);
            if (group.empty())
                continue;
            Hasher& h = hs[group];
            h.str(prefix + name);
            h.num(hashSpec(layer, propPath, false));
        }
    };
    props(spec, "", false);
    for (const SdfPrimSpecHandle& c : spec->GetNameChildren())
    {
        const std::string type = c->GetTypeName().GetString();
        if (type == "GeomSubset")
            props(c, c->GetName() + "/", true);
        else if (type == "BlendShape")
        {
            Hasher& h = hs["blendShapes"];
            h.str(c->GetName());
            h.num(hashTree(layer, c->GetPath(), false));
        }
    }
    for (const auto& [group, h] : hs)
        out[group] = h.h;
    return out;
}

uint64_t groupHash(const Groups& g, const std::string& group)
{
    auto it = g.find(group);
    return it == g.end() ? 0 : it->second;
}

// Snapshots: the stage's nodes written by the USD exporter into a stage in memory.

SdfPathVector targetsOf(const SdfLayerHandle& layer, const SdfPath& rel)
{
    const VtValue v = layer->GetField(rel, SdfFieldKeys->TargetPaths);
    return v.IsHolding<SdfPathListOp>() ? v.UncheckedGet<SdfPathListOp>().GetAppliedItems() : SdfPathVector();
}

template <class T> T defaultOf(const SdfLayerHandle& layer, const SdfPath& attr)
{
    const VtValue v = layer->GetField(attr, SdfFieldKeys->Default);
    return v.IsHolding<T>() ? v.UncheckedGet<T>() : T();
}

/** The SkelAnimation a written Skeleton prim is bound to, or empty. */
SdfPath animationOf(const SdfLayerHandle& layer, const SdfPath& skel)
{
    const SdfPathVector t = targetsOf(layer, skel.AppendProperty(UsdSkelTokens->skelAnimationSource));
    return t.empty() ? SdfPath() : t.front();
}

struct Snapshot
{
    UsdStageRefPtr stage;
    SdfLayerHandle layer;
    SdfPath root;
    std::unordered_map<NodeId, SdfPath> prim;
    std::map<SdfPath, NodeId> nodeAt;
    std::unordered_map<NodeId, Groups> hashes;
    std::map<std::string, SdfPath> materials;
    std::map<SdfPath, std::string> slotOf;
    std::map<std::string, uint64_t> materialHashes;
    /** Each Skeleton prim written: its joints' nodes, in the order of its joints attr (none when synthetic). */
    std::map<SdfPath, std::vector<NodeId>> skelJoints;
    /** The stage's Skeleton prim each written one stands for. */
    std::map<SdfPath, SdfPath> skelStage;
    /** By the stage's Skeleton prim: hashes of its joints (named as the stage names them), rest, bind, anim and weights. */
    std::map<SdfPath, Groups> skeletons;
    /** False when the export failed, so nothing could be compared. */
    bool ok = true;
};

/** The stage's token for each joint of a written Skeleton prim; "+<token>" for a joint the stage doesn't have yet. */
std::vector<std::string> stageTokens(const Stage& s, const Snapshot& out, const SdfPath& e)
{
    const VtTokenArray tokens = defaultOf<VtTokenArray>(out.layer, e.AppendProperty(UsdSkelTokens->joints));
    const auto nodes = out.skelJoints.find(e);
    std::vector<std::string> names;
    for (size_t k = 0; k < tokens.size(); ++k)
    {
        const NodeId n = nodes != out.skelJoints.end() && k < nodes->second.size() ? nodes->second[k] : kNoNode;
        const auto j = s.jointOf.find(n);
        names.push_back(j != s.jointOf.end() ? j->second.GetString() : "+" + tokens[k].GetString());
    }
    return names;
}

Groups skelGroups(const Stage& s, const Snapshot& out, const SdfPath& e)
{
    Groups gr;
    Hasher topo;
    for (const std::string& t : stageTokens(s, out, e))
        topo.str(t);
    gr["joints"] = topo.h;
    for (const char* name : {"restTransforms", "bindTransforms"})
        if (const SdfPath a = e.AppendProperty(TfToken(name)); out.layer->GetAttributeAtPath(a))
            gr[name == std::string("restTransforms") ? "rest" : "bind"] = hashSpec(out.layer, a, false);
    if (const SdfPath anim = animationOf(out.layer, e); !anim.IsEmpty())
    {
        Hasher joints, weights;
        bool anyJoints = false, anyWeights = false;
        for (const char* name : {"joints", "translations", "rotations", "scales"})
            if (const SdfPath a = anim.AppendProperty(TfToken(name)); out.layer->GetAttributeAtPath(a))
            {
                joints.num(hashSpec(out.layer, a, false));
                anyJoints = true;
            }
        for (const char* name : {"blendShapes", "blendShapeWeights"})
            if (const SdfPath a = anim.AppendProperty(TfToken(name)); out.layer->GetAttributeAtPath(a))
            {
                weights.num(hashSpec(out.layer, a, false));
                anyWeights = true;
            }
        if (anyJoints)
            gr["anim"] = joints.h;
        if (anyWeights)
            gr["weights"] = weights.h;
    }
    return gr;
}

/** The written Skeleton prims matched to the stage's: through the links of their joints, or the meshes bound to them. */
void mapSkeletons(const Stage& s, Snapshot& out)
{
    for (const auto& [e, joints] : out.skelJoints)
        for (NodeId j : joints)
            if (auto it = s.partOf.find(j); it != s.partOf.end() && s.jointOf.count(j))
            {
                out.skelStage[e] = it->second;
                break;
            }
    for (const auto& [n, e] : out.prim)
    {
        const auto p = s.primOf.find(n);
        if (p == s.primOf.end() || p->second.ContainsPrimVariantSelection())
            continue;
        const SdfPathVector t = targetsOf(out.layer, e.AppendProperty(UsdSkelTokens->skelSkeleton));
        if (t.empty() || out.skelStage.count(t.front()))
            continue;
        if (const UsdPrim cp = s.stage->GetPrimAtPath(p->second))
            if (const UsdSkelSkeleton sk = UsdSkelBindingAPI(cp).GetInheritedSkeleton())
                out.skelStage[t.front()] = sk.GetPath();
    }
    for (const auto& [e, st] : out.skelStage)
        out.skeletons[st] = skelGroups(s, out, e);
}

/** What a comparison exports: the whole stage, or linked nodes alone and new nodes with everything below them. */
struct Plan
{
    bool full = true;
    std::vector<NodeId> alone, trees;
    bool empty() const { return !full && alone.empty() && trees.empty(); }
};

/** The MaterialX surface the stage's materials are written with (stageAttr::kMaterialX). */
mtlx::Surface stageMaterialX(Stage& s, const Graph& g)
{
    const Node* n = g.node(s.node);
    const std::string chosen = n ? text(*n, stageAttr::kMaterialX) : std::string();
    if (mtlx::Surface out; !chosen.empty() && mtlx::parseSurface(chosen, &out))
        return out;
    if (s.stage && s.mtlxScanned != get_pointer(s.stage))
    {
        s.mtlxScanned = get_pointer(s.stage);
        s.usesMtlx = false;
        for (const UsdPrim& p : s.stage->Traverse())
            if (const UsdShadeMaterial m(p); m && m.GetSurfaceOutput(TfToken("mtlx")))
            {
                s.usesMtlx = true;
                break;
            }
    }
    return s.usesMtlx ? mtlx::Surface::OpenPbr : mtlx::Surface::None;
}

Snapshot snapshot(Stage& s, Graph& g, std::vector<std::string>& warnings, const Plan& plan = {})
{
    LILA_ZONE();

    Snapshot out;
    ExportOptions o;
    std::vector<NodeId> roots;
    if (plan.full)
    {
        for (NodeId c : g.children(s.node))
            if (const Node* n = g.node(c); n && n->type().isA("transform") && !isStage(n))
                roots.push_back(c);
    }
    else
    {
        roots = plan.alone;
        roots.insert(roots.end(), plan.trees.begin(), plan.trees.end());
        o.childless.insert(plan.alone.begin(), plan.alone.end());
        o.localRoots = true;
    }
    s.lastExported = 0;
    if (roots.empty())
        return out;
    ++(plan.full ? s.fullDiffs : s.partialDiffs);
    o.zUp = UsdGeomGetStageUpAxis(s.stage) != UsdGeomTokens->y;
    o.metersPerUnit = UsdGeomGetStageMetersPerUnit(s.stage);
    o.textures = ExportOptions::Textures::Absolute;
    o.materialX = stageMaterialX(s, g);
    o.lods = false;
    o.inMemory = true;
    o.relativeTo = s.node;
    const double tcps = s.stage->GetTimeCodesPerSecond() > 0.0 ? s.stage->GetTimeCodesPerSecond() : 24.0;
    o.timeCodesPerSecond = tcps;
    o.start = s.start;
    o.end = s.end;
    ExportResult r = exportUsd(g, roots, fs::temp_directory_path() / "lilaStage.usda", o);
    if (!r.ok)
    {
        warnings.push_back("the stage's objects could not be compared: " + r.error);
        out.ok = false;
        return out;
    }
    out.stage = r.stage;
    out.layer = r.stage->GetRootLayer();
    out.root = r.stage->GetDefaultPrim().GetPath();
    for (const auto& [n, p] : r.prims)
    {
        out.prim[n] = p;
        out.nodeAt[p] = n;
        out.hashes[n] = hashPrim(out.layer, p);
    }
    s.lastExported = out.prim.size();
    out.materials = r.materials;
    for (const auto& [slot, p] : r.materials)
    {
        out.slotOf[p] = slot;
        out.materialHashes[slot] = hashTree(out.layer, p, true);
    }
    for (const auto& [p, joints] : r.skeletons)
        out.skelJoints[p] = joints;
    mapSkeletons(s, out);
    return out;
}

/**
 * True when a node written alone gets the prim a write of the whole stage gives it: not a joint, skin, blend shape or
 * point instance (their prims depend on others), and under the stage or a plain linked node.
 */
bool writesAlone(Stage& s, Graph& g, NodeId n)
{
    const Node* node = g.node(n);
    if (!node || s.partOf.count(n) || node->type().isA("joint"))
        return false;
    if (node->type().isA("mesh") && (scene::skin::find(g, n) || scene::morphs::find(g, n)))
        return false;
    const NodeId parent = g.parent(n);
    if (parent == s.node)
        return true;
    const Node* p = g.node(parent);
    return p && s.primOf.count(parent) && !p->type().isA("joint") && !p->type().isA("camera") && !p->type().isA("light");
}

/** The prims of these nodes, compared alone: the full plan when one of them can't be. */
Plan planFor(Stage& s, Graph& g, const std::set<NodeId>& linked, const std::set<NodeId>& added)
{
    Plan plan;
    for (NodeId n : linked)
    {
        if (!writesAlone(s, g, n))
            return {};
        plan.alone.push_back(n);
    }
    for (NodeId n : added)
    {
        // Joints and skinned meshes are written with their whole skeleton.
        if (const Node* node = g.node(n); node && (node->type().isA("joint") || (node->type().isA("mesh") && (scene::skin::find(g, n) || scene::morphs::find(g, n)))))
            return {};
        NodeId top = n;
        for (NodeId p = g.parent(top); p != s.node && p != kNoNode && !s.primOf.count(p); p = g.parent(top))
        {
            if (s.partOf.count(p))
                return {};
            top = p;
        }
        if (g.parent(top) != s.node && !writesAlone(s, g, g.parent(top)))
            return {};
        if (std::find(plan.trees.begin(), plan.trees.end(), top) == plan.trees.end())
            plan.trees.push_back(top);
    }
    plan.full = false;
    return plan;
}

/**
 * What the next comparison exports, from the nodes the scene changed: those under the stage, those whose placement
 * depends on them through connections (constraints, drivers), the ones an edited curve, deformer or shading node
 * feeds, and one mesh per edited material. Everything after a batch, an undo, or a change of the sampled range.
 */
Plan planSync(Stage& s, Graph& g)
{
    LILA_ZONE();

    if (s.full)
        return {};
    if (std::isnan(s.start) || std::isnan(s.end))
    {
        const scene::anim::Playback& pb = scene::anim::playback();
        if (pb.animStart != s.comparedStart || pb.animEnd != s.comparedEnd)
            return {};
    }
    if (s.pending.empty())
        return Plan{false, {}, {}};
    std::unordered_map<NodeId, std::vector<NodeId>> feeds;
    for (const auto& [dst, src] : g.connections())
        feeds[src.node].push_back(dst.node);
    std::set<NodeId> dirty, seen;
    std::set<std::string> slots;
    std::vector<NodeId> todo(s.pending.begin(), s.pending.end());
    auto downstream = [&](NodeId id) {
        if (auto it = feeds.find(id); it != feeds.end())
            todo.insert(todo.end(), it->second.begin(), it->second.end());
    };
    while (!todo.empty())
    {
        const NodeId id = todo.back();
        todo.pop_back();
        const Node* n = g.node(id);
        // A deleted joint changes its whole skeleton.
        if (!n && s.jointOf.count(id))
            return {};
        if (!n || !seen.insert(id).second)
            continue;
        if (n->type().isA("material"))
            slots.insert(n->name());
        downstream(id);
        if (n->type().isA("transform") && !isStage(n) && under(g, id, s.node))
        {
            dirty.insert(id);
            // Everything below it moved in the world too, and so did whatever reads those placements.
            for (NodeId d : g.descendants(id))
                downstream(d);
        }
    }
    if (!slots.empty())
    {
        g.evaluate();
        for (const auto& [n, p] : s.primOf)
        {
            const Node* node = g.node(n);
            if (slots.empty())
                break;
            if (!node || !node->type().isA("mesh"))
                continue;
            const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({n, scene::MeshNode::kOutMesh, -1}));
            if (!m)
                continue;
            for (const auto& [slot, faces] : scene::materials::slotUsage(*m))
                if (slots.erase(slot))
                    dirty.insert(n);
        }
    }
    std::set<NodeId> linked, added;
    for (NodeId n : dirty)
    {
        if (s.primOf.count(n))
            linked.insert(n);
        else if (s.partOf.count(n))
            return {};
        else
            added.insert(n);
    }
    return planFor(s, g, linked, added);
}

// Persisting the link with the scene.

std::string hex(uint64_t v) { return std::format("{:016x}", v); }

void writeAttrs(Stage& s, Graph& g)
{
    NoUndo quiet;
    if (!g.node(s.node))
        return;
    std::string data;
    if (s.mode == Stage::Mode::Embedded && !s.edit->IsEmpty())
        s.edit->ExportToString(&data);
    setText(g, s.node, stageAttr::kEditData, data);

    json links = json::array();
    for (const auto& [n, p] : s.primOf)
        if (std::string rel = relPath(g, s.node, n); !rel.empty())
            links.push_back({rel, p.GetString()});
    for (const auto& [n, p] : s.partOf)
        if (std::string rel = relPath(g, s.node, n); !rel.empty())
        {
            if (auto c = s.copyOf.find(n); c != s.copyOf.end())
                links.push_back({rel, p.GetString(), 2, c->second.GetString()});
            else if (auto i = s.instanceOf.find(n); i != s.instanceOf.end())
                links.push_back({rel, p.GetString(), 3, i->second.second});
            else if (auto j = s.jointOf.find(n); j != s.jointOf.end())
                links.push_back({rel, p.GetString(), 4, j->second.GetString()});
            else
                links.push_back({rel, p.GetString(), 1});
        }
    std::sort(links.begin(), links.end());
    setText(g, s.node, stageAttr::kMap, links.dump());

    json base = json::object();
    for (const auto& [key, groups] : s.baseline)
    {
        json gj = json::object();
        for (const auto& [group, h] : groups)
            gj[group] = hex(h);
        base[key] = std::move(gj);
    }
    if (!std::isnan(s.start) && !std::isnan(s.end))
        base["$range"] = {{"start", s.start}, {"end", s.end}};
    setText(g, s.node, stageAttr::kBaseline, base.dump());

    json mats = json::object();
    for (const auto& [slot, p] : s.materialOrigin)
        mats[slot] = p.GetString();
    setText(g, s.node, stageAttr::kMaterials, mats.dump());

    json rules = json::array();
    for (const auto& [p, rule] : s.stage->GetLoadRules().GetRules())
        rules.push_back({p.GetString(), int(rule)});
    setText(g, s.node, stageAttr::kLoadRules, rules.size() == 1 && rules[0][0] == "/" && rules[0][1] == int(UsdStageLoadRules::AllRule) ? "" : rules.dump());

    json stamps = json::object();
    for (const auto& [file, st] : s.stamps)
        stamps[file] = {st.first, st.second};
    setText(g, s.node, stageAttr::kStamps, stamps.dump());
}

void readAttrs(Stage& s, const Graph& g)
{
    const Node& n = *g.node(s.node);
    if (json links = parseJson(text(n, stageAttr::kMap)); links.is_array())
        for (const json& e : links)
        {
            if (!e.is_array() || e.size() < 2 || !e[0].is_string() || !e[1].is_string() || !SdfPath::IsValidPathString(e[1].get<std::string>()))
                continue;
            const NodeId id = resolveRel(g, s.node, e[0].get<std::string>());
            if (!id)
                continue;
            const SdfPath path(e[1].get<std::string>());
            const int kind = e.size() > 2 && e[2].is_number_integer() ? e[2].get<int>() : 0;
            if (kind == 0)
                s.link(id, path);
            else
                s.partOf[id] = path;
            if (kind == 2 && e.size() > 3 && e[3].is_string() && SdfPath::IsValidPathString(e[3].get<std::string>()))
                s.copyOf[id] = SdfPath(e[3].get<std::string>());
            else if (kind == 3 && e.size() > 3 && e[3].is_number_unsigned())
                s.instanceOf[id] = {path, e[3].get<size_t>()};
            else if (kind == 4 && e.size() > 3 && e[3].is_string())
                s.jointOf[id] = TfToken(e[3].get<std::string>());
        }
    if (json base = parseJson(text(n, stageAttr::kBaseline)); base.is_object())
        for (auto& [key, groups] : base.items())
        {
            if (key == "$range")
            {
                s.start = groups.value("start", s.start);
                s.end = groups.value("end", s.end);
                continue;
            }
            if (!groups.is_object())
                continue;
            Groups& into = s.baseline[key];
            for (auto& [group, h] : groups.items())
                if (h.is_string())
                    into[group] = std::stoull(h.get<std::string>(), nullptr, 16);
        }
    if (json mats = parseJson(text(n, stageAttr::kMaterials)); mats.is_object())
        for (auto& [slot, p] : mats.items())
            if (p.is_string() && SdfPath::IsValidPathString(p.get<std::string>()))
                s.materialOrigin[slot] = SdfPath(p.get<std::string>());
    if (json stamps = parseJson(text(n, stageAttr::kStamps)); stamps.is_object())
        for (auto& [file, st] : stamps.items())
            if (st.is_array() && st.size() == 2)
                s.stamps[file] = {st[0].get<int64_t>(), st[1].get<uint64_t>()};
}

std::map<std::string, std::pair<int64_t, uint64_t>> stampsOf(const Stage& s)
{
    std::map<std::string, std::pair<int64_t, uint64_t>> out;
    for (const SdfLayerHandle& l : s.stage->GetUsedLayers())
    {
        if (!l || l->IsAnonymous())
            continue;
        const std::string file = fileOf(l);
        if (file.empty())
            continue;
        std::error_code ec, ec2;
        const auto t = fs::last_write_time(utf8Path(file), ec);
        const auto size = fs::file_size(utf8Path(file), ec2);
        out[file] = {ec ? 0 : int64_t(t.time_since_epoch().count()), ec2 ? 0 : uint64_t(size)};
    }
    return out;
}

/** The layers whose files changed on disk since the stamps were taken. */
std::vector<SdfLayerHandle> changedLayers(const Stage& s)
{
    const auto now = stampsOf(s);
    std::vector<SdfLayerHandle> out;
    for (const SdfLayerHandle& l : s.stage->GetUsedLayers())
    {
        if (!l || l->IsAnonymous())
            continue;
        const std::string file = fileOf(l);
        auto was = s.stamps.find(file);
        auto is = now.find(file);
        if (was != s.stamps.end() && is != now.end() && was->second != is->second)
            out.push_back(l);
    }
    return out;
}

// Composition.

void copyStageMetadata(const SdfLayerHandle& from, const SdfLayerHandle& to)
{
    const SdfPath root = SdfPath::AbsoluteRootPath();
    for (const TfToken& k : {UsdGeomTokens->upAxis, UsdGeomTokens->metersPerUnit, SdfFieldKeys->TimeCodesPerSecond, SdfFieldKeys->FramesPerSecond,
                             SdfFieldKeys->StartTimeCode, SdfFieldKeys->EndTimeCode, SdfFieldKeys->DefaultPrim})
        if (from->HasField(root, k))
            to->SetField(root, k, from->GetField(root, k));
}

/** The source and every layer it sublayers, depth first. */
void layerStack(const SdfLayerHandle& layer, std::vector<SdfLayerRefPtr>& out, int depth = 0)
{
    if (!layer || depth > 32)
        return;
    for (const std::string& sub : std::vector<std::string>(layer->GetSubLayerPaths()))
        if (SdfLayerRefPtr l = SdfLayer::FindOrOpenRelativeToLayer(layer, sub))
        {
            out.push_back(l);
            layerStack(l, out, depth + 1);
        }
}

/** Merges carry (stronger) into target, keeping both layers' opinions. */
void mergeInto(const SdfLayerHandle& target, const SdfLayerHandle& carry)
{
    if (!carry || carry->IsEmpty())
        return;
    if (target->IsEmpty())
    {
        target->TransferContent(carry);
        return;
    }
    SdfLayerRefPtr merged = SdfLayer::CreateAnonymous("merged.usda");
    merged->TransferContent(carry);
    UsdUtilsStitchLayers(merged, target);
    target->TransferContent(merged);
}

/**
 * Opens the source and the edit layer the node's attrs name and composes them under an anonymous root. carry holds
 * edits to keep: the text saved in the .lila, or the previous edit layer when the attrs changed.
 */
void compose(Stage& s, Graph& g, const SdfLayerRefPtr& carry, std::vector<std::string>& warnings)
{
    LILA_ZONE();

    const Node& n = *g.node(s.node);
    s.source = text(n, stageAttr::kFile);
    s.editSpec = text(n, stageAttr::kEditLayer);
    if (s.source.empty())
        throw CommandError(n.name() + ": no USD file set");
    Diagnostics diag;
    const fs::path file = resolveFile(s.source);
    s.sourceLayer = SdfLayer::FindOrOpen(utf8(file));
    if (!s.sourceLayer)
    {
        std::vector<std::string> errors = diag.errors();
        throw CommandError("can't open " + utf8(file) + (errors.empty() ? std::string() : ": " + errors.front()));
    }

    if (s.editSpec.empty())
    {
        s.mode = Stage::Mode::Embedded;
        s.edit = SdfLayer::CreateAnonymous("edits.usda");
        mergeInto(s.edit, carry);
    }
    else
    {
        const fs::path want = resolveFile(s.editSpec);
        std::vector<SdfLayerRefPtr> subs;
        layerStack(s.sourceLayer, subs);
        s.edit = nullptr;
        for (const SdfLayerRefPtr& l : subs)
            if (!l->IsAnonymous() && samePath(utf8Path(fileOf(l)), want))
                s.edit = l;
        if (s.edit)
            s.mode = Stage::Mode::Sublayer;
        else
        {
            if (samePath(want, file))
                throw CommandError(n.name() + ": the edit layer can't be the source file itself; pick a .usda of its own or one of its sublayers");
            s.mode = Stage::Mode::File;
            std::error_code ec;
            s.edit = fs::exists(want, ec) ? SdfLayer::FindOrOpen(utf8(want)) : SdfLayer::CreateNew(utf8(want));
            if (!s.edit)
                throw CommandError(n.name() + ": can't open or create the edit layer " + utf8(want));
        }
        if (carry && !carry->IsEmpty())
        {
            mergeInto(s.edit, carry);
            if (s.mode == Stage::Mode::File)
                s.edit->Save();
        }
    }

    s.root = SdfLayer::CreateAnonymous("lilaStage.usda");
    if (s.mode == Stage::Mode::Sublayer)
        s.root->SetSubLayerPaths({s.sourceLayer->GetIdentifier()});
    else
        s.root->SetSubLayerPaths({s.edit->GetIdentifier(), s.sourceLayer->GetIdentifier()});
    copyStageMetadata(s.sourceLayer, s.root);

    UsdStageLoadRules rules;
    if (json saved = parseJson(text(n, stageAttr::kLoadRules)); saved.is_array())
        for (const json& r : saved)
            if (r.is_array() && r.size() == 2 && r[0].is_string() && SdfPath::IsValidPathString(r[0].get<std::string>()))
                rules.AddRule(SdfPath(r[0].get<std::string>()), UsdStageLoadRules::Rule(r[1].get<int>()));
    s.stage = UsdStage::Open(s.root, UsdStage::LoadNone);
    if (!s.stage)
        throw CommandError("can't compose " + utf8(file));
    if (rules.GetRules().empty())
        s.stage->Load(SdfPath::AbsoluteRootPath());
    else
        s.stage->SetLoadRules(rules);
    s.stage->SetEditTarget(UsdEditTarget(s.edit));
    if (s.key.IsValid())
        TfNotice::Revoke(s.key);
    s.key = TfNotice::Register(TfCreateWeakPtr(&s), &Stage::onChanged, UsdStagePtr(s.stage));
    for (const std::string& e : diag.errors())
        warnings.push_back("OpenUSD: " + e);
    for (const std::string& w : diag.warnings())
        warnings.push_back("OpenUSD: " + w);
}

void logWarnings(Stage& s, const Graph& g, std::vector<std::string> warnings)
{
    const Node* n = g.node(s.node);
    for (const std::string& w : warnings)
        log::warn("{}: {}", n ? n->name() : std::string("usdStage"), w);
    s.warnings = std::move(warnings);
}

// Authoring Lila's edits into the edit layer.

struct Author
{
    Stage& s;
    Snapshot& now;
    std::vector<std::string>& warn;
    SdfLayerHandle L = s.edit;
    std::set<SdfPath> taken;
    std::set<SdfPath> subsetsDone;
    /** Properties written on linked prims, checked afterwards against stronger layers. */
    std::set<SdfPath> written;
    /** Mesh prims whose shape was written. */
    std::set<SdfPath> shapes;
    /** Instances made non-instanceable. */
    std::set<SdfPath> opened;
    /** By written Skeleton prim: the stage's Skeleton prim it is written over, and the stage's token of each of its joints. */
    std::map<SdfPath, SdfPath> skelPaths;
    std::map<SdfPath, VtTokenArray> skelTokens;
    /** By the stage's Skeleton prim: its joints once this comparison is written. */
    std::map<SdfPath, VtTokenArray> skelOrder;
    /** Meshes whose skin is written whatever their baseline says: their skeleton lost joints they may index. */
    std::set<SdfPath> forceSkin;
    /** Joints whose delete was undone this comparison, with where they stood in their skeleton's joints. */
    std::map<NodeId, size_t> restoredAt;

    SdfPrimSpecHandle over(const SdfPath& p)
    {
        if (SdfPrimSpecHandle spec = L->GetPrimAtPath(p))
            return spec;
        return SdfCreatePrimInLayer(L, p);
    }

    SdfAttributeSpecHandle attrSpec(const SdfPath& prim, const std::string& name, const SdfValueTypeName& type, SdfVariability variability)
    {
        if (SdfAttributeSpecHandle a = L->GetAttributeAtPath(prim.AppendProperty(TfToken(name))))
            return a;
        return SdfAttributeSpec::New(over(prim), name, type, variability);
    }

    SdfPath uniqueChild(const SdfPath& parent, const std::string& want)
    {
        std::string name = want;
        for (int k = 1;; ++k)
        {
            const SdfPath c = parent.AppendChild(TfToken(name));
            if (!s.stage->GetPrimAtPath(stagePath(c)) && !L->GetPrimAtPath(c) && !taken.count(c))
            {
                taken.insert(c);
                return c;
            }
            name = want + std::to_string(k);
        }
    }

    void copyGroup(const SdfPath& e, const SdfPath& p, const std::string& group)
    {
        const SdfPrimSpecHandle es = now.layer->GetPrimAtPath(e);
        if (!es)
            return;
        over(p);
        const std::set<std::string> components = componentAttributes(es);
        for (const SdfPropertySpecHandle& prop : es->GetProperties())
            if (groupOf(prop->GetName(), components) == group)
            {
                SdfCopySpec(now.layer, prop->GetPath(), L, p.AppendProperty(TfToken(prop->GetName())));
                written.insert(p.AppendProperty(TfToken(prop->GetName())));
            }
    }

    void visibility(const SdfPath& e, const SdfPath& p)
    {
        if (now.layer->GetPropertyAtPath(e.AppendProperty(UsdGeomTokens->visibility)))
        {
            copyGroup(e, p, "visibility");
            return;
        }
        SdfAttributeSpecHandle a = attrSpec(p, "visibility", SdfValueTypeNames->Token, SdfVariabilityVarying);
        a->SetDefaultValue(VtValue(UsdGeomTokens->inherited));
        written.insert(a->GetPath());
        L->EraseField(a->GetPath(), SdfFieldKeys->TimeSamples);
    }

    void blockStale(const SdfPath& e, const SdfPath& p)
    {
        const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
        if (!cp || !cp.IsA<UsdGeomPointBased>())
            return;
        for (const UsdAttribute& a : cp.GetAuthoredAttributes())
        {
            const std::string name = a.GetName().GetString();
            if (!blockable(name) || now.layer->GetPropertyAtPath(e.AppendProperty(a.GetName())))
                continue;
            SdfAttributeSpecHandle spec = attrSpec(p, name, a.GetTypeName(), a.GetVariability());
            spec->SetDefaultValue(VtValue(SdfValueBlock()));
            L->EraseField(spec->GetPath(), SdfFieldKeys->TimeSamples);
        }
    }

    /** The material prim a Lila material stands for in the stage, written there first when it is Lila's own. */
    SdfPath materialFor(const std::string& slot)
    {
        if (auto it = s.materialOrigin.find(slot); it != s.materialOrigin.end() && (s.stage->GetPrimAtPath(it->second) || L->GetPrimAtPath(it->second)))
            return it->second;
        auto m = now.materials.find(slot);
        if (m == now.materials.end())
            return {};
        const UsdPrim dp = s.stage->GetDefaultPrim();
        const SdfPath scope = (dp ? dp.GetPath() : SdfPath::AbsoluteRootPath()).AppendChild(TfToken("LilaMaterials"));
        SdfPrimSpecHandle scopeSpec = over(scope);
        if (const UsdPrim existing = s.stage->GetPrimAtPath(scope); !existing || !existing.IsDefined())
        {
            scopeSpec->SetSpecifier(SdfSpecifierDef);
            scopeSpec->SetTypeName("Scope");
        }
        const SdfPath dst = uniqueChild(scope, identifier(slot, "Material"));
        SdfCopySpec(now.layer, m->second, L, dst);
        s.materialOrigin[slot] = dst;
        s.baseline["$mat:" + slot]["material"] = now.materialHashes[slot];
        return dst;
    }

    /** A material edit: Lila's network over the prim it came from, with that prim's other shaders switched off. */
    void writeMaterial(const std::string& slot, const SdfPath& origin)
    {
        auto m = now.materials.find(slot);
        if (m == now.materials.end())
            return;
        const UsdPrim co = s.stage->GetPrimAtPath(origin);
        const SdfPrimSpecHandle em = now.layer->GetPrimAtPath(m->second);
        if (!em)
            return;
        if (!origin.GetParentPath().IsAbsoluteRootPath())
            over(origin.GetParentPath());
        SdfCopySpec(now.layer, m->second, L, origin);
        if (!co)
            return;
        // Surface outputs Lila's network doesn't write (a MaterialX one, when it writes none) would still reach the
        // shaders switched off below.
        for (const UsdShadeOutput& o : UsdShadeMaterial(co).GetOutputs())
        {
            if (now.layer->GetAttributeAtPath(m->second.AppendProperty(o.GetAttr().GetName())) || !o.HasConnectedSource())
                continue;
            SdfAttributeSpecHandle spec = attrSpec(origin, o.GetAttr().GetName().GetString(), o.GetTypeName(), SdfVariabilityVarying);
            L->SetField(spec->GetPath(), SdfFieldKeys->ConnectionPaths, VtValue(SdfPathListOp::CreateExplicit({})));
        }
        for (const UsdPrim& c : co.GetChildren())
        {
            if (!now.layer->GetPrimAtPath(m->second.AppendChild(c.GetName())))
            {
                over(c.GetPath())->SetActive(false);
                continue;
            }
            for (const UsdAttribute& a : c.GetAuthoredAttributes())
            {
                if (L->GetAttributeAtPath(a.GetPath()))
                    continue;
                SdfAttributeSpecHandle spec = attrSpec(c.GetPath(), a.GetName().GetString(), a.GetTypeName(), a.GetVariability());
                spec->SetDefaultValue(VtValue(SdfValueBlock()));
                L->SetField(spec->GetPath(), SdfFieldKeys->ConnectionPaths, VtValue(SdfPathListOp::CreateExplicit({})));
            }
        }
    }

    void applyBindingApi(const SdfPath& p)
    {
        if (const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p)); cp && cp.HasAPI<UsdShadeMaterialBindingAPI>())
            return;
        applyApi(p, TfToken("MaterialBindingAPI"));
    }

    void applyApi(const SdfPath& p, const TfToken& api)
    {
        if (const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p)))
            for (const TfToken& t : cp.GetAppliedSchemas())
                if (t == api)
                    return;
        over(p);
        SdfTokenListOp op;
        if (VtValue v = L->GetField(p, UsdTokens->apiSchemas); v.IsHolding<SdfTokenListOp>())
            op = v.UncheckedGet<SdfTokenListOp>();
        for (const TfToken& t : op.GetAppliedItems())
            if (t == api)
                return;
        TfTokenVector pre = op.GetPrependedItems();
        pre.push_back(api);
        op.SetPrependedItems(pre);
        L->SetField(p, UsdTokens->apiSchemas, VtValue(op));
    }

    void writeTargets(const SdfPath& p, const TfToken& name, const SdfPathVector& targets)
    {
        SdfPrimSpecHandle ps = over(p);
        SdfRelationshipSpecHandle r = L->GetRelationshipAtPath(p.AppendProperty(name));
        if (!r)
            r = SdfRelationshipSpec::New(ps, name.GetString(), false, SdfVariabilityUniform);
        L->SetField(r->GetPath(), SdfFieldKeys->TargetPaths, VtValue(SdfPathListOp::CreateExplicit(targets)));
    }

    /** A target in the snapshot as a path in the stage: materials to theirs, other written prims to their nodes' prims. */
    SdfPath mapTarget(const SdfPath& t)
    {
        if (auto it = now.slotOf.find(t); it != now.slotOf.end())
            return materialFor(it->second);
        for (const auto& [e, st] : skelPaths)
            if (t.HasPrefix(e))
                return t.ReplacePrefix(e, st);
        if (auto it = now.nodeAt.find(t.GetPrimPath()); it != now.nodeAt.end())
            if (auto p = s.primOf.find(it->second); p != s.primOf.end())
                return t.ReplacePrefix(t.GetPrimPath(), p->second);
        return {};
    }

    void bind(const SdfPath& e, const SdfPath& p)
    {
        const SdfPath relPath = e.AppendProperty(UsdShadeTokens->materialBinding);
        const VtValue v = now.layer->GetField(relPath, SdfFieldKeys->TargetPaths);
        if (!v.IsHolding<SdfPathListOp>())
            return;
        SdfPathVector out;
        for (const SdfPath& t : v.UncheckedGet<SdfPathListOp>().GetAppliedItems())
            if (SdfPath m = mapTarget(t); !m.IsEmpty())
                out.push_back(m);
        if (out.empty())
            return;
        writeTargets(p, UsdShadeTokens->materialBinding, out);
        written.insert(p.AppendProperty(UsdShadeTokens->materialBinding));
        applyBindingApi(p);
    }

    /** Relationship targets copied from the snapshot pointed into it; point them into the stage. */
    void retarget(const SdfPath& root)
    {
        std::vector<SdfPath> rels;
        L->Traverse(root, [&](const SdfPath& p) {
            if (p.IsPropertyPath() && L->GetRelationshipAtPath(p))
                rels.push_back(p);
        });
        for (const SdfPath& rp : rels)
        {
            const VtValue v = L->GetField(rp, SdfFieldKeys->TargetPaths);
            if (!v.IsHolding<SdfPathListOp>())
                continue;
            SdfPathVector out;
            bool dropped = false;
            for (const SdfPath& t : v.UncheckedGet<SdfPathListOp>().GetAppliedItems())
            {
                if (!now.root.IsEmpty() && !t.HasPrefix(now.root))
                {
                    out.push_back(t);
                    continue;
                }
                if (SdfPath m = mapTarget(t); !m.IsEmpty())
                    out.push_back(m);
                else
                    dropped = true;
            }
            if (dropped && rp.GetNameToken().GetString().starts_with("skel:"))
                warn.push_back(std::format("{}: its skeleton binding isn't written", rp.GetPrimPath().GetString()));
            L->SetField(rp, SdfFieldKeys->TargetPaths, VtValue(SdfPathListOp::CreateExplicit(out)));
        }
    }

    /** GeomSubsets are replaced as a whole: Lila's written in, the prim's others switched off. */
    void subsets(const SdfPath& e, const SdfPath& p)
    {
        if (!subsetsDone.insert(p).second)
            return;
        const SdfPrimSpecHandle es = now.layer->GetPrimAtPath(e);
        if (!es)
            return;
        std::set<std::string> mine;
        for (const SdfPrimSpecHandle& c : es->GetNameChildren())
            if (c->GetTypeName() == "GeomSubset")
                mine.insert(c->GetName());
        if (const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p)))
            for (const UsdPrim& c : cp.GetChildren())
                if (c.IsA<UsdGeomSubset>() && !mine.count(c.GetName().GetString()))
                    deactivate(p.AppendChild(c.GetName()));
        for (const std::string& name : mine)
        {
            const SdfPath dst = p.AppendChild(TfToken(name));
            over(p);
            SdfCopySpec(now.layer, e.AppendChild(TfToken(name)), L, dst);
            retarget(dst);
        }
    }

    /**
     * Instances take no opinions on the prims inside them: the instances around p (and p itself, when it is one and
     * self is set) are made non-instanceable in the edit layer, with a warning.
     */
    void openInstances(const SdfPath& p, bool self)
    {
        const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
        if (!cp)
            return;
        for (UsdPrim q = self ? cp : cp.GetParent(); q && !q.IsPseudoRoot(); q = q.GetParent())
            if (q.IsInstance() && opened.insert(q.GetPath()).second)
            {
                const SdfPrimSpecHandle had = L->GetPrimAtPath(q.GetPath());
                // Opinions the edit layer already held inside it would be lost by instancing it again.
                if (!had || had->GetNameChildren().empty())
                    rememberOpening(q.GetPath(), had);
                over(q.GetPath())->SetInstanceable(false);
                warn.push_back(std::format("{}: made non-instanceable in the edit layer, so the edits inside it could be written", q.GetPath().GetString()));
            }
    }

    /** The instance's instanceable opinion and the baseline of every linked prim inside it, for closeInstances. */
    void rememberOpening(const SdfPath& q, const SdfPrimSpecHandle& had)
    {
        Groups mark{{"open", 1}};
        if (had && had->HasInstanceable())
            mark["was"] = had->GetInstanceable() ? 1 : 0;
        s.baseline[kOpen + q.GetString()] = mark;
        for (const auto& [lp, n] : s.nodeOf)
            if (insideOf(lp, q))
            {
                auto b = s.baseline.find(lp.GetString());
                s.baseline.try_emplace(kOrigin + lp.GetString(), b == s.baseline.end() ? Groups() : b->second);
            }
    }

    /** A deleted object: its prim deactivated, or removed when only the edit layer defines it. */
    void deactivate(const SdfPath& p)
    {
        const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
        if (!cp)
            return;
        if (cp.IsInstanceProxy())
        {
            openInstances(p, false);
            over(p)->SetActive(false);
            return;
        }
        bool ours = true;
        for (const SdfPrimSpecHandle& spec : cp.GetPrimStack())
            ours = ours && spec->GetLayer() == L;
        if (ours)
        {
            const SdfPrimSpecHandle spec = L->GetPrimAtPath(p);
            const SdfPrimSpecHandle parent = L->GetPrimAtPath(p.GetParentPath());
            if (spec && parent)
                parent->RemoveNameChild(spec);
        }
        else
            over(p)->SetActive(false);
    }

    void reactivate(const SdfPath& p)
    {
        if (SdfPrimSpecHandle spec = L->GetPrimAtPath(p); spec && spec->HasActive())
            spec->ClearActive();
    }

    void author(const SdfPath& e, const SdfPath& p, const std::string& group)
    {
        if (group == "xform")
            copyGroup(e, p, "xform");
        else if (group == "visibility")
            visibility(e, p);
        else if (group == "shape")
        {
            copyGroup(e, p, "shape");
            blockStale(e, p);
            subsets(e, p);
            shapes.insert(p);
        }
        else if (group == "binding")
        {
            bind(e, p);
            subsets(e, p);
        }
        else if (group == "skin")
            skin(e, p);
        else if (group == "blendShapes")
            blendShapes(e, p);
        else if (group == "attrs")
            components(e, p);
    }

    /** Warns about component schemas a layer stronger than the edit layer applies or removes against Lila's edit. */
    void schemasOverridden(const UsdPrim& cp, const SdfPath& p, const TfTokenVector& want, const TfTokenVector& have)
    {
        const SdfLayerHandleVector layers = s.stage->GetLayerStack(true);
        const auto rank = [&](const SdfLayerHandle& l) { return size_t(std::find(layers.begin(), layers.end(), l) - layers.begin()); };
        const size_t edit = rank(L);
        if (edit == layers.size())
            return;
        for (const SdfPrimSpecHandle& spec : cp.GetPrimStack())
        {
            if (rank(spec->GetLayer()) >= edit)
                continue;
            const VtValue v = spec->GetInfo(UsdTokens->apiSchemas);
            if (!v.IsHolding<SdfTokenListOp>())
                continue;
            const SdfTokenListOp& op = v.UncheckedGet<SdfTokenListOp>();
            TfTokenVector adds = op.GetExplicitItems();
            for (const TfTokenVector* items : {&op.GetPrependedItems(), &op.GetAppendedItems(), &op.GetAddedItems()})
                adds.insert(adds.end(), items->begin(), items->end());
            auto in = [](const TfTokenVector& v, const TfToken& t) { return std::find(v.begin(), v.end(), t) != v.end(); };
            const std::string layer = spec->GetLayer()->GetDisplayName();
            for (const TfToken& t : have)
                if (!in(want, t) && in(adds, t))
                    warn.push_back(std::format("{}: {} applies {}, which is stronger than the edit layer, so removing it doesn't show", p.GetString(), layer, t.GetString()));
            for (const TfToken& t : want)
                if (in(op.GetDeletedItems(), t) || (op.IsExplicit() && !in(adds, t)))
                    warn.push_back(std::format("{}: {} removes {}, which is stronger than the edit layer, so adding it doesn't show", p.GetString(), layer, t.GetString()));
        }
    }

    /**
     * Component schemas and attributes: the written ones over the stage's, a stage attribute of another type written as
     * that type, and the stage's components Lila no longer has removed (schemas) or blocked (attributes).
     */
    void components(const SdfPath& e, const SdfPath& p)
    {
        const SdfPrimSpecHandle es = now.layer->GetPrimAtPath(e);
        if (!es)
            return;
        const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
        const TfTokenVector want = specComponentSchemas(es);
        const TfTokenVector have = cp ? componentSchemas(cp.GetAppliedSchemas()) : TfTokenVector();
        auto in = [](const TfTokenVector& v, const TfToken& t) { return std::find(v.begin(), v.end(), t) != v.end(); };
        over(p);
        SdfTokenListOp op;
        if (VtValue v = L->GetField(p, UsdTokens->apiSchemas); v.IsHolding<SdfTokenListOp>())
            op = v.UncheckedGet<SdfTokenListOp>();
        TfTokenVector pre = op.GetPrependedItems(), del = op.GetDeletedItems();
        for (const TfToken& t : want)
            if (!in(have, t) && !in(pre, t))
                pre.push_back(t);
        for (const TfToken& t : have)
            if (!in(want, t))
            {
                std::erase(pre, t);
                if (!in(del, t))
                    del.push_back(t);
            }
        for (const TfToken& t : want)
            std::erase(del, t);
        if (pre != op.GetPrependedItems() || del != op.GetDeletedItems())
        {
            op.SetPrependedItems(pre);
            op.SetDeletedItems(del);
            L->SetField(p, UsdTokens->apiSchemas, VtValue(op));
        }
        if (cp)
            schemasOverridden(cp, p, want, have);

        const std::set<std::string> mine = componentAttributes(es);
        for (const std::string& name : mine)
        {
            const SdfAttributeSpecHandle from = now.layer->GetAttributeAtPath(e.AppendProperty(TfToken(name)));
            if (!from)
                continue;
            const SdfPath to = p.AppendProperty(TfToken(name));
            const UsdAttribute there = cp ? cp.GetAttribute(TfToken(name)) : UsdAttribute();
            if (there && there.GetTypeName() == from->GetTypeName() && !L->GetAttributeAtPath(to))
            {
                VtValue cur;
                if (there.Get(&cur) && cur == from->GetDefaultValue())
                    continue;
            }
            written.insert(to);
            if (!there || there.GetTypeName() == from->GetTypeName())
            {
                SdfCopySpec(now.layer, from->GetPath(), L, to);
                continue;
            }
            const std::optional<scene::AttrDef> def = attrDefFor(name, from->GetTypeName(), from->GetAllowedTokens());
            const std::optional<std::vector<scene::PlugValue>> values = def ? plugValues(from->GetDefaultValue(), *def) : std::nullopt;
            const VtValue v = values ? usdValue(*values, *def, there.GetTypeName()) : VtValue();
            if (v.IsEmpty())
            {
                warn.push_back(std::format("{}: Lila's value can't be written as the stage's {}", to.GetString(), there.GetTypeName().GetAsToken().GetString()));
                continue;
            }
            SdfAttributeSpecHandle spec = attrSpec(p, name, there.GetTypeName(), there.GetVariability());
            spec->SetDefaultValue(v);
            L->EraseField(spec->GetPath(), SdfFieldKeys->TimeSamples);
        }
        if (!cp)
            return;
        const std::set<TfToken> haveNames = schemaAttributeNames(have);
        for (const UsdAttribute& a : cp.GetAuthoredAttributes())
        {
            const std::string name = a.GetName().GetString();
            if (mine.count(name) || UsdGeomPrimvar::IsPrimvar(a) || UsdGeomXformOp::IsXformOp(a) || !a.HasAuthoredValue())
                continue;
            if (!haveNames.count(a.GetName()) && cp.GetPrimDefinition().GetAttributeDefinition(a.GetName()))
                continue;
            if (!attrDefFor(name, a.GetTypeName(), {}))
                continue;
            SdfAttributeSpecHandle spec = attrSpec(p, name, a.GetTypeName(), a.GetVariability());
            spec->SetDefaultValue(VtValue(SdfValueBlock()));
            L->EraseField(spec->GetPath(), SdfFieldKeys->TimeSamples);
            written.insert(spec->GetPath());
        }
    }

    void uniformTokens(const SdfPath& p, const std::string& name, const VtTokenArray& v)
    {
        SdfAttributeSpecHandle a = attrSpec(p, name, SdfValueTypeNames->TokenArray, SdfVariabilityUniform);
        a->SetDefaultValue(VtValue(v));
        written.insert(a->GetPath());
    }

    /** A property of a written prim copied over the same property of a stage prim, time samples and all. */
    void copyProperty(const SdfPath& from, const SdfPath& to, const TfToken& name)
    {
        if (!now.layer->GetPropertyAtPath(from.AppendProperty(name)))
            return;
        over(to);
        SdfCopySpec(now.layer, from.AppendProperty(name), L, to.AppendProperty(name));
        written.insert(to.AppendProperty(name));
    }

    /** Skin and blend shapes are per point: a mesh whose point count Lila changed gets its shape written with them. */
    void matchPoints(const SdfPath& e, const SdfPath& p)
    {
        if (shapes.count(p))
            return;
        const UsdGeomPointBased cp(s.stage->GetPrimAtPath(stagePath(p)));
        VtVec3fArray points;
        if (cp)
            cp.GetPointsAttr().Get(&points, UsdTimeCode::EarliestTime());
        if (defaultOf<VtVec3fArray>(now.layer, e.AppendProperty(UsdGeomTokens->points)).size() != points.size())
            author(e, p, "shape");
    }

    /**
     * The stage's Skeleton prim a written one stands for. A synthetic one the stage lacks (a mesh Lila gave blend shapes)
     * is written beside the mesh at p, under a SkelRoot.
     */
    SdfPath skeletonFor(const SdfPath& e, const SdfPath& p)
    {
        if (auto it = skelPaths.find(e); it != skelPaths.end())
            return it->second;
        const auto joints = now.skelJoints.find(e);
        if (joints == now.skelJoints.end() || !joints->second.empty() || p.ContainsPrimVariantSelection())
            return {};
        const SdfPath parent = p.GetParentPath();
        const SdfPath dst = uniqueChild(parent, e.GetName());
        if (!parent.IsAbsoluteRootPath())
            over(parent);
        SdfCopySpec(now.layer, e, L, dst);
        skelPaths[e] = dst;
        skelTokens[e] = defaultOf<VtTokenArray>(now.layer, e.AppendProperty(UsdSkelTokens->joints));
        skelOrder[dst] = skelTokens[e];
        retarget(dst);
        bool rooted = false;
        for (UsdPrim q = s.stage->GetPrimAtPath(stagePath(parent)); q && !q.IsPseudoRoot() && !rooted; q = q.GetParent())
            rooted = q.GetTypeName() == "SkelRoot";
        if (!rooted && !parent.IsAbsoluteRootPath())
            over(parent)->SetTypeName("SkelRoot");
        s.baseline["$skel:" + dst.GetString()] = skelGroups(s, now, e);
        return dst;
    }

    /** Joint indices and weights, the bind transform and skinning method, bound to the skeleton's prim in the stage. */
    void skin(const SdfPath& e, const SdfPath& p)
    {
        const SdfPathVector t = targetsOf(now.layer, e.AppendProperty(UsdSkelTokens->skelSkeleton));
        if (t.empty())
        {
            // Unskinned in Lila: the stage's weights are blocked, and its skeleton unbound.
            if (const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p)))
                for (const UsdAttribute& a : cp.GetAuthoredAttributes())
                    if (a.GetName().GetString().starts_with("primvars:skel:"))
                    {
                        SdfAttributeSpecHandle spec = attrSpec(p, a.GetName().GetString(), a.GetTypeName(), a.GetVariability());
                        spec->SetDefaultValue(VtValue(SdfValueBlock()));
                        L->EraseField(spec->GetPath(), SdfFieldKeys->TimeSamples);
                    }
            writeTargets(p, UsdSkelTokens->skelSkeleton, {});
            return;
        }
        const SdfPath sk = skeletonFor(t.front(), p);
        if (sk.IsEmpty())
        {
            warn.push_back(std::format("{}: its skeleton has no prim in the stage, so its skin isn't written", p.GetString()));
            return;
        }
        matchPoints(e, p);
        copyGroup(e, p, "skin");
        writeTargets(p, UsdSkelTokens->skelSkeleton, {sk});
        written.insert(p.AppendProperty(UsdSkelTokens->skelSkeleton));
        // Lila's indices count the written skeleton's joints; skel:joints names them as the stage does.
        const VtTokenArray& mapped = skelTokens[t.front()];
        const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
        const bool own = cp && UsdSkelBindingAPI(cp).GetJointsAttr().HasAuthoredValue();
        if (own || mapped != skelOrder[sk])
            uniformTokens(p, UsdSkelTokens->skelJoints.GetString(), mapped);
        applyApi(p, TfToken("SkelBindingAPI"));
    }

    /** Lila's blend shapes over the targets of the same names in the stage; new ones written under the mesh. */
    void blendShapes(const SdfPath& e, const SdfPath& p)
    {
        const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
        if (!cp)
            return;
        matchPoints(e, p);
        const VtTokenArray names = defaultOf<VtTokenArray>(now.layer, e.AppendProperty(UsdSkelTokens->skelBlendShapes));
        const SdfPathVector targets = targetsOf(now.layer, e.AppendProperty(UsdSkelTokens->skelBlendShapeTargets));
        VtTokenArray had;
        SdfPathVector hadTargets;
        const UsdSkelBindingAPI binding(cp);
        binding.GetBlendShapesAttr().Get(&had);
        binding.GetBlendShapeTargetsRel().GetTargets(&hadTargets);
        SdfPathVector out;
        for (size_t i = 0; i < names.size() && i < targets.size(); ++i)
        {
            SdfPath dst;
            for (size_t j = 0; j < had.size() && j < hadTargets.size() && dst.IsEmpty(); ++j)
                if (had[j] == names[i])
                    if (const UsdPrim t = s.stage->GetPrimAtPath(hadTargets[j]); t && t.IsA<UsdSkelBlendShape>() && !t.IsInstanceProxy())
                        dst = hadTargets[j];
            if (dst.IsEmpty())
            {
                dst = uniqueChild(p, targets[i].GetName());
                over(p);
                SdfCopySpec(now.layer, targets[i], L, dst);
            }
            else
                writeShape(targets[i], dst);
            out.push_back(dst);
        }
        uniformTokens(p, UsdSkelTokens->skelBlendShapes.GetString(), names);
        writeTargets(p, UsdSkelTokens->skelBlendShapeTargets, out);
        written.insert(p.AppendProperty(UsdSkelTokens->skelBlendShapeTargets));
        applyApi(p, TfToken("SkelBindingAPI"));
    }

    /** A written BlendShape over one of the stage's: its offsets, indices and in-betweens, the stage's others blocked. */
    void writeShape(const SdfPath& from, const SdfPath& to)
    {
        const SdfPrimSpecHandle es = now.layer->GetPrimAtPath(from);
        if (!es)
            return;
        std::set<TfToken> mine;
        for (const SdfPropertySpecHandle& prop : es->GetProperties())
        {
            copyProperty(from, to, prop->GetNameToken());
            mine.insert(prop->GetNameToken());
        }
        if (const UsdPrim cp = s.stage->GetPrimAtPath(to))
            for (const UsdAttribute& a : cp.GetAuthoredAttributes())
            {
                if (mine.count(a.GetName()))
                    continue;
                SdfAttributeSpecHandle spec = attrSpec(to, a.GetName().GetString(), a.GetTypeName(), a.GetVariability());
                spec->SetDefaultValue(VtValue(SdfValueBlock()));
                L->EraseField(spec->GetPath(), SdfFieldKeys->TimeSamples);
            }
    }
};

/** The new token of a joint the stage's skeleton doesn't have: under its parent's, unique among used. */
TfToken newJointToken(const TfToken& parent, const TfToken& written, const std::set<TfToken>& used)
{
    const std::string& w = written.GetString();
    const std::string leaf = w.substr(w.rfind('/') == std::string::npos ? 0 : w.rfind('/') + 1);
    const std::string base = parent.IsEmpty() ? leaf : parent.GetString() + "/" + leaf;
    std::string name = base;
    for (int k = 1; used.count(TfToken(name)); ++k)
        name = base + std::to_string(k);
    return TfToken(name);
}

/** True when an attribute's time samples hold more than one value. */
bool varies(const SdfLayerHandle& layer, const SdfPath& attr)
{
    VtValue first;
    bool any = false;
    for (double t : layer->ListTimeSamplesForPath(attr))
    {
        VtValue v;
        layer->QueryTimeSample(attr, t, &v);
        if (any && v != first)
            return true;
        first = v;
        any = true;
    }
    return false;
}

SdfPath newAnimation(Author& au, const SdfPath& skel)
{
    const SdfPath a = au.uniqueChild(skel, "Animation");
    SdfPrimSpec::New(au.over(skel), a.GetName(), SdfSpecifierDef, "SkelAnimation");
    au.writeTargets(skel, UsdSkelTokens->skelAnimationSource, {a});
    au.applyApi(skel, TfToken("SkelBindingAPI"));
    return a;
}

/**
 * Joint edits written over the stage's Skeleton prims: rest and bind transforms in the stage's joint order, new joints
 * added at the end of it and deleted ones taken out, and the pose on the skeleton's animation, keyed or still. Blend
 * shape weights go on that animation too. Fills the Author's maps from written skeletons to the stage's.
 */
void writeSkeletons(Stage& s, Graph& g, Author& au)
{
    for (const auto& [e, st] : au.now.skelStage)
    {
        const UsdSkelSkeleton sk(s.stage->GetPrimAtPath(st));
        if (!sk || sk.GetPrim().IsInstanceProxy())
            continue;
        const std::vector<NodeId>& nodes = au.now.skelJoints[e];
        const VtTokenArray written = defaultOf<VtTokenArray>(au.now.layer, e.AppendProperty(UsdSkelTokens->joints));
        VtTokenArray had;
        sk.GetJointsAttr().Get(&had);
        const std::set<TfToken> present(had.begin(), had.end());
        std::set<TfToken> used = present;
        VtTokenArray mapped(written.size());
        std::vector<size_t> added;
        // Joints whose delete was undone, by where they stood: they go back there.
        std::map<size_t, size_t> restored;
        const UsdSkelTopology topo(written);
        std::map<TfToken, size_t> indexOf;
        for (size_t k = 0; k < written.size(); ++k)
        {
            const NodeId n = k < nodes.size() ? nodes[k] : kNoNode;
            const auto j = s.jointOf.find(n);
            const auto back = au.restoredAt.find(n);
            if (nodes.empty())
                mapped[k] = k < had.size() ? had[k] : written[k];
            else if (j != s.jointOf.end() && present.count(j->second) && !indexOf.count(j->second))
                mapped[k] = j->second;
            else if (j != s.jointOf.end() && back != au.restoredAt.end() && !used.count(j->second))
            {
                mapped[k] = j->second;
                restored[back->second] = k;
            }
            else
            {
                const int parent = topo.GetParent(k);
                mapped[k] = newJointToken(parent >= 0 ? mapped[size_t(parent)] : TfToken(), written[k], used);
                added.push_back(k);
            }
            used.insert(mapped[k]);
            indexOf[mapped[k]] = k;
        }
        au.skelPaths[e] = st;
        au.skelTokens[e] = mapped;
        au.skelOrder[st] = had;

        Groups& base = s.baseline["$skel:" + st.GetString()];
        const Groups& cur = au.now.skeletons[st];
        if (base.empty())
        {
            base = cur;
            continue;
        }
        if (base == cur)
            continue;
        auto changed = [&](const char* group) { return groupHash(cur, group) != groupHash(base, group); };
        const SdfPath anim = animationOf(au.now.layer, e);
        const UsdPrim stageAnim = UsdSkelBindingAPI(sk.GetPrim()).GetInheritedAnimationSource();
        SdfPath animAt = stageAnim ? stageAnim.GetPath() : SdfPath();
        auto animation = [&] {
            if (animAt.IsEmpty())
                animAt = newAnimation(au, st);
            return animAt;
        };
        if (!nodes.empty())
        {
            std::vector<TfToken> removed;
            for (const TfToken& t : had)
                if (!indexOf.count(t))
                    removed.push_back(t);
            const bool reshaped = !added.empty() || !removed.empty() || !restored.empty();
            VtTokenArray order;
            std::vector<size_t> from;
            for (const TfToken& t : had)
                if (auto it = indexOf.find(t); it != indexOf.end())
                {
                    order.push_back(t);
                    from.push_back(it->second);
                }
            for (const auto& [at, k] : restored)
            {
                const size_t i = std::min(at, order.size());
                order.insert(order.begin() + std::ptrdiff_t(i), mapped[k]);
                from.insert(from.begin() + std::ptrdiff_t(i), k);
            }
            for (size_t k : added)
            {
                order.push_back(mapped[k]);
                from.push_back(k);
            }
            if (reshaped)
            {
                au.uniformTokens(st, UsdSkelTokens->joints.GetString(), order);
                au.skelOrder[st] = order;
                if (sk.GetJointNamesAttr().HasAuthoredValue())
                {
                    VtTokenArray oldNames, names;
                    sk.GetJointNamesAttr().Get(&oldNames);
                    for (size_t i = 0; i < order.size(); ++i)
                    {
                        const auto at = std::find(had.begin(), had.end(), order[i]);
                        const size_t was = size_t(at - had.begin());
                        const NodeId n = from[i] < nodes.size() ? nodes[from[i]] : kNoNode;
                        names.push_back(at != had.end() && was < oldNames.size() ? oldNames[was] : TfToken(g.node(n) ? g.node(n)->name() : ""));
                    }
                    au.uniformTokens(st, UsdSkelTokens->jointNames.GetString(), names);
                }
                for (size_t k : added)
                    if (k < nodes.size())
                    {
                        s.partOf[nodes[k]] = st;
                        s.jointOf[nodes[k]] = mapped[k];
                        s.deletedParts.erase(nodes[k]);
                    }
                // Meshes indexing the stage's order would point at the wrong joints once some are gone or put back.
                if (!removed.empty() || !restored.empty())
                    for (const auto& [n, p] : s.primOf)
                        if (auto m = au.now.prim.find(n); m != au.now.prim.end())
                            if (const SdfPathVector t = targetsOf(au.now.layer, m->second.AppendProperty(UsdSkelTokens->skelSkeleton)); !t.empty() && t.front() == e)
                                au.forceSkin.insert(p);
            }
            auto reordered = [&](const char* name, const char* group) {
                if (!reshaped && !changed(group))
                    return;
                const VtMatrix4dArray src = defaultOf<VtMatrix4dArray>(au.now.layer, e.AppendProperty(TfToken(name)));
                if (src.size() != written.size())
                    return;
                VtMatrix4dArray out;
                for (size_t k : from)
                    out.push_back(src[k]);
                SdfAttributeSpecHandle a = au.attrSpec(st, name, SdfValueTypeNames->Matrix4dArray, SdfVariabilityUniform);
                a->SetDefaultValue(VtValue(out));
                au.written.insert(a->GetPath());
            };
            reordered("restTransforms", "rest");
            reordered("bindTransforms", "bind");

            // The pose: Lila's keys on the animation, or its still pose there when the stage's animation drives the joints.
            bool keyed = false;
            if (!anim.IsEmpty() && au.now.layer->GetAttributeAtPath(anim.AppendProperty(UsdSkelTokens->joints)))
                for (const TfToken& name : {UsdSkelTokens->translations, UsdSkelTokens->rotations, UsdSkelTokens->scales})
                    keyed = keyed || varies(au.now.layer, anim.AppendProperty(name));
            VtTokenArray animJoints;
            if (stageAnim)
                UsdSkelAnimation(stageAnim).GetJointsAttr().Get(&animJoints);
            // Lila writes a posed skeleton's pose on its animation, its rest pose staying the bind pose.
            const bool posed = !anim.IsEmpty() && au.now.layer->GetAttributeAtPath(anim.AppendProperty(UsdSkelTokens->joints));
            if ((reshaped || changed("anim") || changed("rest")) && (posed || !animJoints.empty()))
            {
                const SdfPath a = animation();
                auto still = [&](const TfToken& name, const SdfValueTypeName& type, const VtValue& v) {
                    SdfAttributeSpecHandle at = au.attrSpec(a, name.GetString(), type, SdfVariabilityVarying);
                    at->SetDefaultValue(v);
                    au.L->EraseField(at->GetPath(), SdfFieldKeys->TimeSamples);
                    au.written.insert(at->GetPath());
                };
                if (posed)
                {
                    VtTokenArray names;
                    for (const TfToken& t : defaultOf<VtTokenArray>(au.now.layer, anim.AppendProperty(UsdSkelTokens->joints)))
                    {
                        const auto at = std::find(written.begin(), written.end(), t);
                        names.push_back(at != written.end() ? mapped[size_t(at - written.begin())] : t);
                    }
                    au.uniformTokens(a, UsdSkelTokens->joints.GetString(), names);
                    for (const TfToken& name : {UsdSkelTokens->translations, UsdSkelTokens->rotations, UsdSkelTokens->scales})
                    {
                        const SdfPath attr = anim.AppendProperty(name);
                        const SdfAttributeSpecHandle spec = au.now.layer->GetAttributeAtPath(attr);
                        if (!spec)
                            continue;
                        if (keyed)
                        {
                            au.copyProperty(anim, a, name);
                            continue;
                        }
                        // Sampled but still: its one pose, with no samples to outweigh the stage's.
                        VtValue v = spec->GetDefaultValue();
                        if (const std::set<double> times = au.now.layer->ListTimeSamplesForPath(attr); !times.empty())
                            au.now.layer->QueryTimeSample(attr, *times.begin(), &v);
                        still(name, spec->GetTypeName(), v);
                    }
                }
                else
                {
                    VtVec3fArray translations;
                    VtQuatfArray rotations;
                    VtVec3hArray scales;
                    const VtMatrix4dArray rest = defaultOf<VtMatrix4dArray>(au.now.layer, e.AppendProperty(UsdSkelTokens->restTransforms));
                    if (rest.size() == written.size() && UsdSkelDecomposeTransforms(rest, &translations, &rotations, &scales))
                    {
                        au.uniformTokens(a, UsdSkelTokens->joints.GetString(), mapped);
                        still(UsdSkelTokens->translations, SdfValueTypeNames->Float3Array, VtValue(translations));
                        still(UsdSkelTokens->rotations, SdfValueTypeNames->QuatfArray, VtValue(rotations));
                        still(UsdSkelTokens->scales, SdfValueTypeNames->Half3Array, VtValue(scales));
                    }
                }
            }
        }
        if (changed("weights") && !anim.IsEmpty() && au.now.layer->GetAttributeAtPath(anim.AppendProperty(UsdSkelTokens->blendShapes)))
        {
            const SdfPath a = animation();
            au.copyProperty(anim, a, UsdSkelTokens->blendShapes);
            au.copyProperty(anim, a, UsdSkelTokens->blendShapeWeights);
        }
        base = cur;
    }
}

/** Point instances moved in Lila: their instancer's positions, orientations and scales written for the ones that moved. */
void writeInstances(Author& au, const SdfPath& instancer, const std::vector<std::pair<size_t, glm::dmat4>>& moved, const Axes& axes)
{
    const UsdGeomPointInstancer pi(au.s.stage->GetPrimAtPath(instancer));
    if (!pi)
        return;
    const UsdTimeCode rest = au.s.stage->HasAuthoredTimeCodeRange() ? UsdTimeCode(au.s.stage->GetStartTimeCode()) : UsdTimeCode::EarliestTime();
    VtVec3fArray positions, scales;
    VtQuathArray orientations;
    pi.GetPositionsAttr().Get(&positions, rest);
    pi.GetOrientationsAttr().Get(&orientations, rest);
    pi.GetScalesAttr().Get(&scales, rest);
    bool turned = !orientations.empty(), scaled = !scales.empty();
    for (const auto& [i, local] : moved)
    {
        if (i >= positions.size())
            continue;
        glm::dvec3 t, sc, skew;
        glm::dquat q;
        glm::dvec4 persp;
        if (!glm::decompose(axes.matrixToFile(local), sc, q, t, skew, persp))
            continue;
        positions[i] = GfVec3f(float(t.x), float(t.y), float(t.z));
        if (!turned && std::abs(std::abs(q.w) - 1.0) > 1e-6)
        {
            orientations.assign(positions.size(), GfQuath::GetIdentity());
            turned = true;
        }
        if (turned && orientations.size() == positions.size())
            orientations[i] = GfQuath(GfHalf(float(q.w)), GfVec3h(GfHalf(float(q.x)), GfHalf(float(q.y)), GfHalf(float(q.z))));
        if (!scaled && glm::length(sc - glm::dvec3(1.0)) > 1e-6)
        {
            scales.assign(positions.size(), GfVec3f(1.0f));
            scaled = true;
        }
        if (scaled && scales.size() == positions.size())
            scales[i] = GfVec3f(float(sc.x), float(sc.y), float(sc.z));
    }
    auto put = [&](const TfToken& name, const SdfValueTypeName& type, const VtValue& v) {
        SdfAttributeSpecHandle a = au.attrSpec(instancer, name.GetString(), type, SdfVariabilityVarying);
        a->SetDefaultValue(v);
        au.L->EraseField(a->GetPath(), SdfFieldKeys->TimeSamples);
        au.written.insert(a->GetPath());
    };
    put(UsdGeomTokens->positions, SdfValueTypeNames->Point3fArray, VtValue(positions));
    if (turned)
        put(UsdGeomTokens->orientations, SdfValueTypeNames->QuathArray, VtValue(orientations));
    if (scaled)
        put(UsdGeomTokens->scales, SdfValueTypeNames->Float3Array, VtValue(scales));
}

void pruneEmpty(const SdfLayerHandle& L, SdfPath p);

/** Point instances hidden on their instancer by id (or index, when it has no ids): deleted or taken out in Lila, or shown again by an undo. */
void setHidden(Author& au, const SdfPath& instancer, const std::set<size_t>& hide, const std::set<size_t>& show)
{
    const UsdGeomPointInstancer pi(au.s.stage->GetPrimAtPath(instancer));
    if (!pi || (hide.empty() && show.empty()))
        return;
    VtInt64Array ids, hidden;
    pi.GetIdsAttr().Get(&ids, UsdTimeCode::EarliestTime());
    pi.GetInvisibleIdsAttr().Get(&hidden, UsdTimeCode::EarliestTime());
    auto idOf = [&](size_t i) { return i < ids.size() ? ids[i] : int64_t(i); };
    VtInt64Array kept;
    for (int64_t id : hidden)
        if (std::none_of(show.begin(), show.end(), [&](size_t i) { return idOf(i) == id; }))
            kept.push_back(id);
    hidden = kept;
    for (size_t i : hide)
        if (std::find(hidden.begin(), hidden.end(), idOf(i)) == hidden.end())
            hidden.push_back(idOf(i));
    // Hiding what the weaker layers hide anyway takes the edit layer's opinion out.
    std::optional<std::set<int64_t>> weaker;
    for (const SdfPropertySpecHandle& spec : pi.GetInvisibleIdsAttr().GetPropertyStack(UsdTimeCode::EarliestTime()))
        if (spec->GetLayer() != au.L && spec->HasDefaultValue())
        {
            const VtValue v = spec->GetDefaultValue();
            weaker = v.IsHolding<VtInt64Array>() ? std::set<int64_t>(v.UncheckedGet<VtInt64Array>().begin(), v.UncheckedGet<VtInt64Array>().end())
                                                 : std::set<int64_t>();
            break;
        }
    if (std::set<int64_t>(hidden.begin(), hidden.end()) == weaker.value_or(std::set<int64_t>()))
    {
        const SdfPath attr = instancer.AppendProperty(UsdGeomTokens->invisibleIds);
        if (const SdfPrimSpecHandle ps = au.L->GetPrimAtPath(instancer))
            if (const SdfAttributeSpecHandle a = au.L->GetAttributeAtPath(attr))
                ps->RemoveProperty(a);
        pruneEmpty(au.L, instancer);
        return;
    }
    SdfAttributeSpecHandle a = au.attrSpec(instancer, UsdGeomTokens->invisibleIds.GetString(), SdfValueTypeNames->Int64Array, SdfVariabilityVarying);
    a->SetDefaultValue(VtValue(hidden));
    au.L->EraseField(a->GetPath(), SdfFieldKeys->TimeSamples);
}

/** A copy's baseline key: the point instance it sits in and its prototype prim. Empty when it is in none. */
std::string copyKey(const Stage& s, const Graph& g, NodeId n)
{
    const auto proto = s.copyOf.find(n);
    if (proto == s.copyOf.end())
        return {};
    for (NodeId p = g.parent(n); p != kNoNode; p = g.parent(p))
        if (auto it = s.instanceOf.find(p); it != s.instanceOf.end())
            return std::format("$copy:{}[{}]{}", it->second.first.GetString(), it->second.second, proto->second.GetString());
    return {};
}

std::string instanceKey(const std::pair<SdfPath, size_t>& inst) { return std::format("$inst:{}[{}]", inst.first.GetString(), inst.second); }

/** Hash of a mesh prim's composed points and topology, to tell whether the stage still holds what Lila wrote. */
uint64_t composedShape(const Stage& s, const SdfPath& path)
{
    const UsdGeomMesh mesh(s.stage->GetPrimAtPath(path));
    if (!mesh)
        return 0;
    const UsdTimeCode rest = s.stage->HasAuthoredTimeCodeRange() ? UsdTimeCode(s.stage->GetStartTimeCode()) : UsdTimeCode::EarliestTime();
    Hasher h;
    for (const UsdAttribute& a : {mesh.GetPointsAttr(), mesh.GetFaceVertexCountsAttr(), mesh.GetFaceVertexIndicesAttr()})
    {
        VtValue v;
        a.Get(&v, rest);
        h.value(v);
    }
    return h.h;
}

/** The prim nearest above p that has a node, or the absolute root. */
SdfPath linkedAbove(const Stage& s, const SdfPath& p)
{
    SdfPath q = p.GetParentPath();
    while (!q.IsEmpty() && !q.IsAbsoluteRootPath() && !s.nodeOf.count(q))
        q = q.GetParentPath();
    return q.IsEmpty() ? SdfPath::AbsoluteRootPath() : q;
}

/** The prim of the nearest linked node above n (a node a prim owns counts as that prim), or the absolute root. */
SdfPath linkedParentPrim(const Stage& s, const Graph& g, NodeId n)
{
    for (NodeId p = g.parent(n); p != kNoNode && p != s.node; p = g.parent(p))
    {
        if (auto it = s.primOf.find(p); it != s.primOf.end())
            return it->second;
        // What hangs off a joint is written beside the skeleton, as newParentPrim places it.
        if (auto it = s.partOf.find(p); it != s.partOf.end())
            return s.jointOf.count(p) && !s.nodeOf.count(it->second) ? it->second.GetParentPath() : it->second;
    }
    return SdfPath::AbsoluteRootPath();
}

/** An edited sublayer sits below the source's own opinions, which win where both have one: the prims where they do. */
std::vector<std::string> overridden(const Stage& s, const std::set<SdfPath>& written)
{
    std::set<SdfPath> lost;
    if (s.mode == Stage::Mode::Sublayer)
        for (const SdfPath& p : written)
        {
            if (p.ContainsPrimVariantSelection())
                continue;
            const UsdPrim prim = s.stage->GetPrimAtPath(p.GetPrimPath());
            const UsdProperty prop = prim ? prim.GetProperty(p.GetNameToken()) : UsdProperty();
            const SdfPropertySpecHandleVector stack = prop ? prop.GetPropertyStack() : SdfPropertySpecHandleVector();
            if (!stack.empty() && get_pointer(stack.front()->GetLayer()) != get_pointer(s.edit))
                lost.insert(p.GetPrimPath());
        }
    std::vector<std::string> out;
    for (const SdfPath& p : lost)
        out.push_back(std::format("{}: a stronger layer than the edit layer overrides its edits", p.GetString()));
    return out;
}

/**
 * The prim a new object under parent is written under: its parent's prim; for a copy of a point instance, the
 * prototype (every instance gets it); for a joint, the Skeleton prim when the skeleton has a node of its own, else the
 * prim the skeleton sits in, where Lila's writer puts it beside the skeleton following the joint. Empty when none fits.
 */
SdfPath newParentPrim(Stage& s, Graph& g, NodeId parent, std::vector<std::string>& warn)
{
    if (parent == s.node)
        return SdfPath::AbsoluteRootPath();
    if (auto it = s.primOf.find(parent); it != s.primOf.end())
        return it->second;
    if (auto c = s.copyOf.find(parent); c != s.copyOf.end())
    {
        warn.push_back(std::format("{}: added to the prototype {}, so every instance of it gets it", g.node(parent)->name(), c->second.GetString()));
        if (const auto owner = s.partOf.find(parent); owner != s.partOf.end())
            s.changed.insert(owner->second);
        return c->second;
    }
    const Node* p = g.node(parent);
    const auto skel = s.partOf.find(parent);
    if (!p || !p->type().isA("joint") || skel == s.partOf.end() || !s.stage->GetPrimAtPath(skel->second).IsA<UsdSkelSkeleton>())
        return {};
    return s.nodeOf.count(skel->second) ? skel->second : skel->second.GetParentPath();
}

/** A new object's written prim copied in under parentPrim with everything below it, and linked. */
/** A Skeleton prim written into the stage at st for the written one at e: its joints become its parts, named as written. */
void adoptSkeleton(Stage& s, Author& au, const SdfPath& e, const SdfPath& st)
{
    const VtTokenArray tokens = defaultOf<VtTokenArray>(au.now.layer, e.AppendProperty(UsdSkelTokens->joints));
    const std::vector<NodeId>& nodes = au.now.skelJoints[e];
    for (size_t k = 0; k < nodes.size() && k < tokens.size(); ++k)
    {
        s.partOf[nodes[k]] = st;
        s.jointOf[nodes[k]] = tokens[k];
        s.deletedParts.erase(nodes[k]);
    }
    au.skelPaths[e] = st;
    au.skelTokens[e] = tokens;
    au.skelOrder[st] = tokens;
    s.baseline["$skel:" + st.GetString()] = skelGroups(s, au.now, e);
}

/** Skinning needs a SkelRoot above a skeleton and the meshes bound to it: their closest common Xform becomes one, unless one is there. */
void ensureSkelRoot(Stage& s, Author& au, const SdfPath& e, const SdfPath& st)
{
    SdfPath common = st.GetParentPath();
    for (const auto& [n, em] : au.now.prim)
    {
        const SdfPathVector t = targetsOf(au.now.layer, em.AppendProperty(UsdSkelTokens->skelSkeleton));
        if (auto p = s.primOf.find(n); p != s.primOf.end() && !t.empty() && t.front() == e && !p->second.ContainsPrimVariantSelection())
            common = common.GetCommonPrefix(p->second.GetParentPath());
    }
    for (SdfPath q = common; !q.IsEmpty() && !q.IsAbsoluteRootPath(); q = q.GetParentPath())
        if (const UsdPrim p = s.stage->GetPrimAtPath(q); p && p.GetTypeName() == "SkelRoot")
            return;
    for (SdfPath q = common; !q.IsEmpty() && !q.IsAbsoluteRootPath(); q = q.GetParentPath())
    {
        const UsdPrim p = s.stage->GetPrimAtPath(q);
        const SdfPrimSpecHandle ours = au.L->GetPrimAtPath(q);
        const TfToken type = p ? p.GetTypeName() : ours ? ours->GetTypeName() : TfToken();
        if (type == "Xform" || type.IsEmpty())
        {
            au.over(q)->SetTypeName("SkelRoot");
            return;
        }
    }
}

/**
 * Skeletons Lila made whose joints hang off an object the stage already has: each written as a Skeleton prim (with
 * its SkelAnimation) under that object's prim, under a SkelRoot. Ones under new objects are written with them.
 */
void writeNewSkeletons(Stage& s, Author& au)
{
    for (const auto& [e, joints] : au.now.skelJoints)
    {
        if (joints.empty() || au.skelPaths.count(e) || au.now.skelStage.count(e))
            continue;
        const SdfPath anchor = e.GetParentPath();
        SdfPath parent;
        if (auto n = au.now.nodeAt.find(anchor); n != au.now.nodeAt.end())
        {
            const auto p = s.primOf.find(n->second);
            if (p == s.primOf.end() || p->second.ContainsPrimVariantSelection())
                continue;
            parent = p->second;
        }
        else if (const UsdPrim dp = s.stage->GetDefaultPrim(); anchor == au.now.root && dp)
            parent = dp.GetPath();
        else
            continue;
        const SdfPath dst = au.uniqueChild(parent, e.GetName());
        au.over(parent);
        SdfCopySpec(au.now.layer, e, au.L, dst);
        au.skelPaths[e] = dst;
        au.retarget(dst);
        adoptSkeleton(s, au, e, dst);
        ensureSkelRoot(s, au, e, dst);
    }
}

/**
 * A new object's written prim copied in under parentPrim and linked. Everything below it comes too, unless alone:
 * then only what no object below it owns (its subsets, blend shapes), and the objects below are added on their own.
 */
void addPrim(Stage& s, Author& au, NodeId n, const SdfPath& e, const SdfPath& parentPrim, Graph& g, bool alone = false)
{
    const SdfPath dst = au.uniqueChild(parentPrim, identifier(g.node(n)->name(), "node"));
    if (!parentPrim.IsAbsoluteRootPath())
        au.over(parentPrim);
    SdfCopySpec(au.now.layer, e, au.L, dst);
    if (alone)
    {
        if (const SdfPrimSpecHandle spec = au.L->GetPrimAtPath(dst))
            for (const SdfPrimSpecHandle& c : std::vector<SdfPrimSpecHandle>(spec->GetNameChildren().begin(), spec->GetNameChildren().end()))
            {
                const SdfPath ec = e.AppendChild(c->GetNameToken());
                const bool owned = std::any_of(au.now.prim.begin(), au.now.prim.end(), [&](const auto& m) { return m.second.HasPrefix(ec); }) ||
                                   std::any_of(au.now.skelJoints.begin(), au.now.skelJoints.end(), [&](const auto& k) { return k.first.HasPrefix(ec); });
                if (owned)
                    spec->RemoveNameChild(c);
            }
        au.retarget(dst);
        s.link(n, dst);
        s.baseline[dst.GetString()] = au.now.hashes[n];
        return;
    }
    std::vector<SdfPath> skeletons;
    for (const auto& [sk, joints] : au.now.skelJoints)
        if (sk.HasPrefix(e) && !au.skelPaths.count(sk))
        {
            au.skelPaths[sk] = sk.ReplacePrefix(e, dst);
            skeletons.push_back(sk);
        }
    au.retarget(dst);
    for (const auto& [m, em] : au.now.prim)
        if (em.HasPrefix(e))
        {
            const SdfPath pm = em.ReplacePrefix(e, dst);
            s.link(m, pm);
            s.baseline[pm.GetString()] = au.now.hashes[m];
            // Bound to a skeleton the stage orders differently: its indices name the written skeleton's joints.
            const SdfPathVector t = targetsOf(au.now.layer, em.AppendProperty(UsdSkelTokens->skelSkeleton));
            if (auto tokens = t.empty() ? au.skelTokens.end() : au.skelTokens.find(t.front()); tokens != au.skelTokens.end())
                if (tokens->second != au.skelOrder[au.skelPaths[t.front()]])
                    au.uniformTokens(pm, UsdSkelTokens->skelJoints.GetString(), tokens->second);
        }
    for (NodeId d : g.descendants(n))
        if (!s.primOf.count(d))
            s.partOf[d] = dst;
    for (const SdfPath& sk : skeletons)
        adoptSkeleton(s, au, sk, au.skelPaths[sk]);
}

/**
 * The prim an object below the stage holds its children in: its own, or for a new object one written now, alone
 * (after any new objects above it), so an object moved under it can follow. Empty when it can't have one.
 */
SdfPath ensurePrim(Stage& s, Graph& g, Author& au, NodeId x)
{
    if (x == s.node)
        return SdfPath::AbsoluteRootPath();
    if (auto it = s.primOf.find(x); it != s.primOf.end())
        return it->second;
    const Node* node = g.node(x);
    if (!node || !node->type().isA("transform") || node->type().isA("joint") || isStage(node) || s.partOf.count(x) || s.deleted.count(x) ||
        !under(g, x, s.node))
        return {};
    const SdfPath parent = ensurePrim(s, g, au, g.parent(x));
    const auto e = au.now.prim.find(x);
    if (parent.IsEmpty() || parent.ContainsPrimVariantSelection() || e == au.now.prim.end())
        return {};
    if (!parent.IsAbsoluteRootPath())
        au.openInstances(parent, true);
    addPrim(s, au, x, e->second, parent, g, true);
    return s.primOf[x];
}

/**
 * Takes a point instance out of its instancer so an object added under it can be written: hidden there, and in its
 * place an Xform beside the instancer referencing its prototype. In Lila its holder moves to the instancer's parent,
 * linked to the Xform, and its copies link to the prims under the reference. Returns the Xform's path, or empty.
 */
SdfPath extract(Stage& s, Graph& g, Author& au, NodeId holder, std::map<SdfPath, std::set<size_t>>& hide)
{
    const auto inst = s.instanceOf.find(holder);
    if (inst == s.instanceOf.end())
        return {};
    const auto [instancer, index] = inst->second;
    const UsdGeomPointInstancer pi(s.stage->GetPrimAtPath(instancer));
    const auto insNode = s.nodeOf.find(instancer);
    if (!pi || pi.GetPrim().IsInstanceProxy() || instancer.ContainsPrimVariantSelection() || insNode == s.nodeOf.end())
        return {};
    SdfPathVector protos;
    pi.GetPrototypesRel().GetTargets(&protos);
    const UsdTimeCode rest = s.stage->HasAuthoredTimeCodeRange() ? UsdTimeCode(s.stage->GetStartTimeCode()) : UsdTimeCode::EarliestTime();
    VtIntArray indices;
    pi.GetProtoIndicesAttr().Get(&indices, rest);
    if (index >= indices.size() || indices[index] < 0 || size_t(indices[index]) >= protos.size())
        return {};
    const SdfPath proto = protos[size_t(indices[index])];

    // Baseline keys go by the instance, so they are read before the holder leaves it.
    std::vector<std::tuple<NodeId, SdfPath, std::string>> copies;
    for (NodeId d : g.descendants(holder))
        if (auto c = s.copyOf.find(d); c != s.copyOf.end() && c->second.HasPrefix(proto))
            copies.push_back({d, c->second, copyKey(s, g, d)});
    s.baseline.erase(instanceKey(inst->second));
    hide[instancer].insert(index);

    g.setParent(holder, g.parent(insNode->second), true);
    const SdfPath dst = au.uniqueChild(instancer.GetParentPath(), identifier(g.node(holder)->name(), "instance"));
    s.link(holder, dst);
    const size_t exported = s.lastExported, partial = s.partialDiffs;
    Snapshot alone = snapshot(s, g, au.warn, Plan{false, {holder}, {}});
    s.lastExported = exported;
    s.partialDiffs = partial;
    if (!alone.ok || !alone.prim.count(holder))
        return {};
    if (!dst.GetParentPath().IsAbsoluteRootPath())
        au.over(dst.GetParentPath());
    SdfCopySpec(alone.layer, alone.prim[holder], au.L, dst);
    s.baseline[dst.GetString()] = alone.hashes[holder];
    const SdfPrimSpecHandle ref = SdfPrimSpec::New(au.L->GetPrimAtPath(dst), proto.GetName(), SdfSpecifierDef);
    ref->GetReferenceList().Prepend(SdfReference(std::string(), proto));

    const SdfPath root = dst.AppendChild(proto.GetNameToken());
    for (const auto& [d, path, key] : copies)
    {
        const SdfPath p = path.ReplacePrefix(proto, root);
        Groups b = key.empty() ? Groups() : s.baseline[key];
        s.baseline.erase(key);
        s.link(d, p);
        s.baseline[kFrom + p.GetString()] = b;
        s.baseline[p.GetString()] = std::move(b);
    }
    s.baseline[kExtract + dst.GetString() + "|" + instancer.GetString()] = {{"index", index}};
    s.baseline[kFrom + dst.GetString()] = s.baseline[dst.GetString()];
    au.warn.push_back(std::format("{}: taken out of {} as an Xform referencing its prototype, so what was added under it could be written",
                                  g.node(holder)->name(), instancer.GetString()));
    return dst;
}

/** Removes an over that holds nothing any more, and the empty overs above it. */
void pruneEmpty(const SdfLayerHandle& L, SdfPath p)
{
    static const std::set<TfToken> kStructure{SdfFieldKeys->Specifier, SdfChildrenKeys->PrimChildren, SdfChildrenKeys->PropertyChildren};
    while (!p.IsEmpty() && !p.IsAbsoluteRootPath())
    {
        const SdfPrimSpecHandle spec = L->GetPrimAtPath(p);
        if (!spec || spec->GetSpecifier() != SdfSpecifierOver || !spec->GetNameChildren().empty() || !spec->GetProperties().empty())
            return;
        for (const TfToken& f : L->ListFields(p))
            if (!kStructure.count(f))
                return;
        const SdfPrimSpecHandle parent = p.GetParentPath().IsAbsoluteRootPath() ? L->GetPseudoRoot() : L->GetPrimAtPath(p.GetParentPath());
        if (!parent)
            return;
        parent->RemoveNameChild(spec);
        p = p.GetParentPath();
    }
}

/**
 * Instances opened for edits whose edits have all been undone since: every prim inside back at its baseline from before
 * the opening, none added or deleted. Their opinions come out of the edit layer and they are instanced again.
 */
void closeInstances(Stage& s, const SdfLayerHandle& L)
{
    std::vector<SdfPath> opened;
    for (auto it = s.baseline.lower_bound(kOpen); it != s.baseline.end() && it->first.starts_with(kOpen); ++it)
        opened.push_back(SdfPath(it->first.substr(kOpen.size())));
    for (const SdfPath& q : opened)
    {
        std::vector<std::string> origins;
        const std::string from = kOrigin + q.GetString();
        for (auto it = s.baseline.lower_bound(from); it != s.baseline.end() && it->first.starts_with(from); ++it)
            if (insideOf(SdfPath(it->first.substr(kOrigin.size())), q))
                origins.push_back(it->first);
        size_t linked = 0;
        bool same = true;
        for (const auto& [lp, n] : s.nodeOf)
        {
            if (!insideOf(lp, q))
                continue;
            ++linked;
            const auto o = s.baseline.find(kOrigin + lp.GetString());
            const auto b = s.baseline.find(lp.GetString());
            same = same && o != s.baseline.end() && o->second == (b == s.baseline.end() ? Groups() : b->second);
        }
        const SdfPrimSpecHandle spec = L->GetPrimAtPath(q);
        if (spec && (!same || linked != origins.size()))
            continue;
        if (spec)
        {
            std::vector<SdfPrimSpecHandle> inside;
            for (const SdfPrimSpecHandle& c : spec->GetNameChildren())
                inside.push_back(c);
            for (const SdfPrimSpecHandle& c : inside)
                spec->RemoveNameChild(c);
            const Groups& mark = s.baseline[kOpen + q.GetString()];
            if (auto was = mark.find("was"); was != mark.end())
                spec->SetInstanceable(was->second != 0);
            else
                spec->ClearInstanceable();
            pruneEmpty(L, q);
        }
        s.baseline.erase(kOpen + q.GetString());
        for (const std::string& k : origins)
            s.baseline.erase(k);
    }
}

void removeSpec(const SdfLayerHandle& L, const SdfPath& p)
{
    const SdfPrimSpecHandle spec = L->GetPrimAtPath(p);
    const SdfPrimSpecHandle parent = p.GetParentPath().IsAbsoluteRootPath() ? L->GetPseudoRoot() : L->GetPrimAtPath(p.GetParentPath());
    if (!spec || !parent)
        return;
    parent->RemoveNameChild(spec);
    pruneEmpty(L, p.GetParentPath());
}

/** Every opinion of the spec at from laid over the spec at to, children too; from's own specifier, activation and arcs stay behind. */
void mergeSpec(const SdfLayerHandle& L, const SdfPath& from, const SdfPath& to)
{
    static const std::set<TfToken> kSkip{SdfFieldKeys->Specifier,   SdfFieldKeys->Active,          SdfFieldKeys->References,
                                         SdfFieldKeys->CustomData,  SdfFieldKeys->TypeName,        SdfChildrenKeys->PrimChildren,
                                         SdfChildrenKeys->PropertyChildren, SdfChildrenKeys->VariantSetChildren};
    const SdfPrimSpecHandle src = L->GetPrimAtPath(from);
    if (!src)
        return;
    if (!L->GetPrimAtPath(to))
        SdfCreatePrimInLayer(L, to);
    for (const TfToken& f : L->ListFields(from))
        if (!kSkip.count(f))
            L->SetField(to, f, L->GetField(from, f));
    for (const SdfPropertySpecHandle& p : src->GetProperties())
        SdfCopySpec(L, p->GetPath(), L, to.AppendProperty(p->GetNameToken()));
    for (const SdfPrimSpecHandle& c : src->GetNameChildren())
    {
        const SdfPath dst = to.AppendChild(c->GetNameToken());
        if (L->GetPrimAtPath(dst))
            mergeSpec(L, c->GetPath(), dst);
        else
            SdfCopySpec(L, c->GetPath(), L, dst);
    }
}

/** Rewrites every link, part and baseline key under from to lie under to, once its prim has moved there. */
void movePaths(Stage& s, const SdfPath& from, const SdfPath& to)
{
    auto mv = [&](const SdfPath& p) { return p.HasPrefix(from) ? p.ReplacePrefix(from, to) : p; };
    std::vector<std::pair<NodeId, SdfPath>> links;
    for (const auto& [n, p] : s.primOf)
        if (p.HasPrefix(from))
            links.push_back({n, p});
    for (const auto& [n, p] : links)
        s.nodeOf.erase(p);
    for (const auto& [n, p] : links)
    {
        s.primOf[n] = mv(p);
        s.nodeOf[mv(p)] = n;
    }
    for (auto& [n, p] : s.partOf)
        p = mv(p);
    for (auto& [n, p] : s.copyOf)
        p = mv(p);
    for (auto& [n, inst] : s.instanceOf)
        inst.first = mv(inst.first);
    for (auto& [n, p] : s.deleted)
        p = mv(p);
    for (auto& [n, part] : s.deletedParts)
    {
        part.owner = mv(part.owner);
        if (part.copy)
            part.copy = mv(*part.copy);
        if (part.instance)
            part.instance->first = mv(part.instance->first);
    }
    for (auto& [slot, p] : s.materialOrigin)
        p = mv(p);
    // Keys hold paths after their kind ("$copy:"), and after the separators of the paths before them.
    const std::string f = from.GetString(), t = to.GetString();
    Baseline moved;
    for (auto& [key, groups] : s.baseline)
    {
        std::string k = key;
        for (size_t at = k.find(f); at != std::string::npos; at = k.find(f, at + 1))
        {
            const bool starts = at == 0 || std::string_view(":|]").find(k[at - 1]) != std::string_view::npos;
            const bool ends = at + f.size() == k.size() || std::string_view("/{[|.").find(k[at + f.size()]) != std::string_view::npos;
            if (starts && ends)
            {
                k.replace(at, f.size(), t);
                at += t.size() - 1;
            }
        }
        moved[k] = std::move(groups);
    }
    s.baseline = std::move(moved);
}

const char* const kMovedFrom = "lilaMovedFrom";

/**
 * An object moved to another parent in Lila: its prim moves there in the edit layer. A prim only the edit layer makes
 * moves as it is; any other gets a new prim referencing it, with itself deactivated, and moving that back puts the
 * edits made since onto the original. Returns the prim's new path, or empty when its new parent has no prim to hold it.
 */
SdfPath reparent(Stage& s, Graph& g, Author& au, NodeId n, const SdfPath& q)
{
    const NodeId parent = g.parent(n);
    SdfPath to;
    if (parent == s.node)
        to = SdfPath::AbsoluteRootPath();
    else if (auto it = s.primOf.find(parent); it != s.primOf.end())
        to = it->second;
    else if (auto j = s.partOf.find(parent); j != s.partOf.end() && s.jointOf.count(parent))
        to = s.nodeOf.count(j->second) ? j->second : j->second.GetParentPath();
    else
        to = ensurePrim(s, g, au, parent);
    const UsdPrim cp = s.stage->GetPrimAtPath(q);
    if (to.IsEmpty() || to.ContainsPrimVariantSelection() || q.ContainsPrimVariantSelection() || to.HasPrefix(q) || !cp)
        return {};
    const SdfLayerHandle& L = au.L;
    SdfPath origin;
    if (const SdfPrimSpecHandle spec = L->GetPrimAtPath(q); spec && spec->GetSpecifier() == SdfSpecifierDef)
    {
        const VtDictionary data = spec->GetCustomData();
        if (auto it = data.find(kMovedFrom); it != data.end() && it->second.IsHolding<std::string>())
            origin = SdfPath(it->second.UncheckedGet<std::string>());
    }
    if (!to.IsAbsoluteRootPath())
        au.openInstances(to, true);
    if (cp.IsInstanceProxy())
        au.openInstances(q, false);
    bool own = L->GetPrimAtPath(q) && L->GetPrimAtPath(q)->GetSpecifier() == SdfSpecifierDef;
    for (const SdfPrimSpecHandle& spec : cp.GetPrimStack())
        own = own && ((spec->GetLayer() == L && spec->GetPath() == q) || (!origin.IsEmpty() && spec->GetPath() == origin));

    SdfPath dst;
    if (!origin.IsEmpty() && origin.GetParentPath() == to)
    {
        mergeSpec(L, q, origin);
        if (const SdfPrimSpecHandle o = L->GetPrimAtPath(origin); o && o->HasActive())
            o->ClearActive();
        removeSpec(L, q);
        pruneEmpty(L, origin);
        dst = origin;
    }
    else if (own)
    {
        dst = au.uniqueChild(to, q.GetName());
        if (!to.IsAbsoluteRootPath())
            au.over(to);
        SdfCopySpec(L, q, L, dst);
        removeSpec(L, q);
    }
    else
    {
        dst = au.uniqueChild(to, q.GetName());
        const SdfPrimSpecHandle under = to.IsAbsoluteRootPath() ? L->GetPseudoRoot() : au.over(to);
        const SdfPrimSpecHandle moved = SdfPrimSpec::New(under, dst.GetName(), SdfSpecifierDef);
        // Stronger than the inactive original it references.
        moved->SetActive(true);
        moved->GetReferenceList().Prepend(SdfReference(std::string(), q));
        moved->SetCustomData(kMovedFrom, VtValue(q.GetString()));
        au.over(q)->SetActive(false);
    }
    movePaths(s, q, dst);
    return dst;
}

/**
 * Point instances taken out of their instancer that again hold only their prototype, unchanged since: the Xform beside
 * the instancer goes from the edit layer, the instance shows on its instancer again, and its holder goes back under
 * the instancer node as a point instance. Returns the holders put back.
 */
std::vector<NodeId> mergeBack(Stage& s, Graph& g, Author& au, std::map<SdfPath, std::set<size_t>>& show)
{
    std::vector<NodeId> back;
    std::vector<std::string> keys;
    for (auto it = s.baseline.lower_bound(kExtract); it != s.baseline.end() && it->first.starts_with(kExtract); ++it)
        keys.push_back(it->first);
    auto current = [&](NodeId n, const SdfPath& p) {
        if (auto c = au.now.hashes.find(n); c != au.now.hashes.end())
            return c->second;
        auto b = s.baseline.find(p.GetString());
        return b == s.baseline.end() ? Groups() : b->second;
    };
    auto unchanged = [&](NodeId n, const SdfPath& p) {
        auto o = s.baseline.find(kFrom + p.GetString());
        return o != s.baseline.end() && o->second == current(n, p);
    };
    for (const std::string& key : keys)
    {
        const std::string rest = key.substr(kExtract.size());
        const size_t bar = rest.find('|');
        if (bar == std::string::npos || !SdfPath::IsValidPathString(rest.substr(0, bar)) || !SdfPath::IsValidPathString(rest.substr(bar + 1)))
            continue;
        const SdfPath dst(rest.substr(0, bar)), instancer(rest.substr(bar + 1));
        const size_t index = size_t(groupHash(s.baseline[key], "index"));
        const auto h = s.nodeOf.find(dst);
        const auto ins = s.nodeOf.find(instancer);
        if (h == s.nodeOf.end() || ins == s.nodeOf.end() || !g.node(h->second) || !g.node(ins->second) || !unchanged(h->second, dst))
            continue;
        const NodeId holder = h->second;
        std::vector<std::pair<NodeId, SdfPath>> copies;
        bool same = true;
        for (NodeId d : g.descendants(holder))
        {
            const Node* dn = g.node(d);
            if (!dn || !dn->type().isA("transform"))
                continue;
            const auto p = s.primOf.find(d);
            same = p != s.primOf.end() && insideOf(p->second, dst) && unchanged(d, p->second);
            if (!same)
                break;
            copies.push_back({d, p->second});
        }
        const UsdGeomPointInstancer pi(s.stage->GetPrimAtPath(instancer));
        if (!same || !pi)
            continue;
        SdfPathVector protos;
        pi.GetPrototypesRel().GetTargets(&protos);
        VtIntArray indices;
        pi.GetProtoIndicesAttr().Get(&indices, s.stage->HasAuthoredTimeCodeRange() ? UsdTimeCode(s.stage->GetStartTimeCode()) : UsdTimeCode::EarliestTime());
        if (index >= indices.size() || indices[index] < 0 || size_t(indices[index]) >= protos.size())
            continue;
        const SdfPath proto = protos[size_t(indices[index])];
        const SdfPath root = dst.AppendChild(proto.GetNameToken());

        removeSpec(au.L, dst);
        show[instancer].insert(index);
        g.setParent(holder, ins->second, true);
        s.unlink(holder);
        s.partOf[holder] = instancer;
        s.instanceOf[holder] = {instancer, index};
        s.baseline.erase(dst.GetString());
        s.baseline.erase(kFrom + dst.GetString());
        au.now.prim.erase(holder);
        for (const auto& [d, p] : copies)
        {
            s.unlink(d);
            s.partOf[d] = instancer;
            s.copyOf[d] = p.ReplacePrefix(root, proto);
            s.baseline.erase(p.GetString());
            s.baseline.erase(kFrom + p.GetString());
            au.now.prim.erase(d);
        }
        s.baseline.erase(key);
        for (auto it = s.deleted.begin(); it != s.deleted.end();)
            it = it->second.HasPrefix(dst) ? s.deleted.erase(it) : std::next(it);
        back.push_back(holder);
    }
    return back;
}

/** Baselines of point instances put back on their instancer, from their nodes as they now sit under it. */
void rebaselineInstances(Stage& s, Graph& g, const std::vector<NodeId>& holders, std::vector<std::string>& warn)
{
    const size_t exported = s.lastExported, partial = s.partialDiffs;
    const Snapshot again = snapshot(s, g, warn, Plan{false, {}, holders});
    s.lastExported = exported;
    s.partialDiffs = partial;
    for (NodeId h : holders)
    {
        const auto inst = s.instanceOf.find(h);
        if (inst == s.instanceOf.end())
            continue;
        if (auto hh = again.hashes.find(h); hh != again.hashes.end())
            s.baseline[instanceKey(inst->second)]["xform"] = groupHash(hh->second, "xform");
        for (NodeId d : g.descendants(h))
            if (auto c = again.hashes.find(d); c != again.hashes.end())
                if (const std::string key = copyKey(s, g, d); !key.empty())
                    s.baseline[key] = c->second;
    }
}

/** Changed property groups of one linked object written over its prim at p (a prototype prim, for a copy). */
void authorChanges(Stage& s, Graph& g, Author& au, NodeId n, const SdfPath& e, const SdfPath& p, Groups& base)
{
    // A prim moved this comparison has only its spec in the edit layer until the change block ends.
    const UsdPrim cp = s.stage->GetPrimAtPath(stagePath(p));
    if (!cp && !au.L->GetPrimAtPath(p))
        return;
    const Groups& cur = au.now.hashes[n];
    std::set<std::string> groups;
    for (const auto& [k, v] : cur)
        groups.insert(k);
    for (const auto& [k, v] : base)
        groups.insert(k);
    for (const std::string& group : groups)
    {
        const uint64_t h = groupHash(cur, group);
        if (h == groupHash(base, group) && !(group == "skin" && au.forceSkin.count(p)))
            continue;
        if (cp && cp.IsInstanceProxy())
            au.openInstances(p, false);
        au.author(e, p, group);
        if (h)
            base[group] = h;
        else
            base.erase(group);
    }
}

/** Writes what changed in Lila since the baseline as overs in the edit layer. Returns the warnings. */
std::vector<std::string> sync(Stage& s, Graph& g)
{
    LILA_ZONE();

    std::vector<std::string> warn;
    Diagnostics diag;
    const Plan plan = planSync(s, g);
    Snapshot now = plan.empty() ? Snapshot() : snapshot(s, g, warn, plan);
    if (plan.empty())
        s.lastExported = 0;
    const Axes axes = makeAxes(UsdGeomGetStageUpAxis(s.stage) != UsdGeomTokens->y, UsdGeomGetStageMetersPerUnit(s.stage));
    Busy busy(s);
    std::set<SdfPath> written, shapes;
    Author au{s, now, warn};
    // Prims inside LOD levels are written with their level selected, after this block: deletes, edits and additions.
    struct LevelItem
    {
        enum Kind
        {
            Delete,
            Edit,
            Add,
        } kind;
        NodeId n;
        SdfPath p;
    };
    std::map<SdfPath, std::vector<LevelItem>> inLevels;
    // Point instances to hide and show on each instancer, written once all are known.
    std::map<SdfPath, std::set<size_t>> hide, show;
    // Point instances getting new objects come out of their instancer first: the holder's own export needs a stage that
    // composes as it goes, which it doesn't inside a change block. Their snapshot prims are from before the move.
    for (NodeId n : g.descendants(s.node))
    {
        const Node* node = g.node(n);
        const NodeId holder = g.parent(n);
        if (!node || !node->type().isA("transform") || node->type().isA("joint") || s.primOf.count(n) || s.partOf.count(n) || s.deleted.count(n) ||
            s.deletedParts.count(n) || !s.instanceOf.count(holder) || !now.prim.count(n))
            continue;
        if (!extract(s, g, au, holder, hide).IsEmpty())
        {
            now.prim.erase(holder);
            now.hashes.erase(holder);
        }
    }
    std::vector<NodeId> merged;
    {
        SdfChangeBlock block;

        // Deleted objects deactivate their prims, and so do objects moved out of the stage.
        std::set<NodeId> skip;
        std::vector<std::pair<SdfPath, NodeId>> gone;
        for (const auto& [n, p] : s.primOf)
            if (!g.node(n) || !under(g, n, s.node))
                gone.push_back({p, n});
        std::sort(gone.begin(), gone.end());
        std::vector<SdfPath> doneGone;
        for (const auto& [p, n] : gone)
        {
            const bool covered = std::any_of(doneGone.begin(), doneGone.end(), [&](const SdfPath& d) { return p.HasPrefix(d); });
            if (!covered)
            {
                if (p.ContainsPrimVariantSelection())
                    inLevels[variantPrefix(p)].push_back({LevelItem::Delete, n, p});
                else
                    au.deactivate(p);
                doneGone.push_back(p);
            }
            s.unlink(n);
            s.deleted[n] = p;
        }
        std::set<SdfPath> partsGone;
        std::map<SdfPath, std::set<SdfPath>> copiesGone;
        for (auto it = s.partOf.begin(); it != s.partOf.end();)
        {
            if (!g.node(it->first))
            {
                const bool covered = std::any_of(doneGone.begin(), doneGone.end(), [&](const SdfPath& d) { return it->second.HasPrefix(d); });
                Stage::Part& part = s.deletedParts[it->first];
                part.owner = it->second;
                if (auto inst = s.instanceOf.find(it->first); inst != s.instanceOf.end())
                {
                    if (!covered)
                        hide[inst->second.first].insert(inst->second.second);
                    part.instance = inst->second;
                    s.instanceOf.erase(inst);
                }
                else if (auto c = s.copyOf.find(it->first); c != s.copyOf.end())
                {
                    if (!covered)
                        copiesGone[it->second].insert(c->second);
                    part.copy = c->second;
                }
                else if (auto j = s.jointOf.find(it->first); j != s.jointOf.end())
                {
                    part.joint = j->second;
                    VtTokenArray had;
                    if (const UsdSkelSkeleton sk{s.stage->GetPrimAtPath(it->second)})
                        sk.GetJointsAttr().Get(&had);
                    if (const auto at = std::find(had.begin(), had.end(), j->second); at != had.end())
                        part.jointAt = size_t(at - had.begin());
                    s.jointOf.erase(j);
                }
                else if (!covered)
                    partsGone.insert(it->second);
                s.copyOf.erase(it->first);
                it = s.partOf.erase(it);
            }
            else
                ++it;
        }
        for (const SdfPath& p : partsGone)
            warn.push_back(std::format("{}: deleting what it built isn't written", p.GetString()));
        // A copy deleted from an instance that is left removes it from the prototype, as editing one edits it there; an
        // instance left with no copies at all is hidden instead.
        for (const auto& [instancer, protos] : copiesGone)
        {
            std::set<SdfPath> lost;
            for (const auto& [h, inst] : s.instanceOf)
            {
                if (inst.first != instancer || !g.node(h))
                    continue;
                std::set<SdfPath> has;
                for (NodeId d : g.descendants(h))
                    if (auto c = s.copyOf.find(d); c != s.copyOf.end())
                        has.insert(c->second);
                if (has.empty())
                    hide[instancer].insert(inst.second);
                for (const SdfPath& q : protos)
                    if (!has.count(q) && std::any_of(has.begin(), has.end(), [&](const SdfPath& c) { return q.HasPrefix(c); }))
                        lost.insert(q);
            }
            std::vector<SdfPath> removed;
            for (const SdfPath& q : lost)
            {
                if (std::any_of(removed.begin(), removed.end(), [&](const SdfPath& r) { return q.HasPrefix(r); }))
                    continue;
                au.deactivate(q);
                removed.push_back(q);
                warn.push_back(std::format("{}: removed from its prototype, so every instance of it loses it", q.GetString()));
            }
            if (!removed.empty())
                s.changed.insert(instancer);
        }

        // Parts back from an undo: joints, point instances and copies again, their instances shown and prototype parts back.
        for (NodeId n : g.descendants(s.node))
        {
            auto d = s.deletedParts.find(n);
            if (d == s.deletedParts.end() || s.partOf.count(n) || s.primOf.count(n))
                continue;
            s.partOf[n] = d->second.owner;
            if (d->second.joint)
                s.jointOf[n] = *d->second.joint;
            if (d->second.jointAt)
                au.restoredAt[n] = *d->second.jointAt;
            if (d->second.copy)
            {
                s.copyOf[n] = *d->second.copy;
                for (NodeId h = g.parent(n); h != kNoNode; h = g.parent(h))
                    if (auto inst = s.instanceOf.find(h); inst != s.instanceOf.end())
                    {
                        show[inst->second.first].insert(inst->second.second);
                        break;
                    }
                if (const SdfPrimSpecHandle spec = au.L->GetPrimAtPath(*d->second.copy); spec && spec->HasActive())
                {
                    au.reactivate(*d->second.copy);
                    s.changed.insert(d->second.owner);
                }
            }
            if (d->second.instance)
            {
                s.instanceOf[n] = *d->second.instance;
                show[d->second.instance->first].insert(d->second.instance->second);
            }
            s.deletedParts.erase(d);
        }

        merged = mergeBack(s, g, au, show);
        writeSkeletons(s, g, au);
        writeNewSkeletons(s, au);

        // Objects moved to another parent, parents first: their prims follow in the edit layer.
        std::vector<std::pair<SdfPath, NodeId>> reparented;
        for (const auto& [n, p] : s.primOf)
            reparented.push_back({p, n});
        std::sort(reparented.begin(), reparented.end());
        for (const auto& [was, n] : reparented)
        {
            const SdfPath p = s.primOf[n];
            const NodeId parent = g.parent(n);
            const Node* pn = g.node(parent);
            const bool newParent = parent != s.node && pn && pn->type().isA("transform") && !s.primOf.count(parent) && !s.partOf.count(parent);
            if (!newParent && linkedParentPrim(s, g, n) == linkedAbove(s, p))
                continue;
            if (reparent(s, g, au, n, p).IsEmpty())
            {
                warn.push_back(std::format("{}: moved under {}, which has no prim to hold it, so the move isn't written", g.node(n)->name(),
                                           g.node(g.parent(n))->name()));
                skip.insert(n);
            }
        }

        // Changed property groups on linked objects.
        std::vector<std::pair<NodeId, SdfPath>> linked(s.primOf.begin(), s.primOf.end());
        std::sort(linked.begin(), linked.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& [n, p] : linked)
        {
            if (skip.count(n))
                continue;
            auto e = now.prim.find(n);
            if (e == now.prim.end())
                continue;
            if (p.ContainsPrimVariantSelection())
                inLevels[variantPrefix(p)].push_back({LevelItem::Edit, n, p});
            else
                authorChanges(s, g, au, n, e->second, p, s.baseline[p.GetString()]);
        }

        // Point instances: a moved one moves on its instancer; an edited copy edits its prototype, and so every instance of it.
        std::map<SdfPath, std::vector<std::pair<size_t, glm::dmat4>>> moved;
        for (const auto& [n, inst] : s.instanceOf)
        {
            auto e = now.prim.find(n);
            if (e == now.prim.end())
                continue;
            const uint64_t h = groupHash(now.hashes[n], "xform");
            Groups& base = s.baseline[instanceKey(inst)];
            if (h == groupHash(base, "xform"))
                continue;
            moved[inst.first].push_back({inst.second, g.node(n)->as<glm::dmat4>(scene::Transform::kMatrix)});
            base["xform"] = h;
        }
        for (const auto& [instancer, list] : moved)
            writeInstances(au, instancer, list, axes);
        std::map<SdfPath, NodeId> protoWriter;
        for (const auto& [n, proto] : s.copyOf)
        {
            auto e = now.prim.find(n);
            const std::string key = copyKey(s, g, n);
            if (e == now.prim.end() || key.empty())
                continue;
            Groups& base = s.baseline[key];
            if (base == now.hashes[n])
                continue;
            if (auto [it, first] = protoWriter.insert({proto, n}); !first)
            {
                warn.push_back(std::format("{}: another copy of {} was edited too; {}'s edits were written", g.node(n)->name(), proto.GetString(),
                                           g.node(it->second)->name()));
                continue;
            }
            Groups was = base;
            authorChanges(s, g, au, n, e->second, proto, was);
            base = now.hashes[n];
            if (const auto owner = s.partOf.find(n); owner != s.partOf.end())
                s.changed.insert(owner->second);
        }

        // New objects: their written prims copied in under their parent's prim, with everything below them.
        std::set<NodeId> handled;
        for (NodeId n : g.descendants(s.node))
        {
            const Node* node = g.node(n);
            if (s.primOf.count(n) || s.partOf.count(n) || !node || !node->type().isA("transform"))
                continue;
            if (handled.count(g.parent(n)))
            {
                handled.insert(n);
                continue;
            }
            if (isStage(node))
            {
                handled.insert(n);
                continue;
            }
            // Back from an undo (or into the stage again): the deactivated prim comes back; one the edit layer had defined
            // is written anew.
            if (auto d = s.deleted.find(n); d != s.deleted.end())
            {
                const SdfPath p = d->second;
                if (s.stage->GetPrimAtPath(stagePath(p)))
                {
                    au.reactivate(p);
                    s.link(n, p);
                    if (auto e = now.prim.find(n); e != now.prim.end() && !p.ContainsPrimVariantSelection())
                        authorChanges(s, g, au, n, e->second, p, s.baseline[p.GetString()]);
                    continue;
                }
                s.deleted.erase(d);
            }
            handled.insert(n);
            if (node->type().isA("joint"))
            {
                warn.push_back(std::format("{}: new joints aren't written", node->name()));
                continue;
            }
            auto e = now.prim.find(n);
            if (e == now.prim.end())
                continue;
            const NodeId parent = g.parent(n);
            const SdfPath parentPrim = newParentPrim(s, g, parent, warn);
            if (parentPrim.IsEmpty())
            {
                warn.push_back(std::format("{}: objects added under {} aren't written", node->name(), g.node(parent)->name()));
                continue;
            }
            if (parentPrim.ContainsPrimVariantSelection())
            {
                inLevels[variantPrefix(parentPrim)].push_back({LevelItem::Add, n, parentPrim});
                continue;
            }
            if (!parentPrim.IsAbsoluteRootPath())
                au.openInstances(parentPrim, true);
            addPrim(s, au, n, e->second, parentPrim, g);
        }

        std::set<SdfPath> instancers;
        for (const auto* m : {&hide, &show})
            for (const auto& [instancer, ids] : *m)
                instancers.insert(instancer);
        for (const SdfPath& instancer : instancers)
            setHidden(au, instancer, hide[instancer], show[instancer]);

        // Material edits go over the material prims they came from.
        for (const auto& [slot, h] : now.materialHashes)
        {
            Groups& base = s.baseline["$mat:" + slot];
            if (groupHash(base, "material") == h)
                continue;
            auto origin = s.materialOrigin.find(slot);
            if (origin != s.materialOrigin.end() && s.stage->GetPrimAtPath(origin->second))
                au.writeMaterial(slot, origin->second);
            base["material"] = h;
        }

        closeInstances(s, au.L);
    }
    if (!merged.empty())
        rebaselineInstances(s, g, merged, warn);

    // Each LOD level selected in turn, its prims written into its variant.
    for (const auto& [level, items] : inLevels)
    {
        VariantScope scope(s, level);
        SdfChangeBlock block;
        for (const LevelItem& it : items)
        {
            if (it.kind == LevelItem::Delete)
                au.deactivate(it.p);
            else if (it.kind == LevelItem::Edit)
                authorChanges(s, g, au, it.n, now.prim[it.n], it.p, s.baseline[it.p.GetString()]);
            else
                addPrim(s, au, it.n, now.prim[it.n], it.p, g);
        }
    }
    written = std::move(au.written);
    shapes = std::move(au.shapes);

    // A stacked mesh's written shape is its stack's output: a later rebuild that finds it unchanged keeps the base.
    for (const SdfPath& p : shapes)
        if (auto n = s.nodeOf.find(p); n != s.nodeOf.end() && !p.ContainsPrimVariantSelection() && !scene::modifiers::stack(g, n->second).empty())
            s.baseline["$out:" + p.GetString()]["shape"] = composedShape(s, p);

    s.pending.clear();
    s.full = !now.ok;
    s.comparedStart = scene::anim::playback().animStart;
    s.comparedEnd = scene::anim::playback().animEnd;
    for (std::string& w : overridden(s, written))
        warn.push_back(std::move(w));
    for (const std::string& e : diag.errors())
        warn.push_back("OpenUSD: " + e);
    return warn;
}

// Rebuilding the nodes of prims the stage changed.

void adopt(Stage& s, Graph& g, const ImportResult& r)
{
    std::set<NodeId> linked;
    for (const auto& [p, n] : r.nodeOf)
    {
        s.link(n, p);
        linked.insert(n);
    }
    for (const auto* list : {&r.nodes, &r.joints})
        for (NodeId n : *list)
        {
            if (linked.count(n) || !g.node(n))
                continue;
            SdfPath owner = SdfPath::AbsoluteRootPath();
            for (NodeId p = g.parent(n); p != kNoNode && p != s.node; p = g.parent(p))
                if (auto it = s.primOf.find(p); it != s.primOf.end())
                {
                    owner = it->second;
                    break;
                }
            s.partOf[n] = owner;
        }
    std::map<SdfPath, size_t> nextJoint;
    for (const auto& [n, skel] : r.jointSkeletons)
    {
        // The importer builds a skeleton's joints in the order of its joints attr.
        const size_t k = nextJoint[skel]++;
        if (!s.partOf.count(n))
            continue;
        s.partOf[n] = skel;
        VtTokenArray tokens;
        if (const UsdSkelSkeleton sk{s.stage->GetPrimAtPath(skel)})
            sk.GetJointsAttr().Get(&tokens);
        if (k < tokens.size())
            s.jointOf[n] = tokens[k];
    }
    for (const auto& [n, proto] : r.copies)
        if (s.partOf.count(n))
            s.copyOf[n] = proto;
    for (const ImportResult::PointInstance& pi : r.pointInstances)
        if (s.partOf.count(pi.holder))
            s.instanceOf[pi.holder] = {pi.instancer, pi.index};
    for (const auto& [p, slot] : r.materialSlots)
        s.materialOrigin.emplace(slot, p);
}

void purge(Stage& s, const Graph& g)
{
    for (auto it = s.primOf.begin(); it != s.primOf.end();)
    {
        if (!g.node(it->first))
        {
            s.nodeOf.erase(it->second);
            it = s.primOf.erase(it);
        }
        else
            ++it;
    }
    for (auto it = s.partOf.begin(); it != s.partOf.end();)
    {
        if (g.node(it->first))
        {
            ++it;
            continue;
        }
        s.copyOf.erase(it->first);
        s.instanceOf.erase(it->first);
        s.jointOf.erase(it->first);
        it = s.partOf.erase(it);
    }
}

NodeId nodeAbove(const Stage& s, const SdfPath& p)
{
    const SdfPath q = linkedAbove(s, p);
    if (auto it = s.nodeOf.find(q); it != s.nodeOf.end())
        return it->second;
    return s.node;
}

/**
 * Builds the prim at path again: its node is reused, with every node below when recursive. A mesh keeps its modifier
 * stack, deformers and skin: only the base mesh under them is replaced, and not even that when the stage still holds
 * the shape Lila wrote. Meshes elsewhere skinned to joints this builds again are bound to the new joints in the same
 * pass. Nodes whose prims are gone are deleted, and so are the stack nodes they leave feeding nothing.
 */
void rebuild(Stage& s, Graph& g, const SdfPath& path, bool recursive, std::vector<std::string>& warnings)
{
    LILA_ZONE();

    Busy busy(s);
    PathMap reuse;
    for (const auto& [p, n] : s.nodeOf)
        if (recursive ? stagePath(p).HasPrefix(path) : p == path)
            reuse[p] = n;
    std::vector<std::pair<std::string, NodeId>> also;
    if (recursive)
    {
        std::set<NodeId> parts;
        for (auto it = s.partOf.begin(); it != s.partOf.end();)
            if (stagePath(it->second).HasPrefix(path))
            {
                parts.insert(it->first);
                s.copyOf.erase(it->first);
                s.instanceOf.erase(it->first);
                s.jointOf.erase(it->first);
                it = s.partOf.erase(it);
            }
            else
                ++it;
        for (const auto& [n, p] : s.primOf)
        {
            const Node* node = g.node(n);
            if (reuse.count(p) || p.ContainsPrimVariantSelection() || !node || !node->type().isA("mesh"))
                continue;
            const NodeId sk = scene::skin::find(g, n);
            if (!sk)
                continue;
            for (const scene::skin::Influence& inf : scene::skin::influences(g, sk))
                if (parts.count(inf.joint))
                {
                    also.push_back({p.GetString(), nodeAbove(s, p)});
                    break;
                }
        }
        for (const auto& [p, parent] : also)
            reuse[SdfPath(p)] = s.nodeOf[SdfPath(p)];
        for (NodeId n : parts)
            if (g.node(n))
                deleteWithInputs(g, n);
    }

    // What feeds the reused meshes, to clear out afterwards whatever ends up feeding no mesh at all.
    std::set<NodeId> stackNodes;
    std::set<SdfPath> keep;
    for (const auto& [p, n] : reuse)
    {
        const Node* node = g.node(n);
        if (!node || !node->type().isA("mesh"))
            continue;
        const std::vector<NodeId> stack = scene::modifiers::stack(g, n);
        stackNodes.insert(stack.begin(), stack.end());
        if (const NodeId src = scene::modifiers::stackSource(g, n))
            stackNodes.insert(src);
        if (stack.empty() || p.ContainsPrimVariantSelection())
            continue;
        auto out = s.baseline.find("$out:" + p.GetString());
        if (out != s.baseline.end() && groupHash(out->second, "shape") == composedShape(s, p))
            keep.insert(p);
    }
    for (const auto& [p, n] : reuse)
    {
        s.nodeOf.erase(p);
        s.primOf.erase(n);
    }

    const UsdPrim top = path.IsAbsoluteRootPath() ? s.stage->GetPseudoRoot() : s.stage->GetPrimAtPath(path);
    if (top && (top.IsPseudoRoot() || (top.IsActive() && top.IsDefined() && !top.IsAbstract())))
    {
        ImportOptions o;
        o.parent = top.IsPseudoRoot() ? s.node : nodeAbove(s, path);
        o.prim = top.IsPseudoRoot() ? std::string() : path.GetString();
        o.reuse = &reuse;
        o.shallow = !recursive;
        o.alsoBuild = also;
        o.keepShape = &keep;
        o.live = true;
        o.setPlayback = false;
        ImportResult r;
        importStage(g, s.stage, resolveFile(s.source), o, r);
        if (!r.ok)
            warnings.push_back(path.GetString() + ": " + r.error);
        for (const std::string& w : r.warnings)
            warnings.push_back(w);
        adopt(s, g, r);
    }
    for (const auto& [p, n] : reuse)
        if (g.node(n))
            deleteWithInputs(g, n);
    for (NodeId id : stackNodes)
        if (const Node* n = g.node(id); n && scene::modifiers::asModifier(n) && scene::modifiers::meshOf(g, id) == kNoNode)
            deleteWithInputs(g, id);
    for (NodeId id : stackNodes)
        if (const Node* n = g.node(id); n && !scene::modifiers::asModifier(n) && g.destinations({id, scene::MeshSourceNode::kOutMesh, -1}).empty() &&
                                        n->type().isA("meshSource"))
            g.deleteNode(id);
    purge(s, g);
}

/** The material's Lila material made again from the stage. */
void refreshMaterial(Stage& s, Graph& g, const std::string& slot)
{
    auto it = s.materialOrigin.find(slot);
    if (it == s.materialOrigin.end())
        return;
    const UsdShadeMaterial m(s.stage->GetPrimAtPath(it->second));
    if (!m)
        return;
    if (Node* old = scene::materials::find(g, slot))
        g.deleteNode(old->id());
    MaterialReader reader(g, resolveFile(s.source));
    const std::string made = reader.slot(m);
    if (made != slot)
        s.materialOrigin[made] = it->second;
}

/** Takes the nodes' current state as the baseline for the prims under any of paths, and for the materials in slots. */
void rebaseline(Stage& s, Graph& g, const std::vector<SdfPath>& paths, const std::set<std::string>& slots, std::vector<std::string>& warnings)
{
    const bool all = std::any_of(paths.begin(), paths.end(), [](const SdfPath& p) { return p.IsAbsoluteRootPath(); });
    auto inside = [&](const SdfPath& p) { return std::any_of(paths.begin(), paths.end(), [&](const SdfPath& q) { return stagePath(p).HasPrefix(q); }); };
    Plan plan;
    if (!all)
    {
        std::set<NodeId> linked;
        bool parts = false;
        for (const auto& [n, p] : s.primOf)
            if (inside(p))
                linked.insert(n);
        for (const auto& [n, p] : s.partOf)
            parts = parts || inside(p);
        if (!parts)
            plan = planFor(s, g, linked, {});
    }
    Snapshot now = snapshot(s, g, warnings, plan);
    for (const auto& [n, p] : s.primOf)
    {
        if (!inside(p))
            continue;
        if (auto h = now.hashes.find(n); h != now.hashes.end())
            s.baseline[p.GetString()] = h->second;
        else if (plan.full)
            s.baseline.erase(p.GetString());
    }
    for (const auto& [n, inst] : s.instanceOf)
        if (auto h = now.hashes.find(n); h != now.hashes.end() && inside(inst.first))
            s.baseline[instanceKey(inst)]["xform"] = groupHash(h->second, "xform");
    for (const auto& [n, proto] : s.copyOf)
        if (auto h = now.hashes.find(n); h != now.hashes.end())
            if (const std::string key = copyKey(s, g, n); !key.empty())
                s.baseline[key] = h->second;
    for (const auto& [slot, h] : now.materialHashes)
        if (all || slots.count(slot) || !s.baseline.count("$mat:" + slot))
            s.baseline["$mat:" + slot]["material"] = h;
    for (const auto& [path, groups] : now.skeletons)
        if (all || inside(path) || !s.baseline.count("$skel:" + path.GetString()))
            s.baseline["$skel:" + path.GetString()] = groups;
    if (plan.full)
    {
        s.pending.clear();
        s.full = false;
        s.comparedStart = scene::anim::playback().animStart;
        s.comparedEnd = scene::anim::playback().animEnd;
    }
}

/** Applies the changes notices collected: the prims they touched are built again. Returns how many were. */
size_t flush(Stage& s, Graph& g, bool syncFirst)
{
    if (s.resynced.empty() && s.changed.empty())
        return 0;
    LILA_ZONE();

    std::vector<std::string> warnings;
    if (syncFirst)
        warnings = sync(s, g);
    const std::set<SdfPath> res = std::move(s.resynced), chg = std::move(s.changed);
    s.resynced.clear();
    s.changed.clear();

    std::vector<SdfPath> roots;
    if (res.count(SdfPath::AbsoluteRootPath()))
        roots.push_back(SdfPath::AbsoluteRootPath());
    else
        for (const SdfPath& p : res)
            if (roots.empty() || !p.HasPrefix(roots.back()))
                roots.push_back(p);
    auto underRoot = [&](const SdfPath& p) { return std::any_of(roots.begin(), roots.end(), [&](const SdfPath& r) { return p.HasPrefix(r); }); };

    std::set<std::string> slots;
    std::set<SdfPath> materialPrims;
    for (const auto& [slot, origin] : s.materialOrigin)
    {
        materialPrims.insert(origin);
        const bool touched = std::any_of(roots.begin(), roots.end(), [&](const SdfPath& r) { return origin.HasPrefix(r); }) ||
                             std::any_of(chg.begin(), chg.end(), [&](const SdfPath& c) { return c.HasPrefix(origin); });
        if (touched)
            slots.insert(slot);
    }
    {
        Busy busy(s);
        for (const std::string& slot : slots)
            refreshMaterial(s, g, slot);
    }

    std::vector<SdfPath> rebuilt;
    for (const SdfPath& r : roots)
    {
        rebuild(s, g, r, true, warnings);
        rebuilt.push_back(r);
    }
    std::set<SdfPath> done;
    auto ownsParts = [&](const SdfPath& q) {
        return std::any_of(s.partOf.begin(), s.partOf.end(), [&](const auto& part) { return stagePath(part.second) == q; });
    };
    auto rebuildAt = [&](SdfPath q) {
        while (!q.IsAbsoluteRootPath() && !q.IsEmpty() && !s.nodeOf.count(q) && !ownsParts(q))
            q = q.GetParentPath();
        if (q.IsAbsoluteRootPath() || q.IsEmpty() || underRoot(q) || !done.insert(q).second)
            return;
        // Whatever the prim built besides its own node (joints, point instances, LOD levels) is built again with it.
        bool owns = ownsParts(q) || !s.nodeOf.count(q);
        if (auto it = s.nodeOf.find(q); it != s.nodeOf.end())
            if (const Node* qn = g.node(it->second); qn && qn->type().isA("lodGroup"))
                owns = true;
        rebuild(s, g, q, owns, warnings);
        rebuilt.push_back(q);
    };
    for (const SdfPath& c : chg)
    {
        if (underRoot(c) || std::any_of(materialPrims.begin(), materialPrims.end(), [&](const SdfPath& m) { return c.HasPrefix(m); }))
            continue;
        rebuildAt(c);
        // A prototype prim changed: every point instance built from it, under its instancer.
        std::set<SdfPath> instancers;
        for (const auto& [n, proto] : s.copyOf)
            if (c.HasPrefix(proto) || proto.HasPrefix(c))
                if (auto owner = s.partOf.find(n); owner != s.partOf.end())
                    instancers.insert(owner->second);
        for (const SdfPath& i : instancers)
            rebuildAt(i);
    }
    rebaseline(s, g, rebuilt, slots, warnings);
    s.stamps = stampsOf(s);
    writeAttrs(s, g);
    logWarnings(s, g, std::move(warnings));
    if (!rebuilt.empty())
        io::markModified();
    return rebuilt.size();
}

// The per-node state.

void refreshTargets(Stage& s, Graph& g);

Stage& ensure(Graph& g, NodeId id)
{
    listen(g);
    const Node* n = g.node(id);
    if (!isStage(n))
        throw CommandError(std::string(n ? n->name() : "node") + " is not a usdStage");
    if (Stage* s = findStage(id); s && s->object == n)
    {
        refreshTargets(*s, g);
        return *s;
    }
    stages().erase(id);
    auto s = std::make_unique<Stage>();
    s->node = id;
    s->object = n;
    SdfLayerRefPtr carry;
    if (const std::string data = text(*n, stageAttr::kEditData); !data.empty())
    {
        carry = SdfLayer::CreateAnonymous("edits.usda");
        if (!carry->ImportFromString(data))
            log::warn("{}: its saved edit layer could not be read", n->name());
    }
    std::vector<std::string> warnings;
    compose(*s, g, carry, warnings);
    readAttrs(*s, g);
    for (const std::string& w : warnings)
        log::warn("{}: {}", n->name(), w);
    Stage& ref = *s;
    stages()[id] = std::move(s);
    return ref;
}

/** The node's File or Edit layer changed: compose again (the edits kept and moved) and rebuild. */
void refreshTargets(Stage& s, Graph& g)
{
    const Node& n = *g.node(s.node);
    if (text(n, stageAttr::kFile) == s.source && text(n, stageAttr::kEditLayer) == s.editSpec)
        return;
    NoUndo quiet;
    std::vector<std::string> warnings = sync(s, g);
    SdfLayerRefPtr carry = SdfLayer::CreateAnonymous("edits.usda");
    carry->TransferContent(s.edit);
    const std::string oldSource = s.source, oldEdit = s.editSpec;
    try
    {
        compose(s, g, carry, warnings);
    }
    catch (const std::exception& e)
    {
        setText(g, s.node, stageAttr::kFile, oldSource);
        setText(g, s.node, stageAttr::kEditLayer, oldEdit);
        compose(s, g, carry, warnings);
        warnings.push_back(e.what());
    }
    std::set<SdfPath> carried;
    carry->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& p) {
        if (p.IsPropertyPath())
            carried.insert(p);
    });
    for (std::string& w : overridden(s, carried))
        warnings.push_back(std::move(w));
    rebuild(s, g, SdfPath::AbsoluteRootPath(), true, warnings);
    s.full = true;
    rebaseline(s, g, {SdfPath::AbsoluteRootPath()}, {}, warnings);
    s.stamps = stampsOf(s);
    writeAttrs(s, g);
    logWarnings(s, g, std::move(warnings));
}

std::vector<NodeId> stageNodes(const Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (isStage(g.node(id)))
            out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

void prune(const Graph& g)
{
    for (auto it = stages().begin(); it != stages().end();)
        it = g.node(it->first) == it->second->object && isStage(g.node(it->first)) ? std::next(it) : stages().erase(it);
}

// Undo: the edit layer, links and baseline before and after a command.

struct Saved
{
    std::string layer;
    std::vector<std::tuple<NodeId, SdfPath, bool>> links;
    std::unordered_map<NodeId, SdfPath> copies;
    std::unordered_map<NodeId, std::pair<SdfPath, size_t>> instances;
    std::unordered_map<NodeId, TfToken> joints;
    Baseline baseline;
    std::map<std::string, SdfPath> materials;
    UsdStageLoadRules rules;
};

Saved save(const Stage& s)
{
    Saved v;
    s.edit->ExportToString(&v.layer);
    for (const auto& [n, p] : s.primOf)
        v.links.push_back({n, p, false});
    for (const auto& [n, p] : s.partOf)
        v.links.push_back({n, p, true});
    v.copies = s.copyOf;
    v.instances = s.instanceOf;
    v.joints = s.jointOf;
    v.baseline = s.baseline;
    v.materials = s.materialOrigin;
    v.rules = s.stage->GetLoadRules();
    return v;
}

void restore(Stage& s, Graph& g, const Saved& v)
{
    Busy busy(s);
    s.edit->ImportFromString(v.layer);
    if (!(s.stage->GetLoadRules() == v.rules))
        s.stage->SetLoadRules(v.rules);
    s.primOf.clear();
    s.nodeOf.clear();
    s.partOf.clear();
    for (const auto& [n, p, part] : v.links)
        if (part)
            s.partOf[n] = p;
        else
            s.link(n, p);
    s.copyOf = v.copies;
    s.instanceOf = v.instances;
    s.jointOf = v.joints;
    s.baseline = v.baseline;
    s.materialOrigin = v.materials;
    s.resynced.clear();
    s.changed.clear();
    s.full = true;
    writeAttrs(s, g);
}

struct StageStates
{
    NodeId node = kNoNode;
    Saved before, after;
};

/**
 * Brackets a command's scene edits: the opening mark puts the stage back once undo has restored the nodes, the closing
 * one moves it forward once redo has made them again.
 */
struct StageMark final : UndoAction
{
    std::shared_ptr<StageStates> states;
    bool opening = true;
    StageMark(std::shared_ptr<StageStates> st, bool open) : states(std::move(st)), opening(open) {}
    void undo() override
    {
        if (Stage* s = opening ? findStage(states->node) : nullptr)
            restore(*s, Graph::current(), states->before);
    }
    void redo() override
    {
        if (Stage* s = opening ? nullptr : findStage(states->node))
            restore(*s, Graph::current(), states->after);
    }
    size_t bytes() const override { return opening ? states->before.layer.size() + states->after.layer.size() : 64; }
};

/** Opened before a stage command changes anything, closed once it has. */
class StageStep
{
public:
    explicit StageStep(Stage& s) : mStage(s), mStates(std::make_shared<StageStates>())
    {
        mStates->node = s.node;
        mStates->before = save(s);
        UndoStack::get().record(std::make_unique<StageMark>(mStates, true));
    }
    void done()
    {
        mStates->after = save(mStage);
        UndoStack::get().record(std::make_unique<StageMark>(mStates, false));
    }

private:
    Stage& mStage;
    std::shared_ptr<StageStates> mStates;
};

// Values as usda text, for the Attribute Editor and scripts.

std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string valueText(const VtValue& v)
{
    auto list = [](const auto& items, auto&& one) {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i)
            out += (i ? ", " : "") + one(items[i]);
        return out + "]";
    };
    if (v.IsHolding<std::string>())
        return quote(v.UncheckedGet<std::string>());
    if (v.IsHolding<TfToken>())
        return quote(v.UncheckedGet<TfToken>().GetString());
    if (v.IsHolding<SdfAssetPath>())
        return "@" + v.UncheckedGet<SdfAssetPath>().GetAssetPath() + "@";
    if (v.IsHolding<bool>())
        return v.UncheckedGet<bool>() ? "true" : "false";
    if (v.IsHolding<VtStringArray>())
        return list(v.UncheckedGet<VtStringArray>(), [](const std::string& s) { return quote(s); });
    if (v.IsHolding<VtTokenArray>())
        return list(v.UncheckedGet<VtTokenArray>(), [](const TfToken& t) { return quote(t.GetString()); });
    if (v.IsHolding<VtArray<SdfAssetPath>>())
        return list(v.UncheckedGet<VtArray<SdfAssetPath>>(), [](const SdfAssetPath& a) { return "@" + a.GetAssetPath() + "@"; });
    return TfStringify(v);
}

std::string jsonText(const json& j, bool array)
{
    if (j.is_string())
        return quote(j.get<std::string>());
    if (j.is_boolean())
        return j.get<bool>() ? "1" : "0";
    if (j.is_array())
    {
        std::string out = array ? "[" : "(";
        for (size_t i = 0; i < j.size(); ++i)
            out += (i ? ", " : "") + jsonText(j[i], false);
        return out + (array ? "]" : ")");
    }
    return j.dump();
}

VtValue parseValue(const SdfValueTypeName& type, const json& value)
{
    std::string text;
    if (value.is_string())
    {
        text = TfStringTrim(value.get<std::string>());
        const TfToken scalar = type.GetScalarType().GetAsToken();
        if (type == SdfValueTypeNames->Bool)
            text = text == "true" || text == "True" ? "1" : text == "false" || text == "False" ? "0" : text;
        else if ((scalar == SdfValueTypeNames->String.GetAsToken() || scalar == SdfValueTypeNames->Token.GetAsToken()) && !type.IsArray() &&
                 !text.starts_with('"') && !text.starts_with('\''))
            text = quote(text);
        else if (type == SdfValueTypeNames->Asset && !text.starts_with('@'))
            text = "@" + text + "@";
    }
    else
        text = jsonText(value, type.IsArray());
    const std::string doc = "#usda 1.0\ndef \"P\"\n{\n    " + type.GetAsToken().GetString() + " v = " + text + "\n}\n";
    SdfLayerRefPtr tmp = SdfLayer::CreateAnonymous("value.usda");
    Diagnostics diag;
    if (!tmp->ImportFromString(doc))
        throw CommandError("can't read " + text + " as a " + type.GetAsToken().GetString());
    const SdfAttributeSpecHandle a = tmp->GetAttributeAtPath(SdfPath("/P.v"));
    if (!a || !a->HasDefaultValue())
        throw CommandError("can't read " + text + " as a " + type.GetAsToken().GetString());
    return a->GetDefaultValue();
}

// Commands.

Graph& graph() { return Graph::current(); }

NodeId stageArg(Graph& g, const Args& a, const char* cmd, size_t index)
{
    if (a.positional.size() > index && a.positional[index].is_string())
    {
        const std::string name = a.positional[index].get<std::string>();
        const Node* n = g.find(name);
        if (!n)
            throw CommandError(std::format("{}: no node named '{}'", cmd, name));
        if (isStage(n))
            return n->id();
        if (NodeId st = stageOf(g, n->id()))
            return st;
        throw CommandError(std::format("{}: {} is not a usdStage or part of one", cmd, name));
    }
    for (NodeId id : g.selection())
    {
        if (isStage(g.node(id)))
            return id;
        if (NodeId st = stageOf(g, id))
            return st;
    }
    if (std::vector<NodeId> all = stageNodes(g); all.size() == 1)
        return all.front();
    throw CommandError(std::format("{}: give the usdStage, or select it", cmd));
}

/** An object linked to a prim: its stage and prim. */
std::pair<Stage*, SdfPath> linkedObject(Graph& g, const std::string& name, const char* cmd)
{
    const Node* n = g.find(name);
    if (!n)
        throw CommandError(std::format("{}: no node named '{}'", cmd, name));
    const NodeId st = stageOf(g, n->id());
    if (!st)
        throw CommandError(std::format("{}: {} is not part of a usdStage", cmd, name));
    Stage& s = ensure(g, st);
    auto it = s.primOf.find(n->id());
    if (it == s.primOf.end())
        throw CommandError(std::format("{}: {} isn't linked to a prim (add it to the stage by saving first)", cmd, name));
    return {&s, it->second};
}

std::vector<std::string> objectArgs(Graph& g, const Args& a)
{
    std::vector<std::string> out;
    for (const json& v : a.positional)
        if (v.is_string())
            out.push_back(v.get<std::string>());
    if (out.empty())
        for (NodeId id : g.selection())
            out.push_back(g.path(id));
    return out;
}

const char* ruleName(UsdStageLoadRules::Rule r)
{
    switch (r)
    {
    case UsdStageLoadRules::AllRule: return "all";
    case UsdStageLoadRules::OnlyRule: return "only";
    case UsdStageLoadRules::NoneRule: return "none";
    }
    return "all";
}

/** The payload load rules a shot written by usdStageWriteShot recorded (customLayerData lila:loadRules), as the loadRules attr holds them. */
json recordedLoadRules(const fs::path& file)
{
    json out = json::array();
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(utf8(file));
    if (!layer)
        return out;
    const VtDictionary data = layer->GetCustomLayerData();
    const VtValue* rules = data.GetValueAtPath("lila:loadRules");
    if (!rules || !rules->IsHolding<VtDictionary>())
        return out;
    for (const auto& [prim, rule] : rules->UncheckedGet<VtDictionary>())
    {
        if (!SdfPath::IsValidPathString(prim) || !rule.IsHolding<std::string>())
            continue;
        const std::string& r = rule.UncheckedGet<std::string>();
        const auto kind = r == "none" ? UsdStageLoadRules::NoneRule : r == "only" ? UsdStageLoadRules::OnlyRule : UsdStageLoadRules::AllRule;
        out.push_back({prim, int(kind)});
    }
    if (out.size() == 1 && out[0][0] == "/" && out[0][1] == int(UsdStageLoadRules::AllRule))
        return json::array();
    return out;
}

json cmdOpen(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("usdStageOpen: path required");
    Graph& g = graph();
    const fs::path file = resolveFile(a.positional[0].get<std::string>());
    std::error_code ec;
    if (!fs::exists(file, ec))
        throw CommandError("usdStageOpen: no file " + utf8(file));
    NodeId parent = kNoNode;
    if (a.has("parent"))
    {
        const Node* p = g.find(a.get<std::string>("parent"));
        if (!p)
            throw CommandError("usdStageOpen: no node named '" + a.get<std::string>("parent") + "'");
        parent = p->id();
    }
    const scene::NodeType* type = scene::types::find(kStageType);
    if (!type)
        throw CommandError("usdStageOpen: the usdStage node type isn't registered");
    const std::string name = a.get<std::string>("name", identifier(utf8(file.stem()), "Stage"));
    const NodeId id = g.createNode(*type, name, parent)->id();
    {
        NoUndo quiet;
        setText(g, id, stageAttr::kFile, utf8(file));
        if (const std::string edit = a.get<std::string>("editLayer", ""); !edit.empty())
            setText(g, id, stageAttr::kEditLayer, utf8(resolveFile(edit)));
        if (const std::string surface = a.get<std::string>("materialX", ""); !surface.empty())
        {
            if (mtlx::Surface parsed; !mtlx::parseSurface(surface, &parsed))
                throw CommandError("usdStageOpen: materialX is openPBR, standardSurface or none");
            setText(g, id, stageAttr::kMaterialX, surface);
        }
        if (!a.get<bool>("loadPayloads", true))
            setText(g, id, stageAttr::kLoadRules, json::array({json::array({"/", int(UsdStageLoadRules::NoneRule)})}).dump());
        else if (const json recorded = recordedLoadRules(file); !recorded.empty())
            setText(g, id, stageAttr::kLoadRules, recorded.dump());
    }
    Stage* sp = nullptr;
    try
    {
        sp = &ensure(g, id);
    }
    catch (...)
    {
        g.deleteNode(id);
        throw;
    }
    Stage& s = *sp;
    std::vector<std::string> warnings;
    if (a.has("variants"))
    {
        const json& v = a.flags["variants"];
        if (!v.is_object())
            throw CommandError("usdStageOpen: variants takes a dict, {'set': 'variant'} or {'/prim/path': {'set': 'variant'}}");
        Busy busy(s);
        for (auto& [key, value] : v.items())
        {
            std::vector<std::pair<SdfPath, std::map<std::string, std::string>>> todo;
            if (value.is_string())
                eachPrim(s, [&](const UsdPrim& p, const SdfPath& link) {
                    if (p.GetVariantSets().HasVariantSet(key))
                        todo.push_back({link, {{key, value.get<std::string>()}}});
                });
            else if (value.is_object() && SdfPath::IsValidPathString(key))
            {
                std::map<std::string, std::string> sets;
                for (auto& [set, sel] : value.items())
                    if (sel.is_string())
                        sets[set] = sel.get<std::string>();
                todo.push_back({SdfPath(key), sets});
            }
            for (const auto& [path, sets] : todo)
                for (const auto& [set, sel] : sets)
                {
                    bool ok = false;
                    {
                        VariantScope scope(s, path);
                        const UsdPrim p = s.stage->GetPrimAtPath(stagePath(path));
                        ok = p && p.GetVariantSets().HasVariantSet(set) && p.GetVariantSets().GetVariantSet(set).HasAuthoredVariant(sel);
                    }
                    if (ok)
                        selectVariant(s, path, set, sel);
                    else
                        warnings.push_back(std::format("{}: no variant {} in set {}", path.GetString(), sel, set));
                }
        }
        // Nothing is built yet, so nothing needs building again.
        s.changed.clear();
    }

    ImportOptions o;
    o.parent = id;
    o.live = true;
    o.setPlayback = a.get<bool>("setPlayback", true);
    ImportResult r;
    {
        Busy busy(s);
        importStage(g, s.stage, file, o, r);
    }
    if (!r.ok)
    {
        stages().erase(id);
        g.deleteNode(id);
        throw CommandError("usdStageOpen: " + r.error);
    }
    adopt(s, g, r);
    if (s.stage->HasAuthoredTimeCodeRange() && s.stage->GetTimeCodesPerSecond() > 0.0)
    {
        s.start = s.stage->GetStartTimeCode() / s.stage->GetTimeCodesPerSecond();
        s.end = s.stage->GetEndTimeCode() / s.stage->GetTimeCodesPerSecond();
    }
    else if (r.end > r.start)
    {
        s.start = r.start;
        s.end = r.end;
    }
    s.full = true;
    rebaseline(s, g, {SdfPath::AbsoluteRootPath()}, {}, warnings);
    s.stamps = stampsOf(s);
    writeAttrs(s, g);
    for (const std::string& w : r.warnings)
        warnings.push_back(w);
    logWarnings(s, g, warnings);
    for (const std::string& sk : r.skipped)
        log::info("usdStageOpen: skipped {}", sk);
    if (a.get<bool>("select", true))
        g.select({id});
    const std::string path = g.path(id);
    if (!a.get<bool>("report", false))
        return path;
    std::vector<std::string> nodes;
    for (NodeId n : r.nodes)
        if (g.node(n))
            nodes.push_back(g.path(n));
    return {{"stage", path}, {"prims", s.primOf.size()}, {"nodes", nodes}, {"materials", r.materials},
            {"skipped", r.skipped}, {"warnings", warnings}, {"upAxis", r.upAxis}, {"metersPerUnit", r.metersPerUnit}};
}

json reloadOne(Stage& s, Graph& g, bool all)
{
    NoUndo quiet;
    std::vector<std::string> warnings = sync(s, g);
    json files = json::array();
    const std::vector<SdfLayerHandle> changed = changedLayers(s);
    std::vector<SdfLayerHandle> layers = all ? std::vector<SdfLayerHandle>(s.stage->GetUsedLayers()) : changed;
    for (const SdfLayerHandle& l : layers)
    {
        if (!l || l->IsAnonymous())
            continue;
        // usdMtlx can't read a .mtlx again into the layer it made, so one is read again only when it changed.
        if (l->GetFileFormat()->GetFormatId() == TfToken("mtlx") && std::find(changed.begin(), changed.end(), l) == changed.end())
            continue;
        if (get_pointer(l) == get_pointer(s.edit) && l->IsDirty())
        {
            if (!all)
                warnings.push_back("the edit layer changed on disk while it has unsaved edits; it was not read again");
            continue;
        }
        if (l->Reload(true))
            files.push_back(fileOf(l));
    }
    logWarnings(s, g, warnings);
    const size_t n = flush(s, g, false);
    s.stamps = stampsOf(s);
    writeAttrs(s, g);
    return {{"reloaded", files}, {"updated", n}};
}

json cmdReload(const Args& a)
{
    Graph& g = graph();
    std::vector<NodeId> ids;
    if (!a.positional.empty())
        for (size_t i = 0; i < a.positional.size(); ++i)
            ids.push_back(stageArg(g, a, "usdStageReload", i));
    else
        ids = stageNodes(g);
    json out = json::object();
    for (NodeId id : ids)
        out[g.path(id)] = reloadOne(ensure(g, id), g, true);
    return out;
}

size_t updateAll(Graph& g)
{
    size_t n = 0;
    prune(g);
    for (NodeId id : stageNodes(g))
    {
        Stage* s = findStage(id);
        if (!s)
            continue;
        ensure(g, id);
        if (!changedLayers(*s).empty())
            n += reloadOne(*s, g, false)["updated"].get<size_t>();
        else
        {
            NoUndo quiet;
            n += flush(*s, g, true);
        }
    }
    return n;
}

json cmdVariant(const Args& a)
{
    Graph& g = graph();
    Stage& s = ensure(g, stageArg(g, a, "usdStageVariant", 0));
    const std::string set = a.get<std::string>("set", "");
    if (set.empty() || !a.has("variant"))
        throw CommandError("usdStageVariant: set and variant required");
    const std::string variant = a.get<std::string>("variant");
    // prim is a link path, which can name an LOD level, or a composed path, which takes that prim in every level.
    const std::string prim = a.get<std::string>("prim", "");
    if (!prim.empty() && !SdfPath::IsValidPathString(prim))
        throw CommandError("usdStageVariant: no prim at " + prim);
    const SdfPath want = prim.empty() ? SdfPath() : SdfPath(prim);
    std::vector<SdfPath> targets;
    std::string missing;
    bool found = false;
    eachPrim(s, [&](const UsdPrim& p, const SdfPath& link) {
        if (!want.IsEmpty() && link != want && p.GetPath() != want)
            return;
        found = true;
        if (!p.GetVariantSets().HasVariantSet(set))
            return;
        if (!p.GetVariantSets().GetVariantSet(set).HasAuthoredVariant(variant) && missing.empty())
            missing = link.GetString();
        targets.push_back(link);
    });
    if (!want.IsEmpty() && !found)
        throw CommandError("usdStageVariant: no prim at " + prim);
    if (targets.empty())
        throw CommandError(want.IsEmpty() ? "usdStageVariant: no prim has a variant set named " + set
                                          : std::format("usdStageVariant: {} has no variant {} in set {}", prim, variant, set));
    if (!missing.empty())
        throw CommandError(std::format("usdStageVariant: {} has no variant {} in set {}", missing, variant, set));
    StageStep step(s);
    logWarnings(s, g, sync(s, g));
    json out = json::object();
    for (const SdfPath& t : targets)
    {
        selectVariant(s, t, set, variant);
        out[t.GetString()] = variant;
    }
    flush(s, g, false);
    writeAttrs(s, g);
    step.done();
    return out;
}

json cmdLoad(const Args& a)
{
    Graph& g = graph();
    Stage& s = ensure(g, stageArg(g, a, "usdStageLoad", 0));
    const std::string prim = a.get<std::string>("prim", "/");
    if (!SdfPath::IsValidPathString(prim) || !s.stage->GetPrimAtPath(SdfPath(prim)))
        throw CommandError("usdStageLoad: no prim at " + prim);
    const bool load = a.get<bool>("load", true);
    StageStep step(s);
    logWarnings(s, g, sync(s, g));
    if (load)
        s.stage->Load(SdfPath(prim));
    else
        s.stage->Unload(SdfPath(prim));
    flush(s, g, false);
    writeAttrs(s, g);
    step.done();
    json loaded = json::array();
    for (const SdfPath& p : s.stage->GetLoadSet())
        loaded.push_back(p.GetString());
    return {{"loaded", loaded}};
}

json cmdExtract(const Args& a)
{
    Graph& g = graph();
    json out = json::array();
    for (const std::string& name : objectArgs(g, a))
    {
        auto [sp, prim] = linkedObject(g, name, "usdStageExtract");
        Stage& s = *sp;
        const NodeId n = g.find(name)->id();
        StageStep step(s);
        std::vector<std::string> warnings;
        {
            Busy busy(s);
            Snapshot none;
            Author au{s, none, warnings};
            au.deactivate(prim);
        }
        std::vector<NodeId> drop;
        for (const auto& [node, p] : s.primOf)
            if (p.HasPrefix(prim))
                drop.push_back(node);
        for (const auto& [node, p] : s.partOf)
            if (p.HasPrefix(prim))
                drop.push_back(node);
        for (NodeId d : drop)
            s.unlink(d);
        g.setParent(n, g.parent(s.node), true);
        writeAttrs(s, g);
        if (!warnings.empty())
            logWarnings(s, g, warnings);
        step.done();
        out.push_back(g.path(n));
    }
    return out;
}

/** The edit layer's sublayers, as the files they resolve to, strongest first. */
std::vector<std::string> addedLayers(const Stage& s)
{
    std::vector<std::string> out;
    for (const std::string& sub : std::vector<std::string>(s.edit->GetSubLayerPaths()))
    {
        SdfLayerRefPtr l = SdfLayer::FindOrOpenRelativeToLayer(s.edit, sub);
        out.push_back(l ? fileOf(l) : sub);
    }
    return out;
}

/** Where the edit layer lists a sublayer: relative to its folder when it is a file, absolute when it lives in the scene. */
std::string sublayerPath(const Stage& s, const fs::path& file)
{
    if (s.edit->IsAnonymous())
        return generic(file);
    std::error_code ec;
    const fs::path rel = fs::relative(file, utf8Path(fileOf(s.edit)).parent_path(), ec);
    if (ec || rel.empty())
        return generic(file);
    const std::string r = generic(rel);
    return r.starts_with(".") ? r : "./" + r;
}

/** The index of a file among the edit layer's sublayers, or -1. */
int sublayerIndex(const Stage& s, const fs::path& file)
{
    const std::vector<std::string> layers = addedLayers(s);
    for (size_t i = 0; i < layers.size(); ++i)
        if (samePath(utf8Path(layers[i]), file))
            return int(i);
    return -1;
}

json cmdWriteShot(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("usdStageWriteShot: path required");
    Graph& g = graph();
    Stage& s = ensure(g, stageArg(g, a, "usdStageWriteShot", 1));
    const fs::path out = resolveFile(a.positional[0].get<std::string>());
    std::string ext = utf8(out.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".usda" && ext != ".usd" && ext != ".usdc")
        throw CommandError("usdStageWriteShot: the file must end in .usda, .usd or .usdc");
    NoUndo quiet;
    logWarnings(s, g, sync(s, g));
    writeAttrs(s, g);
    SdfLayerRefPtr shot = SdfLayer::CreateAnonymous("shot.usda");
    if (s.mode != Stage::Mode::Sublayer)
        shot->TransferContent(s.edit);
    copyStageMetadata(s.root, shot);
    const fs::path source = resolveFile(s.source);
    std::error_code ec;
    fs::path rel = fs::relative(source, out.parent_path(), ec);
    std::string sub = ec || rel.empty() ? generic(source) : generic(rel);
    if (!ec && !rel.empty() && !sub.starts_with("."))
        sub = "./" + sub;
    std::vector<std::string> subs;
    // Layers added to the stage go between the edits and the source, as the stage composes them.
    if (s.mode != Stage::Mode::Sublayer)
        for (const std::string& layer : addedLayers(s))
        {
            fs::path r = fs::relative(utf8Path(layer), out.parent_path(), ec);
            std::string p = ec || r.empty() ? generic(utf8Path(layer)) : generic(r);
            subs.push_back(!ec && !r.empty() && !p.starts_with(".") ? "./" + p : p);
        }
    subs.push_back(sub);
    shot->SetSubLayerPaths(subs);
    shot->SetDocumentation("Edits made in Lila, over " + utf8(source.filename()));
    if (const UsdStageLoadRules& rules = s.stage->GetLoadRules(); !(rules == UsdStageLoadRules::LoadAll()))
    {
        VtDictionary byPath;
        for (const auto& [prim, rule] : rules.GetRules())
            byPath[prim.GetString()] = VtValue(std::string(ruleName(rule)));
        VtDictionary lila;
        lila["loadRules"] = VtValue(byPath);
        VtDictionary data = shot->GetCustomLayerData();
        data["lila"] = VtValue(lila);
        shot->SetCustomLayerData(data);
    }
    fs::create_directories(out.parent_path(), ec);
    if (!shot->Export(utf8(out)))
        throw CommandError("usdStageWriteShot: can't write " + utf8(out));
    return utf8(out);
}

const char* modeName(Stage::Mode m)
{
    switch (m)
    {
    case Stage::Mode::Embedded: return "embedded";
    case Stage::Mode::File: return "file";
    case Stage::Mode::Sublayer: return "sublayer";
    }
    return "embedded";
}

json cmdAddLayer(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("usdStageAddLayer: path required");
    Graph& g = graph();
    Stage& s = ensure(g, stageArg(g, a, "usdStageAddLayer", 1));
    const fs::path file = resolveFile(a.positional[0].get<std::string>());
    std::string ext = utf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".usda" && ext != ".usd" && ext != ".usdc")
        throw CommandError("usdStageAddLayer: the file must end in .usda, .usd or .usdc");
    if (samePath(file, resolveFile(s.source)) || (!s.edit->IsAnonymous() && samePath(file, utf8Path(fileOf(s.edit)))))
        throw CommandError("usdStageAddLayer: " + utf8(file) + " is already the stage's source or edit layer");
    if (sublayerIndex(s, file) >= 0)
        throw CommandError("usdStageAddLayer: " + utf8(file) + " is already one of its layers");
    std::error_code ec;
    if (!fs::exists(file, ec))
    {
        fs::create_directories(file.parent_path(), ec);
        SdfLayerRefPtr created = SdfLayer::CreateNew(utf8(file));
        if (!created)
            throw CommandError("usdStageAddLayer: can't create " + utf8(file));
        copyStageMetadata(s.sourceLayer, created);
        created->Save();
    }
    else if (!SdfLayer::FindOrOpen(utf8(file)))
        throw CommandError("usdStageAddLayer: can't read " + utf8(file));

    StageStep step(s);
    logWarnings(s, g, sync(s, g));
    std::vector<std::string> subs = s.edit->GetSubLayerPaths();
    const int position = std::clamp(a.get<int>("position", 0), 0, int(subs.size()));
    subs.insert(subs.begin() + position, sublayerPath(s, file));
    s.edit->SetSubLayerPaths(subs);
    flush(s, g, false);
    s.stamps = stampsOf(s);
    writeAttrs(s, g);
    step.done();
    return addedLayers(s);
}

json cmdRemoveLayer(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("usdStageRemoveLayer: path required");
    Graph& g = graph();
    Stage& s = ensure(g, stageArg(g, a, "usdStageRemoveLayer", 1));
    const fs::path file = resolveFile(a.positional[0].get<std::string>());
    const int at = sublayerIndex(s, file);
    if (at < 0)
        throw CommandError("usdStageRemoveLayer: " + utf8(file) + " is not a layer added to the stage");
    StageStep step(s);
    logWarnings(s, g, sync(s, g));
    s.edit->RemoveSubLayerPath(at);
    flush(s, g, false);
    s.stamps = stampsOf(s);
    writeAttrs(s, g);
    step.done();
    return addedLayers(s);
}

json cmdInfo(const Args& a)
{
    Graph& g = graph();
    Stage& s = ensure(g, stageArg(g, a, "usdStageInfo", 0));
    json layers = json::array();
    for (const SdfLayerHandle& l : s.stage->GetUsedLayers())
        if (l && !l->IsAnonymous())
            layers.push_back(fileOf(l));
    json variants = json::array(), payloads = json::array();
    eachPrim(s, [&](const UsdPrim& p, const SdfPath& link) {
        const SdfPath level = variantPrefix(link);
        if (p.HasVariantSets())
            for (const std::string& set : p.GetVariantSets().GetNames())
            {
                UsdVariantSet vs = p.GetVariantSets().GetVariantSet(set);
                json e{{"prim", link.GetString()}, {"set", set}, {"selection", vs.GetVariantSelection()}, {"variants", vs.GetVariantNames()}};
                if (!level.IsEmpty())
                    e["level"] = level.GetVariantSelection().second;
                variants.push_back(std::move(e));
            }
        if (level.IsEmpty() && p.HasAuthoredPayloads())
            payloads.push_back({{"prim", p.GetPath().GetString()}, {"loaded", p.IsLoaded()}});
    });
    json out = {{"file", s.source},
                {"editLayer", {{"mode", modeName(s.mode)}, {"path", s.mode == Stage::Mode::Embedded ? std::string() : fileOf(s.edit)}}},
                {"layers", layers},
                {"addedLayers", addedLayers(s)},
                {"variants", variants},
                {"payloads", payloads},
                {"prims", s.primOf.size()},
                {"upAxis", UsdGeomGetStageUpAxis(s.stage).GetString()},
                {"metersPerUnit", UsdGeomGetStageMetersPerUnit(s.stage)},
                {"warnings", s.warnings},
                {"stats", {{"lastExported", s.lastExported}, {"fullDiffs", s.fullDiffs}, {"partialDiffs", s.partialDiffs}}}};
    if (a.get<bool>("edits", false))
    {
        std::string t;
        s.edit->ExportToString(&t);
        out["edits"] = t;
    }
    return out;
}

UsdTimeCode currentCode(const Stage& s, const Graph& g)
{
    const double tcps = s.stage->GetTimeCodesPerSecond() > 0.0 ? s.stage->GetTimeCodesPerSecond() : 24.0;
    return UsdTimeCode(g.time() * tcps);
}

json propertyJson(const UsdProperty& p, UsdTimeCode t, bool full)
{
    json e{{"name", p.GetName().GetString()}, {"custom", p.IsCustom()}};
    if (const UsdAttribute at = p.As<UsdAttribute>())
    {
        e["type"] = at.GetTypeName().GetAsToken().GetString();
        e["timeSamples"] = at.GetNumTimeSamples();
        e["authored"] = at.HasAuthoredValue();
        VtValue v;
        if (at.Get(&v, at.ValueMightBeTimeVarying() ? t : UsdTimeCode::Default()))
        {
            if (!full && v.IsArrayValued() && v.GetArraySize() > 64)
            {
                e["value"] = std::format("[{} values]", v.GetArraySize());
                e["truncated"] = true;
            }
            else
                e["value"] = valueText(v);
        }
        else
            e["value"] = nullptr;
    }
    else if (const UsdRelationship r = p.As<UsdRelationship>())
    {
        e["type"] = "rel";
        SdfPathVector ts;
        r.GetTargets(&ts);
        json targets = json::array();
        for (const SdfPath& tp : ts)
            targets.push_back(tp.GetString());
        e["targets"] = targets;
    }
    return e;
}

const char* arcName(PcpArcType t)
{
    switch (t)
    {
    case PcpArcTypeInherit: return "inherit";
    case PcpArcTypeVariant: return "variant";
    case PcpArcTypeRelocate: return "relocate";
    case PcpArcTypeReference: return "reference";
    case PcpArcTypePayload: return "payload";
    case PcpArcTypeSpecialize: return "specialize";
    default: return "local";
    }
}

/** Calls fn(layer, path, node) for every place that can hold opinions on prim, strongest first. */
template <class Fn>
void forEachSite(const UsdPrim& prim, Fn&& fn)
{
    for (const PcpNodeRef& node : prim.GetPrimIndex().GetNodeRange())
    {
        if (node.IsInert() || !node.HasSpecs())
            continue;
        for (const SdfLayerRefPtr& layer : node.GetLayerStack()->GetLayers())
            fn(SdfLayerHandle(layer), node.GetPath(), node);
    }
}

json opinionSite(const Stage& s, const SdfLayerHandle& l, const SdfPath& path, const PcpNodeRef& node)
{
    json e{{"layer", l->IsAnonymous() ? l->GetDisplayName() : fileOf(l)}, {"path", path.GetString()}, {"arc", arcName(node.GetArcType())}};
    // Raw pointers: under C++20, TfWeakPtr == TfRefPtr picks Tf's reversed friend operator, which calls itself forever.
    if (get_pointer(l) == get_pointer(s.edit))
        e["role"] = "edits";
    else if (l == s.stage->GetSessionLayer())
        e["role"] = "session";
    else
        for (const std::string& sub : std::vector<std::string>(s.edit->GetSubLayerPaths()))
            if (SdfLayer::FindRelativeToLayer(s.edit, sub) == l)
                e["role"] = "added";
    return e;
}

json primOpinions(const Stage& s, const UsdPrim& prim)
{
    json out = json::array();
    forEachSite(prim, [&](const SdfLayerHandle& l, const SdfPath& path, const PcpNodeRef& node) {
        const SdfPrimSpecHandle spec = l->GetPrimAtPath(path);
        if (!spec)
            return;
        json e = opinionSite(s, l, path, node);
        e["specifier"] = spec->GetSpecifier() == SdfSpecifierDef ? "def" : spec->GetSpecifier() == SdfSpecifierClass ? "class" : "over";
        if (!spec->GetTypeName().IsEmpty())
            e["type"] = spec->GetTypeName().GetString();
        if (spec->HasActive())
            e["active"] = spec->GetActive();
        if (spec->HasKind())
            e["kind"] = spec->GetKind().GetString();
        if (spec->HasInstanceable())
            e["instanceable"] = spec->GetInstanceable();
        json variants = json::object();
        for (const auto& [set, choice] : spec->GetVariantSelections())
            variants[set] = choice;
        if (!variants.empty())
            e["variantSelections"] = variants;
        out.push_back(e);
    });
    return out;
}

std::string listOpText(const SdfTargetsProxy& op)
{
    std::string t;
    const auto items = [&](const char* how, const SdfPathVector& paths) {
        if (paths.empty())
            return;
        t += (t.empty() ? "" : " ") + std::string(how) + " [";
        for (size_t i = 0; i < paths.size(); ++i)
            t += (i ? ", " : "") + paths[i].GetString();
        t += "]";
    };
    if (op.IsExplicit())
        items("", op.GetExplicitItems());
    items("prepend", op.GetPrependedItems());
    items("append", op.GetAppendedItems());
    items("delete", op.GetDeletedItems());
    return t.empty() ? std::string("[]") : TfStringTrim(t);
}

/**
 * Every opinion on a property, strongest first. For an attribute, wins marks the opinion its value comes from (the
 * strongest with a default or time samples), and losesTo names the layer that beats an opinion of Lila's edit layer.
 */
json propertyOpinions(const Stage& s, const UsdProperty& p, bool full)
{
    json opinions = json::array();
    bool won = false, editLoses = false;
    std::string winner;
    forEachSite(p.GetPrim(), [&](const SdfLayerHandle& l, const SdfPath& path, const PcpNodeRef& node) {
        const SdfPropertySpecHandle spec = l->GetPropertyAtPath(path.AppendProperty(p.GetName()));
        if (!spec)
            return;
        json e = opinionSite(s, l, path, node);
        if (const SdfAttributeSpecHandle at = TfDynamic_cast<SdfAttributeSpecHandle>(spec))
        {
            const size_t samples = l->GetNumTimeSamplesForPath(spec->GetPath());
            if (at->HasDefaultValue())
            {
                const VtValue v = at->GetDefaultValue();
                if (v.IsHolding<SdfValueBlock>())
                    e["value"] = "None (blocked)";
                else if (!full && v.IsArrayValued() && v.GetArraySize() > 64)
                    e["value"] = std::format("[{} values]", v.GetArraySize());
                else
                    e["value"] = valueText(v);
            }
            if (samples > 0)
                e["timeSamples"] = samples;
            if (at->HasDefaultValue() || samples > 0)
            {
                if (!won)
                {
                    won = true;
                    e["wins"] = true;
                    winner = e["layer"].get<std::string>();
                }
                else if (get_pointer(l) == get_pointer(s.edit))
                    editLoses = true;
            }
        }
        else if (const SdfRelationshipSpecHandle rel = TfDynamic_cast<SdfRelationshipSpecHandle>(spec))
            e["targets"] = listOpText(rel->GetTargetPathList());
        if (spec->GetVariability() == SdfVariabilityUniform)
            e["uniform"] = true;
        opinions.push_back(e);
    });
    json out{{"name", p.GetName().GetString()}, {"opinions", opinions}};
    if (p.Is<UsdAttribute>() && !won)
        out["fallback"] = true;
    if (editLoses)
        out["losesTo"] = winner;
    return out;
}

json cmdOpinions(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("usdStageOpinions: object required");
    Graph& g = graph();
    auto [sp, link] = linkedObject(g, a.positional[0].get<std::string>(), "usdStageOpinions");
    VariantScope scope(*sp, link);
    const SdfPath path = stagePath(link);
    const UsdPrim prim = sp->stage->GetPrimAtPath(path);
    if (!prim)
        throw CommandError("usdStageOpinions: the stage has no prim at " + path.GetString());
    const bool full = a.get<bool>("full", false);
    json props = json::array();
    if (a.positional.size() > 1 && a.positional[1].is_string())
    {
        const UsdProperty p = prim.GetProperty(TfToken(a.positional[1].get<std::string>()));
        if (!p)
            throw CommandError("usdStageOpinions: " + path.GetString() + " has no property " + a.positional[1].get<std::string>());
        props.push_back(propertyOpinions(*sp, p, full));
    }
    else
        for (const UsdProperty& p : prim.GetProperties())
            props.push_back(propertyOpinions(*sp, p, full));
    return {{"prim", path.GetString()}, {"specs", primOpinions(*sp, prim)}, {"properties", props}};
}

/** Adds losesTo to a property usdStagePrim lists, when a stronger layer beats the edit layer's opinion on it. */
void markLoss(const Stage& s, const UsdProperty& p, json& e)
{
    if (const json o = propertyOpinions(s, p, false); o.contains("losesTo"))
        e["losesTo"] = o["losesTo"];
}

json cmdPrim(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("usdStagePrim: object required");
    Graph& g = graph();
    auto [sp, link] = linkedObject(g, a.positional[0].get<std::string>(), "usdStagePrim");
    VariantScope scope(*sp, link);
    const SdfPath path = stagePath(link);
    const UsdPrim prim = sp->stage->GetPrimAtPath(path);
    if (!prim)
        throw CommandError("usdStagePrim: the stage has no prim at " + path.GetString());
    const UsdTimeCode t = currentCode(*sp, g);
    const bool full = a.get<bool>("full", false);
    json props = json::array();
    for (const UsdProperty& p : prim.GetProperties())
    {
        props.push_back(propertyJson(p, t, full));
        markLoss(*sp, p, props.back());
    }
    json schemas = json::array();
    for (const TfToken& api : prim.GetAppliedSchemas())
        schemas.push_back(api.GetString());
    json out = {{"prim", path.GetString()},
                {"type", prim.GetTypeName().GetString()},
                {"schemas", schemas},
                {"instanceProxy", prim.IsInstanceProxy()},
                {"properties", props}};
    if (const SdfPath level = variantPrefix(link); !level.IsEmpty())
        out["level"] = level.GetVariantSelection().second;
    return out;
}

json cmdSetProperty(const Args& a)
{
    if (a.positional.size() < 3 || !a.positional[0].is_string() || !a.positional[1].is_string())
        throw CommandError("usdStageSetProperty: object, property and value required");
    Graph& g = graph();
    auto [sp, link] = linkedObject(g, a.positional[0].get<std::string>(), "usdStageSetProperty");
    Stage& s = *sp;
    const SdfPath path = stagePath(link);
    const std::string name = a.positional[1].get<std::string>();
    const json& value = a.positional[2];
    UsdPrim prim;
    {
        VariantScope probe(s, link);
        prim = s.stage->GetPrimAtPath(path);
        if (!prim)
            throw CommandError("usdStageSetProperty: the stage has no prim at " + path.GetString());
        if (prim.IsInstanceProxy())
            throw CommandError("usdStageSetProperty: " + path.GetString() + " is inside an instance, whose prims can't be edited");
    }
    if (!SdfPath::IsValidNamespacedIdentifier(name))
        throw CommandError("usdStageSetProperty: " + name + " isn't a valid property name");
    StageStep step(s);
    logWarnings(s, g, sync(s, g));
    std::optional<VariantScope> scope;
    std::optional<UsdEditContext> target;
    if (const SdfPath level = variantPrefix(link); !level.IsEmpty())
    {
        scope.emplace(s, link);
        target.emplace(s.stage, UsdEditTarget::ForLocalDirectVariant(s.edit, level));
    }
    prim = s.stage->GetPrimAtPath(path);
    json result;
    if (UsdRelationship rel = prim.GetRelationship(TfToken(name)))
    {
        SdfPathVector targets;
        std::vector<std::string> items;
        if (value.is_array())
            for (const json& v : value)
                items.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        else if (value.is_string())
            items = TfStringTokenize(value.get<std::string>(), ", []");
        for (std::string item : items)
        {
            if (item.starts_with('<') && item.ends_with('>'))
                item = item.substr(1, item.size() - 2);
            if (!SdfPath::IsValidPathString(item))
                throw CommandError("usdStageSetProperty: " + item + " isn't a path");
            targets.push_back(SdfPath(item));
        }
        if (!rel.SetTargets(targets))
            throw CommandError("usdStageSetProperty: can't set " + name);
    }
    else
    {
        UsdAttribute at = prim.GetAttribute(TfToken(name));
        if (!at)
        {
            const std::string type = a.get<std::string>("type", "");
            const SdfValueTypeName tn = SdfSchema::GetInstance().FindType(TfToken(type));
            if (type.empty() || !tn)
                throw CommandError("usdStageSetProperty: " + path.GetString() + " has no property " + name + "; give type= to add it");
            at = prim.CreateAttribute(TfToken(name), tn, true);
        }
        const VtValue v = parseValue(at.GetTypeName(), value);
        const UsdTimeCode t = at.GetNumTimeSamples() > 0 ? currentCode(s, g) : UsdTimeCode::Default();
        if (!at.Set(v, t))
            throw CommandError("usdStageSetProperty: can't set " + name + " to " + valueText(v));
    }
    target.reset();
    scope.reset();
    if (link.ContainsPrimVariantSelection())
        s.changed.insert(lodGroupOf(link));
    flush(s, g, false);
    writeAttrs(s, g);
    step.done();
    if (const UsdPrim now = s.stage->GetPrimAtPath(path))
        if (const UsdProperty p = now.GetProperty(TfToken(name)))
        {
            result = propertyJson(p, currentCode(s, g), false);
            markLoss(s, p, result);
            if (result.contains("losesTo"))
                log::warn("{}: the edit to {} doesn't show; {} has a stronger opinion", path.GetString(), name,
                             result["losesTo"].get<std::string>());
        }
    return result;
}

json cmdUpdate(const Args&) { return updateAll(graph()); }

// Events from the front.

void sceneOpened()
{
    Graph& g = graph();
    stages().clear();
    for (NodeId id : stageNodes(g))
    {
        try
        {
            NoUndo quiet;
            Stage& s = ensure(g, id);
            // Saved after a comparison, so the nodes are what the baseline says.
            s.full = false;
            s.pending.clear();
            if (std::isnan(s.start) || std::isnan(s.end))
            {
                s.comparedStart = scene::anim::playback().animStart;
                s.comparedEnd = scene::anim::playback().animEnd;
            }
            const std::vector<SdfLayerHandle> changed = changedLayers(s);
            if (changed.empty())
                continue;
            std::vector<std::string> warnings;
            {
                Busy busy(s);
                for (const SdfLayerHandle& l : changed)
                    l->Reload(false);
            }
            log::info("{}: its USD files changed since the scene was saved; reloading", g.node(id)->name());
            rebuild(s, g, SdfPath::AbsoluteRootPath(), true, warnings);
            s.full = true;
            rebaseline(s, g, {SdfPath::AbsoluteRootPath()}, {}, warnings);
            s.stamps = stampsOf(s);
            writeAttrs(s, g);
            logWarnings(s, g, std::move(warnings));
        }
        catch (const std::exception& e)
        {
            log::error("{}: {}", g.node(id)->name(), e.what());
        }
    }
}

void beforeSave()
{
    Graph& g = graph();
    prune(g);
    for (NodeId id : stageNodes(g))
    {
        try
        {
            NoUndo quiet;
            Stage& s = ensure(g, id);
            std::vector<std::string> warnings = sync(s, g);
            if (s.mode != Stage::Mode::Embedded && s.edit->IsDirty() && !s.edit->Save())
                warnings.push_back("can't write the edit layer " + fileOf(s.edit));
            s.stamps = stampsOf(s);
            writeAttrs(s, g);
            logWarnings(s, g, std::move(warnings));
        }
        catch (const std::exception& e)
        {
            log::error("{}: {}", g.node(id)->name(), e.what());
        }
    }
}
} // namespace

bool run(std::string_view name, const Args& a, json& result)
{
    if (name == "usdStageOpen")
        result = cmdOpen(a);
    else if (name == "usdStageReload")
        result = cmdReload(a);
    else if (name == "usdStageUpdate")
        result = cmdUpdate(a);
    else if (name == "usdStageVariant")
        result = cmdVariant(a);
    else if (name == "usdStageLoad")
        result = cmdLoad(a);
    else if (name == "usdStageExtract")
        result = cmdExtract(a);
    else if (name == "usdStageWriteShot")
        result = cmdWriteShot(a);
    else if (name == "usdStageAddLayer")
        result = cmdAddLayer(a);
    else if (name == "usdStageRemoveLayer")
        result = cmdRemoveLayer(a);
    else if (name == "usdStageInfo")
        result = cmdInfo(a);
    else if (name == "usdStagePrim")
        result = cmdPrim(a);
    else if (name == "usdStageOpinions")
        result = cmdOpinions(a);
    else if (name == "usdStageSetProperty")
        result = cmdSetProperty(a);
    else if (name == "usdStage.sceneOpened")
        sceneOpened();
    else if (name == "usdStage.sceneNew")
        stages().clear();
    else if (name == "usdStage.beforeSave")
        beforeSave();
    else if (name == "usdStage.idle")
    {
        try
        {
            updateAll(graph());
        }
        catch (const std::exception& e)
        {
            log::warn("usdStage: {}", e.what());
        }
    }
    else
        return false;
    return true;
}

UsdStagePtr liveStage(const std::string& node)
{
    Graph& g = graph();
    Args a;
    if (!node.empty())
        a.positional.push_back(node);
    return ensure(g, stageArg(g, a, "lila.usd.stage", 0)).stage;
}
std::vector<HydraView> hydraViews(Graph& g)
{
    std::vector<HydraView> out;
    for (auto& [id, s] : stages())
    {
        const Node* n = g.node(id);
        if (!n || !s->stage)
            continue;
        HydraView v;
        v.node = id;
        v.stage = s->stage;
        const Axes axes = makeAxes(UsdGeomGetStageUpAxis(s->stage) != UsdGeomTokens->y, UsdGeomGetStageMetersPerUnit(s->stage));
        v.toWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix) * axes.toLila;
        v.toLila = axes.toLila;
        v.turnedToLila = glm::dmat4(axes.basis) * axes.toLila;
        v.timeCodesPerSecond = s->stage->GetTimeCodesPerSecond() > 0.0 ? s->stage->GetTimeCodesPerSecond() : 24.0;
        for (const auto& [node, link] : s->primOf)
            if (const Node* ln = g.node(node))
                v.links.push_back({stagePath(link), node, ln->type().isA("camera") || ln->type().isA("light")});
        std::set<SdfPath> realized;
        for (const auto& [node, inst] : s->instanceOf)
            realized.insert(stagePath(inst.first));
        v.realized.assign(realized.begin(), realized.end());
        out.push_back(std::move(v));
    }
    return out;
}
scene::NodeId nodeForPrim(std::string_view path)
{
    if (path.empty() || path.front() != '/' || !SdfPath::IsValidPathString(std::string(path)))
        return kNoNode;
    const SdfPath prim(std::string{path});
    Graph& g = Graph::current();
    NodeId found = kNoNode;
    for (const auto& [id, s] : stages())
    {
        auto it = s->nodeOf.find(prim);
        if (it == s->nodeOf.end() || !g.node(it->second))
            continue;
        if (g.isSelected(id))
            return it->second;
        if (found == kNoNode)
            found = it->second;
    }
    return found;
}
} // namespace lila::usd::stage
