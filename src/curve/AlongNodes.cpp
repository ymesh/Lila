#include "curve/AlongNodes.h"

#include "core/Profile.h"
#include "curve/Along.h"
#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "curve/Pipe.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Duplicate.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>

namespace lila::curve
{
using namespace scene;
using namespace helpers;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
const std::vector<std::string> kAxes = {"+x", "-x", "+y", "-y", "+z", "-z"};
const std::vector<std::string> kUpModes = {"curveFrames", "autoNormals", "aimCurve"};

AttrDef hidden(std::string name, PlugType type, PlugValue def, std::string doc, bool stored = false)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrHidden | (stored ? AttrStorable : 0u),
            .doc = std::move(doc)};
}

/** inMesh, enabled and exportEnabled, as every modifier has them first. */
std::vector<AttrDef> modifierHead()
{
    return {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        {.name = "enabled", .type = PlugType::Bool, .defaultValue = true, .flags = kKey, .doc = "Turn the modifier off without removing it."},
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = kIn, .doc = "Apply the modifier when exporting."},
    };
}

void registerModifier(std::string name, std::string icon, std::string doc, std::vector<AttrDef> attrs, ModifierNode::Apply apply)
{
    if (types::find(name))
        return;
    NodeType t;
    t.name = std::move(name);
    t.icon = std::move(icon);
    t.doc = std::move(doc);
    t.ownAttrs = modifierHead();
    for (AttrDef& a : attrs)
        t.ownAttrs.push_back(std::move(a));
    t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden});
    const int out = int(t.ownAttrs.size()) - 1;
    t.create = [apply, out] {
        auto n = std::make_unique<ModifierNode>();
        n->apply = apply;
        n->outAttr = out;
        return n;
    };
    types::registerType(std::move(t));
}

/** The first spline on a curve input, carried into the modified mesh's object space. */
std::optional<Spline> splineIn(const Node& n, const char* attr, const glm::dmat4& toLocal)
{
    const Splines& sp = splinesOf(n.value(idx(n, attr)));
    if (sp.empty())
        return std::nullopt;
    Spline s = sp.front();
    transform(s, toLocal);
    return s;
}

/** The rail a modifier follows: its curve input carried by toLocal, framed by its up settings. */
Rail railOf(const Node& n, const glm::dmat4& toLocal, const char* cmd)
{
    std::optional<Spline> path = splineIn(n, "curve", toLocal);
    if (!path)
        throw std::runtime_error(std::string("no curve: connect one with ") + cmd);
    RailOptions ro;
    ro.up = UpMode(std::clamp<int64_t>(n.as<int64_t>(idx(n, "upMode")), 0, 2));
    ro.upVector = n.as<D3>(idx(n, "upVector"));
    ro.aim = splineIn(n, "aimCurve", toLocal);
    if (ro.up == UpMode::AimCurve && !ro.aim)
        throw std::runtime_error("upMode is aimCurve but no aim curve is connected");
    Rail rail(*path, ro);
    if (!rail.valid())
        throw std::runtime_error("the curve has no length");
    return rail;
}

/** The rail in the modified mesh's object space. */
Rail localRail(const Node& n, const char* cmd) { return railOf(n, glm::inverse(n.as<glm::dmat4>(idx(n, "targetMatrix"))), cmd); }

std::function<double(double)> scaleRampOf(const Node& n) { return parseRamp(s(n, "scaleRamp")); }

/** Twist in radians at t: the linear twist plus the ramp's degrees. */
std::function<double(double)> twistOf(const Node& n)
{
    double total = glm::radians(f(n, "twist"));
    std::function<double(double)> ramp = parseRamp(s(n, "twistRamp"));
    if (total == 0.0 && !ramp)
        return {};
    return [total, ramp](double t) { return total * t + (ramp ? glm::radians(ramp(t)) : 0.0); };
}

std::vector<AttrDef> curveFollowInputs()
{
    return {
        hidden("curve", PlugType::Data, DataPtr(), "The curve to follow, world space."),
        hidden("aimCurve", PlugType::Data, DataPtr(), "A second curve the frame's up aims at (upMode aimCurve), world space."),
        hidden("targetMatrix", PlugType::Matrix, glm::dmat4(1.0), "This mesh's world matrix when the curve was connected.", true),
    };
}

std::vector<AttrDef> frameAttrs(const std::string& cat)
{
    return {
        eAttr("forwardAxis", kAxes, 0, "The mesh axis laid along the curve.", cat),
        eAttr("upMode", kUpModes, 0,
              "How the mesh turns about the curve: the curve's own frames (no twist, with its tilt), auto normals leaning toward the up "
              "vector, or aimed at a second curve.",
              cat),
        vAttr("upVector", D3(0, 1, 0), "Up for auto normals, and where curve frames start.", cat),
        fAttr("roll", 0.0, "Turn about the curve, degrees.", cat),
    };
}

std::vector<AttrDef> rampAttrs(const std::string& cat)
{
    return {
        sAttr("scaleRamp", "Cross-section scale along the curve: keys \"t:scale\" with t from 0 to 1, e.g. \"0:1 1:0.2\".", cat),
        fAttr("twist", 0.0, "Twist from the curve's start to its end, degrees.", cat),
        sAttr("twistRamp", "Extra twist along the curve: keys \"t:degrees\", e.g. \"0:0 0.5:90 1:0\".", cat),
    };
}

geo::Mesh deformRun(const geo::Mesh& in, const Node& n)
{
    LILA_ZONE();

    Rail rail = localRail(n, "deformAlongCurve");
    DeformAlongOptions o;
    o.forward = AlongAxis(std::clamp<int64_t>(i(n, "forwardAxis"), 0, 5));
    o.roll = glm::radians(f(n, "roll"));
    o.offset = f(n, "offset");
    o.offsetFraction = f(n, "offsetFraction");
    o.stretch = b(n, "stretch");
    o.scale = scaleRampOf(n);
    o.twist = twistOf(n);
    o.loop = b(n, "loop");
    o.extrapolate = i(n, "endBehavior") == 0;
    o.envelope = std::clamp(f(n, "envelope"), 0.0, 1.0);
    geo::Mesh m = in;
    deformAlongCurve(m, rail, o);
    return m;
}

ArrayAlongOptions arrayOptionsOf(const Node& n)
{
    ArrayAlongOptions o;
    o.fill = ArrayFill(std::clamp<int64_t>(i(n, "fill"), 0, 1));
    o.count = int(std::clamp<int64_t>(i(n, "count"), 1, 100000));
    o.spacing = std::max(0.0, f(n, "spacing"));
    o.fitToLength = b(n, "fitToLength");
    o.startOffset = std::max(0.0, f(n, "startOffset"));
    o.endOffset = std::max(0.0, f(n, "endOffset"));
    o.forward = AlongAxis(std::clamp<int64_t>(i(n, "forwardAxis"), 0, 5));
    o.roll = glm::radians(f(n, "roll"));
    o.align = b(n, "align");
    o.deform = b(n, "deform");
    o.weld = b(n, "weld");
    o.scale = scaleRampOf(n);
    o.twist = twistOf(n);
    D3 rr = v(n, "randomRotate");
    o.randomRotate = D3(glm::radians(rr.x), glm::radians(rr.y), glm::radians(rr.z));
    o.randomScale = std::clamp(f(n, "randomScale"), 0.0, 1.0);
    o.randomOffset = v(n, "randomOffset");
    o.seed = uint32_t(i(n, "seed"));
    return o;
}

geo::Mesh arrayRun(const geo::Mesh& in, const Node& n)
{
    LILA_ZONE();

    Rail rail = localRail(n, "arrayAlongCurve");
    const MeshPtr& start = n.as<MeshPtr>(idx(n, "startMesh"));
    const MeshPtr& end = n.as<MeshPtr>(idx(n, "endMesh"));
    return arrayAlongCurve(in, start && !start->empty() ? start.get() : nullptr, end && !end->empty() ? end.get() : nullptr, rail,
                           arrayOptionsOf(n));
}

std::vector<geo::Index> facesFromJson(const std::string& text)
{
    std::vector<geo::Index> out;
    if (text.empty())
        return out;
    json j = json::parse(text, nullptr, false);
    if (j.is_array())
        for (const json& e : j)
            if (e.is_number_integer() && e.get<int64_t>() >= 0)
                out.push_back(geo::Index(e.get<int64_t>()));
    return out;
}

geo::Mesh flowRun(const geo::Mesh& in, const Node& n)
{
    LILA_ZONE();

    MeshPtr target = n.as<MeshPtr>(idx(n, "targetMesh"));
    if (!target)
        throw std::runtime_error("no target: connect one with flowAlongSurface");
    geo::Mesh surface = *target;
    glm::dmat4 toLocal = glm::inverse(n.as<glm::dmat4>(idx(n, "targetMatrix"))) * n.as<glm::dmat4>(idx(n, "targetWorld"));
    for (geo::Index p = 0; p < surface.pointCapacity(); ++p)
        if (surface.pointValid(p))
            surface.setPosition(p, D3(toLocal * glm::dvec4(surface.position(p), 1.0)));
    FlowOptions o;
    o.param = FlowParam(std::clamp<int64_t>(i(n, "parametrization"), 0, 1));
    o.uvSet = s(n, "uvSet").empty() ? std::string("uv0") : s(n, "uvSet");
    o.rect = glm::dvec4(f(n, "uMin"), f(n, "vMin"), f(n, "uMax"), f(n, "vMax"));
    o.faces = facesFromJson(s(n, "targetFaces"));
    o.swapUv = b(n, "swapUV");
    o.heightScale = f(n, "heightScale");
    o.baseAtOrigin = i(n, "heightBase") == 0;
    o.envelope = std::clamp(f(n, "envelope"), 0.0, 1.0);
    geo::Mesh m = in;
    flowAlongSurface(m, surface, o);
    return m;
}

Node& nodeNamed(Graph& g, const std::string& name, const char* kind, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA(kind))
        throw CommandError(std::string(cmd) + ": " + name + " is not a " + kind);
    return *n;
}

/** Option flags for every editable attr of a node type, enums by name, without defaults so only given ones apply. */
std::vector<FlagDef> modifierAttrFlags(const std::string& type)
{
    std::vector<FlagDef> out;
    for (const AttrDef& d : types::find(type)->ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden) || d.name == "enabled" || d.name == "exportEnabled")
            continue;
        out.push_back(helpers::flagFromAttr(d));
    }
    return out;
}

/** The selected curves and meshes, in selection order. */
void selectedByKind(Graph& g, std::vector<Node*>& curves, std::vector<Node*>& meshes)
{
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
        {
            if (n->type().isA("curve"))
                curves.push_back(n);
            else if (n->type().isA("mesh"))
                meshes.push_back(n);
        }
}

/** Wires a curve into a modifier's input and records the mesh's world matrix the curve is read against. */
void connectCurve(Graph& g, Node& mod, NodeId mesh, Node* curve, Node* aim)
{
    if (curve)
        g.connect({curve->id(), CurveShape::kWorldSpace, -1}, {mod.id(), idx(mod, "curve"), -1}, true);
    if (aim)
        g.connect({aim->id(), CurveShape::kWorldSpace, -1}, {mod.id(), idx(mod, "aimCurve"), -1}, true);
    if (Node* m = g.node(mesh))
        g.setValue({mod.id(), idx(mod, "targetMatrix"), -1}, m->as<glm::dmat4>(Transform::kWorldMatrix));
}

/** Modifiers added from the stack menu pick up the selected curve. */
void curveHook(Graph& g, Node& mod, NodeId mesh)
{
    std::vector<Node*> curves, meshes;
    selectedByKind(g, curves, meshes);
    connectCurve(g, mod, mesh, curves.empty() ? nullptr : curves.front(), nullptr);
}

void connectTarget(Graph& g, Node& mod, NodeId source, Node& target)
{
    g.connect({target.id(), MeshNode::kOutMesh, -1}, {mod.id(), idx(mod, "targetMesh"), -1}, true);
    g.connect({target.id(), Transform::kWorldMatrix, -1}, {mod.id(), idx(mod, "targetWorld"), -1}, true);
    if (Node* m = g.node(source))
        g.setValue({mod.id(), idx(mod, "targetMatrix"), -1}, m->as<glm::dmat4>(Transform::kWorldMatrix));
}

/** Modifiers added from the stack menu flow onto another selected mesh. */
void flowHook(Graph& g, Node& mod, NodeId mesh)
{
    std::vector<Node*> curves, meshes;
    selectedByKind(g, curves, meshes);
    for (auto it = meshes.rbegin(); it != meshes.rend(); ++it)
        if ((*it)->id() != mesh)
        {
            connectTarget(g, mod, mesh, **it);
            return;
        }
}

/** The meshes and the curve (and aim curve) a curve command works on: flags first, then the selection. */
struct CurveCall
{
    std::vector<Node*> meshes;
    Node* curve = nullptr;
    Node* aim = nullptr;
};

CurveCall curveCall(Graph& g, const Args& a, const char* cmd)
{
    CurveCall c;
    std::vector<Node*> selCurves, selMeshes;
    selectedByKind(g, selCurves, selMeshes);
    for (const json& p : a.positional)
        c.meshes.push_back(&nodeNamed(g, p.get<std::string>(), "mesh", cmd));
    if (c.meshes.empty())
        c.meshes = selMeshes;
    c.curve = a.has("curve") ? &nodeNamed(g, a.get<std::string>("curve"), "curve", cmd) : (selCurves.empty() ? nullptr : selCurves[0]);
    if (a.has("aimCurve"))
        c.aim = &nodeNamed(g, a.get<std::string>("aimCurve"), "curve", cmd);
    else if (a.get<std::string>("upMode", "") == "aimCurve")
        for (Node* n : selCurves)
            if (n != c.curve)
            {
                c.aim = n;
                break;
            }
    if (c.meshes.empty() || !c.curve)
        throw CommandError(std::string(cmd) + ": select meshes and a curve, or give meshes and curve=");
    if (a.get<std::string>("upMode", "") == "aimCurve" && !c.aim)
        throw CommandError(std::string(cmd) + ": upMode aimCurve needs a second curve (select it, or give aimCurve=)");
    return c;
}

std::string addModifier(Node& mesh, const char* kind)
{
    return ops::execute("addModifier", {{"args", {mesh.path()}}, {"kind", kind}}).get<std::string>();
}

/** Copies of the piece at the placements as instances (sharing its outMesh) under a new group. Returns the group. */
Node* placeInstances(Graph& g, Node& piece, Node* start, Node* end, const std::vector<ArrayPlacement>& places, const std::string& name)
{
    Node* group = g.createNode(*types::find("transform"), name, kNoNode, true);
    std::unordered_map<NodeId, NodeId> copies;
    for (const ArrayPlacement& pl : places)
    {
        Node& src = pl.piece == ArrayPiece::Start ? *start : pl.piece == ArrayPiece::End ? *end : piece;
        const NodeId inst = duplicate::copyTree(g, src.id(), group->id(), duplicate::Mode::Instance, src.name(), copies);
        Transform::setLocalMatrix(g, inst, pl.transform);
    }
    return group;
}

void registerAlongOptionLayouts()
{
    namespace opt = ops::options;
    {
        opt::Override o;
        o.order = {"forwardAxis", "upMode", "stretch", "offset", "offsetFraction"};
        o.hidden = {"curve", "aimCurve"};
        o.ranges["offset"] = {-1e6, 1e6, 1.0, " cm"};
        o.ranges["offsetFraction"] = {-1.0, 1.0, 0.01, ""};
        o.ranges["roll"] = {-360.0, 360.0, 1.0, " deg"};
        o.ranges["twist"] = {-3600.0, 3600.0, 1.0, " deg"};
        o.ranges["envelope"] = {0.0, 1.0, 0.05, ""};
        o.labels = {{"offsetFraction", "Offset (fraction of length)"}, {"stretch", "Stretch to Fit"}, {"scaleRamp", "Scale Ramp (t:scale ...)"},
                    {"twistRamp", "Twist Ramp (t:deg ...)"}};
        opt::setOverride("deformAlongCurve", std::move(o));
    }
    {
        opt::Override o;
        o.order = {"output", "fill", "count", "spacing", "fitToLength", "startOffset", "endOffset", "deform"};
        o.hidden = {"aimCurve", "curve", "startMesh", "endMesh", "name"};
        for (const AttrDef& d : types::find("curveArrayModifier")->ownAttrs)
            if (!d.category.empty())
                o.sections[d.name] = d.category;
        o.ranges["spacing"] = {0.0, 1e6, 1.0, " cm"};
        o.ranges["startOffset"] = {0.0, 1e6, 1.0, " cm"};
        o.ranges["endOffset"] = {0.0, 1e6, 1.0, " cm"};
        o.ranges["roll"] = {-360.0, 360.0, 1.0, " deg"};
        o.ranges["twist"] = {-3600.0, 3600.0, 1.0, " deg"};
        o.ranges["randomScale"] = {0.0, 1.0, 0.05, ""};
        o.labels = {{"spacing", "Spacing (0 = piece length)"}, {"deform", "Deform Copies along Curve"}, {"scaleRamp", "Scale Ramp (t:scale ...)"},
                    {"twistRamp", "Twist Ramp (t:deg ...)"}};
        opt::setOverride("arrayAlongCurve", std::move(o));
    }
    {
        opt::Override o;
        o.order = {"parametrization", "uvSet", "heightScale", "heightBase"};
        o.hidden = {"target", "faces"};
        o.ranges["envelope"] = {0.0, 1.0, 0.05, ""};
        o.labels = {{"swapUV", "Swap U and V"}};
        opt::setOverride("flowAlongSurface", std::move(o));
    }
}
} // namespace

void registerAlongTypes()
{
    const std::string place = "Placement", shape = "Shape", fill = "Fill", frame = "Orientation", rnd = "Random", pieces = "Pieces";

    std::vector<AttrDef> deform = curveFollowInputs();
    for (AttrDef& a : frameAttrs(place))
        deform.push_back(a);
    for (AttrDef a : {fAttr("offset", 0.0, "Where the mesh starts along the curve, cm.", place),
                      fAttr("offsetFraction", 0.0, "Where the mesh starts along the curve, as a fraction of its length (added to offset).", place),
                      bAttr("stretch", false, "Stretch the mesh to run from its start to the curve's end; off keeps its length.", place),
                      bAttr("loop", true, "On a closed curve, wrap past the seam; off treats it as open at its start.", place),
                      eAttr("endBehavior", {"extrapolate", "clamp"}, 0,
                            "Past an open curve's end: carry on straight along the end tangent, or pile up at the end.", place),
                      fAttr("envelope", 1.0, "How much of the deformation applies, 0 to 1.", place, 0.0, 1.0)})
        deform.push_back(a);
    for (AttrDef& a : rampAttrs(shape))
        deform.push_back(a);
    registerModifier("curveDeformModifier", "route",
                     "Bends the mesh along a curve, its forward axis running along it (Curve Warp, Path Deform): offset, stretch to "
                     "fit, roll, twist and scale ramps, the curve's tilt and radius. Editing or moving the curve updates it.",
                     deform, deformRun);
    modifiers::setCreateHook("curveDeformModifier", curveHook);

    std::vector<AttrDef> array = curveFollowInputs();
    array.push_back(hidden("startMesh", PlugType::Mesh, MeshPtr(), "A mesh for the first copy (connect another mesh's outMesh)."));
    array.push_back(hidden("endMesh", PlugType::Mesh, MeshPtr(), "A mesh for the last copy."));
    for (AttrDef a : {eAttr("fill", {"count", "spacing"}, 0, "Fill by a number of copies, or by the step between them.", fill),
                      iAttr("count", 5, "Copies (count fill), start and end pieces included.", fill, 1),
                      fAttr("spacing", 0.0, "Step between copies, cm (spacing fill); 0 is the piece's own length so copies butt together.", fill, 0.0),
                      bAttr("fitToLength", true, "Spacing fill: round the count and even the step so the copies run exactly from end to end.", fill),
                      fAttr("startOffset", 0.0, "Room left free at the curve's start, cm.", fill, 0.0),
                      fAttr("endOffset", 0.0, "Room left free at the curve's end, cm.", fill, 0.0)})
        array.push_back(a);
    for (AttrDef& a : frameAttrs(frame))
        array.push_back(a);
    for (AttrDef a : {bAttr("align", true, "Turn copies to follow the curve; off, they keep their orientation and only move.", frame),
                      bAttr("deform", false, "Bend each copy along its stretch of the curve (trims, rails, fences) instead of placing it rigidly.", frame),
                      bAttr("weld", true, "Deformed copies: merge the points where neighbours meet.", frame)})
        array.push_back(a);
    for (AttrDef& a : rampAttrs(shape))
        array.push_back(a);
    for (AttrDef a : {vAttr("randomRotate", D3(0.0), "Random turn per copy about the piece's axes, plus or minus degrees.", rnd),
                      fAttr("randomScale", 0.0, "Random uniform scale per copy, plus or minus this fraction.", rnd, 0.0, 1.0),
                      vAttr("randomOffset", D3(0.0), "Random shift per copy along the piece's axes, plus or minus cm.", rnd),
                      iAttr("seed", 1, "Random seed.", rnd)})
        array.push_back(a);
    registerModifier("curveArrayModifier", "route",
                     "Repeats the mesh along a curve: by count or spacing, fitted to its length, optional start and end pieces, each "
                     "copy placed on the curve's frame or bent along it, with random turn, scale and offset. Follows curve edits.",
                     array, arrayRun);
    modifiers::setCreateHook("curveArrayModifier", curveHook);

    std::vector<AttrDef> flow = {
        hidden("targetMesh", PlugType::Mesh, MeshPtr(), "The surface's outMesh."),
        hidden("targetWorld", PlugType::Matrix, glm::dmat4(1.0), "The surface's worldMatrix."),
        hidden("targetMatrix", PlugType::Matrix, glm::dmat4(1.0), "This mesh's world matrix when the surface was connected.", true),
        sAttr("targetFaces", "Faces of the surface to flow onto, as a JSON list; empty is all of it.", AttrHidden),
        eAttr("parametrization", {"uv", "grid"}, 0,
              "How surface positions are found: the surface's UVs, or the rows and columns of a rectangular patch of quads (targetFaces)."),
        sAttr("uvSet", "UV set to map through (uv parametrization); empty is uv0."),
        fAttr("uMin", 0.0, "Start of the coordinate range the mesh's X extent maps onto."),
        fAttr("vMin", 0.0, "Start of the coordinate range the mesh's Z extent maps onto (Z runs against V)."),
        fAttr("uMax", 1.0, "End of the U range."),
        fAttr("vMax", 1.0, "End of the V range."),
        bAttr("swapUV", false, "Map X to V and Z to U."),
        fAttr("heightScale", 1.0, "Height off the surface per unit of the mesh's Y."),
        eAttr("heightBase", {"origin", "bottom"}, 0, "Measure height from Y = 0 (the construction plane) or from the mesh's lowest point."),
        fAttr("envelope", 1.0, "How much of the flow applies, 0 to 1.", 0.0, 1.0),
    };
    registerModifier("surfaceFlowModifier", "texture",
                     "Flows the mesh onto a surface (FlowAlongSrf): its XZ footprint maps onto the surface's UV rectangle, or a "
                     "rectangular patch of quads, and its height follows the surface normal. Editing or moving the surface updates it.",
                     flow, flowRun);
    modifiers::setCreateHook("surfaceFlowModifier", flowHook);
}

void registerAlongCommands()
{
    ops::registerCommand({
        .name = "deformAlongCurve",
        .doc = "Bend meshes along a curve with a live curveDeform modifier (Curve Warp, Path Deform). Select the meshes and the curve (a "
               "second curve aims the up direction with upMode=aimCurve). The forward axis runs along the curve from its start; editing "
               "or moving the curve updates the meshes. Returns the modifiers.",
        .positionalDoc = "meshes",
        .flags = withFlags(modifierAttrFlags("curveDeformModifier"),
                           {nodeFlag({"curve", "c", T::String, nullptr, "The curve to follow; else the selected curve."}, "curve"),
                            nodeFlag({"aimCurve", "", T::String, nullptr, "The curve up aims at (upMode aimCurve); else the second selected curve."}, "curve")}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            CurveCall c = curveCall(g, a, "deformAlongCurve");
            json out = json::array();
            for (Node* mesh : c.meshes)
            {
                Node* mod = g.find(addModifier(*mesh, "curveDeform"));
                connectCurve(g, *mod, mesh->id(), c.curve, c.aim);
                applyFlags(g, *mod, a, "deformAlongCurve");
                out.push_back(mod->path());
            }
            g.select({c.meshes.front()->id()});
            return out;
        },
        .examples = {"lila.ops.deformAlongCurve(\"Rail\", curve=\"Path\", forwardAxis=\"+x\", stretch=True)"},
    });

    ops::registerCommand({
        .name = "arrayAlongCurve",
        .doc = "Repeat meshes along a curve. output=merged adds a live curveArray modifier to each mesh (copies follow curve edits); "
               "output=instances places instances of the mesh under a group instead (one shared mesh each, ready to export as "
               "instances; not live). Select the piece meshes and the curve. Returns the modifiers, or the groups.",
        .positionalDoc = "meshes",
        .flags = withFlags(modifierAttrFlags("curveArrayModifier"),
                           {nodeFlag({"curve", "c", T::String, nullptr, "The curve to follow; else the selected curve."}, "curve"),
                            nodeFlag({"aimCurve", "", T::String, nullptr, "The curve up aims at (upMode aimCurve); else the second selected curve."}, "curve"),
                            nodeFlag({"startMesh", "", T::String, nullptr, "A mesh to use for the first copy."}, "mesh"),
                            nodeFlag({"endMesh", "", T::String, nullptr, "A mesh to use for the last copy."}, "mesh"),
                            {"output", "o", T::String, "merged", "merged: one mesh with a live modifier; instances: an instance per copy under a group.",
                             {"merged", "instances"}},
                            {"name", "n", T::String, nullptr, "Name of the instance group; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            CurveCall c = curveCall(g, a, "arrayAlongCurve");
            Node* start = a.has("startMesh") ? &nodeNamed(g, a.get<std::string>("startMesh"), "mesh", "arrayAlongCurve") : nullptr;
            Node* end = a.has("endMesh") ? &nodeNamed(g, a.get<std::string>("endMesh"), "mesh", "arrayAlongCurve") : nullptr;
            json out = json::array();
            std::vector<NodeId> made;
            for (Node* mesh : c.meshes)
            {
                if (mesh == start || mesh == end)
                    continue;
                Node* mod = g.find(addModifier(*mesh, "curveArray"));
                connectCurve(g, *mod, mesh->id(), c.curve, c.aim);
                applyFlags(g, *mod, a, "arrayAlongCurve");
                if (start)
                    g.connect({start->id(), MeshNode::kOutMesh, -1}, {mod->id(), idx(*mod, "startMesh"), -1}, true);
                if (end)
                    g.connect({end->id(), MeshNode::kOutMesh, -1}, {mod->id(), idx(*mod, "endMesh"), -1}, true);
                if (a.get<std::string>("output", "merged") != "instances")
                {
                    out.push_back(mod->path());
                    made.push_back(mesh->id());
                    continue;
                }
                // Instances: the modifier only resolves the settings; placements come from the world-space curve.
                ArrayAlongOptions o = arrayOptionsOf(*mod);
                if (o.deform)
                    throw CommandError("arrayAlongCurve: deformed copies differ from each other, so they can't be instances: use output=merged");
                g.evaluate();
                std::vector<ArrayPlacement> places;
                try
                {
                    Rail rail = railOf(*mod, glm::dmat4(1.0), "arrayAlongCurve");
                    auto length = [&](Node* n) {
                        MeshPtr m = n ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
                        if (!m)
                            return -1.0;
                        auto [lo, hi] = forwardExtent(*m, o.forward);
                        return hi - lo;
                    };
                    places = arrayPlacements(rail, o, std::max(0.0, length(mesh)), length(start), length(end));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("arrayAlongCurve: ") + e.what());
                }
                ops::execute("removeModifier", {{"args", {mod->path()}}});
                Node* group = placeInstances(g, *mesh, start, end, places, a.get<std::string>("name", mesh->name() + "_alongCurve"));
                out.push_back(group->path());
                made.push_back(group->id());
            }
            if (!made.empty())
                g.select(made);
            return out;
        },
        .examples = {"lila.ops.arrayAlongCurve(\"Post\", curve=\"Fence\", fill=\"spacing\", spacing=150)",
                     "lila.ops.arrayAlongCurve(\"Plank\", curve=\"Path\", fill=\"spacing\", deform=True)"},
    });

    ops::registerCommand({
        .name = "flowAlongSurface",
        .doc = "Flow meshes onto a surface with a live surfaceFlow modifier (FlowAlongSrf): each mesh's XZ footprint maps onto the "
               "surface's UV rectangle, or onto a rectangular patch of quads (selected faces, parametrization grid), and its height "
               "(Y) follows the surface normal. Select the meshes, then the surface last. Returns the modifiers.",
        .positionalDoc = "meshes",
        .flags = withFlags(modifierAttrFlags("surfaceFlowModifier"),
                           {nodeFlag({"target", "t", T::String, nullptr, "The surface mesh; else the last selected mesh."}, "mesh"),
                            {"faces", "", T::List, nullptr, "Surface faces to flow onto; else its selected faces, else all of it."}}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            std::vector<Node*> selCurves, selMeshes, sources;
            selectedByKind(g, selCurves, selMeshes);
            Node* target = nullptr;
            if (a.has("target"))
                target = &nodeNamed(g, a.get<std::string>("target"), "mesh", "flowAlongSurface");
            for (const json& p : a.positional)
                sources.push_back(&nodeNamed(g, p.get<std::string>(), "mesh", "flowAlongSurface"));
            if (!target && !selMeshes.empty())
                target = selMeshes.back();
            if (sources.empty())
                for (Node* n : selMeshes)
                    if (n != target)
                        sources.push_back(n);
            if (!target || sources.empty())
                throw CommandError("flowAlongSurface: select the meshes to flow, then the surface last");
            std::vector<geo::Index> faces;
            if (a.has("faces"))
                faces = a.flags["faces"].get<std::vector<geo::Index>>();
            else
                for (uint32_t id : components::selected(target->id(), ComponentMode::Face))
                    faces.push_back(id);
            json out = json::array();
            for (Node* src : sources)
            {
                if (src == target)
                    throw CommandError("flowAlongSurface: a mesh can't flow onto itself");
                Node* mod = g.find(addModifier(*src, "surfaceFlow"));
                connectTarget(g, *mod, src->id(), *target);
                if (!faces.empty())
                {
                    g.setValue({mod->id(), idx(*mod, "targetFaces"), -1}, json(faces).dump());
                    if (!a.has("parametrization"))
                        g.setValue({mod->id(), idx(*mod, "parametrization"), -1}, int64_t(1));
                }
                applyFlags(g, *mod, a, "flowAlongSurface");
                out.push_back(mod->path());
            }
            g.select({sources.front()->id()});
            return out;
        },
        .examples = {"lila.ops.flowAlongSurface(\"Logo\", target=\"Hull\")"},
    });
    registerAlongOptionLayouts();
    registerCurveDeformCommands();
}
} // namespace lila::curve
