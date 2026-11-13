#include "scene/GeoSidecar.h"
#include "scene/MeshNodes.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/AttrBuilders.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/Modifiers.h"
#include "scene/RigUtil.h"

#include <glm/gtx/quaternion.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <format>

#include <algorithm>
#include <unordered_set>
#include <format>

namespace lila::scene
{
using namespace attrdefs;

namespace
{
/** A size whose default for new primitives (sized to fit a 10 cm cube) differs from what scene files leaving it out mean. */
AttrDef sizeAttr(std::string name, double def, double fileDefault, std::string doc)
{
    AttrDef a = floatAttr(std::move(name), def, std::move(doc), 0.0);
    a.fileDefault = fileDefault;
    return a;
}

/** lo matches the least the generator reads it as; hi keeps a typo from building a mesh that never finishes. */
AttrDef intAttr(std::string name, int64_t def, std::string doc, double lo = 1.0, double hi = 1000.0)
{
    return {.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi, .softMax = std::min(hi, 100.0)};
}

AttrDef keyableBoolAttr(std::string name, bool def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}

double f(const Node& n, const char* attr) { return n.as<double>(n.type().attrIndex(attr)); }
int count(const Node& n, const char* attr, int lo = 1) { return std::max(lo, int(n.as<int64_t>(n.type().attrIndex(attr)))); }
bool b(const Node& n, const char* attr) { return n.as<bool>(n.type().attrIndex(attr)); }

AttrDef heightFraction()
{
    return floatAttr("fraction", 1.0, "Portion of the height kept, from the top: 1 is whole, 0.5 a dome open at the bottom.", 0.01, 1.0);
}

AttrDef sweepFraction()
{
    return floatAttr("fraction", 1.0, "Portion of a full turn swept around the up axis: 1 is whole, 0.5 half.", 0.01, 1.0);
}

AttrDef cutCaps() { return keyableBoolAttr("cutCaps", true, "Close the flat ends a partial sweep leaves."); }

AttrDef capTypeAttr()
{
    AttrDef a{.name = "capType", .type = PlugType::Enum, .defaultValue = int64_t(geo::CapFill::Quads),
              .flags = AttrInput | AttrStorable | AttrKeyable, .enumNames = {"quads", "fan", "ngon", "none"},
              .doc = "How each flat end is filled inside its rings: quads (a grid joined to the rim by a ring, one triangle when "
                     "subdivisionsAxis is odd), fan (triangles to the centre), ngon (one polygon) or none (left open)."};
    // Scene files saved before capType had n-gon caps.
    a.fileDefault = int64_t(geo::CapFill::Ngon);
    return a;
}

AttrDef discCapTypeAttr()
{
    AttrDef a{.name = "capType", .type = PlugType::Enum, .defaultValue = int64_t(geo::CapFill::Quads),
              .flags = AttrInput | AttrStorable | AttrKeyable, .enumNames = {"quads", "fan", "ngon"},
              .doc = "How the innermost ring is filled: quads (a grid joined to it by a ring, one triangle when subdivisionsAxis is "
                     "odd), fan (triangles to the centre) or ngon (one polygon). subdivisionsRings 0 is always one n-gon."};
    a.fileDefault = int64_t(geo::CapFill::Fan);
    return a;
}

geo::CapFill capFill(const Node& n) { return geo::CapFill(std::clamp<int64_t>(n.as<int64_t>(n.type().attrIndex("capType")), 0, 3)); }

double frac(const Node& n) { return std::clamp(f(n, "fraction"), 0.01, 1.0); }

const std::vector<std::string> kUpAxes = {"x", "y", "z", "-x", "-y", "-z"};

AttrDef upAxisAttr()
{
    return {.name = "upAxis", .type = PlugType::Enum, .defaultValue = int64_t(1), .flags = AttrInput | AttrStorable | AttrKeyable,
            .enumNames = kUpAxes, .doc = "The direction the primitive's up (its height, poles or face normal) points in object space."};
}

/** Turns a mesh built up +Y so that its up points along axis (an upAxis index), keeping it a rotation so winding holds. */
void orientUp(geo::Mesh& m, int64_t axis)
{
    if (axis == 1 || axis < 0 || axis >= int64_t(kUpAxes.size()))
        return;
    auto turn = [axis](const glm::dvec3& p) -> glm::dvec3 {
        switch (axis)
        {
        case 0: return {p.y, -p.x, p.z};
        case 2: return {p.x, -p.z, p.y};
        case 3: return {-p.y, p.x, p.z};
        case 4: return {p.x, -p.y, -p.z};
        default: return {p.x, p.z, -p.y};
        }
    };
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, turn(m.position(p)));
}

struct Primitive
{
    const char* type;    // node type, also the command name
    const char* display; // default object name
    std::vector<AttrDef> attrs;
    GeneratorNode::Build build;
    /** Its fraction cuts the height, so a partial one is placed on a surface by its open base. */
    bool domed = false;
};

std::vector<Primitive> primitives()
{
    return {
        {"polyCube", "Cube",
         {sizeAttr("width", 10, 100, "Size along X, cm."), sizeAttr("height", 10, 100, "Size along the up axis, cm."),
          sizeAttr("depth", 10, 100, "Size along Z, cm."), intAttr("subdivisionsX", 1, "Segments across."),
          intAttr("subdivisionsY", 1, "Segments up."), intAttr("subdivisionsZ", 1, "Segments deep."),
          keyableBoolAttr("baseAtOrigin", false, "Sit on the ground instead of centring.")},
         [](const Node& n) {
             return geo::makeCube({f(n, "width"), f(n, "height"), f(n, "depth"), count(n, "subdivisionsX"), count(n, "subdivisionsY"),
                                   count(n, "subdivisionsZ"), b(n, "baseAtOrigin")});
         }},
        {"polySphere", "Sphere",
         {sizeAttr("radius", 5, 50, "cm."), intAttr("subdivisionsAxis", 20, "Segments around.", 3.0),
          intAttr("subdivisionsHeight", 20, "Segments pole to pole.", 2.0), keyableBoolAttr("baseAtOrigin", false, "Sit on the ground."),
          heightFraction()},
         [](const Node& n) {
             return geo::makeSphere(
                 {f(n, "radius"), count(n, "subdivisionsAxis", 3), count(n, "subdivisionsHeight", 2), b(n, "baseAtOrigin"), frac(n)});
         },
         true},
        {"polyCylinder", "Cylinder",
         {sizeAttr("radius", 5, 50, "cm."), sizeAttr("height", 10, 100, "cm."), intAttr("subdivisionsAxis", 20, "Segments around.", 3.0),
          intAttr("subdivisionsHeight", 1, "Segments up."),
          intAttr("subdivisionsCaps", 0, "Rings of quads around each cap's fill; 0 and 1 both leave just the fill.", 0.0), capTypeAttr(),
          keyableBoolAttr("caps", true, "Close the ends; off leaves them open whatever capType says."), keyableBoolAttr("baseAtOrigin", false, "Sit on the ground."),
          sweepFraction(), cutCaps()},
         [](const Node& n) {
             return geo::makeCylinder({f(n, "radius"), f(n, "height"), count(n, "subdivisionsAxis", 3), count(n, "subdivisionsHeight"),
                                       count(n, "subdivisionsCaps", 0), b(n, "caps"), b(n, "baseAtOrigin"), frac(n), b(n, "cutCaps"), capFill(n)});
         }},
        {"polyCapsule", "Capsule",
         {sizeAttr("radius", 2.5, 50, "cm."), sizeAttr("height", 10, 200, "End to end, caps included, cm."),
          intAttr("subdivisionsAxis", 20, "Segments around.", 3.0), intAttr("subdivisionsHeight", 1, "Segments along the straight part."),
          intAttr("subdivisionsCaps", 6, "Rings on each rounded end."), keyableBoolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) {
             return geo::makeCapsule({f(n, "radius"), f(n, "height"), count(n, "subdivisionsAxis", 3), count(n, "subdivisionsHeight"),
                                      count(n, "subdivisionsCaps"), b(n, "baseAtOrigin")});
         }},
        {"polyCone", "Cone",
         {sizeAttr("radius", 5, 50, "cm."), sizeAttr("height", 10, 100, "cm."), intAttr("subdivisionsAxis", 20, "Segments around.", 3.0),
          intAttr("subdivisionsHeight", 1, "Segments up."),
          intAttr("subdivisionsCap", 0, "Rings of quads around the base's fill; 0 and 1 both leave just the fill.", 0.0), capTypeAttr(),
          keyableBoolAttr("baseAtOrigin", false, "Sit on the ground."), sweepFraction(), cutCaps(),
          keyableBoolAttr("capTip", false, "Cut the point off at tipRadius and fill it as capType, so quads leave no triangles at the tip."),
          floatAttr("tipRadius", 0.05, "Radius of the capped tip, cm; small keeps it looking pointed.", 0.0)},
         [](const Node& n) {
             return geo::makeCone({f(n, "radius"), f(n, "height"), count(n, "subdivisionsAxis", 3), count(n, "subdivisionsHeight"),
                                   count(n, "subdivisionsCap", 0), b(n, "baseAtOrigin"), frac(n), b(n, "cutCaps"), capFill(n), b(n, "capTip"),
                                   f(n, "tipRadius")});
         }},
        {"polyPlane", "Plane",
         {sizeAttr("width", 10, 100, "Size along X, cm."), sizeAttr("depth", 10, 100, "Size along Z, cm."),
          intAttr("subdivisionsX", 1, "Segments across."), intAttr("subdivisionsZ", 1, "Segments deep.")},
         [](const Node& n) { return geo::makePlane({f(n, "width"), f(n, "depth"), count(n, "subdivisionsX"), count(n, "subdivisionsZ")}); }},
        {"polyTorus", "Torus",
         {sizeAttr("radius", 3.5, 50, "Ring radius, cm."), sizeAttr("sectionRadius", 1.5, 20, "Tube radius, cm."),
          intAttr("subdivisionsAxis", 20, "Segments around the ring.", 3.0), intAttr("subdivisionsHeight", 20, "Segments around the tube.", 3.0),
          sweepFraction(), cutCaps()},
         [](const Node& n) {
             return geo::makeTorus({f(n, "radius"), f(n, "sectionRadius"), count(n, "subdivisionsAxis", 3), count(n, "subdivisionsHeight", 3), frac(n),
                                    b(n, "cutCaps")});
         }},
        {"polyDisc", "Disc",
         {sizeAttr("radius", 5, 50, "cm."), intAttr("subdivisionsAxis", 20, "Segments around.", 3.0),
          intAttr("subdivisionsRings", 1, "Rings; 0 is one n-gon.", 0.0), sweepFraction(), discCapTypeAttr()},
         [](const Node& n) {
             return geo::makeDisc({f(n, "radius"), count(n, "subdivisionsAxis", 3), count(n, "subdivisionsRings", 0), frac(n), capFill(n)});
         }},
        {"polyPipe", "Pipe",
         {sizeAttr("radius", 5, 50, "Outer radius, cm."), sizeAttr("thickness", 1, 10, "Wall thickness, cm."),
          sizeAttr("height", 20, 100, "cm."),
          intAttr("subdivisionsAxis", 20, "Segments around.", 3.0), intAttr("subdivisionsHeight", 1, "Segments up."),
          keyableBoolAttr("baseAtOrigin", false, "Sit on the ground."), sweepFraction(), cutCaps()},
         [](const Node& n) {
             return geo::makePipe({f(n, "radius"), f(n, "thickness"), f(n, "height"), count(n, "subdivisionsAxis", 3),
                                   count(n, "subdivisionsHeight"), b(n, "baseAtOrigin"), frac(n), b(n, "cutCaps")});
         }},
        {"polyQuadSphere", "QuadSphere",
         {sizeAttr("radius", 5, 50, "cm."), intAttr("subdivisions", 3, "Segments per cube face edge."), heightFraction()},
         [](const Node& n) { return geo::makeQuadSphere({f(n, "radius"), count(n, "subdivisions"), frac(n)}); },
         true},
        {"polyIcoSphere", "IcoSphere",
         {sizeAttr("radius", 5, 50, "cm."), intAttr("subdivisions", 3, "1 is an icosahedron; each level splits every triangle in four.", 1.0, 7.0),
          keyableBoolAttr("baseAtOrigin", false, "Sit on the ground."), heightFraction()},
         [](const Node& n) { return geo::makeIcoSphere({f(n, "radius"), count(n, "subdivisions", 1), b(n, "baseAtOrigin"), frac(n)}); },
         true},
    };
}

using rigutil::nodeNamed;

json vecToJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

uint64_t gNextPointsToken = 1;
std::pair<uint64_t, std::vector<glm::dvec3>> gStashedPoints;
} // namespace

uint64_t stashPoints(std::vector<glm::dvec3> points)
{
    gStashedPoints = {gNextPointsToken++, std::move(points)};
    return gStashedPoints.first;
}

void MeshNode::compute(const EvalContext& ctx)
{
    Transform::compute(ctx);
    const MeshPtr& in = as<MeshPtr>(kInMesh);
    // A stack whose subdivision is off or gone hands on the spacing layers its first modifier added.
    if (in && geo::hasFitSpacing(*in))
    {
        auto m = std::make_shared<geo::Mesh>(*in);
        geo::removeFitSpacing(*m);
        setOutput(kOutMesh, MeshPtr(std::move(m)));
        return;
    }
    setOutput(kOutMesh, in);
}

NodeType MeshNode::makeType(const NodeType& base)
{
    auto vec3Attr = [](std::string name, std::string shortName, glm::dvec3 def, std::string doc) {
        AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable,
                  .doc = std::move(doc)};
        for (int i = 0; i < 3; ++i)
        {
            a.childNames[i] = name + char('X' + i);
            a.childShort[i] = shortName + char('x' + i);
        }
        return a;
    };
    NodeType t;
    t.name = "mesh";
    t.icon = "cube";
    t.base = &base;
    t.dag = true;
    t.ownAttrs = {
        {.name = "inMesh", .shortName = "i", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Base mesh, or the output of a generator."},
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "Final mesh, as drawn and exported."},
        {.name = "symmetry", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .enumNames = {"off", "x", "y", "z", "radial"},
         .doc = "Mirror plane (object space) every component edit is applied across, or radial: every edit repeated round radialAxis."},
        {.name = "smoothPreview", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .doc = "Display-only Catmull-Clark levels (the 3 key); exports stay unsmoothed.", .minValue = 0.0, .maxValue = 3.0},
        {.name = "retopoDrawReference", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "Retopo Draw's reference surface: connected from its outMesh when this mesh is being drawn over another.",
         .fileAlias = "quadDrawReference"},
        {.name = "retopoDrawDots", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Retopo Draw's unconnected dots, a JSON list of object-space [x, y, z].", .fileAlias = "quadDrawDots"},
        {.name = "symmetryPlane", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .enumNames = {"pivot", "custom"}, .doc = "Where the symmetry plane lies: through the pivot, or at symmetryOffset."},
        {.name = "symmetryOffset", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable,
         .doc = "The custom symmetry plane's position along the symmetry axis, object space, cm."},
        {.name = "radialCount", .type = PlugType::Int, .defaultValue = int64_t(6), .flags = AttrInput | AttrStorable,
         .doc = "Radial symmetry: how many wedges repeat round the axis.", .minValue = 2.0, .maxValue = double(geo::kMaxRadialCount)},
        {.name = "radialAxis", .type = PlugType::Enum, .defaultValue = int64_t(1), .flags = AttrInput | AttrStorable,
         .enumNames = {"x", "y", "z", "custom"}, .doc = "Radial symmetry's axis (object space); custom uses radialDirection."},
        vec3Attr("radialDirection", "rdd", glm::dvec3(0.0, 1.0, 0.0), "Radial symmetry's axis when radialAxis is custom, object space."),
        {.name = "radialMirror", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
         .doc = "Radial symmetry also mirrors each wedge across a plane through the axis (dihedral symmetry: rims, flowers)."},
        {.name = "radialPhase", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable,
         .doc = "Radial symmetry: degrees the wedge boundaries (and mirror planes) turn about the axis.", .softMin = -180.0, .softMax = 180.0},
        vec3Attr("radialCenter", "rdc", glm::dvec3(0.0), "Where the radial axis passes when symmetryPlane is custom, object space, cm."),
        {.name = "retopoGuides", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Topology guides drawn with the Pencil: JSON, object space."},
        {.name = "retopoConstructs", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Live topology constructs (patches, strips, cylinders...) and their settings: JSON, object space."},
    };
    t.create = [] { return std::make_unique<MeshNode>(); };
    return t;
}

void GeneratorNode::compute(const EvalContext&)
{
    LILA_ZONE();

    try
    {
        setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(build(*this))));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        setOutput(outAttr, MeshPtr());
    }
}

std::optional<geo::SymmetrySpec> symmetrySpec(const Node& mesh)
{
    if (!mesh.type().isA("mesh"))
        return std::nullopt;
    const int64_t v = mesh.as<int64_t>(MeshNode::kSymmetry);
    geo::SymmetrySpec spec;
    if (v >= 1 && v <= 3)
    {
        spec.axis = int(v - 1);
        spec.offset = symmetryOffset(mesh, spec.axis);
        return spec;
    }
    if (v != kSymmetryRadial)
        return std::nullopt;
    const int count = int(std::clamp<int64_t>(mesh.as<int64_t>(MeshNode::kRadialCount), 2, geo::kMaxRadialCount));
    const bool mirror = mesh.as<bool>(MeshNode::kRadialMirror);
    const double phase = mesh.as<double>(MeshNode::kRadialPhase);
    const glm::dvec3 origin = mesh.as<int64_t>(MeshNode::kSymmetryPlane) == 1 ? mesh.as<glm::dvec3>(MeshNode::kRadialCenter)
                                                                             : mesh.as<glm::dvec3>(Transform::kRotatePivot);
    const int64_t axis = mesh.as<int64_t>(MeshNode::kRadialAxis);
    if (axis >= 0 && axis <= 2)
    {
        spec.axis = int(axis);
        spec.radial = geo::axisRadial(int(axis), origin, count, mirror, phase);
    }
    else
    {
        const glm::dvec3 dir = mesh.as<glm::dvec3>(MeshNode::kRadialDirection);
        spec.axis = -1;
        spec.radial = geo::RadialSymmetry(origin, dir, count, mirror, geo::radialReference(dir, phase));
    }
    return spec;
}

geo::SymmetrySpec paintSymmetrySpec(const Node& mesh)
{
    if (std::optional<geo::SymmetrySpec> spec = symmetrySpec(mesh))
        return *spec;
    geo::SymmetrySpec spec;
    spec.offset = symmetryOffset(mesh, 0);
    return spec;
}

std::optional<geo::SymmetryMap> symmetryMapFor(const Node& node, geo::Mesh& mesh, geo::SymmetrySync* sync)
{
    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(node);
    if (!spec)
        return std::nullopt;
    const double tolerance = std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
    return geo::SymmetryMap::persistent(mesh, *spec, tolerance, sync);
}

std::string symmetryLabel(const geo::SymmetrySpec& spec)
{
    if (!spec.radial)
        return std::string(1, char('x' + std::clamp(spec.axis, 0, 2)));
    return std::format("radial {}{}", spec.radial->count(), spec.radial->mirror() ? " + mirror" : "");
}

double symmetryOffset(const Node& mesh, int axis)
{
    if (axis < 0 || axis > 2 || !mesh.type().isA("transform"))
        return 0.0;
    if (mesh.type().isA("mesh") && mesh.as<int64_t>(MeshNode::kSymmetryPlane) == 1)
        return mesh.as<double>(MeshNode::kSymmetryOffset);
    return mesh.as<glm::dvec3>(Transform::kRotatePivot)[axis];
}

SymmetryState symmetryState(const Graph& g, const std::vector<NodeId>& nodes)
{
    SymmetryState s;
    bool mixed = false, mixedRadial = false;
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        ++s.meshes;
        int64_t v = n->as<int64_t>(MeshNode::kSymmetry);
        if (v < 1 || v > kSymmetryRadial)
            continue;
        const int axis = v == kSymmetryRadial ? SymmetryState::kRadialState : int(v - 1);
        const int count = v == kSymmetryRadial ? int(n->as<int64_t>(MeshNode::kRadialCount)) : 0;
        const bool mirror = v == kSymmetryRadial && n->as<bool>(MeshNode::kRadialMirror);
        mixed = mixed || (s.on > 0 && s.axis != axis);
        mixedRadial = mixedRadial || (s.on > 0 && (s.radialCount != count || s.radialMirror != mirror));
        s.axis = axis;
        s.radialCount = count;
        s.radialMirror = mirror;
        ++s.on;
    }
    if (mixed)
        s.axis = -1;
    if (mixed || mixedRadial)
        s.radialCount = 0, s.radialMirror = false;
    return s;
}

void registerMeshTypes()
{
    if (types::find("mesh"))
        return;
    types::registerType(MeshNode::makeType(*types::find("transform")));
    for (Primitive& p : primitives())
    {
        NodeType t;
        t.name = p.type;
        t.icon = "cube-plus";
        t.doc = std::string("Builds the ") + p.display + " primitive from these settings. Editing its components bakes it into a stored base mesh.";
        t.ownAttrs = p.attrs;
        t.ownAttrs.push_back(upAxisAttr());
        t.ownAttrs.push_back({.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(),
                              .flags = AttrOutput | AttrHidden, .doc = "Generated mesh."});
        int out = int(t.ownAttrs.size()) - 1;
        GeneratorNode::Build build = [shape = p.build](const Node& n) {
            geo::Mesh m = shape(n);
            orientUp(m, n.as<int64_t>(n.type().attrIndex("upAxis")));
            return m;
        };
        t.create = [build, out] {
            auto n = std::make_unique<GeneratorNode>();
            n->build = build;
            n->outAttr = out;
            return n;
        };
        types::registerType(std::move(t));
    }
}

json meshStats(Node& n); // ComponentQuery.cpp

glm::dvec3 sceneUp()
{
    return math::upAxis() == math::UpAxis::Z ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
}

std::vector<std::string> meshObjects(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    const bool named = !a.positional.empty();
    std::vector<std::string> out;
    std::unordered_set<NodeId> seen;
    auto add = [&](const Node& n) {
        if (seen.insert(n.id()).second)
            out.push_back(n.path());
    };
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no object named " + name);
        if (n->type().isA("mesh"))
        {
            add(*n);
            continue;
        }
        bool any = false;
        for (NodeId d : g.descendants(n->id()))
            if (const Node* m = g.node(d); m && m->type().isA("mesh"))
            {
                add(*m);
                any = true;
            }
        if (!any && named)
            throw CommandError(std::string(cmd) + ": " + name + " is not a mesh and has no meshes under it");
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select a mesh or a group of meshes");
    return out;
}

void placePrimitive(Graph& g, NodeId meshId, const glm::dvec3& position, const glm::dvec3& normal)
{
    Node* mesh = g.node(meshId);
    if (!mesh)
        return;
    glm::dvec3 at = position;
    if (glm::length(normal) > 1e-12)
    {
        const glm::dvec3 up = glm::normalize(normal);
        const glm::dmat3 r = glm::mat3_cast(glm::rotation(sceneUp(), up));
        g.setValue({meshId, Transform::kRotate, -1}, matrixToEuler(r, RotateOrder(mesh->as<int64_t>(Transform::kRotateOrder))));
        static const std::vector<Primitive> all = primitives();
        std::optional<PlugRef> src = g.source({meshId, MeshNode::kInMesh, -1});
        Node* gen = src ? g.node(src->node) : nullptr;
        const bool domed = gen && std::ranges::any_of(all, [&](const Primitive& p) { return p.domed && gen->type().name == p.type; });
        if (domed && gen->as<double>(gen->type().attrIndex("fraction")) < 1.0)
        {
            g.evaluate();
            if (MeshPtr m = gen->as<MeshPtr>(src->attr))
                at -= up * m->bounds().min[math::upAxis() == math::UpAxis::Z ? 2 : 1];
        }
    }
    g.setValue({meshId, Transform::kTranslate, -1}, at);
}

void registerMeshCommands()
{
    using T = ArgType;

    for (const Primitive& p : primitives())
    {
        std::vector<FlagDef> flags = {
            {"name", "n", T::String, nullptr, "Name of the mesh; made unique."},
            {"position", "", T::Vec3, nullptr, "World position."},
            {"rotation", "", T::Vec3, nullptr, "Rotation in degrees."},
            {"normal", "", T::Vec3, nullptr,
             "Turn the up axis to this direction, as for placing on a surface; a dome cut by fraction then sits on position by its open base."},
            {"generator", "", T::Bool, true, "Keep the generator node so its parameters stay editable."},
        };
        for (const AttrDef& a : p.attrs)
            if (a.type == PlugType::Enum)
                flags.push_back({a.name, "", T::String, a.enumNames[std::get<int64_t>(a.defaultValue)], a.doc, a.enumNames});
            else
                flags.push_back({a.name, "", a.type == PlugType::Bool ? T::Bool : a.type == PlugType::Int ? T::Int : T::Float, toJson(a.defaultValue), a.doc,
                                 {}, {}, a.minValue, a.maxValue});
        flags.push_back({"upAxis", "", T::String, nullptr, "Direction the primitive's up points: x, y, z, -x, -y or -z; the scene up axis when left out.",
                         kUpAxes});
        std::string type = p.type, display = p.display;
        ops::registerCommand({
            .name = type,
            .doc = std::string("Create a ") + p.display + " mesh. Returns [mesh, generator] (generator is None with generator=False).",
            .flags = flags,
            .run = [type, display](const Args& a) -> json {
                Graph& g = Graph::current();
                const std::string up = a.get<std::string>("upAxis", math::upAxis() == math::UpAxis::Z ? "z" : "y");
                if (std::ranges::find(kUpAxes, up) == kUpAxes.end())
                    throw CommandError(type + ": upAxis must be x, y, z, -x, -y or -z");
                // Recorded so adjusting the op later keeps the axis it was made with, whatever the scene up is then.
                lastOp::limitFlag("upAxis", up);
                const NodeType* genType = types::find(type);
                Node* gen = g.createNode(*genType, {}, kNoNode, true);
                json values = a.flags;
                values["upAxis"] = up;
                for (auto& [flag, value] : values.items())
                {
                    int idx = genType->attrIndex(flag);
                    if (idx >= 0 && !value.is_null())
                        g.setValue({gen->id(), idx, -1}, fromJson(genType->attrs[idx], value));
                }
                Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", display), kNoNode, true);
                g.connect({gen->id(), static_cast<GeneratorNode*>(gen)->outAttr, -1}, {mesh->id(), MeshNode::kInMesh, -1});
                auto vec = [&](const char* flag) { return glm::dvec3(a.flags[flag][0], a.flags[flag][1], a.flags[flag][2]); };
                if (a.has("rotation"))
                    g.setValue({mesh->id(), Transform::kRotate, -1}, vec("rotation"));
                if (a.has("normal"))
                    placePrimitive(g, mesh->id(), a.has("position") ? vec("position") : glm::dvec3(0.0), vec("normal"));
                else if (a.has("position"))
                    g.setValue({mesh->id(), Transform::kTranslate, -1}, vec("position"));
                json genName = gen->path();
                if (!a.get<bool>("generator", true))
                {
                    g.evaluate();
                    MeshPtr baked = mesh->as<MeshPtr>(MeshNode::kOutMesh);
                    g.disconnect({mesh->id(), MeshNode::kInMesh, -1});
                    g.setValue({mesh->id(), MeshNode::kInMesh, -1}, baked);
                    g.deleteNode(gen->id());
                    genName = nullptr;
                }
                g.select({mesh->id()});
                return json::array({mesh->path(), genName});
            },
        });
        ops::setAdjustable(type);
    }

    ops::registerCommand({
        .name = "smoothPreview",
        .doc = "Display meshes subdivided without changing them (the 1/2/3 keys), including every mesh under a given group. level 0 is off.",
        .positionalDoc = "objects",
        .flags = {{"level", "", T::Int, 2, "Subdivision levels to display (0 to 3).", {}, {}, 0.0, 3.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            int64_t level = std::clamp<int64_t>(a.get<int>("level", 2), 0, 3);
            // A display toggle, like Maya's: undo steps over it.
            UndoStack& undo = UndoStack::get();
            const bool was = undo.enabled();
            undo.setEnabled(false);
            struct Restore
            {
                UndoStack& undo;
                bool was;
                ~Restore() { undo.setEnabled(was); }
            } restore{undo, was};
            Graph& g = Graph::current();
            std::vector<NodeId> ids;
            for (const std::string& name : a.objects())
            {
                const NodeId id = nodeNamed(name).id();
                ids.push_back(id);
                std::ranges::copy(g.descendants(id), std::back_inserter(ids));
            }
            std::ranges::sort(ids);
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            for (NodeId id : ids)
                if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    g.setValue({id, MeshNode::kSmoothPreview, -1}, level);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "meshInfo",
        .doc = "Counts and health of a mesh: points, edges, faces, triangles, tri/quad/ngon faces, open border edges and loops, "
               "non-manifold edges, shells, closed, object and world bounds, symmetry, smooth preview, modifiers, material slots "
               "(and the cage's counts under modifiers).",
        .positionalDoc = "object",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("meshInfo: no object");
            Node& n = nodeNamed(objs[0]);
            if (!n.type().isA("mesh"))
                throw CommandError("meshInfo: " + objs[0] + " is not a mesh");
            return meshStats(n);
        },
    });

    ops::registerCommand({
        .name = "meshData",
        .doc = "A mesh's points and faces as plain lists: {points: [[x,y,z]...], faces: [[i,j,k,...]...]}. "
               "base=True reads the editable base mesh (under modifiers) instead of the final result.",
        .positionalDoc = "mesh",
        .flags = {{"base", "", T::Bool, false, "The base mesh, not the modified result."},
                  {"worldSpace", "", T::Bool, false, "Points in world space."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("meshData: no mesh");
            Node& n = nodeNamed(objs.back());
            if (!n.type().isA("mesh"))
                throw CommandError("meshData: " + objs.back() + " is not a mesh");
            Graph::current().evaluate();
            MeshPtr m = a.get<bool>("base") ? modifiers::cage(Graph::current(), n.id()) : n.as<MeshPtr>(MeshNode::kOutMesh);
            json points = json::array(), faces = json::array();
            if (!m)
                return {{"points", points}, {"faces", faces}};
            glm::dmat4 w = a.get<bool>("worldSpace") ? n.as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
            std::vector<geo::Index> remap(m->pointCapacity(), geo::kInvalid);
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                {
                    remap[p] = geo::Index(points.size());
                    glm::dvec3 q = glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
                    points.push_back({q.x, q.y, q.z});
                }
            for (geo::Index f = 0; f < m->faceCapacity(); ++f)
                if (m->faceValid(f))
                {
                    json face = json::array();
                    m->forEachFaceCorner(f, [&](geo::Index c) { face.push_back(remap[m->cornerPoint(c)]); });
                    faces.push_back(std::move(face));
                }
            return {{"points", std::move(points)}, {"faces", std::move(faces)}};
        },
    });

    ops::registerCommand({
        .name = "setMeshPoints",
        .doc = "Move every point of a mesh's base (under its modifiers) at once, in meshData's point order, in object space. "
               "Topology stays. One undo step. Python's Node.set_points takes a numpy array, faster.",
        .positionalDoc = "mesh",
        .flags = {{"points", "", T::List, json::array(), "[[x, y, z], ...], one per point, in cm."},
                  {"buffer", "", T::Int, 0, "Internal: points handed over from Python (Node.set_points)."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("setMeshPoints: no mesh");
            std::vector<glm::dvec3> pts;
            if (uint64_t token = uint64_t(a.get<int64_t>("buffer")); token != 0)
            {
                if (token != gStashedPoints.first)
                    throw CommandError("setMeshPoints: the buffer is gone; pass points= instead");
                pts = std::move(gStashedPoints.second);
                gStashedPoints = {};
            }
            else
                for (const json& p : a.flags.value("points", json::array()))
                    pts.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>());
            Node& n = nodeNamed(objs.back());
            if (!n.type().isA("mesh"))
                throw CommandError("setMeshPoints: " + objs.back() + " is not a mesh");
            Graph& g = Graph::current();
            PlugRef base = modifiers::editPlug(g, n.id());
            g.evaluate();
            const MeshPtr& cur = std::get<MeshPtr>(g.storedValue(base));
            if (!cur)
                throw CommandError("setMeshPoints: " + objs.back() + " has no mesh");
            if (pts.size() != cur->numPoints())
                throw CommandError(std::format("setMeshPoints: {} has {} points, got {}", objs.back(), cur->numPoints(), pts.size()));
            auto edited = std::make_shared<geo::Mesh>(*cur);
            std::span<glm::dvec3> dst = edited->positions();
            if (edited->numPoints() == edited->pointCapacity())
                tbb::parallel_for(tbb::blocked_range<size_t>(0, pts.size(), 1 << 16), [&](const tbb::blocked_range<size_t>& r) {
                    std::copy(pts.begin() + r.begin(), pts.begin() + r.end(), dst.begin() + r.begin());
                });
            else
                for (geo::Index p = 0, k = 0; p < edited->pointCapacity(); ++p)
                    if (edited->pointValid(p))
                        dst[p] = pts[k++];
            g.setValue(base, MeshPtr(std::move(edited)));
            return int64_t(pts.size());
        },
    });

    ops::registerCommand({
        .name = "createMesh",
        .doc = "Make a mesh from points and faces (lists of point indices, any size). Returns its name.",
        .flags = {{"points", "", T::List, nullptr, "[[x, y, z], ...] in cm."},
                  {"faces", "", T::List, nullptr, "[[i, j, k, ...], ...]."},
                  {"uvs", "", T::List, nullptr, "[[u, v], ...]: one per face corner, face by face in faces' order. Fills uv0."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            if (!a.has("points") || !a.has("faces"))
                throw CommandError("createMesh: give points= and faces=");
            std::vector<glm::dvec3> pts;
            for (const json& p : a.flags["points"])
                pts.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>());
            std::vector<geo::Index> sizes, idx;
            for (const json& f : a.flags["faces"])
            {
                sizes.push_back(geo::Index(f.size()));
                for (const json& i : f)
                {
                    geo::Index v = i.get<geo::Index>();
                    if (v >= pts.size())
                        throw CommandError("createMesh: face index " + std::to_string(v) + " is out of range");
                    idx.push_back(v);
                }
            }
            auto mesh = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(pts, sizes, idx));
            if (a.has("uvs"))
            {
                const json& uvs = a.flags["uvs"];
                if (uvs.size() != idx.size())
                    throw CommandError(std::format("createMesh: {} uvs for {} face corners", uvs.size(), idx.size()));
                if (mesh->numFaces() != sizes.size())
                    throw CommandError("createMesh: uvs need every face to be valid");
                std::span<glm::vec2> uv = mesh->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
                size_t at = 0;
                for (geo::Index f = 0; f < mesh->numFaces(); ++f)
                {
                    geo::Index c = mesh->faceCorner(f);
                    for (geo::Index k = 0; k < sizes[f]; ++k, ++at, c = mesh->cornerNext(c))
                        uv[c] = glm::vec2(uvs[at].at(0).get<float>(), uvs[at].at(1).get<float>());
                }
            }
            Node* n = Graph::current().createNode(*types::find("mesh"), a.get<std::string>("name", "Mesh"), kNoNode, true);
            Graph::current().setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(mesh)));
            Graph::current().select({n->id()});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "importObj",
        .doc = "Import an OBJ file; one mesh per object. Returns the new mesh names.\n\n"
               "A georeferenced export (metadata.xml, _offset.xyz, .rcinfo or .prj beside it) sets the scene's georeference or is "
               "moved into it; the log says which.",
        .positionalDoc = "path",
        .flags = {{"georeference", "", ArgType::Bool, true, "Place it by the georeference files beside it."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("importObj: path required");
            std::string path = a.positional[0].get<std::string>();
            progress::Scope scope("Importing " + path + "...", 4.0);
            geo::ObjReadResult r;
            {
                progress::Scope read("Reading " + path, 0.0, progress::None, 3.0);
                progress::runOnWorker([&] { r = geo::readObjFile(path); });
            }
            if (!r.ok())
                throw CommandError("importObj: " + r.error);
            if (a.get<bool>("georeference", true) && geoSidecar::find(path))
            {
                std::vector<geo::Mesh*> meshes;
                for (geo::ObjObject& o : r.objects)
                    meshes.push_back(&o.mesh);
                geoSidecar::place(path, meshes);
            }
            Graph& g = Graph::current();
            Graph::Batch batch(g);
            json names = json::array();
            std::vector<NodeId> ids;
            for (geo::ObjObject& o : r.objects)
            {
                std::string wanted = Graph::validName(o.name) ? o.name : "Mesh";
                Node* n = g.createNode(*types::find("mesh"), wanted, kNoNode, true);
                g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(o.mesh))));
                names.push_back(n->path());
                ids.push_back(n->id());
            }
            g.select(ids);
            return names;
        },
    });

    ops::registerCommand({
        .name = "exportObj",
        .doc = "Export meshes to an OBJ file in world space. Defaults to the selection.",
        .positionalDoc = "path objects...",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("exportObj: path required");
            std::string path = a.positional[0].get<std::string>();
            std::vector<std::string> names;
            for (size_t k = 1; k < a.positional.size(); ++k)
                names.push_back(a.positional[k].get<std::string>());
            if (names.empty())
                for (NodeId id : Graph::current().selection())
                    names.push_back(Graph::current().node(id)->path());
            Graph& g = Graph::current();
            g.evaluate();
            std::vector<geo::ObjObject> objects;
            for (const std::string& name : names)
            {
                Node& n = nodeNamed(name);
                if (!n.type().isA("mesh"))
                    continue;
                MeshPtr m = modifiers::exportMesh(g, n.id());
                if (!m)
                    continue;
                geo::ObjObject o;
                o.name = name;
                o.mesh = *m;
                glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
                for (geo::Index p = 0; p < o.mesh.pointCapacity(); ++p)
                    if (o.mesh.pointValid(p))
                        o.mesh.setPosition(p, glm::dvec3(world * glm::dvec4(o.mesh.position(p), 1.0)));
                objects.push_back(std::move(o));
            }
            if (objects.empty())
                throw CommandError("exportObj: no meshes to export");
            progress::Scope scope("Exporting " + path + "...");
            bool written = false;
            progress::runOnWorker([&] {
                std::string text = geo::writeObj(objects);
                if (FILE* fp = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(text.data(), 1, text.size(), fp);
                    std::fclose(fp);
                    written = true;
                }
            });
            if (!written)
                throw CommandError("exportObj: can't write " + path);
            return path;
        },
    });
}
} // namespace lila::scene
