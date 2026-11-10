#include "maya/MayaRig.h"

#include "core/Profile.h"
#include "deform/Deform.h"
#include "deform/Surface.h"
#include "geo/Attribute.h"
#include "ops/Command.h"
#include "scene/Constraint.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/Ik.h"
#include "scene/MathNodes.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <format>
#include <map>
#include <numbers>

namespace lila::maya
{
namespace
{
using scene::Graph;
using scene::NodeId;
using scene::PlugRef;
using scene::PlugType;
using scene::PlugValue;
using scene::kNoNode;
using json = nlohmann::json;

constexpr double kDeg = std::numbers::pi / 180.0;

enum class Unit : uint8_t
{
    None,
    Linear,
    Angle,
};

/**
 * Where a Maya plug's value lives in Lila: the Maya value (in internal units) is the Lila plug's value times factor.
 * A Vec3's Maya component c is Lila component comp[c] times sign[c]. A plug-less Matrix end is a constant identity.
 */
struct End
{
    enum Kind : uint8_t
    {
        None,
        Scalar,
        Vec3,
        Matrix,
    };
    Kind kind = None;
    PlugRef plug;
    double factor = 1.0;
    int comp[3] = {0, 1, 2};
    double sign[3] = {1.0, 1.0, 1.0};
    bool lilaSpace = false;
    Unit unit = Unit::None;

    bool identityComps() const { return comp[0] == 0 && comp[1] == 1 && comp[2] == 2 && sign[0] == 1.0 && sign[1] == 1.0 && sign[2] == 1.0; }
    End component(int c) const
    {
        End e = *this;
        e.kind = Scalar;
        e.plug.component = comp[c];
        e.factor = factor * sign[c];
        return e;
    }
};

bool near1(double r) { return std::abs(r - 1.0) < 1e-9; }

const std::set<std::string_view>& mathTypes()
{
    static const std::set<std::string_view> k = {
        "multiplyDivide", "plusMinusAverage", "condition", "reverse", "clamp", "setRange", "remapValue", "blendColors", "blendTwoAttr",
        "blendWeighted", "pairBlend", "choice", "distanceBetween", "vectorProduct", "multDoubleLinear", "addDoubleLinear", "multMatrix",
        "addMatrix", "wtAddMatrix", "inverseMatrix", "transposeMatrix", "holdMatrix", "fourByFourMatrix", "decomposeMatrix", "composeMatrix",
        "blendMatrix", "pickMatrix", "aimMatrix", "addDL", "multDL", "closestPointOnMesh"};
    return k;
}

/** The Lila math node a Maya utility type converts to: its own name, or the older name Maya 2026 shortened. */
std::string_view mathType(std::string_view mayaType)
{
    if (mayaType == "addDL")
        return "addDoubleLinear";
    if (mayaType == "multDL")
        return "multDoubleLinear";
    return mayaType;
}

const std::set<std::string_view>& constraintTypes()
{
    static const std::set<std::string_view> k = {"parentConstraint", "pointConstraint", "orientConstraint", "aimConstraint", "scaleConstraint",
                                                 "poleVectorConstraint"};
    return k;
}

const std::set<std::string_view>& nonlinearHandles()
{
    static const std::set<std::string_view> k = {"deformBend", "deformTwist", "deformSquash", "deformSine", "deformWave", "deformFlare"};
    return k;
}

/** Deformers with a Lila counterpart, by Maya type. */
const std::map<std::string_view, std::string_view>& deformerMap()
{
    static const std::map<std::string_view, std::string_view> k = {
        {"cluster", "softMod"}, {"softMod", "softMod"}, {"ffd", "ffd"},   {"deltaMush", "deltaMush"},   {"tension", "deltaMush"},
        {"wrap", "wrap"},       {"proximityWrap", "wrap"}, {"jiggle", "jiggle"}, {"shrinkWrap", "shrinkwrap"}, {"nonLinear", ""},
        {"textureDeformer", "textureDeformer"}};
    return k;
}

/** A Maya compound array's short name to its long one, which names the Lila attr group. */
std::string groupName(std::string_view type, std::string_view seg)
{
    if (type == "remapValue")
        return seg == "vl" ? "value" : seg == "cl" ? "color" : std::string(seg);
    if (type == "blendMatrix")
        return seg == "tgt" ? "target" : std::string(seg);
    if (type == "wtAddMatrix")
        return seg == "i" ? "wtMatrix" : std::string(seg);
    return std::string(seg);
}

/** The unit of a math node attr, by its Lila name: what constants convert by and what set driven keys read it in. */
Unit mathUnit(std::string_view type, std::string_view attr)
{
    if (type == "pairBlend")
        return attr.starts_with("inRotate") || attr == "outRotate" ? Unit::Angle : attr.starts_with("inTranslate") || attr == "outTranslate" ? Unit::Linear : Unit::None;
    if (type == "composeMatrix")
        return attr == "inputRotate" ? Unit::Angle : attr == "inputTranslate" ? Unit::Linear : Unit::None;
    if (type == "decomposeMatrix")
        return attr == "outputRotate" ? Unit::Angle : attr == "outputTranslate" ? Unit::Linear : Unit::None;
    if (type == "distanceBetween")
        return attr == "point1" || attr == "point2" || attr == "distance" ? Unit::Linear : Unit::None;
    if (type == "closestPointOnMesh")
        return attr == "inPosition" || attr == "position" ? Unit::Linear : Unit::None;
    if (type == "addDoubleLinear" || type == "addDL")
        return Unit::Linear;
    if (type == "multDoubleLinear" || type == "multDL")
        return attr == "input1" || attr == "output" ? Unit::Linear : Unit::None;
    return Unit::None;
}

struct LilaAttr
{
    int attr = -1;
    int component = -1;
    int element = -1;
};

/** A Maya attribute path on a node of the given Lila type: the attr or Vec3 child its last segment names, the element its last index picks. */
std::optional<LilaAttr> resolveAttr(const scene::NodeType& t, std::string_view mayaType, std::string_view path)
{
    std::vector<AttrSeg> segs = splitAttr(path);
    if (segs.empty())
        return std::nullopt;
    LilaAttr out;
    for (const AttrSeg& s : segs)
        if (s.lo >= 0)
            out.element = s.lo;
    const std::string& leaf = segs.back().name;
    const std::string group = segs.size() > 1 ? groupName(mayaType, segs[segs.size() - 2].name) : std::string();
    for (int i = 0; i < int(t.attrs.size()); ++i)
    {
        const scene::AttrDef& a = t.attrs[size_t(i)];
        const size_t dot = a.name.find('.');
        const bool grouped = dot != std::string::npos;
        if (a.name == leaf || a.shortName == leaf || (grouped && a.name.substr(dot + 1) == leaf && (group.empty() || a.name.substr(0, dot) == group)))
        {
            out.attr = i;
            return out;
        }
        if (a.type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                if (a.childNames[size_t(c)] == leaf || a.childShort[size_t(c)] == leaf)
                {
                    out.attr = i;
                    out.component = c;
                    return out;
                }
    }
    return std::nullopt;
}

/** Values of a Maya weight list: "wl[k].w[a:b]" dense values, by Maya vertex. */
std::map<int, double> weightList(const MNode& n, int k)
{
    std::map<int, double> out;
    for (const AttrValue& a : n.attrs)
    {
        std::vector<AttrSeg> s = splitAttr(a.attr);
        if (s.size() != 2 || !oneOf(s[0].name, {"wl", "weightList"}) || !oneOf(s[1].name, {"w", "weights"}) || s[0].lo != k || s[1].lo < 0)
            continue;
        for (size_t i = 0; i < a.nums.size(); ++i)
            out[s[1].lo + int(i)] = a.nums[i];
    }
    return out;
}

/** "vtx[0:3]" and friends to Maya vertex indices; all of them for "vtx[*]". Sets *all for the whole-mesh forms. */
void addComponents(const std::vector<std::string>& comps, std::set<int>& out, bool* all)
{
    for (const std::string& c : comps)
    {
        std::string kind;
        int lo = 0, hi = 0;
        if (!parseComponent(c, &kind, &lo, &hi))
            continue;
        if (kind != "vtx" && kind != "pt" && kind != "cv")
            continue;
        if (hi < 0)
        {
            *all = true;
            continue;
        }
        for (int v = lo; v <= hi; ++v)
            out.insert(v);
    }
}

std::string plugText(const FileIndex& ix, const Plug& p) { return std::format("{}.{}", shortName(ix.node(p.node).name), p.attr); }

/** A Maya time or driven-key curve as keys over the driver's Lila value (key times scaled by fps / factor). */
scene::anim::Curve overDriver(scene::anim::Curve c, double fps, double factor)
{
    for (scene::anim::Key& k : c.keys)
    {
        k.time = k.time * fps / factor;
        for (double* a : {&k.inAngle, &k.outAngle})
            *a = std::atan(std::tan(*a * kDeg) * factor) / kDeg;
        if (factor < 0.0)
        {
            std::swap(k.in, k.out);
            std::swap(k.inAngle, k.outAngle);
        }
    }
    if (factor < 0.0)
        std::swap(c.pre, c.post);
    c.sort();
    return c;
}
} // namespace

struct RigBuilder::Impl
{
    RigContext c;
    Graph& g;
    const FileIndex& ix;

    std::map<int, NodeId> math;                      // Maya utility node -> Lila node
    std::map<int, int> choiceKind;                   // choice node -> End::Kind of its inputs
    std::map<int, NodeId> constraints;               // Maya constraint -> Lila node
    std::map<std::pair<int, std::string>, int> aliasWeight; // (constraint, weight alias) -> target index
    std::map<int, NodeId> deformers;                 // Maya deformer -> Lila deformer (the first geometry's)
    std::map<std::pair<int, long long>, NodeId> curves; // (Maya curve, output scale) -> Lila curve or driven channel
    std::map<std::pair<NodeId, int>, NodeId> inverses; // (Lila transform, attr) -> inverseMatrix of it
    std::map<PlugRef, NodeId> packers;                 // array element -> the multiplyDivide gathering its components
    std::map<int, NodeId> pins;                      // follicle / uvPin coordinate -> surfacePin (by Maya node * 1024 + index)
    std::map<int, std::map<int, NodeId>> pinFrames;   // Maya pin node -> output index -> node giving the Maya frame
    std::map<int, NodeId> follicleXform;             // follicle shape -> decomposeMatrix of its frame
    std::set<std::pair<int, std::string>> userAttrs; // converted (node, long name)
    std::map<std::string, int> skipped;
    bool approx = false;

    explicit Impl(RigContext ctx) : c(std::move(ctx)), g(c.g), ix(c.ix) {}

    const MNode& node(int i) const { return ix.node(i); }
    std::string name(int i, const char* fallback) const { return lilaName(node(i).name, c.opts.nameSpace, fallback); }
    void skip(const std::string& what) { ++skipped[what]; }
    void warn(const std::string& what) { c.r.warnings.push_back(what); }

    NodeId lilaOf(int maya) const
    {
        auto it = c.transforms.find(maya);
        return it == c.transforms.end() ? kNoNode : it->second;
    }

    /** The Maya transform a shape node sits under, as its Lila node. */
    NodeId lilaParentOf(int shape) const { return shape >= 0 && ix.parent[size_t(shape)] >= 0 ? lilaOf(ix.parent[size_t(shape)]) : kNoNode; }

    // ---- small graph helpers --------------------------------------------------------------------------------

    int attrOf(NodeId id, std::string_view attr) const { return g.node(id)->type().attrIndex(attr); }

    NodeId makeNode(const char* type, const std::string& nm)
    {
        const scene::NodeType* t = scene::types::find(type);
        return g.createNode(*t, nm, kNoNode, true)->id();
    }

    /** Sets a numeric plug from a double, whatever its Lila type. */
    void setNumber(const PlugRef& p, double v)
    {
        if (p.component >= 0)
        {
            if (p.element >= 0)
            {
                glm::dvec3 cur = g.node(p.node)->elementOr<glm::dvec3>(p.attr, p.element);
                cur[p.component] = v;
                g.setValue({p.node, p.attr, -1, p.element}, cur);
            }
            else
                g.setValue(p, v);
            return;
        }
        switch (g.attr(p).type)
        {
        case PlugType::Bool: g.setValue(p, v != 0.0); break;
        case PlugType::Int:
        case PlugType::Enum: g.setValue(p, int64_t(std::llround(v))); break;
        case PlugType::Float: g.setValue(p, v); break;
        default: break;
        }
    }

    bool tryConnect(const PlugRef& src, const PlugRef& dst, std::string* why)
    {
        try
        {
            g.connect(src, dst);
            return true;
        }
        catch (const std::exception& e)
        {
            // Maya's graph is per plug, Lila's per node: a decomposeMatrix reading the rotate order of the transform it
            // drives would be a cycle here. A plug nothing drives can hand over its value instead.
            if (copyUndriven(src, dst, 1.0))
                return true;
            if (why)
                *why = e.what();
            return false;
        }
    }

    bool copyUndriven(const PlugRef& src, const PlugRef& dst, double ratio)
    {
        const scene::AttrDef& def = g.attr(src);
        if (!(def.flags & scene::AttrInput) || (def.flags & scene::AttrOutput) || g.source(src) || g.source({src.node, src.attr, -1}))
            return false;
        try
        {
            const PlugValue v = g.value(src);
            if (const auto* d = std::get_if<double>(&v))
                setNumber(dst, *d * ratio);
            else if (const auto* i = std::get_if<int64_t>(&v))
                setNumber(dst, double(*i) * ratio);
            else if (const auto* b = std::get_if<bool>(&v))
                setNumber(dst, *b ? ratio : 0.0);
            else if (dst.component < 0 && near1(ratio))
                g.setValue(dst, v);
            else
                return false;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    /**
     * Array elements take no component connections: an element driven by component gets a pass-through
     * multiplyDivide gathering its components, holding the element's value for the undriven ones.
     */
    PlugRef elementComponent(const PlugRef& p, std::string* why)
    {
        auto [it, fresh] = packers.try_emplace({p.node, p.attr, -1, p.element}, kNoNode);
        if (fresh)
        {
            const NodeId pack = makeNode("multiplyDivide", "vectorPack");
            g.setValue({pack, attrOf(pack, "operation"), -1}, int64_t(0));
            g.setValue({pack, attrOf(pack, "input1"), -1}, g.node(p.node)->elementOr<glm::dvec3>(p.attr, p.element));
            if (!tryConnect({pack, attrOf(pack, "output"), -1}, {p.node, p.attr, -1, p.element}, why))
                return {};
            it->second = pack;
        }
        return {it->second, attrOf(it->second, "input1"), p.component};
    }

    /** src's value times ratio into dst, through a unitConversion when the ratio isn't 1. */
    bool connectScalar(const PlugRef& src, PlugRef dst, double ratio, std::string* why)
    {
        if (dst.element >= 0 && dst.component >= 0)
        {
            dst = elementComponent(dst, why);
            if (!dst.valid())
                return false;
        }
        if (near1(ratio))
            return tryConnect(src, dst, why);
        NodeId uc = makeNode("unitConversion", "unitConversion");
        g.setValue({uc, attrOf(uc, "conversionFactor"), -1}, ratio);
        return tryConnect(src, {uc, attrOf(uc, "input"), -1}, why) && tryConnect({uc, attrOf(uc, "output"), -1}, dst, why);
    }

    /** A Lila-space matrix as Maya's, or the reverse: c3 on one side, its transpose on the other, through a multMatrix. */
    PlugRef changeSpace(const PlugRef& src, bool toLila)
    {
        NodeId mm = makeNode("multMatrix", "axisChange");
        const int in = attrOf(mm, "matrixIn");
        const glm::dmat4 c3(c.c3), c3t(glm::transpose(c.c3));
        g.setValue({mm, in, -1, 0}, toLila ? c3t : c3);
        g.connect(src, {mm, in, -1, 1});
        g.setValue({mm, in, -1, 2}, toLila ? c3 : c3t);
        return {mm, attrOf(mm, "matrixSum"), -1};
    }

    bool connectEnds(const End& s, const End& d, std::string* why)
    {
        if (d.kind == End::None || !d.plug.valid())
        {
            *why = "the destination is not converted";
            return false;
        }
        if (s.kind == End::Matrix || d.kind == End::Matrix)
        {
            if (s.kind != d.kind)
            {
                *why = "a matrix connects to a non-matrix plug";
                return false;
            }
            if (!s.plug.valid())
                return true;
            PlugRef src = s.plug;
            if (!c.identityAxes && s.lilaSpace != d.lilaSpace)
                src = changeSpace(src, d.lilaSpace);
            return tryConnect(src, d.plug, why);
        }
        if (!s.plug.valid())
        {
            *why = "the source is not converted";
            return false;
        }
        if (s.kind == End::Scalar && d.kind == End::Scalar)
            return connectScalar(s.plug, d.plug, s.factor / d.factor, why);
        if (s.kind != End::Vec3 || d.kind != End::Vec3)
        {
            *why = "a vector connects to a scalar plug";
            return false;
        }
        const bool same = std::equal(std::begin(s.comp), std::end(s.comp), std::begin(d.comp)) &&
                          std::equal(std::begin(s.sign), std::end(s.sign), std::begin(d.sign));
        if (same && near1(s.factor / d.factor))
            return tryConnect(s.plug, d.plug, why);
        End dd = d;
        bool ok = true;
        for (int k = 0; k < 3; ++k)
        {
            End a = s.component(k), b = dd.component(k);
            ok = connectScalar(a.plug, b.plug, a.factor / b.factor, why) && ok;
        }
        return ok;
    }

    // ---- ends ---------------------------------------------------------------------------------------------

    End vecEnd(NodeId id, int attr, int group, Unit unit) const
    {
        End e;
        e.kind = End::Vec3;
        e.plug = {id, attr, -1};
        e.unit = unit;
        e.factor = unit == Unit::Angle ? kDeg : 1.0;
        for (int k = 0; k < 3; ++k)
        {
            e.comp[k] = c.axes.to[k];
            e.sign[k] = group == 2 ? 1.0 : c.axes.factor(group, k);
        }
        return e;
    }

    End matrixEnd(const PlugRef& p, bool lilaSpace) const
    {
        End e;
        e.kind = End::Matrix;
        e.plug = p;
        e.lilaSpace = lilaSpace;
        return e;
    }

    PlugRef inverseOf(NodeId id, int attr)
    {
        auto [it, fresh] = inverses.try_emplace({id, attr}, kNoNode);
        if (fresh)
        {
            it->second = makeNode("inverseMatrix", g.node(id)->name() + "_inverse");
            g.connect({id, attr, -1}, {it->second, attrOf(it->second, "inputMatrix"), -1});
        }
        return {it->second, attrOf(it->second, "outputMatrix"), -1};
    }

    /** A plug of a converted transform, joint or ikHandle. */
    End transformEnd(int maya, NodeId id, const std::string& attr)
    {
        using scene::Transform;
        std::vector<AttrSeg> segs = splitAttr(attr);
        if (segs.empty())
            return {};
        const std::string& a = segs.back().name;
        struct Group
        {
            std::array<std::string_view, 2> names;
            int attr;
            int group; // 0 translate-like, 1 rotate-like, 2 scale-like
            Unit unit;
        };
        const bool joint = scene::rig::isJoint(*g.node(id));
        const bool handle = g.node(id)->type().isA("ikHandle");
        const Group groups[] = {
            {{"t", "translate"}, Transform::kTranslate, 0, Unit::Linear},
            {{"r", "rotate"}, Transform::kRotate, 1, Unit::Angle},
            {{"s", "scale"}, Transform::kScale, 2, Unit::None},
            {{"rp", "rotatePivot"}, Transform::kRotatePivot, 0, Unit::Linear},
            {{"sp", "scalePivot"}, Transform::kScalePivot, 0, Unit::Linear},
            {{"rpt", "rotatePivotTranslate"}, Transform::kRotatePivotTranslate, 0, Unit::Linear},
            {{"spt", "scalePivotTranslate"}, Transform::kScalePivotTranslate, 0, Unit::Linear},
            {{"ra", "rotateAxis"}, Transform::kRotateAxis, 1, Unit::Angle},
            {{"jo", "jointOrient"}, joint ? int(scene::Joint::kJointOrient) : -1, 1, Unit::Angle},
            {{"is", "inverseScale"}, joint ? int(scene::Joint::kInverseScale) : -1, 2, Unit::None},
            {{"pv", "poleVector"}, handle ? int(scene::IkHandle::kPoleVector) : -1, 0, Unit::None},
        };
        static const char* kAxes[3][2] = {{"x", "X"}, {"y", "Y"}, {"z", "Z"}};
        for (const Group& gr : groups)
        {
            if (gr.attr < 0)
                continue;
            End v = vecEnd(id, gr.attr, gr.group, gr.unit);
            if (a == gr.names[0] || a == gr.names[1])
                return v;
            for (int k = 0; k < 3; ++k)
                for (std::string_view n : gr.names)
                    if (a == std::string(n) + kAxes[k][n.size() <= 3 ? 0 : 1])
                        return v.component(k);
        }
        if (oneOf(a, {"v", "visibility"}))
            return {.kind = End::Scalar, .plug = {id, Transform::kVisibility, -1}};
        if (oneOf(a, {"ro", "rotateOrder"}) && (c.identityAxes || ix.input(maya, {"ro", "rotateOrder"}) == std::nullopt))
            return {.kind = End::Scalar, .plug = {id, Transform::kRotateOrder, -1}};
        if (oneOf(a, {"wm", "worldMatrix"}))
            return matrixEnd({id, Transform::kWorldMatrix, -1}, true);
        if (oneOf(a, {"m", "matrix", "xm", "xformMatrix"}))
            return matrixEnd({id, Transform::kMatrix, -1}, true);
        if (oneOf(a, {"opm", "offsetParentMatrix"}))
            return matrixEnd({id, Transform::kOffsetParentMatrix, -1}, true);
        if (oneOf(a, {"wim", "worldInverseMatrix"}))
            return matrixEnd(inverseOf(id, Transform::kWorldMatrix), true);
        if (oneOf(a, {"im", "inverseMatrix"}))
            return matrixEnd(inverseOf(id, Transform::kMatrix), true);
        if (oneOf(a, {"pm", "parentMatrix", "pim", "parentInverseMatrix"}))
        {
            NodeId p = g.parent(id);
            if (p == kNoNode)
                return matrixEnd({}, true);
            const bool inverse = oneOf(a, {"pim", "parentInverseMatrix"});
            return matrixEnd(inverse ? inverseOf(p, Transform::kWorldMatrix) : PlugRef{p, Transform::kWorldMatrix, -1}, true);
        }
        if (handle && oneOf(a, {"twi", "twist"}))
            return {.kind = End::Scalar, .plug = {id, scene::IkHandle::kPoleAngle, -1}, .factor = kDeg, .unit = Unit::Angle};
        if (handle && oneOf(a, {"ikb", "ikBlend"}))
            return {.kind = End::Scalar, .plug = {id, scene::IkHandle::kIkBlend, -1}};
        // User attributes, by long or short name.
        for (const AddedAttr& added : node(maya).added)
            if ((added.longName == a || added.shortName == a) && userAttrs.contains({maya, added.longName}))
            {
                const int idx = attrOf(id, added.longName);
                const bool angle = added.type == "doubleAngle";
                return {.kind = End::Scalar, .plug = {id, idx, -1}, .factor = angle ? kDeg : 1.0,
                        .unit = angle ? Unit::Angle : added.type == "doubleLinear" ? Unit::Linear : Unit::None};
            }
        return {};
    }

    /** A plug of a converted math node. */
    End mathEnd(int maya, NodeId id, const std::string& attr)
    {
        const scene::Node* n = g.node(id);
        std::string path = attr;
        if (node(maya).type == "choice")
        {
            std::vector<AttrSeg> segs = splitAttr(attr);
            const int kind = choiceKind.contains(maya) ? choiceKind.at(maya) : End::Scalar;
            if (!segs.empty() && oneOf(segs.back().name, {"i", "input"}))
                path = std::format("{}[{}]", kind == End::Matrix ? "inputMatrix" : kind == End::Vec3 ? "inputVector" : "input", std::max(segs.back().lo, 0));
            else if (!segs.empty() && oneOf(segs.back().name, {"o", "output"}))
                path = kind == End::Matrix ? "outputMatrix" : kind == End::Vec3 ? "outputVector" : "output";
        }
        auto la = resolveAttr(n->type(), node(maya).type, path);
        if (!la)
            return {};
        const scene::AttrDef& def = n->type().attrs[size_t(la->attr)];
        End e;
        e.plug = {id, la->attr, la->component, (def.flags & scene::AttrArray) ? std::max(la->element, -1) : -1};
        const std::string_view baseName = def.name;
        e.unit = mathUnit(node(maya).type, baseName);
        e.factor = e.unit == Unit::Angle ? kDeg : 1.0;
        if (la->component >= 0)
            e.kind = End::Scalar;
        else if (def.type == PlugType::Vec3)
            e.kind = End::Vec3;
        else if (def.type == PlugType::Matrix)
            e.kind = End::Matrix;
        else if (def.type == PlugType::Float || def.type == PlugType::Int || def.type == PlugType::Bool || def.type == PlugType::Enum)
            e.kind = End::Scalar;
        return e;
    }

    /** The End a plug of any converted Maya node stands for, converting the node when it hasn't been. */
    End endOf(const Plug& p, std::string* why)
    {
        const MNode& n = node(p.node);
        if (NodeId id = lilaOf(p.node); id != kNoNode)
        {
            End e = transformEnd(p.node, id, p.attr);
            if (e.kind == End::None)
                *why = std::format("{}.{} is not converted", g.node(id)->path(), p.attr);
            return e;
        }
        if (mathTypes().contains(n.type))
        {
            NodeId id = mathNode(p.node);
            if (id == kNoNode)
            {
                *why = std::format("{} '{}' could not be converted", n.type, shortName(n.name));
                return {};
            }
            End e = mathEnd(p.node, id, p.attr);
            if (e.kind == End::None)
                *why = std::format("{}.{} has no Lila counterpart", shortName(n.name), p.attr);
            return e;
        }
        if (constraintTypes().contains(n.type))
            return constraintEnd(p, why);
        if (n.type == "follicle" || n.type == "uvPin" || n.type == "proximityPin")
            return pinEnd(p, why);
        if (std::vector<PlugRef> b = c.blendPlugs(p.node, p.attr); !b.empty())
            return {.kind = End::Scalar, .plug = b.front()};
        if (auto it = deformers.find(p.node); it != deformers.end())
            if (End e = deformerEnd(p.node, it->second, p.attr); e.kind != End::None)
                return e;
        if (n.type == "expression")
        {
            c.handled.insert(p.node);
            *why = std::format("expression '{}': MEL expressions are not converted, the channel keeps its value", shortName(n.name));
            return {};
        }
        *why = std::format("{} '{}' is not converted", n.type, shortName(n.name));
        return {};
    }

    // ---- connections ----------------------------------------------------------------------------------------

    /** Maya's src drives d: unitConversions fold into the factor, curves are made to land in d's units. */
    bool connectInto(const Plug& src, End d, std::string* why, int depth = 0)
    {
        if (src.node < 0 || depth > 32)
        {
            *why = "the source is missing";
            return false;
        }
        const MNode& sn = node(src.node);
        if (sn.type == "unitConversion")
        {
            c.handled.insert(src.node);
            auto in = ix.input(src.node, {"i", "input"});
            if (!in)
            {
                *why = std::format("unitConversion '{}' has no input", shortName(sn.name));
                return false;
            }
            const double cf = num(sn, {"cf", "conversionFactor"}, 1.0);
            if (cf == 0.0)
                return true;
            d.factor /= cf;
            return connectInto(*in, d, why, depth + 1);
        }
        if (sn.type.starts_with("animCurve") && oneOf(src.attr, {"o", "output"}))
            return connectCurve(src.node, d, why);
        End s = endOf(src, why);
        if (s.kind == End::None)
            return false;
        if (staticSource(src))
        {
            // A rotate order copies as Maya's value: networks read it in Maya's axes.
            if (std::vector<AttrSeg> segs = splitAttr(src.attr); !segs.empty() && oneOf(segs.front().name, {"ro", "rotateOrder"}) && d.plug.valid())
            {
                const auto order = scene::RotateOrder(std::clamp(int64_t(num(node(src.node), {"ro", "rotateOrder"}, 0.0)), int64_t(0), int64_t(5)));
                const bool lilaChannel = g.node(d.plug.node)->type().isA("transform") && d.plug.attr == scene::Transform::kRotateOrder;
                setNumber(d.plug, double(int64_t(lilaChannel ? c.axes.order(order) : order)));
                return true;
            }
            if (copyEnds(s, d))
                return true;
        }
        return connectEnds(s, d, why);
    }

    /**
     * A transform's rotate order, pivots, joint orient and rotate axis when nothing drives them in Maya: networks read
     * them from the transform they drive (a decomposeMatrix's rotate order), which Lila's per-node graph can't connect,
     * so their value is copied.
     */
    bool staticSource(const Plug& p) const
    {
        if (lilaOf(p.node) == kNoNode)
            return false;
        std::vector<AttrSeg> segs = splitAttr(p.attr);
        if (segs.empty())
            return false;
        static const std::set<std::string_view> k = {"ro", "rotateOrder", "rp", "rotatePivot", "sp", "scalePivot", "rpt", "rotatePivotTranslate",
                                                     "spt", "scalePivotTranslate", "jo", "jointOrient", "ra", "rotateAxis"};
        return k.contains(segs.front().name) && !ix.input(p.node, {p.attr});
    }

    bool copyEnds(const End& s, const End& d)
    {
        if (!s.plug.valid() || !d.plug.valid())
            return false;
        if (s.kind == End::Scalar && d.kind == End::Scalar)
            return copyUndriven(s.plug, d.plug, s.factor / d.factor);
        if (s.kind != End::Vec3 || d.kind != End::Vec3)
            return false;
        bool ok = true;
        for (int k = 0; k < 3; ++k)
        {
            End a = s.component(k), b = d.component(k);
            if (b.plug.element >= 0)
                return false;
            ok = copyUndriven(a.plug, b.plug, a.factor / b.factor) && ok;
        }
        return ok;
    }

    /** Factor from a Maya curve's file values to its internal ones. */
    double curveUnit(const std::string& type) const
    {
        const char kind = type.size() == 11 ? type[10] : 'U';
        return kind == 'L' ? c.lin : kind == 'A' ? c.ang * kDeg : 1.0;
    }

    /** A Maya anim curve or set driven key driving d. Its keys are scaled to land straight in d's units. */
    bool connectCurve(int cn, const End& d, std::string* why)
    {
        c.handled.insert(cn);
        const MNode& n = node(cn);
        if (d.kind != End::Scalar)
        {
            *why = std::format("curve '{}' drives a non-scalar plug", shortName(n.name));
            return false;
        }
        if (n.type.size() != 11 || (n.type[9] != 'T' && n.type[9] != 'U'))
        {
            *why = std::format("{} '{}' is not converted", n.type, shortName(n.name));
            return false;
        }
        const double scale = curveUnit(n.type) / d.factor;
        const auto key = std::pair{cn, std::llround(scale * 1e9)};
        NodeId id = kNoNode;
        if (auto it = curves.find(key); it != curves.end())
            id = it->second;
        else
        {
            std::optional<scene::anim::Curve> curve = c.readCurve(n, scale, 1.0, 1.0 / d.factor, &approx);
            if (!curve)
                return true;
            if (n.type[9] == 'T')
            {
                const char* type = d.unit == Unit::Angle ? "animCurveTA" : d.unit == Unit::Linear ? "animCurveTL" : "animCurveTU";
                id = g.createNode(*scene::types::find(type), name(cn, type), kNoNode, false)->id();
                scene::anim::setCurve(g, id, *curve);
                ++c.r.curves;
            }
            else
            {
                id = drivenKey(cn, *curve, why);
                if (id == kNoNode)
                    return false;
            }
            curves[key] = id;
        }
        const int out = n.type[9] == 'T' ? int(scene::anim::CurveNode::kOutput) : int(scene::drivers::kChannelOutput);
        return tryConnect({id, out, -1}, d.plug, why);
    }

    /** A set driven key as a drivenChannel reading its driver's Lila plug; keys in Lila's units of the driver. */
    NodeId drivenKey(int cn, const scene::anim::Curve& curve, std::string* why)
    {
        auto in = ix.input(cn, {"i", "input"});
        NodeId ch = scene::drivers::createChannel(g, PlugRef{}, PlugRef{}, curve, "", name(cn, "drivenChannel"));
        if (!in)
            return ch;
        // Through unitConversions the curve reads internal units; straight from a plug with a unit, its UI units.
        Plug driver = *in;
        double cf = 1.0;
        bool converted = false;
        for (int guard = 0; guard < 16 && node(driver.node).type == "unitConversion"; ++guard)
        {
            c.handled.insert(driver.node);
            cf *= num(node(driver.node), {"cf", "conversionFactor"}, 1.0);
            converted = true;
            auto up = ix.input(driver.node, {"i", "input"});
            if (!up)
                break;
            driver = *up;
        }
        End s = endOf(driver, why);
        if (s.kind != End::Scalar || !s.plug.valid())
        {
            if (s.kind != End::None)
                *why = std::format("set driven key '{}' reads a vector or matrix", shortName(node(cn).name));
            g.deleteNode(ch);
            return kNoNode;
        }
        const double ui = converted ? 1.0 : s.unit == Unit::Angle ? c.ang * kDeg : s.unit == Unit::Linear ? c.lin : 1.0;
        const double factor = s.factor * cf / ui;
        if (std::abs(factor) < 1e-15)
            return ch;
        scene::drivers::setChannelCurve(g, ch, overDriver(curve, c.fps, factor));
        if (!tryConnect(s.plug, {ch, scene::drivers::kInput, -1}, why))
        {
            g.deleteNode(ch);
            return kNoNode;
        }
        return ch;
    }

    bool connectPlugs(const Plug& src, const Plug& dst, std::string* why)
    {
        End d = endOf(dst, why);
        if (d.kind == End::None)
            return false;
        if (d.plug.valid() && d.kind != End::Matrix && g.source(d.plug))
            return true;
        return connectInto(src, d, why);
    }

    // ---- math nodes -----------------------------------------------------------------------------------------

    /** Copies a node's setAttr values onto its Lila node: plain, vector, matrix, ranged arrays and packed compound arrays. */
    void copyConstants(int maya, NodeId id)
    {
        const MNode& mn = node(maya);
        const scene::NodeType& t = g.node(id)->type();
        for (const AttrValue& av : mn.attrs)
        {
            std::vector<AttrSeg> segs = splitAttr(av.attr);
            if (segs.empty() || av.nums.empty())
                continue;
            const AttrSeg& last = segs.back();
            // A compound array element (or range of them) holding its children's values in declaration order.
            const std::string group = groupName(mn.type, last.name);
            std::vector<int> kids = t.groupChildren(group);
            if (!kids.empty() && last.lo >= 0)
            {
                size_t at = 0;
                for (int e = last.lo; e <= std::max(last.lo, last.hi) && at < av.nums.size(); ++e)
                    for (int k : kids)
                        at = setConstant(maya, {id, k, -1, e}, av, at);
                continue;
            }
            std::string path = av.attr;
            if (mn.type == "choice")
                continue;
            auto la = resolveAttr(t, mn.type, path);
            if (!la)
                continue;
            const scene::AttrDef& def = t.attrs[size_t(la->attr)];
            if (def.flags & scene::AttrOutput)
                continue;
            const bool isArray = (def.flags & scene::AttrArray) != 0;
            if (isArray && last.lo >= 0 && la->component < 0)
            {
                size_t at = 0;
                for (int e = last.lo; e <= std::max(last.lo, last.hi) && at < av.nums.size(); ++e)
                    at = setConstant(maya, {id, la->attr, -1, e}, av, at);
                continue;
            }
            setConstant(maya, {id, la->attr, la->component, isArray ? la->element : -1}, av, 0);
        }
    }

    /** Sets p from av's numbers starting at `at`, in Lila units; returns where the next value starts. */
    size_t setConstant(int maya, const PlugRef& p, const AttrValue& av, size_t at)
    {
        const scene::AttrDef& def = g.attr(p);
        const Unit unit = mathUnit(node(maya).type, def.name);
        const double f = unit == Unit::Angle ? c.ang : unit == Unit::Linear ? c.lin : 1.0;
        try
        {
            if (p.component >= 0 || def.type == PlugType::Float || def.type == PlugType::Int || def.type == PlugType::Bool || def.type == PlugType::Enum)
            {
                if (at < av.nums.size())
                    setNumber(p, av.nums[at] * f);
                return at + 1;
            }
            if (def.type == PlugType::Vec3)
            {
                // Maya's 2D compounds (plusMinusAverage input2D) arrive as Vec3s with z unused.
                const size_t width = av.type == "double2" || av.type == "float2" ? 2 : 3;
                if (at + width - 1 < av.nums.size())
                    g.setValue(p, glm::dvec3(av.nums[at], av.nums[at + 1], width == 3 ? av.nums[at + 2] : 0.0) * f);
                return at + width;
            }
            if (def.type == PlugType::Matrix)
            {
                if (at + 15 < av.nums.size())
                {
                    glm::dmat4 value = matrixOf(std::vector<double>(av.nums.begin() + long(at), av.nums.begin() + long(at) + 16));
                    value[3] = glm::dvec4(glm::dvec3(value[3]) * c.lin, value[3].w);
                    g.setValue(p, value);
                }
                return at + 16;
            }
        }
        catch (const std::exception& e)
        {
            warn(std::format("{}.{}: {}", shortName(node(maya).name), av.attr, e.what()));
        }
        return at + 1;
    }

    /** What a choice node passes on, from the plugs feeding its inputs. */
    int choiceInputKind(int maya)
    {
        for (const auto& [src, dst] : ix.inputs(maya))
        {
            if (!oneOf(splitAttr(dst.attr).front().name, {"i", "input"}))
                continue;
            std::string why;
            Plug s = src;
            while (node(s.node).type == "unitConversion")
                if (auto up = ix.input(s.node, {"i", "input"}))
                    s = *up;
                else
                    break;
            if (node(s.node).type.starts_with("animCurve"))
                return End::Scalar;
            End e = endOf(s, &why);
            if (e.kind != End::None)
                return e.kind;
        }
        return End::Scalar;
    }

    NodeId mathNode(int maya)
    {
        if (auto it = math.find(maya); it != math.end())
            return it->second;
        const MNode& mn = node(maya);
        const scene::NodeType* type = scene::types::find(mathType(mn.type));
        if (!type)
            return kNoNode;
        c.handled.insert(maya);
        if (mn.type == "choice")
            choiceKind[maya] = choiceInputKind(maya);
        NodeId id = g.createNode(*type, name(maya, mn.type.c_str()), kNoNode, false)->id();
        math[maya] = id;
        copyConstants(maya, id);
        if (mn.type == "choice")
        {
            const int kind = choiceKind[maya];
            const int in = attrOf(id, kind == End::Matrix ? "inputMatrix" : kind == End::Vec3 ? "inputVector" : "input");
            for (const AttrValue& av : mn.attrs)
                if (std::vector<AttrSeg> s = splitAttr(av.attr); !s.empty() && oneOf(s[0].name, {"i", "input"}) && s[0].lo >= 0 && kind == End::Scalar)
                    for (size_t k = 0; k < av.nums.size(); ++k)
                        g.setValue({id, in, -1, s[0].lo + int(k)}, av.nums[k]);
            setNumber({id, attrOf(id, "selector"), -1}, num(mn, {"s", "selector"}, 0.0));
        }
        if (mn.type == "blendMatrix" || mn.type == "aimMatrix")
            for (const AttrValue& av : mn.attrs)
                if (oneOf(av.attr, {"premat", "preSpaceMatrix", "pstmat", "postSpaceMatrix"}))
                    warn(std::format("{} '{}': pre and post space matrices are ignored", mn.type, shortName(mn.name)));
        for (const auto& [src, dst] : ix.inputs(maya))
        {
            std::string why;
            if (mn.type == "closestPointOnMesh" && oneOf(dst.attr, {"im", "inMesh"}))
            {
                // Maya reads the mesh's points in its own space, whichever of its mesh outputs feeds in; inputMatrix places it.
                auto mesh = c.meshes.find(src.node);
                if (mesh == c.meshes.end() || !tryConnect({mesh->second, scene::MeshNode::kOutMesh, -1}, {id, attrOf(id, "inMesh"), -1}, &why))
                    skip(std::format("{} driving {} ({})", plugText(ix, src), plugText(ix, dst), why.empty() ? "not an imported mesh" : why));
                continue;
            }
            End d = mathEnd(maya, id, dst.attr);
            if (d.kind == End::None)
            {
                skip(std::format("{} driving {} (no such Lila attribute)", plugText(ix, src), plugText(ix, dst)));
                continue;
            }
            if (!connectInto(src, d, &why))
                skip(std::format("{} driving {} ({})", plugText(ix, src), plugText(ix, dst), why));
        }
        if (mn.type == "closestPointOnMesh" && !c.identityAxes)
        {
            // Its mesh arrives in Lila's axes and everything else in Maya's: turn the points back first.
            const int im = attrOf(id, "inputMatrix");
            const NodeId mm = makeNode("multMatrix", name(maya, "cpom") + "_axes");
            const int in = attrOf(mm, "matrixIn");
            g.setValue({mm, in, -1, 0}, glm::dmat4(glm::transpose(c.c3)));
            if (auto src = g.source({id, im, -1}))
                g.connect(*src, {mm, in, -1, 1});
            g.connect({mm, attrOf(mm, "matrixSum"), -1}, {id, im, -1}, true);
        }
        return id;
    }

    // ---- user attributes ------------------------------------------------------------------------------------

    void addUserAttrs()
    {
        for (const auto& [maya, id] : c.transforms)
        {
            const MNode& mn = node(maya);
            for (const AddedAttr& a : mn.added)
            {
                if (!a.parent.empty() || a.longName == "lockInfluenceWeights" || a.longName.empty())
                    continue;
                PlugType type;
                const std::string& t = a.type;
                if (t == "double" || t == "float" || t == "doubleLinear" || t == "doubleAngle" || t == "time")
                    type = PlugType::Float;
                else if (t == "long" || t == "short" || t == "byte")
                    type = PlugType::Int;
                else if (t == "bool")
                    type = PlugType::Bool;
                else if (t == "enum")
                    type = PlugType::Enum;
                else
                    continue;
                const scene::NodeType& nt = g.node(id)->type();
                if (nt.attrIndex(a.longName) >= 0 || !Graph::validName(a.longName))
                    continue;
                const double unit = t == "doubleAngle" ? c.ang : t == "doubleLinear" ? c.lin : 1.0;
                scene::AttrDef def;
                def.name = a.longName;
                if (a.shortName != a.longName && nt.attrIndex(a.shortName) < 0 && Graph::validName(a.shortName))
                    def.shortName = a.shortName;
                def.type = type;
                // Keyable either way: files often make an attr keyable with setAttr -k, which isn't kept.
                def.flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable;
                if (type == PlugType::Enum)
                    def.enumNames = a.enumNames.empty() ? std::vector<std::string>{"0"} : a.enumNames;
                const double dv = a.defaultValue.value_or(0.0) * unit;
                def.defaultValue = type == PlugType::Float ? PlugValue(dv) : type == PlugType::Bool ? PlugValue(dv != 0.0) : PlugValue(int64_t(std::llround(dv)));
                if (a.min)
                    def.minValue = *a.min * unit;
                if (a.max)
                    def.maxValue = *a.max * unit;
                if (a.softMin)
                    def.softMin = *a.softMin * unit;
                if (a.softMax)
                    def.softMax = *a.softMax * unit;
                try
                {
                    const int idx = g.addAttr(id, def);
                    userAttrs.insert({maya, a.longName});
                    for (const AttrValue& v : mn.attrs)
                        if ((v.attr == a.longName || v.attr == a.shortName) && !v.nums.empty())
                            setNumber({id, idx, -1}, v.nums[0] * unit);
                }
                catch (const std::exception& e)
                {
                    warn(std::format("{}.{}: {}", g.node(id)->path(), a.longName, e.what()));
                }
            }
        }
    }

    // ---- constraints ----------------------------------------------------------------------------------------

    /** The transform a constraint drives: where its outputs go, else its DAG parent. */
    int constrainedOf(int maya) const
    {
        for (const auto& [src, dst] : ix.outputs(maya))
            if (lilaOf(dst.node) != kNoNode && dst.node != maya)
                return dst.node;
        return ix.parent[size_t(maya)];
    }

    /** Target index -> the Maya transform feeding it, from the tg[i].* connections. */
    std::map<int, int> constraintTargets(int maya) const
    {
        std::map<int, int> out;
        for (const auto& [src, dst] : ix.inputs(maya))
        {
            std::vector<AttrSeg> s = splitAttr(dst.attr);
            if (s.size() < 2 || !oneOf(s[0].name, {"tg", "target"}) || s[0].lo < 0 || oneOf(s[1].name, {"tw", "targetWeight"}))
                continue;
            if (lilaOf(src.node) != kNoNode)
                out[s[0].lo] = src.node;
        }
        return out;
    }

    /** A target's weight: what feeds tg[i].tw (usually the constraint's own alias attr), else its own value. */
    double targetWeight(int maya, int i)
    {
        const MNode& mn = node(maya);
        for (const auto& [src, dst] : ix.inputs(maya))
        {
            std::vector<AttrSeg> s = splitAttr(dst.attr);
            if (s.size() != 2 || !oneOf(s[0].name, {"tg", "target"}) || s[0].lo != i || !oneOf(s[1].name, {"tw", "targetWeight"}))
                continue;
            if (src.node == maya)
            {
                for (const AddedAttr& a : mn.added)
                    if (a.shortName == src.attr || a.longName == src.attr)
                    {
                        aliasWeight[{maya, a.shortName}] = i;
                        aliasWeight[{maya, a.longName}] = i;
                        return num(mn, {a.shortName, a.longName}, a.defaultValue.value_or(1.0));
                    }
                aliasWeight[{maya, src.attr}] = i;
                return num(mn, {src.attr}, 1.0);
            }
            aliasWeight[{maya, std::format("tg[{}].tw", i)}] = i;
            return 1.0;
        }
        return num(mn, {std::format("tg[{}].tw", i), std::format("target[{}].targetWeight", i)}, 1.0);
    }

    glm::dvec3 mayaVec(const MNode& n, Names whole, std::array<Names, 3> comps, glm::dvec3 def) const { return vec3(n, whole, comps, def); }

    /** A Maya-space direction (unitless) as Lila's. */
    glm::dvec3 dir(const glm::dvec3& v) const { return c.c3 * v; }

    /** A rotation in Maya degrees (XYZ) as a Lila rotation matrix. */
    glm::dmat3 rotation(const glm::dvec3& mayaDeg, scene::RotateOrder order = scene::RotateOrder::XYZ) const
    {
        return c.c3 * glm::dmat3(scene::eulerToMatrix(mayaDeg * c.ang, order)) * glm::transpose(c.c3);
    }

    NodeId constraintNode(int maya)
    {
        if (auto it = constraints.find(maya); it != constraints.end())
            return it->second;
        using scene::ConstraintNode;
        const MNode& mn = node(maya);
        const std::string where = std::format("{} '{}'", mn.type, shortName(mn.name));
        const int objMaya = constrainedOf(maya);
        const NodeId obj = objMaya >= 0 ? lilaOf(objMaya) : kNoNode;
        constraints[maya] = kNoNode;
        if (obj == kNoNode)
        {
            skip(where + " (the constrained object was not converted)");
            c.handled.insert(maya);
            return kNoNode;
        }
        std::map<int, int> targets = constraintTargets(maya);
        if (targets.empty())
        {
            skip(where + " (no converted targets)");
            c.handled.insert(maya);
            return kNoNode;
        }
        c.handled.insert(maya);
        g.evaluate();
        scene::Node* o = g.node(obj);
        if (mn.type == "scaleConstraint")
            return constraints[maya] = scaleConstraint(maya, obj, targets);
        if (mn.type == "poleVectorConstraint")
            return constraints[maya] = poleVectorConstraint(maya, obj, targets);

        const NodeId id = g.createNode(*scene::types::find(mn.type), name(maya, mn.type.c_str()), kNoNode, false)->id();
        constraints[maya] = id;
        g.setValue({id, ConstraintNode::kRotateOrder}, o->value(scene::Transform::kRotateOrder));
        if (scene::rig::isJoint(*o))
            g.setValue({id, ConstraintNode::kJointOrient}, o->value(scene::Joint::kJointOrient));
        g.setValue({id, ConstraintNode::kRotatePivot}, o->value(scene::Transform::kRotatePivot));
        g.setValue({id, ConstraintNode::kRotatePivotTranslate}, o->value(scene::Transform::kRotatePivotTranslate));
        // The rest pose holds while every weight is 0; an aim constraint also aims from its translate.
        glm::dvec3 restT = o->as<glm::dvec3>(scene::Transform::kTranslate), restR = o->as<glm::dvec3>(scene::Transform::kRotate);
        if (num(mn, {"erp", "enableRestPosition"}, 0.0) != 0.0)
        {
            if ((mn.type == "parentConstraint" || mn.type == "pointConstraint") && lastAttr(mn, {"rst", "restTranslate"}))
                restT = c.axes.translate(mayaVec(mn, {"rst", "restTranslate"}, {Names{"rtx"}, Names{"rty"}, Names{"rtz"}}, glm::dvec3(0.0)) * c.lin);
            if (mn.type != "pointConstraint" && lastAttr(mn, {"rsrr", "restRotate"}))
                restR = c.axes.rotate(mayaVec(mn, {"rsrr", "restRotate"}, {Names{"rrx"}, Names{"rry"}, Names{"rrz"}}, glm::dvec3(0.0)) * c.ang);
        }
        g.setValue({id, ConstraintNode::kRestTranslate}, restT);
        if (mn.type == "parentConstraint")
            g.setValue({id, ConstraintNode::kPivotOffsets}, true);
        g.setValue({id, ConstraintNode::kRestRotate}, restR);
        if (NodeId p = g.parent(obj); p != kNoNode)
            g.connect({p, scene::Transform::kWorldMatrix, -1}, {id, ConstraintNode::kParentMatrix, -1});
        if (o->as<glm::dmat4>(scene::Transform::kOffsetParentMatrix) != glm::dmat4(1.0) || g.source({obj, scene::Transform::kOffsetParentMatrix, -1}))
            warn(std::format("{}: constrained through an offsetParentMatrix, which the constraint ignores", where));

        // Maya reads rotation offsets in the constrained object's rotate order.
        const auto objOrder = scene::RotateOrder(std::clamp(int64_t(num(node(objMaya), {"ro", "rotateOrder"}, 0.0)), int64_t(0), int64_t(5)));
        for (const auto& [i, t] : targets)
        {
            const NodeId tl = lilaOf(t);
            std::string why;
            if (!tryConnect({tl, scene::Transform::kWorldMatrix, -1}, {id, ConstraintNode::kTargetMatrix, -1, i}, &why))
            {
                skip(std::format("{} target '{}' ({})", where, shortName(node(t).name), why));
                continue;
            }
            g.setValue({id, ConstraintNode::kTargetWeight, -1, i}, targetWeight(maya, i));
            const glm::dvec3 trp = g.node(tl)->as<glm::dvec3>(scene::Transform::kRotatePivot);
            if (mn.type == "parentConstraint")
            {
                const std::string tg = std::format("tg[{}].", i), tgl = std::format("target[{}].", i);
                glm::dvec3 tot = mayaVec(mn, {tg + "tot", tgl + "targetOffsetTranslate"},
                                         {Names{tg + "totx"}, Names{tg + "toty"}, Names{tg + "totz"}}, glm::dvec3(0.0));
                glm::dvec3 tor = mayaVec(mn, {tg + "tor", tgl + "targetOffsetRotate"},
                                         {Names{tg + "torx"}, Names{tg + "tory"}, Names{tg + "torz"}}, glm::dvec3(0.0));
                glm::dmat4 offset = glm::translate(glm::dmat4(1.0), trp) * glm::translate(glm::dmat4(1.0), c.axes.translate(tot * c.lin)) *
                                    glm::dmat4(rotation(tor, objOrder));
                g.setValue({id, ConstraintNode::kTargetOffset, -1, i}, offset);
            }
            else if (glm::length(trp) > 1e-9)
                warn(std::format("{}: target '{}' has a rotate pivot; Lila's {} uses its origin", where, shortName(node(t).name), mn.type));
        }
        const glm::dvec3 off = mayaVec(mn, {"o", "offset"}, {Names{"ox"}, Names{"oy"}, Names{"oz"}}, glm::dvec3(0.0));
        if (mn.type == "pointConstraint")
            g.setValue({id, ConstraintNode::kOffset}, c.axes.translate(off * c.lin));
        else if (mn.type == "orientConstraint" || mn.type == "aimConstraint")
            g.setValue({id, ConstraintNode::kOffset}, scene::matrixToEuler(rotation(off, objOrder), scene::RotateOrder::XYZ));
        if (mn.type == "aimConstraint")
        {
            g.setValue({id, ConstraintNode::kAimVector}, dir(mayaVec(mn, {"a", "aimVector"}, {Names{"ax"}, Names{"ay"}, Names{"az"}}, glm::dvec3(1, 0, 0))));
            g.setValue({id, ConstraintNode::kUpVector}, dir(mayaVec(mn, {"u", "upVector"}, {Names{"ux"}, Names{"uy"}, Names{"uz"}}, glm::dvec3(0, 1, 0))));
            g.setValue({id, ConstraintNode::kWorldUpVector},
                       dir(mayaVec(mn, {"wu", "worldUpVector"}, {Names{"wux"}, Names{"wuy"}, Names{"wuz"}}, glm::dvec3(0, 1, 0))));
            g.setValue({id, ConstraintNode::kWorldUpType}, int64_t(std::clamp(num(mn, {"wut", "worldUpType"}, 0.0), 0.0, 4.0)));
            if (auto up = ix.input(maya, {"wum", "worldUpMatrix"}))
            {
                std::string why;
                End e = endOf(*up, &why);
                if (e.kind != End::Matrix || !e.plug.valid() || !tryConnect(e.plug, {id, ConstraintNode::kWorldUpMatrix, -1}, &why))
                    skip(std::format("{} world up object ({})", where, why));
            }
            for (const auto& [src, dst] : ix.inputs(maya))
                if (oneOf(dst.attr, {"ct", "constraintTranslate", "ctx", "cty", "ctz"}) && src.node == objMaya && g.source({obj, scene::Transform::kTranslate, -1}))
                    warn(std::format("{}: the aimed object's translate is driven; Lila aims from its translate when imported", where));
        }
        return id;
    }

    NodeId scaleConstraint(int maya, NodeId obj, const std::map<int, int>& targets)
    {
        const MNode& mn = node(maya);
        const NodeId id = makeNode("scaleConstraint", name(maya, "scaleConstraint"));
        const int scale = attrOf(id, "targets.scale"), weight = attrOf(id, "targets.weight");
        for (const auto& [i, t] : targets)
        {
            std::string why;
            if (!tryConnect({lilaOf(t), scene::Transform::kScale, -1}, {id, scale, -1, i}, &why))
                skip(std::format("scaleConstraint '{}' target '{}' ({})", shortName(mn.name), shortName(node(t).name), why));
            g.setValue({id, weight, -1, i}, targetWeight(maya, i));
        }
        g.setValue({id, attrOf(id, "offset"), -1}, c.axes.scale(mayaVec(mn, {"o", "offset"}, {Names{"ox"}, Names{"oy"}, Names{"oz"}}, glm::dvec3(1.0))));
        g.setValue({id, attrOf(id, "restScale"), -1}, g.node(obj)->as<glm::dvec3>(scene::Transform::kScale));
        return id;
    }

    NodeId poleVectorConstraint(int maya, NodeId handle, const std::map<int, int>& targets)
    {
        const MNode& mn = node(maya);
        const NodeId id = makeNode("poleVectorConstraint", name(maya, "poleVectorConstraint"));
        const int matrix = attrOf(id, "targets.matrix"), weight = attrOf(id, "targets.weight");
        for (const auto& [i, t] : targets)
        {
            std::string why;
            if (!tryConnect({lilaOf(t), scene::Transform::kWorldMatrix, -1}, {id, matrix, -1, i}, &why))
                skip(std::format("poleVectorConstraint '{}' target '{}' ({})", shortName(mn.name), shortName(node(t).name), why));
            g.setValue({id, weight, -1, i}, targetWeight(maya, i));
        }
        // The chain's start joint, by its parent's world matrix and its translate, so the pole doesn't read what the handle drives.
        std::vector<NodeId> chain = scene::ik::chainOf(g, handle);
        if (!chain.empty())
        {
            const NodeId start = chain.front();
            if (NodeId p = g.parent(start); p != kNoNode)
                g.connect({p, scene::Transform::kWorldMatrix, -1}, {id, attrOf(id, "pivotMatrix"), -1});
            g.setValue({id, attrOf(id, "pivot"), -1}, g.node(start)->as<glm::dvec3>(scene::Transform::kTranslate));
        }
        else
            warn(std::format("poleVectorConstraint '{}': its ikHandle has no converted chain", shortName(mn.name)));
        g.setValue({id, attrOf(id, "restTranslate"), -1}, g.node(handle)->as<glm::dvec3>(scene::IkHandle::kPoleVector));
        return id;
    }

    End constraintEnd(const Plug& p, std::string* why)
    {
        const MNode& mn = node(p.node);
        const NodeId id = constraintNode(p.node);
        if (id == kNoNode)
        {
            *why = std::format("{} '{}' is not converted", mn.type, shortName(mn.name));
            return {};
        }
        std::vector<AttrSeg> segs = splitAttr(p.attr);
        const std::string& a = segs.empty() ? p.attr : segs.back().name;
        static const char* kAxes[3] = {"x", "y", "z"};
        auto vecOrComp = [&](End v, std::string_view shortN, std::string_view longN) -> std::optional<End> {
            if (a == shortN || a == longN)
                return v;
            for (int k = 0; k < 3; ++k)
                if (a == std::string(shortN) + kAxes[k] || a == std::string(longN) + char('X' + k))
                    return v.component(k);
            return std::nullopt;
        };
        if (mn.type == "scaleConstraint")
        {
            if (auto e = vecOrComp(vecEnd(id, attrOf(id, "constraintScale"), 2, Unit::None), "cs", "constraintScale"))
                return *e;
        }
        else if (mn.type == "poleVectorConstraint")
        {
            if (auto e = vecOrComp(vecEnd(id, attrOf(id, "constraintTranslate"), 0, Unit::None), "ct", "constraintTranslate"))
                return *e;
        }
        else
        {
            if (auto e = vecOrComp(vecEnd(id, scene::ConstraintNode::kConstraintTranslate, 0, Unit::Linear), "ct", "constraintTranslate"))
                return *e;
            if (auto e = vecOrComp(vecEnd(id, scene::ConstraintNode::kConstraintRotate, 1, Unit::Angle), "cr", "constraintRotate"))
                return *e;
        }
        if (auto it = aliasWeight.find({p.node, p.attr}); it != aliasWeight.end())
        {
            const int w = mn.type == "scaleConstraint" || mn.type == "poleVectorConstraint" ? attrOf(id, "targets.weight") : int(scene::ConstraintNode::kTargetWeight);
            return {.kind = End::Scalar, .plug = {id, w, -1, it->second}};
        }
        *why = std::format("{}.{} has no Lila counterpart", shortName(mn.name), p.attr);
        return {};
    }

    // ---- IK -------------------------------------------------------------------------------------------------

    void buildIk(int maya)
    {
        using scene::IkHandle;
        const MNode& mn = node(maya);
        const NodeId handle = lilaOf(maya);
        const std::string where = std::format("ikHandle '{}'", shortName(mn.name));
        auto solverNode = ix.input(maya, {"hsv", "ikSolver"});
        const std::string solver = solverNode ? node(solverNode->node).type : std::string("ikSCsolver");
        if (solver != "ikRPsolver" && solver != "ikSCsolver" && solver != "ik2Bsolver")
        {
            skip(std::format("{} ({} is not converted; the joints keep their pose and keys)", where, solver));
            return;
        }
        auto startPlug = ix.input(maya, {"hsj", "startJoint"});
        auto effPlug = ix.input(maya, {"hee", "endEffector"});
        if (!startPlug || !effPlug)
        {
            skip(where + " (no start joint or end effector)");
            return;
        }
        c.handled.insert(effPlug->node);
        // The effector sits under the joint before the end and follows the end joint's translate.
        int endMaya = -1;
        for (const auto& [src, dst] : ix.inputs(effPlug->node))
            if (oneOf(dst.attr, {"tx", "ty", "tz", "t", "translateX", "translate"}) && lilaOf(src.node) != kNoNode)
                endMaya = src.node;
        if (endMaya < 0)
            if (int p = ix.parent[size_t(effPlug->node)]; p >= 0)
                for (int k : ix.children[size_t(p)])
                    if (node(k).type == "joint")
                        endMaya = k;
        const NodeId start = lilaOf(startPlug->node), end = endMaya >= 0 ? lilaOf(endMaya) : kNoNode;
        if (start == kNoNode || end == kNoNode)
        {
            skip(where + " (its joints were not converted)");
            return;
        }
        std::vector<NodeId> chain;
        for (NodeId j = end; j != kNoNode && scene::rig::isJoint(*g.node(j)); j = g.parent(j))
        {
            chain.push_back(j);
            if (j == start)
                break;
        }
        std::reverse(chain.begin(), chain.end());
        if (chain.size() < 2 || chain.front() != start || chain.size() > size_t(IkHandle::kMaxJoints))
        {
            skip(where + " (its chain could not be followed or is longer than Lila's limit)");
            return;
        }
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            if (g.source({chain[i], scene::Joint::kSolvedRotate, -1}))
            {
                skip(where + " (a joint is already solved by another handle)");
                return;
            }
        if (solver == "ikSCsolver" && chain.size() == 2)
        {
            singleBone(handle, start, end, where);
            return;
        }
        if (solver == "ikRPsolver" && chain.size() > 3)
            warn(std::format("{}: a {}-joint rotate plane chain is solved with FABRIK, close to Maya's but not identical", where, chain.size()));

        g.evaluate();
        // Maya's pole vector lives in the handle's parent space; Lila's is a world direction.
        glm::dvec3 pv(0.0);
        if (solver != "ikSCsolver")
        {
            pv = c.axes.translate(mayaVec(mn, {"pv", "poleVector"}, {Names{"pvx"}, Names{"pvy"}, Names{"pvz"}}, glm::dvec3(0, 0, 1)));
            if (NodeId p = g.parent(handle); p != kNoNode)
                pv = scene::rigutil::orthonormal(glm::dmat3(g.node(p)->as<glm::dmat4>(scene::Transform::kWorldMatrix))) * pv;
        }
        g.setValue({handle, IkHandle::kPoleVector}, pv);
        g.setValue({handle, IkHandle::kPoleAngle}, num(mn, {"twi", "twist"}, 0.0) * c.ang);
        g.setValue({handle, IkHandle::kIkBlend}, num(mn, {"ikb", "ikBlend"}, 1.0));
        g.setValue({handle, IkHandle::kJointCount}, int64_t(chain.size()));
        g.setValue({handle, IkHandle::kEndEffector}, g.node(end)->name());
        if (NodeId parent = g.parent(start); parent != kNoNode)
            g.connect({parent, scene::Transform::kWorldMatrix, -1}, {handle, IkHandle::kChainParentMatrix, -1});
        for (size_t i = 0; i + 1 < chain.size(); ++i)
        {
            g.connect({handle, IkHandle::slotSolved(int(i)), -1}, {chain[i], scene::Joint::kSolvedRotate, -1});
            g.connect({handle, IkHandle::kIkBlend, -1}, {chain[i], scene::Joint::kIkBlend, -1});
        }
        refreshWithPreferred(handle, chain);
        if (solver == "ikSCsolver" && chain.size() == 3)
            singleChainPole(handle, chain, where);
    }

    /**
     * Maya's single chain solve keeps the limb's bend plane turning with the handle's orientation. As a pole vector:
     * the rest bend direction, carried in the handle's frame (its parent's world matrix times its own rest rotation).
     */
    void singleChainPole(NodeId handle, const std::vector<NodeId>& chain, const std::string& where)
    {
        const glm::dvec3 p0(fkWorld(chain[0])[3]), p1(fkWorld(chain[1])[3]), p2(fkWorld(chain[2])[3]);
        const glm::dvec3 axis = p2 - p0;
        if (glm::length(axis) < 1e-12)
            return;
        glm::dvec3 bend = (p1 - p0) - glm::dot(p1 - p0, glm::normalize(axis)) * glm::normalize(axis);
        if (glm::length(bend) < 1e-9)
        {
            warn(where + ": a straight single chain has no bend plane; it keeps its rest one");
            return;
        }
        const glm::dmat4 hw = fkWorld(handle);
        const glm::dmat3 h = scene::rigutil::orthonormal(glm::dmat3(hw));
        const glm::dvec3 local = glm::transpose(h) * glm::normalize(bend);
        const NodeId vp = makeNode("vectorProduct", g.node(handle)->name() + "_bend");
        g.setValue({vp, attrOf(vp, "operation"), -1}, int64_t(3));
        g.setValue({vp, attrOf(vp, "input1"), -1}, local);
        // The handle's own rest turn, then its parent's world: reading the handle itself would make a cycle.
        glm::dmat4 own(scene::rigutil::orthonormal(glm::dmat3(g.node(handle)->as<glm::dmat4>(scene::Transform::kMatrix))));
        if (NodeId parent = g.parent(handle); parent != kNoNode)
        {
            const NodeId mm = makeNode("multMatrix", g.node(handle)->name() + "_frame");
            const int in = attrOf(mm, "matrixIn");
            g.setValue({mm, in, -1, 0}, own);
            g.connect({parent, scene::Transform::kWorldMatrix, -1}, {mm, in, -1, 1});
            g.connect({mm, attrOf(mm, "matrixSum"), -1}, {vp, attrOf(vp, "matrix"), -1});
        }
        else
            g.setValue({vp, attrOf(vp, "matrix"), -1}, own);
        std::string why;
        if (!tryConnect({vp, attrOf(vp, "output"), -1}, {handle, scene::IkHandle::kPoleVector, -1}, &why))
            skip(std::format("{} bend plane ({})", where, why));
    }

    /**
     * A handle's chain keeps its rest layout, start joint's translate included, so a start joint whose translate is
     * driven (a hip point-constrained to its control) would solve from where it was. Its translate's drivers go into
     * the chain's parent frame instead, the layout's translate is cleared, and the pole vector pivot follows.
     */
    void liveStartTranslate(NodeId handle)
    {
        using scene::IkHandle;
        std::vector<NodeId> chain = scene::ik::chainOf(g, handle);
        if (chain.empty())
            return;
        const NodeId start = chain.front();
        const PlugRef whole{start, scene::Transform::kTranslate, -1};
        std::optional<PlugRef> comps[3];
        bool driven = false;
        for (int k = 0; k < 3; ++k)
        {
            comps[k] = g.source({start, scene::Transform::kTranslate, k});
            driven = driven || comps[k];
        }
        auto wholeSrc = g.source(whole);
        if (!driven && !wholeSrc)
            return;
        const NodeId compose = makeNode("composeMatrix", g.node(start)->name() + "_ikRoot");
        const int it = attrOf(compose, "inputTranslate");
        g.setValue({compose, it, -1}, g.node(start)->as<glm::dvec3>(scene::Transform::kTranslate));
        if (wholeSrc)
            g.connect(*wholeSrc, {compose, it, -1});
        for (int k = 0; k < 3; ++k)
            if (comps[k])
                g.connect(*comps[k], {compose, it, k});
        PlugRef root{compose, attrOf(compose, "outputMatrix"), -1};
        if (NodeId parent = g.parent(start); parent != kNoNode)
        {
            const NodeId mm = makeNode("multMatrix", g.node(start)->name() + "_ikRootSpace");
            const int in = attrOf(mm, "matrixIn");
            g.connect(root, {mm, in, -1, 0});
            g.connect({parent, scene::Transform::kWorldMatrix, -1}, {mm, in, -1, 1});
            root = {mm, attrOf(mm, "matrixSum"), -1};
        }
        g.connect(root, {handle, IkHandle::kChainParentMatrix, -1}, true);
        glm::dmat4 local = g.node(handle)->as<glm::dmat4>(IkHandle::slotLocal(0));
        local[3] = glm::dvec4(glm::dvec3(local[3]) - g.node(start)->as<glm::dvec3>(scene::Transform::kTranslate), 1.0);
        g.setValue({handle, IkHandle::slotLocal(0), -1}, local);
        for (const auto& [maya, id] : constraints)
            if (id != kNoNode && g.node(id)->type().name == "poleVectorConstraint" && lilaOf(constrainedOf(maya)) == handle)
            {
                g.connect(root, {id, attrOf(id, "pivotMatrix"), -1}, true);
                g.setValue({id, attrOf(id, "pivot"), -1}, glm::dvec3(0.0));
            }
    }

    /** A node's world matrix from its own and its ancestors' stored channels: the rest pose, before IK or constraints. */
    glm::dmat4 fkWorld(NodeId id) const
    {
        glm::dmat4 world(1.0);
        for (NodeId cur = id; cur != kNoNode; cur = g.parent(cur))
        {
            const scene::Node* n = g.node(cur);
            const auto order = scene::RotateOrder(std::clamp<int64_t>(n->as<int64_t>(scene::Transform::kRotateOrder), 0, 5));
            world = n->as<glm::dmat4>(scene::Transform::kOffsetParentMatrix) *
                    scene::Transform::localMatrix(*n, scene::eulerToMatrix(n->as<glm::dvec3>(scene::Transform::kRotate), order)) * world;
        }
        return world;
    }

    /**
     * A single chain handle on one bone: Maya points the bone at the handle and rolls it with the handle's
     * orientation. That is an aim constraint at the handle, its up axis riding the handle's rotation.
     */
    void singleBone(NodeId handle, NodeId joint, NodeId end, const std::string& where)
    {
        using scene::ConstraintNode;
        scene::Node* j = g.node(joint);
        const glm::dvec3 bone = g.node(end)->as<glm::dvec3>(scene::Transform::kTranslate);
        if (glm::length(bone) < 1e-12)
        {
            skip(where + " (its bone has no length)");
            return;
        }
        const glm::dvec3 aim = glm::normalize(bone);
        const int a = std::abs(aim.x) > std::abs(aim.y) ? (std::abs(aim.x) > std::abs(aim.z) ? 0 : 2) : (std::abs(aim.y) > std::abs(aim.z) ? 1 : 2);
        glm::dvec3 up(0.0);
        up[(a + 1) % 3] = 1.0;
        const glm::dmat3 h = scene::rigutil::orthonormal(glm::dmat3(fkWorld(handle)));
        const glm::dmat3 w = scene::rigutil::orthonormal(glm::dmat3(fkWorld(joint)));
        const NodeId id = makeNode("aimConstraint", g.node(handle)->name() + "_aim");
        g.setValue({id, ConstraintNode::kRotateOrder}, j->value(scene::Transform::kRotateOrder));
        g.setValue({id, ConstraintNode::kJointOrient}, j->value(scene::Joint::kJointOrient));
        g.setValue({id, ConstraintNode::kRotatePivot}, j->value(scene::Transform::kRotatePivot));
        g.setValue({id, ConstraintNode::kRotatePivotTranslate}, j->value(scene::Transform::kRotatePivotTranslate));
        g.setValue({id, ConstraintNode::kRestTranslate}, j->value(scene::Transform::kTranslate));
        g.setValue({id, ConstraintNode::kRestRotate}, j->value(scene::Transform::kRotate));
        g.setValue({id, ConstraintNode::kAimVector}, aim);
        g.setValue({id, ConstraintNode::kUpVector}, up);
        g.setValue({id, ConstraintNode::kWorldUpType}, int64_t(ConstraintNode::kUpObjectRotation));
        g.setValue({id, ConstraintNode::kWorldUpVector}, glm::transpose(h) * (w * up));
        if (NodeId p = g.parent(joint); p != kNoNode)
            g.connect({p, scene::Transform::kWorldMatrix, -1}, {id, ConstraintNode::kParentMatrix, -1});
        g.connect({handle, scene::Transform::kWorldMatrix, -1}, {id, ConstraintNode::kTargetMatrix, -1, 0});
        g.connect({handle, scene::Transform::kWorldMatrix, -1}, {id, ConstraintNode::kWorldUpMatrix, -1});
        g.setValue({id, ConstraintNode::kTargetWeight, -1, 0}, 1.0);
        std::string why;
        if (!tryConnect({id, ConstraintNode::kConstraintRotate, -1}, {joint, scene::Transform::kRotate, -1}, &why))
            skip(std::format("{} ({})", where, why));
        g.setValue({handle, scene::IkHandle::kJointCount}, int64_t(0));
    }

    /**
     * Stretch: a chain joint whose translate or scale is driven (a stretchy limb's distance network) feeds its rest
     * layout slot through a composeMatrix, so the solve reaches with the bones' live lengths.
     */
    void liveChainSlots(NodeId handle)
    {
        using scene::IkHandle;
        std::vector<NodeId> chain = scene::ik::chainOf(g, handle);
        for (size_t i = 1; i < chain.size(); ++i)
        {
            const NodeId j = chain[i];
            auto drivers = [&](int attr) {
                std::array<std::optional<PlugRef>, 4> out; // whole, then x, y, z
                out[0] = g.source({j, attr, -1});
                for (int k = 0; k < 3; ++k)
                    out[size_t(k + 1)] = g.source({j, attr, k});
                return out;
            };
            auto t = drivers(scene::Transform::kTranslate), sc = drivers(scene::Transform::kScale);
            const bool driven = std::any_of(t.begin(), t.end(), [](const auto& p) { return p.has_value(); }) ||
                                std::any_of(sc.begin(), sc.end(), [](const auto& p) { return p.has_value(); });
            if (!driven)
                continue;
            const glm::dmat4 rest = g.node(handle)->as<glm::dmat4>(IkHandle::slotLocal(int(i)));
            const NodeId compose = makeNode("composeMatrix", g.node(j)->name() + "_ikSlot");
            const int ti = attrOf(compose, "inputTranslate"), si = attrOf(compose, "inputScale");
            g.setValue({compose, ti, -1}, g.node(j)->as<glm::dvec3>(scene::Transform::kTranslate));
            g.setValue({compose, si, -1}, g.node(j)->as<glm::dvec3>(scene::Transform::kScale));
            g.setValue({compose, attrOf(compose, "inputRotate"), -1},
                       scene::matrixToEuler(scene::rigutil::orthonormal(glm::dmat3(rest)), scene::RotateOrder::XYZ));
            for (size_t k = 0; k < 4; ++k)
            {
                const int comp = int(k) - 1;
                if (t[k])
                    g.connect(*t[k], {compose, ti, comp});
                if (sc[k])
                    g.connect(*sc[k], {compose, si, comp});
            }
            std::string why;
            if (!tryConnect({compose, attrOf(compose, "outputMatrix"), -1}, {handle, IkHandle::slotLocal(int(i)), -1}, &why))
                skip(std::format("stretch of '{}' in its IK chain ({})", g.node(j)->path(), why));
        }
    }

    /** ik::refresh on the joints' preferred angles where Maya has them, which set the bend of a straight chain. */
    void refreshWithPreferred(NodeId handle, const std::vector<NodeId>& chain)
    {
        std::vector<std::pair<NodeId, glm::dvec3>> restore;
        for (const auto& [maya, id] : c.transforms)
        {
            if (std::find(chain.begin(), chain.end(), id) == chain.end())
                continue;
            glm::dvec3 pa = vec3(node(maya), {"pa", "preferredAngle"}, {Names{"pax"}, Names{"pay"}, Names{"paz"}}, glm::dvec3(0.0));
            if (glm::length(pa) < 1e-9)
                continue;
            restore.push_back({id, g.node(id)->as<glm::dvec3>(scene::Transform::kRotate)});
            g.setValue({id, scene::Transform::kRotate, -1}, c.axes.rotate(pa * c.ang));
        }
        scene::ik::refresh(g, handle);
        for (const auto& [id, r] : restore)
            g.setValue({id, scene::Transform::kRotate, -1}, r);
    }

    // ---- deformers ------------------------------------------------------------------------------------------

    /** The geometry index k of a deformer whose output feeds the given input plug, from "og[k]". */
    static int outputIndex(const std::string& attr)
    {
        std::vector<AttrSeg> s = splitAttr(attr);
        return s.empty() ? 0 : std::max(s[0].lo, 0);
    }

    /** Deformers on a mesh shape's history, the first applied first, with the geometry index each deforms. */
    std::vector<std::pair<int, int>> deformerChain(int shape) const
    {
        std::vector<std::pair<int, int>> out;
        auto src = ix.input(shape, {"i", "inMesh"});
        for (int guard = 0; src && guard < 128; ++guard)
        {
            const std::string& t = node(src->node).type;
            if (t == "groupParts" || t == "groupId")
            {
                src = ix.input(src->node, {"ig", "inputGeometry"});
                continue;
            }
            if (!src->attr.starts_with("og") && !src->attr.starts_with("outputGeometry"))
                break;
            const int k = outputIndex(src->attr);
            out.push_back({src->node, k});
            src = ix.input(src->node, {std::format("ip[{}].ig", k), std::format("input[{}].inputGeometry", k)});
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    /** Maya vertices a deformer acts on for geometry k: its component tag, or the groupParts feeding it. Empty with all = true for every vertex. */
    std::set<int> membership(int deformer, int k, int shape, bool* all) const
    {
        std::set<int> out;
        *all = false;
        const MNode& dn = node(deformer);
        const std::string tag = text(dn, {std::format("ip[{}].gtg", k), std::format("input[{}].componentTagExpression", k)});
        if (!tag.empty())
        {
            if (tag == "*")
            {
                *all = true;
                return out;
            }
            // Tags live on the shapes under the deformed shape's transform (the original shape keeps them).
            const int xf = ix.parent[size_t(shape)];
            bool found = false;
            for (int s : xf >= 0 ? ix.children[size_t(xf)] : std::vector<int>{shape})
            {
                const MNode& sn = node(s);
                std::map<int, std::string> names;
                std::map<int, std::vector<std::string>> comps;
                for (const AttrValue& a : sn.attrs)
                {
                    std::vector<AttrSeg> seg = splitAttr(a.attr);
                    if (seg.size() != 2 || !oneOf(seg[0].name, {"gtag", "componentTags"}) || seg[0].lo < 0)
                        continue;
                    if (oneOf(seg[1].name, {"gtagnm", "componentTagName"}) && !a.strs.empty())
                        names[seg[0].lo] = a.strs[0];
                    else if (oneOf(seg[1].name, {"gtagcmp", "componentTagContents"}))
                        comps[seg[0].lo] = a.strs;
                }
                for (const auto& [i, nm] : names)
                    if (nm == tag)
                    {
                        addComponents(comps[i], out, all);
                        found = true;
                    }
            }
            if (!found)
                *all = true;
            return out;
        }
        auto src = ix.input(deformer, {std::format("ip[{}].ig", k), std::format("input[{}].inputGeometry", k)});
        if (src && node(src->node).type == "groupParts")
        {
            const AttrValue* ic = lastAttr(node(src->node), {"ic", "inputComponents"});
            if (ic)
            {
                addComponents(ic->strs, out, all);
                return out;
            }
        }
        *all = true;
        return out;
    }

    /** A deformer's per-point weights on the Lila mesh: its weight list, zero outside its membership. Null when every point weighs 1. */
    scene::DataPtr pointWeights(int deformer, int k, int shape, size_t points)
    {
        auto pts = c.pointOf.find(shape);
        if (pts == c.pointOf.end())
            return {};
        bool all = true;
        const std::set<int> members = membership(deformer, k, shape, &all);
        const std::map<int, double> w = weightList(node(deformer), k);
        auto out = std::make_shared<scene::PointWeights>();
        out->values.assign(points, all ? 1.0f : 0.0f);
        bool any = !all;
        if (!all)
            for (int v : members)
                if (size_t(v) < pts->second.size() && pts->second[size_t(v)] < points)
                    out->values[pts->second[size_t(v)]] = 1.0f;
        for (const auto& [v, wt] : w)
            if (size_t(v) < pts->second.size() && pts->second[size_t(v)] < points && (all || members.contains(v)))
            {
                out->values[pts->second[size_t(v)]] = float(wt);
                any = any || wt != 1.0;
            }
        return any ? scene::DataPtr(out) : scene::DataPtr();
    }

    /** The Lila transform a deformer input matrix comes from ("ma", "dlm", ...), or none. */
    NodeId matrixSource(int deformer, Names names) const
    {
        auto src = ix.input(deformer, names);
        if (!src)
            return kNoNode;
        if (NodeId id = lilaOf(src->node); id != kNoNode)
            return id;
        return lilaParentOf(src->node);
    }

    void setParam(NodeId d, const char* attr, PlugValue v)
    {
        const int a = attrOf(d, attr);
        if (a >= 0)
            g.setValue({d, a, -1}, std::move(v));
    }

    void rebind(NodeId d)
    {
        try
        {
            ops::execute("rebindDeformer", {{"args", json::array({g.node(d)->path()})}});
        }
        catch (const std::exception& e)
        {
            warn(std::format("{}: binding failed: {}", g.node(d)->path(), e.what()));
        }
    }

    /** The nonlinear handle's shape type and the Lila deformer it becomes. */
    std::pair<int, std::string> nonlinearKind(int deformer) const
    {
        int handleShape = -1;
        auto take = [&](int xf) {
            if (xf >= 0)
                for (int k : ix.children[size_t(xf)])
                    if (nonlinearHandles().contains(node(k).type))
                        handleShape = k;
        };
        if (auto src = ix.input(deformer, {"ma", "matrix"}))
            take(nonlinearHandles().contains(node(src->node).type) ? ix.parent[size_t(src->node)] : src->node);
        if (handleShape < 0)
            if (auto src = ix.input(deformer, {"dd", "deformerData"}))
                handleShape = src->node;
        if (handleShape < 0)
            return {-1, ""};
        static const std::map<std::string_view, std::string> kTypes = {{"deformBend", "bendDeformer"},   {"deformTwist", "twistDeformer"},
                                                                        {"deformSquash", "squashDeformer"}, {"deformSine", "sineDeformer"},
                                                                        {"deformWave", "waveDeformer"},   {"deformFlare", "taperDeformer"}};
        auto it = kTypes.find(node(handleShape).type);
        return {handleShape, it == kTypes.end() ? std::string() : it->second};
    }

    /** A nonlinear parameter: the nonLinear node's own value (it drives the handle), else the handle shape's. */
    double nlParam(int deformer, int handleShape, Names names, double def) const
    {
        if (lastAttr(node(deformer), names))
            return num(node(deformer), names, def);
        for (const AddedAttr& a : node(deformer).added)
            if (oneOf(a.shortName, names) || oneOf(a.longName, names))
                if (a.defaultValue)
                    def = *a.defaultValue;
        return handleShape >= 0 ? num(node(handleShape), names, def) : def;
    }

    void buildDeformers()
    {
        for (const auto& [shape, mesh] : c.meshes)
        {
            std::vector<std::pair<int, int>> chain = deformerChain(shape);
            bool afterSkin = false;
            for (const auto& [d, k] : chain)
            {
                const std::string& t = node(d).type;
                if (t == "skinCluster")
                {
                    afterSkin = true;
                    continue;
                }
                if (t == "blendShape" || t == "tweak")
                    continue;
                if (!deformerMap().contains(t))
                {
                    c.handled.insert(d);
                    skip(std::format("{} '{}' on {} (no Lila counterpart; the mesh is its input)", t, shortName(node(d).name), g.node(mesh)->path()));
                    continue;
                }
                try
                {
                    buildDeformer(d, k, shape, mesh, !afterSkin && scene::skin::find(g, mesh) != kNoNode);
                }
                catch (const std::exception& e)
                {
                    warn(std::format("{} '{}': {}", t, shortName(node(d).name), e.what()));
                }
            }
        }
    }

    void buildDeformer(int d, int k, int shape, NodeId mesh, bool belowSkin)
    {
        const MNode& dn = node(d);
        const std::string where = std::format("{} '{}'", dn.type, shortName(dn.name));
        std::string lilaType(deformerMap().at(dn.type));
        int handleShape = -1;
        if (dn.type == "nonLinear")
        {
            std::tie(handleShape, lilaType) = nonlinearKind(d);
            if (lilaType.empty())
            {
                c.handled.insert(d);
                skip(where + " (its handle type is not converted)");
                return;
            }
        }
        c.handled.insert(d);
        g.evaluate();
        const NodeId id = scene::deformers::addDeformer(g, lilaType, {mesh}, name(d, lilaType.c_str()), belowSkin).front();
        deformers.try_emplace(d, id);
        setParam(id, "envelope", num(dn, {"en", "envelope"}, 1.0));
        size_t points = 0;
        if (auto pts = c.pointOf.find(shape); pts != c.pointOf.end())
            for (geo::Index p : pts->second)
                if (p != geo::kInvalid)
                    points = std::max(points, size_t(p) + 1);
        if (scene::DataPtr w = pointWeights(d, k, shape, points))
            g.setValue({id, scene::DeformerNode::kWeights, -1}, w);

        if (dn.type == "cluster" || dn.type == "softMod")
        {
            const NodeId h = matrixSource(d, {"ma", "matrix"});
            if (h == kNoNode)
                throw std::runtime_error("its handle was not converted");
            const bool relative = num(dn, {"rel", "relative"}, 0.0) != 0.0;
            g.connect({h, relative ? scene::Transform::kMatrix : scene::Transform::kWorldMatrix, -1}, {id, attrOf(id, "handleMatrix"), -1});
            glm::dmat4 pre(1.0);
            if (const AttrValue* pm = lastAttr(dn, {"pm", "bindPreMatrix"}); pm && pm->nums.size() >= 16)
                pre = matrixOf(pm->nums);
            pre[3] = glm::dvec4(glm::dvec3(pre[3]) * c.lin, pre[3].w);
            const glm::dmat4 c4(c.c3);
            setParam(id, "bindMatrix", glm::inverse(c4 * pre * glm::transpose(c4)));
            if (dn.type == "cluster")
                setParam(id, "falloffRadius", 0.0);
            else
            {
                setParam(id, "falloffRadius", num(dn, {"fr", "falloffRadius"}, 5.0) * c.lin);
                setParam(id, "falloffCenter", c.axes.translate(vec3(dn, {"fcr", "falloffCenter"}, {Names{"fcx"}, Names{"fcy"}, Names{"fcz"}}, glm::dvec3(0.0)) * c.lin));
                if (num(dn, {"fom", "falloffMode"}, 0.0) != 0.0 || num(dn, {"fas", "falloffAroundSelection"}, 0.0) != 0.0)
                    warn(where + ": surface falloff and falloff around selection are converted as a volume falloff from its centre");
                if (lastAttr(dn, {"fc", "falloffCurve"}))
                    approx = true;
            }
        }
        else if (dn.type == "ffd")
            buildFfd(d, id, where);
        else if (dn.type == "nonLinear")
            buildNonlinear(d, id, handleShape, lilaType);
        else if (dn.type == "deltaMush" || dn.type == "tension")
        {
            setParam(id, "iterations", int64_t(num(dn, {"si", "smoothingIterations"}, 10.0)));
            setParam(id, "smoothStep", std::clamp(num(dn, {"ss", "smoothingStep"}, dn.type == "tension" ? 1.0 : 0.5), 0.0, 1.0));
            setParam(id, "displacement", num(dn, {"dsp", "displacement"}, 1.0));
            setParam(id, "pinBorders", num(dn, {"pbv", "pinBorderVertices"}, 1.0) != 0.0);
            if (dn.type == "tension")
                warn(where + ": converted as a delta mush, which keeps the rest shape's detail rather than its edge lengths");
            rebind(id);
        }
        else if (dn.type == "wrap" || dn.type == "proximityWrap")
        {
            int slot = 0;
            for (const auto& [src, dst] : ix.inputs(d))
            {
                std::vector<AttrSeg> s = splitAttr(dst.attr);
                const bool driver = dn.type == "wrap" ? oneOf(s.front().name, {"dp", "driverPoints"})
                                                       : s.size() == 2 && oneOf(s[1].name, {"curdrv", "driverGeometry"});
                if (!driver)
                    continue;
                auto driverMesh = c.meshes.find(src.node);
                if (driverMesh == c.meshes.end())
                {
                    skip(std::format("{} driver '{}' (not an imported mesh)", where, shortName(node(src.node).name)));
                    continue;
                }
                g.connect({driverMesh->second, scene::MeshNode::kOutMesh, -1}, {id, attrOf(id, "drivers.mesh"), -1, slot});
                g.connect({driverMesh->second, scene::Transform::kWorldMatrix, -1}, {id, attrOf(id, "drivers.matrix"), -1, slot});
                ++slot;
            }
            if (dn.type == "proximityWrap")
                setParam(id, "maxDrivers", int64_t(std::clamp(num(dn, {"maxd", "maxDrivers"}, 10.0), 1.0, 32.0)));
            if (slot == 0)
                throw std::runtime_error("no driver mesh was converted");
            approx = true;
            rebind(id);
        }
        else if (dn.type == "jiggle")
        {
            setParam(id, "stiffness", 300.0 * num(dn, {"sf", "stiffness"}, 0.5));
            setParam(id, "damping", 16.0 * num(dn, {"dp", "damping"}, 0.5));
            warn(where + ": stiffness and damping are mapped onto Lila's spring rates; the motion is close, not identical");
        }
        else if (dn.type == "textureDeformer")
        {
            // Maya moves by strength times the texture's value plus offset; Lila by strength times its distance from midLevel.
            auto tex = ix.input(d, {"t", "texture", "tr", "textureR"});
            const MNode* file = tex ? &node(tex->node) : nullptr;
            if (!file || file->type != "file")
                throw std::runtime_error("only a file texture converts");
            c.handled.insert(tex->node);
            setParam(id, "image", c.resolveImage(text(*file, {"ftn", "fileTextureName"}), where));
            const double strength = num(dn, {"s", "strength"}, 1.0), offset = num(dn, {"o", "offset"}, 0.0);
            setParam(id, "strength", strength * c.lin);
            setParam(id, "midLevel", std::abs(strength) > 1e-12 ? -offset / strength : 0.0);
            const int direction = int(num(dn, {"d", "direction"}, 1.0));
            setParam(id, "direction", int64_t(direction == 2 ? 1 : 0));
            if (direction == 0 || int(num(dn, {"ps", "pointSpace"}, 2.0)) != 2)
                warn(where + ": converted as a uv lookup displacing along the normals or by the rgb vector");
            rebind(id);
        }
        else if (dn.type == "shrinkWrap")
        {
            auto tgt = ix.input(d, {"tgt", "targetGeom"});
            auto target = tgt ? c.meshes.find(tgt->node) : c.meshes.end();
            if (target == c.meshes.end())
                throw std::runtime_error("its target is not an imported mesh");
            g.connect({target->second, scene::MeshNode::kOutMesh, -1}, {id, attrOf(id, "target"), -1});
            g.connect({target->second, scene::Transform::kWorldMatrix, -1}, {id, attrOf(id, "targetMatrix"), -1});
            const int prj = int(num(dn, {"prj", "projection"}, 0.0));
            if (prj == 5)
                setParam(id, "mode", int64_t(1));
            else if (prj != 4)
                warn(std::format("{}: projection {} is converted as closest point", where, prj));
            setParam(id, "offset", (num(dn, {"o", "offset"}, 0.0) + num(dn, {"ti", "targetInflation"}, 0.0)) * c.lin);
        }
    }

    void buildFfd(int d, NodeId id, const std::string& where)
    {
        const MNode& dn = node(d);
        auto dlp = ix.input(d, {"dlp", "deformedLatticePoints"});
        const int latShape = dlp ? dlp->node : -1;
        const NodeId lat = latShape >= 0 ? lilaParentOf(latShape) : kNoNode;
        if (lat == kNoNode || !g.node(lat)->type().isA("lattice"))
            throw std::runtime_error("its lattice was not converted");
        const MNode& ln = node(latShape);
        glm::ivec3 nm(int(num(ln, {"sd", "sDivisions"}, 2.0)), int(num(ln, {"td", "tDivisions"}, 5.0)), int(num(ln, {"ud", "uDivisions"}, 2.0)));
        // The cached lattice: "s t u count" then count points, s fastest.
        std::vector<glm::dvec3> pts;
        if (const AttrValue* cc = lastAttr(ln, {"cc", "cachedLattice"}))
        {
            std::vector<double> v = cc->nums;
            if (v.empty())
                for (const std::string& s : cc->strs)
                    v.push_back(std::atof(s.c_str()));
            if (v.size() >= 4)
            {
                nm = glm::ivec3(int(v[0]), int(v[1]), int(v[2]));
                for (size_t i = 4; i + 2 < v.size(); i += 3)
                    pts.push_back({v[i], v[i + 1], v[i + 2]});
            }
        }
        nm = glm::max(nm, glm::ivec3(2));
        const size_t count = size_t(nm.x) * size_t(nm.y) * size_t(nm.z);
        if (pts.size() != count)
        {
            pts.resize(count);
            for (int k = 0; k < nm.z; ++k)
                for (int j = 0; j < nm.y; ++j)
                    for (int i = 0; i < nm.x; ++i)
                        pts[deform::latticeIndex(nm, i, j, k)] = deform::latticeRest(nm, i, j, k);
        }
        for (const AttrValue& a : ln.attrs)
        {
            std::vector<AttrSeg> s = splitAttr(a.attr);
            if (s.size() != 1 || !oneOf(s[0].name, {"pt", "controlPoints"}) || s[0].lo < 0)
                continue;
            for (size_t i = 0; i + 2 < a.nums.size(); i += 3)
                if (size_t p = size_t(s[0].lo) + i / 3; p < pts.size())
                    pts[p] += glm::dvec3(a.nums[i], a.nums[i + 1], a.nums[i + 2]);
        }
        // Lila orders the points along its own axes: Maya axis a is Lila axis to[a], walked backwards where it flips.
        glm::ivec3 nl;
        for (int a = 0; a < 3; ++a)
            nl[c.axes.to[a]] = nm[a];
        geo::Mesh mesh = deform::latticeMesh(nl);
        for (int k = 0; k < nm.z; ++k)
            for (int j = 0; j < nm.y; ++j)
                for (int i = 0; i < nm.x; ++i)
                {
                    const int idx[3] = {i, j, k};
                    int li[3];
                    for (int a = 0; a < 3; ++a)
                        li[c.axes.to[a]] = c.axes.sign[a] > 0 ? idx[a] : nm[a] - 1 - idx[a];
                    mesh.setPosition(geo::Index(deform::latticeIndex(nl, li[0], li[1], li[2])), c.c3 * pts[deform::latticeIndex(nm, i, j, k)]);
                }
        g.setValue({lat, scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
        setParam(id, "divisions", glm::dvec3(nl));
        setParam(id, "interpolation", int64_t(num(dn, {"lo", "local"}, 0.0) != 0.0 ? 2 : 1));
        const int outside = int(num(dn, {"ot", "outsideLattice"}, 0.0));
        setParam(id, "outsideFalloff", outside == 1 ? 1e6 : outside == 2 ? num(dn, {"ofd", "outsideFalloffDist"}, 1.0) : 0.0);
        g.connect({lat, scene::MeshNode::kOutMesh, -1}, {id, attrOf(id, "lattice"), -1});
        g.connect({lat, scene::Transform::kWorldMatrix, -1}, {id, attrOf(id, "latticeMatrix"), -1});
        g.evaluate();
        NodeId base = matrixSource(d, {"blm", "baseLatticeMatrix"});
        if (base == kNoNode)
            warn(where + ": no base lattice; the lattice's current placement is its rest");
        setParam(id, "baseMatrix", g.node(base != kNoNode ? base : lat)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        if (num(dn, {"lo", "local"}, 0.0) != 0.0)
            approx = true;
    }

    void buildNonlinear(int d, NodeId id, int handleShape, const std::string& type)
    {
        const NodeId h = handleShape >= 0 ? lilaParentOf(handleShape) : kNoNode;
        if (h == kNoNode)
            throw std::runtime_error("its handle was not converted");
        g.connect({h, scene::Transform::kWorldMatrix, -1}, {id, attrOf(id, "handleMatrix"), -1});
        auto p = [&](Names names, double def) { return nlParam(d, handleShape, names, def); };
        if (type != "waveDeformer")
        {
            setParam(id, "lowBound", p({"lb", "lowBound"}, -1.0));
            setParam(id, "highBound", p({"hb", "highBound"}, 1.0));
        }
        const std::string where = std::format("{} '{}'", type, shortName(node(d).name));
        if (type == "bendDeformer")
            // Maya's curvature is the bend per unit along the handle; Lila's angle spans the bounds.
            setParam(id, "angle", p({"cur", "curvature"}, 0.0) * c.ang * (p({"hb", "highBound"}, 1.0) - p({"lb", "lowBound"}, -1.0)));
        else if (type == "twistDeformer")
        {
            // Maya twists the other way round the handle's Y.
            setParam(id, "startAngle", -p({"sa", "startAngle"}, 0.0) * c.ang);
            setParam(id, "endAngle", -p({"ea", "endAngle"}, 0.0) * c.ang);
        }
        else if (type == "squashDeformer")
        {
            // Maya compresses by 1 / (1 - factor) for a negative factor.
            const double f = p({"fac", "factor"}, 0.0);
            const double sy = f < 0.0 ? 1.0 / (1.0 - f) : 1.0 + f;
            setParam(id, "factor", sy - 1.0);
            // Lila bulges to 1 / sqrt(sy) at full expand, Maya to about 1 / sy.
            const double lilaPeak = 1.0 / std::sqrt(sy) - 1.0;
            setParam(id, "expand", p({"exp", "expand"}, 1.0) * (std::abs(lilaPeak) > 1e-9 ? (1.0 / sy - 1.0) / lilaPeak : 2.0));
            setParam(id, "maxExpandPos", p({"mp", "maxExpandPos"}, 0.5));
            warn(where + ": the length matches Maya's; the sides bulge by Lila's profile, close to Maya's");
        }
        else if (type == "sineDeformer" || type == "waveDeformer")
        {
            const double wl = p({"wav", "wavelength"}, type == "sineDeformer" ? 2.0 : 1.0);
            const double off = p({"off", "offset"}, 0.0) / (std::abs(wl) > 1e-12 ? wl : 1.0);
            setParam(id, "amplitude", p({"amp", "amplitude"}, 0.0));
            setParam(id, "wavelength", wl);
            setParam(id, "dropoff", p({"dr", "dropoff"}, 0.0));
            // Maya's offset is a length along the wave; its wave is a cosine of the distance out from minRadius.
            setParam(id, "offset", type == "sineDeformer" ? off : -off - 0.25);
            if (type == "waveDeformer")
            {
                setParam(id, "minRadius", p({"mnr", "minRadius"}, 0.0));
                setParam(id, "maxRadius", p({"mxr", "maxRadius"}, 1.0));
                if (p({"mnr", "minRadius"}, 0.0) != 0.0 || p({"dr", "dropoff"}, 0.0) < 0.0)
                    warn(where + ": a minimum radius or a negative dropoff ripples differently from Maya's");
            }
        }
        else if (type == "taperDeformer")
        {
            const double sx = p({"sfx", "startFlareX"}, 1.0), sz = p({"sfz", "startFlareZ"}, 1.0);
            const double ex = p({"efx", "endFlareX"}, 1.0), ez = p({"efz", "endFlareZ"}, 1.0);
            setParam(id, "startScale", (sx + sz) * 0.5);
            setParam(id, "endScale", (ex + ez) * 0.5);
            // Maya's curve scales the flare; Lila's adds to it. They agree midway.
            const double curve = p({"crv", "curve"}, 0.0);
            setParam(id, "curve", curve * (sx + sz + ex + ez) * 0.25);
            if (std::abs(sx - sz) > 1e-9 || std::abs(ex - ez) > 1e-9)
                warn(where + ": different X and Z flares are averaged");
            if (curve != 0.0)
                warn(where + ": its curve bulges by Lila's profile, matching Maya's midway");
        }
    }

    /** A plug of a converted deformer: its envelope, or a nonlinear parameter. */
    End deformerEnd(int maya, NodeId id, const std::string& attr)
    {
        if (oneOf(attr, {"en", "envelope"}))
            return {.kind = End::Scalar, .plug = {id, scene::DeformerNode::kEnvelope, -1}};
        static const std::map<std::string_view, std::pair<const char*, Unit>> kParams = {
            {"lb", {"lowBound", Unit::None}},        {"lowBound", {"lowBound", Unit::None}},
            {"hb", {"highBound", Unit::None}},       {"highBound", {"highBound", Unit::None}},
            {"sa", {"startAngle", Unit::Angle}},     {"startAngle", {"startAngle", Unit::Angle}},
            {"ea", {"endAngle", Unit::Angle}},       {"endAngle", {"endAngle", Unit::Angle}},
            {"fac", {"factor", Unit::None}},         {"factor", {"factor", Unit::None}},
            {"exp", {"expand", Unit::None}},         {"expand", {"expand", Unit::None}},
            {"amp", {"amplitude", Unit::None}},      {"amplitude", {"amplitude", Unit::None}},
            {"wav", {"wavelength", Unit::None}},     {"wavelength", {"wavelength", Unit::None}},
            {"off", {"offset", Unit::None}},         {"offset", {"offset", Unit::None}},
            {"dr", {"dropoff", Unit::None}},         {"dropoff", {"dropoff", Unit::None}},
            {"crv", {"curve", Unit::None}},          {"curve", {"curve", Unit::None}},
            {"fr", {"falloffRadius", Unit::Linear}}, {"falloffRadius", {"falloffRadius", Unit::Linear}},
        };
        if (node(maya).type == "nonLinear" && oneOf(attr, {"cur", "curvature"}))
        {
            // angle = curvature * (high - low), with the bounds as imported.
            const double span = g.node(id)->as<double>(attrOf(id, "highBound")) - g.node(id)->as<double>(attrOf(id, "lowBound"));
            return {.kind = End::Scalar, .plug = {id, attrOf(id, "angle"), -1}, .factor = span > 1e-12 ? kDeg / span : kDeg, .unit = Unit::Angle};
        }
        const std::string& lilaType = g.node(id)->type().name;
        if (auto it = kParams.find(attr); it != kParams.end())
            if (int a = attrOf(id, it->second.first); a >= 0)
            {
                std::string_view param = it->second.first;
                double factor = it->second.second == Unit::Angle ? kDeg : 1.0;
                if (lilaType == "twistDeformer")
                    factor = -factor;
                if (param == "offset" && (lilaType == "sineDeformer" || lilaType == "waveDeformer"))
                {
                    if (lilaType == "waveDeformer")
                        return {};
                    factor = g.node(id)->as<double>(attrOf(id, "wavelength"));
                }
                if (param == "factor" && lilaType == "squashDeformer")
                    return {};
                return {.kind = End::Scalar, .plug = {id, a, -1}, .factor = factor, .unit = it->second.second};
            }
        return {};
    }

    // ---- surface pins ---------------------------------------------------------------------------------------

    /** The Lila mesh a pin's geometry input comes from. */
    NodeId pinMesh(int maya, Names names) const
    {
        auto src = ix.input(maya, names);
        if (!src)
            return kNoNode;
        auto it = c.meshes.find(src->node);
        return it == c.meshes.end() ? kNoNode : it->second;
    }

    /** A surfacePin on mesh at uv, reading its first uv set; a local one gives the frame in the mesh's own space. */
    NodeId makePin(NodeId mesh, const std::string& nm, double u, double v, bool local)
    {
        const NodeId pin = makeNode("surfacePin", nm);
        g.connect({mesh, scene::MeshNode::kOutMesh, -1}, {pin, attrOf(pin, "inMesh"), -1});
        if (!local)
            g.connect({mesh, scene::Transform::kWorldMatrix, -1}, {pin, attrOf(pin, "meshMatrix"), -1});
        g.setValue({pin, attrOf(pin, "uvSet"), -1}, geo::attr::uvName(0));
        g.setValue({pin, attrOf(pin, "u"), -1}, u);
        g.setValue({pin, attrOf(pin, "v"), -1}, v);
        return pin;
    }

    /**
     * Lila's pin frame has z on the normal and x along u; Maya's puts the normal on normalAxis and the u tangent on
     * tangentAxis (0..5: +x +y +z -x -y -z). The local turn taking one to the other, in Lila space.
     */
    glm::dmat4 frameTurn(int normalAxis, int tangentAxis) const
    {
        auto axis = [](int k) {
            glm::dvec3 v(0.0);
            v[k % 3] = k >= 3 ? -1.0 : 1.0;
            return v;
        };
        const glm::dvec3 n = axis(normalAxis), t = axis(tangentAxis);
        if (std::abs(glm::dot(n, t)) > 0.5)
            return glm::dmat4(1.0);
        // Maya's local axes as Lila frame columns: t -> x, n -> z, n x t -> y.
        const glm::dmat3 maya(t, glm::cross(n, t), n);
        return glm::dmat4(glm::transpose(maya) * glm::transpose(c.c3));
    }

    /** A uvPin's coordinate: packed "coord[a:b]" pairs, or its u and v set one by one. */
    static std::pair<double, double> uvCoordinate(const MNode& mn, int index)
    {
        double u = 0.0, v = 0.0;
        for (const AttrValue& a : mn.attrs)
        {
            std::vector<AttrSeg> s = splitAttr(a.attr);
            if (s.empty() || !oneOf(s[0].name, {"coord", "coordinate"}) || s[0].lo < 0 || a.nums.empty())
                continue;
            if (s.size() == 1)
            {
                const size_t at = size_t(index - s[0].lo) * 2;
                if (index >= s[0].lo && index <= std::max(s[0].lo, s[0].hi) && at + 1 < a.nums.size())
                    u = a.nums[at], v = a.nums[at + 1];
            }
            else if (s[0].lo == index && oneOf(s[1].name, {"cu", "coordinateU"}))
                u = a.nums[0];
            else if (s[0].lo == index && oneOf(s[1].name, {"cv", "coordinateV"}))
                v = a.nums[0];
        }
        return {u, v};
    }

    /** The node whose matrix output is pin output index of a Maya follicle, uvPin or proximityPin, in Lila space. */
    NodeId pinFrame(int maya, int index, std::string* why)
    {
        if (auto it = pinFrames[maya].find(index); it != pinFrames[maya].end())
            return it->second;
        const MNode& mn = node(maya);
        pinFrames[maya][index] = kNoNode;
        c.handled.insert(maya);
        const NodeId mesh = pinMesh(maya, mn.type == "follicle" ? Names{"inm", "inputMesh"} : Names{"curgeom", "deformedGeometry"});
        if (mesh == kNoNode)
        {
            *why = std::format("{} '{}' is not on an imported mesh", mn.type, shortName(mn.name));
            return kNoNode;
        }
        const std::string base = name(maya, "pin");
        double u = 0.5, v = 0.5;
        glm::dmat4 post(1.0), offset(1.0);
        if (mn.type == "follicle")
        {
            u = num(mn, {"pu", "parameterU"}, 0.0);
            v = num(mn, {"pv", "parameterV"}, 0.0);
            // A follicle's z is the normal and its x points against u.
            post = frameTurn(2, 3);
        }
        else if (mn.type == "uvPin")
        {
            std::tie(u, v) = uvCoordinate(mn, index);
            post = frameTurn(int(num(mn, {"nrm", "normalAxis"}, 0.0)), int(num(mn, {"tng", "tangentAxis"}, 2.0)));
        }
        else
        {
            // proximityPin: bound now to the surface point nearest its input matrix, keeping the input's offset from it.
            glm::dmat4 in(1.0);
            if (const AttrValue* a = lastAttr(mn, {std::format("imat[{}]", index), std::format("inputMatrix[{}]", index)}); a && a->nums.size() >= 16)
            {
                in = matrixOf(a->nums);
                in[3] = glm::dvec4(glm::dvec3(in[3]) * c.lin, 1.0);
                const glm::dmat4 c4(c.c3);
                in = c4 * in * glm::transpose(c4);
            }
            g.evaluate();
            scene::MeshPtr surface = g.node(mesh)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            const glm::dmat4 w = g.node(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            if (!surface)
            {
                *why = "its mesh has no geometry";
                return kNoNode;
            }
            deform::SurfacePoint sp;
            if (!deform::locateNearest(*surface, glm::dvec3(glm::inverse(w) * in[3]), geo::attr::uvName(0), sp) || !sp.hasUv)
            {
                *why = std::format("proximityPin '{}': its mesh has no uvs to pin by", shortName(mn.name));
                return kNoNode;
            }
            glm::dvec2 uv = sp.uv[0] * sp.bary[0] + sp.uv[1] * sp.bary[1] + sp.uv[2] * sp.bary[2];
            u = uv.x;
            v = uv.y;
            std::vector<glm::dvec3> pts(surface->positions().begin(), surface->positions().end());
            offset = glm::inverse(w * deform::surfaceFrame(pts, sp)) * in;
        }
        // uvPins frame the surface in the mesh's space and carry its world matrix, scale included, as Maya's do.
        const bool local = mn.type != "follicle";
        const NodeId pin = makePin(mesh, base, u, v, local);
        pins[maya * 1024 + index] = pin;
        if (!local && post == glm::dmat4(1.0))
            return pinFrames[maya][index] = pin;
        const NodeId mm = makeNode("multMatrix", base + "_frame");
        const int inA = attrOf(mm, "matrixIn");
        g.setValue({mm, inA, -1, 0}, mn.type == "proximityPin" ? offset : post);
        g.connect({pin, attrOf(pin, "outMatrix"), -1}, {mm, inA, -1, 1});
        if (local)
            g.connect({mesh, scene::Transform::kWorldMatrix, -1}, {mm, inA, -1, 2});
        return pinFrames[maya][index] = mm;
    }

    End pinEnd(const Plug& p, std::string* why)
    {
        const MNode& mn = node(p.node);
        std::vector<AttrSeg> segs = splitAttr(p.attr);
        if (segs.empty())
            return {};
        const int index = std::max(segs.front().lo, 0);
        const std::string& a = segs.back().name;
        // Inputs: the coordinates, which may be driven.
        if (mn.type != "proximityPin")
        {
            const bool uAttr = oneOf(a, {"pu", "parameterU", "cu", "coordinateU"}), vAttr = oneOf(a, {"pv", "parameterV", "cv", "coordinateV"});
            if (uAttr || vAttr)
            {
                if (pinFrame(p.node, index, why) == kNoNode)
                    return {};
                const NodeId pin = pins[p.node * 1024 + index];
                return {.kind = End::Scalar, .plug = {pin, attrOf(pin, uAttr ? "u" : "v"), -1}};
            }
        }
        const NodeId frame = pinFrame(p.node, index, why);
        if (frame == kNoNode)
            return {};
        const PlugRef matrixOut{frame, g.node(frame)->type().name == "surfacePin" ? attrOf(frame, "outMatrix") : attrOf(frame, "matrixSum"), -1};
        if (oneOf(a, {"omat", "outputMatrix"}))
            return matrixEnd(matrixOut, true);
        if (mn.type == "follicle" && oneOf(a, {"ot", "outTranslate", "otx", "oty", "otz", "or", "outRotate", "orx", "ory", "orz"}))
        {
            auto [it, fresh] = follicleXform.try_emplace(p.node, kNoNode);
            if (fresh)
            {
                // The follicle's transform takes its world frame as translate and rotate (Maya leaves it unparented).
                it->second = makeNode("decomposeMatrix", name(p.node, "follicle") + "_decompose");
                g.connect(matrixOut, {it->second, attrOf(it->second, "inputMatrix"), -1});
                if (NodeId xf = lilaParentOf(p.node); xf != kNoNode)
                    g.setValue({it->second, attrOf(it->second, "inputRotateOrder"), -1}, g.node(xf)->value(scene::Transform::kRotateOrder));
            }
            const bool rot = a.starts_with("or") || a.starts_with("outR");
            End v = vecEnd(it->second, attrOf(it->second, rot ? "outputRotate" : "outputTranslate"), rot ? 1 : 0, rot ? Unit::Angle : Unit::Linear);
            // The decompose works in Lila space already, so its components map straight across.
            for (int k = 0; k < 3; ++k)
            {
                v.comp[k] = c.axes.to[k];
                v.sign[k] = c.axes.factor(rot ? 1 : 0, k);
            }
            if (a.size() == 3 && (a[2] == 'x' || a[2] == 'y' || a[2] == 'z'))
                return v.component(a[2] - 'x');
            return v;
        }
        *why = std::format("{}.{} has no Lila counterpart", shortName(mn.name), p.attr);
        return {};
    }

    // ---- the pass over every connection ---------------------------------------------------------------------

    /** Inputs the converted nodes take care of themselves, or that are Maya bookkeeping. */
    static bool internalInput(const std::string& attr)
    {
        static const std::set<std::string_view> k = {"is", "inverseScale", "iog", "instObjGroups", "hsj", "hee", "hsv", "startJoint", "endEffector",
                                                     "ikSolver", "dl", "drawInfo", "sml", "msg", "message", "hp", "handlePath", "bnm", "fbda",
                                                     "ic", "inCurve"};
        std::vector<AttrSeg> s = splitAttr(attr);
        return !s.empty() && k.contains(s.front().name);
    }

    void connectAll()
    {
        for (const FileIndex::Conn& cn : ix.conns)
        {
            const MNode& dn = node(cn.dst.node);
            const MNode& sn = node(cn.src.node);
            if (internalInput(cn.dst.attr) || sn.type == "reference" || dn.type == "reference")
                continue;
            const bool toTransform = lilaOf(cn.dst.node) != kNoNode;
            const bool toConstraintWeight = constraintTypes().contains(dn.type) && cn.src.node != cn.dst.node &&
                                            aliasWeight.contains({cn.dst.node, cn.dst.attr});
            // Into a converted deformer only its envelope and the settings Lila has; the rest is the deformer's own wiring.
            const bool toDeformer = deformers.contains(cn.dst.node) && cn.src.node != cn.dst.node &&
                                    deformerEnd(cn.dst.node, deformers.at(cn.dst.node), cn.dst.attr).kind != End::None;
            const bool toPin = (dn.type == "follicle" || dn.type == "uvPin") &&
                               oneOf(splitAttr(cn.dst.attr).back().name, {"pu", "pv", "parameterU", "parameterV", "cu", "cv", "coordinateU", "coordinateV"});
            if (!toTransform && !toConstraintWeight && !toDeformer && !toPin)
                continue;
            if (sn.type.starts_with("animCurveT") && !c.opts.animation)
                continue;
            std::string why;
            if (!connectPlugs(cn.src, cn.dst, &why))
                skip(std::format("{} driving {} ({})", plugText(ix, cn.src), plugText(ix, cn.dst), why));
        }
    }

    void report()
    {
        if (approx)
            warn("rig: some Maya settings were approximated (curve tangents, lattice local mode, softMod falloff curves, wrap bindings)");
        for (const auto& [what, count] : skipped)
            c.r.skipped.push_back(count > 1 ? std::format("{} ({} times)", what, count) : what);
    }
};

RigBuilder::RigBuilder(RigContext ctx) : m(std::make_unique<Impl>(std::move(ctx)))
{
    m->addUserAttrs();
}

RigBuilder::~RigBuilder() = default;

void RigBuilder::build()
{
    LILA_ZONE();

    Impl& r = *m;
    const FileIndex& ix = r.ix;
    for (size_t i = 0; i < ix.file.nodes.size(); ++i)
        if (ix.file.nodes[i].type == "ikHandle" && r.lilaOf(int(i)) != kNoNode)
            r.buildIk(int(i));
    r.buildDeformers();
    for (size_t i = 0; i < ix.file.nodes.size(); ++i)
        if (constraintTypes().contains(ix.file.nodes[i].type))
            r.constraintNode(int(i));
    r.connectAll();
    for (size_t i = 0; i < ix.file.nodes.size(); ++i)
        if (ix.file.nodes[i].type == "ikHandle" && r.lilaOf(int(i)) != kNoNode && !scene::ik::chainOf(r.g, r.lilaOf(int(i))).empty())
        {
            r.liveStartTranslate(r.lilaOf(int(i)));
            r.liveChainSlots(r.lilaOf(int(i)));
        }
    // Utility nodes nothing converted reads still come across, so the network is whole.
    for (size_t i = 0; i < ix.file.nodes.size(); ++i)
        if (mathTypes().contains(ix.file.nodes[i].type))
            r.mathNode(int(i));
    for (size_t i = 0; i < ix.file.nodes.size(); ++i)
    {
        const MNode& n = ix.file.nodes[i];
        if (n.type == "expression" && !r.c.handled.contains(int(i)))
        {
            r.c.handled.insert(int(i));
            std::string body = text(n, {"ixp", "internalExpression", "es", "expression"});
            if (body.size() > 60)
                body = body.substr(0, 57) + "...";
            r.skip(std::format("expression '{}' (MEL expressions are not converted; driven channels keep their values): {}", shortName(n.name), body));
        }
        else if (oneOf(n.type, {"motionPath", "curveInfo", "pointOnCurveInfo", "pointOnSurfaceInfo"}) &&
                 !r.c.handled.contains(int(i)))
        {
            r.c.handled.insert(int(i));
            r.skip(std::format("{} '{}' (needs NURBS curves or surfaces, which are not imported)", n.type, shortName(n.name)));
        }
        else if (oneOf(n.type, {"HIKCharacterNode", "HIKSolverNode", "HIKState2SK", "HIKSK2State", "HIKProperty2State", "HIKControlSetNode",
                                "HIKRetargeterNode", "HIKEffector2State", "HIKFK2State", "HIKState2FK", "HIKState2Effector"}) &&
                 !r.c.handled.contains(int(i)))
        {
            r.c.handled.insert(int(i));
            r.skip(std::format("HumanIK node '{}' ({}; the skeleton keeps its pose and keys)", shortName(n.name), n.type));
        }
    }
    r.report();
}

bool RigBuilder::drive(const Plug& src, const std::vector<PlugRef>& dst, std::string* why)
{
    if (dst.empty())
        return false;
    End d{.kind = End::Scalar, .plug = dst.front()};
    if (!m->connectInto(src, d, why))
        return false;
    for (size_t k = 1; k < dst.size(); ++k)
        if (auto s = m->g.source(dst.front()))
            m->tryConnect(*s, dst[k], why);
    return true;
}

bool RigBuilder::convertedAttr(int node, const std::string& longName) const { return m->userAttrs.contains({node, longName}); }

bool RigBuilder::convertsDeformer(std::string_view t) { return deformerMap().contains(t); }

bool RigBuilder::convertsInputsOf(std::string_view t)
{
    return mathTypes().contains(t) || constraintTypes().contains(t) || deformerMap().contains(t) || oneOf(t, {"follicle", "uvPin", "proximityPin"});
}

bool RigBuilder::ownsShape(std::string_view t)
{
    return constraintTypes().contains(t) || nonlinearHandles().contains(t) ||
           oneOf(t, {"ikEffector", "clusterHandle", "softModHandle", "baseLattice", "lattice", "follicle"});
}
} // namespace lila::maya
