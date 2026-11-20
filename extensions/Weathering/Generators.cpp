// Roof, window, wire, pile and rooftop generators: commands, their live geometry-nodes groups, menus and option boxes.
// Each lays out the user's pieces (piecesOrDefault), fitted by their bounds; the placeholders only fill roles nobody
// supplied. Live results are a mesh whose geometry-nodes group reads the hosts through Object Info, so editing a host
// rebuilds it; realize=True turns the instances into scene objects.

#include "GenNodes.h"
#include "GenPlaceholders.h"
#include "Pieces.h"
#include "RoofLayout.h"
#include "RooftopLayout.h"
#include "Truss.h"
#include "Weathering.h"
#include "WindowLayout.h"
#include "WireLayout.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Components.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/OptionBox.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

using namespace lila;
using namespace lila::ui;

namespace weathering
{
namespace generators
{
using T = ArgType;
using scene::Node;

std::string pathOf(const json& made) { return made.is_array() ? made.at(0).get<std::string>() : made.get<std::string>(); }

glm::dmat4 worldOf(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    return n && n->type().isA("transform") ? n->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
}

gen::Mesh worldMesh(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    const auto m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    if (!m)
        return {};
    gen::Mesh out = *m;
    const glm::dmat4 w = worldOf(g, id);
    for (gen::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, glm::dvec3(w * glm::dvec4(out.position(p), 1.0)));
    return out;
}

void meshesBelow(Graph& g, NodeId id, std::vector<NodeId>& out)
{
    if (Node* n = g.node(id); n && n->type().isA("mesh"))
        out.push_back(id);
    for (NodeId c : g.children(id))
        meshesBelow(g, c, out);
}

/** The meshes named (or under the groups named), else under the selection. */
std::vector<NodeId> hostMeshes(Graph& g, const Args& a, const char* cmd, bool allowCurves = false, std::vector<NodeId>* curves = nullptr)
{
    std::vector<NodeId> out;
    std::set<NodeId> seen;
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (allowCurves && n->type().isA("curve"))
        {
            curves->push_back(n->id());
            continue;
        }
        std::vector<NodeId> found;
        meshesBelow(g, n->id(), found);
        for (NodeId m : found)
            if (seen.insert(m).second)
                out.push_back(m);
    }
    return out;
}

std::string uniqueGnGroup(const std::string& base)
{
    const json existing = ops::execute("gnGroups", json::object());
    auto taken = [&](const std::string& n) {
        for (const json& e : existing)
            if ((e.is_string() && e.get<std::string>() == n) || (e.is_object() && e.value("name", std::string()) == n))
                return true;
        return false;
    };
    Graph& g = Graph::current();
    std::string name = base;
    for (int k = 2; taken(name) || g.find(name); ++k)
        name = base + std::to_string(k);
    return name;
}

std::string safeName(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "W" + out;
    return out;
}

std::string newGroup(const std::string& name) { return pathOf(ops::execute("group", {{"empty", true}, {"name", safeName(name)}})); }

std::string weightsOf(const std::vector<Piece>& pieces)
{
    std::vector<double> w;
    for (const Piece& p : pieces)
        w.push_back(p.weight);
    return gen::joinNumbers(w);
}

std::vector<gen::Bounds> boundsOf(Graph& g, const std::vector<Piece>& pieces)
{
    std::vector<gen::Bounds> out;
    for (const Piece& p : pieces)
        out.push_back(gen::boundsOf(pieceMesh(g, p)));
    return out;
}

std::vector<double> weightList(const std::vector<Piece>& pieces)
{
    std::vector<double> w;
    for (const Piece& p : pieces)
        w.push_back(p.weight);
    return w;
}

json namesOf(const std::vector<Piece>& pieces)
{
    json out = json::array();
    for (const Piece& p : pieces)
        out.push_back(p.name);
    return out;
}

/** A live group in the making: nodes, links and exposed inputs, Object Info nodes added by helpers. */
struct Live
{
    json nodes = json::array(), links = json::array(), inputs = json::array();
    int infos = 0;

    void info(Graph& g, NodeId id, const std::string& into, bool world)
    {
        const std::string name = "Info" + std::to_string(++infos);
        nodes.push_back({{"name", name}, {"type", "ObjectInfo"}, {"position", json::array({0, 160 * infos})},
                         {"props", {{"object", g.node(id)->path()}, {"transformSpace", world ? "world" : "original"}}}});
        links.push_back(name + ".geometry -> " + into);
    }
    void pieces(Graph& g, const std::vector<Piece>& list, const std::string& into)
    {
        for (const Piece& p : list)
            info(g, p.mesh, into, false);
    }
    void expose(const char* name, const char* type, json value, const char* doc, const std::string& node)
    {
        inputs.push_back({{"name", name}, {"type", type}, {"default", std::move(value)}, {"description", doc}});
        links.push_back(std::string("GroupInput.") + name + " -> " + node + "." + name);
    }
};

/** Builds the group onto a new mesh under a new scene group. Returns group, mesh, modifier, gnGroup and count. */
json finishLive(Graph& g, Live& l, const std::string& end, const std::string& name, const std::string& what)
{
    l.links.push_back(end + " -> GroupOutput.geometry");
    const std::string groupPath = newGroup(name);
    const NodeId groupId = g.find(groupPath)->id();
    const std::string mesh = pathOf(ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", g.node(groupId)->name() + "Mesh"}}));
    const NodeId meshId = g.find(mesh)->id();
    const std::string gnGroup = uniqueGnGroup(g.node(groupId)->name() + "_Gen");
    ops::execute("gnBuildGroup", {{"args", {json{{"name", gnGroup}, {"description", what}, {"inputs", l.inputs}, {"nodes", l.nodes}, {"links", l.links}}}},
                                  {"replace", false}});
    const std::string mod = pathOf(ops::execute("gnApply", {{"args", {mesh, gnGroup}}}));
    ops::execute("parent", {{"args", {g.node(meshId)->path(), g.node(groupId)->path()}}});
    g.evaluate();
    int64_t count = 0;
    if (Node* m = g.find(mod))
        if (const lila::gn::GeometryPtr geo = lila::gn::modifierGeometry(m->id()); geo && geo->instances)
            count = int64_t(geo->instances->size());
    return {{"group", g.node(groupId)->path()}, {"mesh", g.node(meshId)->path()}, {"modifier", mod}, {"gnGroup", gnGroup}, {"count", count}};
}

/** realize=True: the live result's instances as scene objects in a group of the result's name; the live mesh goes. */
json realizeLive(Graph& g, const json& live, const std::string& name)
{
    const json made = ops::execute("instancesToObjects", {{"args", {live["mesh"]}}, {"name", safeName(name) + "_Objects"}});
    const NodeId objects = g.find(made["group"].get<std::string>())->id();
    const std::string liveGroup = live["group"].get<std::string>();
    ops::execute("delete", {{"args", {liveGroup}}});
    try
    {
        ops::execute("delete", {{"args", {live["gnGroup"]}}});
    }
    catch (const std::exception&)
    {
    }
    ops::execute("rename", {{"args", {g.node(objects)->path(), safeName(name)}}});
    json out = {{"group", g.node(objects)->path()}, {"objects", made["objects"]}, {"count", made["count"]}};
    return out;
}

/** Style-dependent default: the flag's value when given, else realistic or toon's. */
double styled(const Args& a, const char* flag, Style s, double realistic, double toon)
{
    return a.has(flag) ? a.get<double>(flag) : (s == Style::Toon ? toon : realistic);
}

/** Face indices of a mesh from faces= or the component selection. */
std::vector<gen::Index> facesArg(const Args& a, NodeId mesh)
{
    std::vector<gen::Index> out;
    if (a.has("faces"))
    {
        for (const json& f : a.flags["faces"])
            out.push_back(gen::Index(f.get<int64_t>()));
        return out;
    }
    for (uint32_t f : scene::components::selected(mesh, scene::ComponentMode::Face))
        out.push_back(f);
    return out;
}

// ------------------------------------------------------------------------------------------------ roofing

const char* roleOfKind(const std::string& kind)
{
    if (kind == "planks")
        return gen::role::kRoofPlank;
    if (kind == "tiles")
        return gen::role::kRoofTile;
    return gen::role::kRoofSheet;
}

json runRoofing(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<NodeId> hosts = hostMeshes(g, a, "roofing");
    if (hosts.empty())
        throw CommandError("roofing: give the roofs (meshes, Blockwork blocks or groups), or select them");
    const Style style = styleArg(a);
    const std::string kind = a.get<std::string>("kind", "sheets");
    const std::vector<Piece> cover = piecesOrDefault(g, a, "pieces", roleOfKind(kind), style, "roofing");
    std::vector<Piece> caps;
    if (a.get<bool>("ridge", true))
        caps = piecesOrDefault(g, a, "caps", gen::role::kRidgeCap, style, "roofing");
    std::vector<gen::Index> faces;
    if (hosts.size() == 1)
        faces = facesArg(a, hosts[0]);
    else if (a.has("faces"))
        throw CommandError("roofing: faces= takes one host");
    g.evaluate();

    roof::Options o;
    o.overlap = a.get<double>("overlap", 0.15);
    o.sideOverlap = a.get<double>("sideOverlap", 0.08);
    o.overhang = a.get<double>("overhang", 5.0);
    o.scale = a.get<double>("scale", 1.0);
    o.damage = a.get<double>("damage", 0.3);
    if (a.has("missing"))
        o.missing = a.get<double>("missing");
    if (a.has("lifted"))
        o.lifted = a.get<double>("lifted");
    o.liftAngle = a.get<double>("liftAngle", 0.0);
    o.sag = a.get<double>("sag", 0.0);
    o.neatness = styled(a, "neatness", style, 0.3, 1.0);
    o.stagger = a.get<double>("stagger", kind == "tiles" ? 0.5 : 0.0);
    o.ridge = a.get<bool>("ridge", true);
    o.maxSlope = a.get<double>("maxSlope", 75.0);
    o.seed = uint32_t(a.get<int64_t>("seed", 1));

    gen::Mesh joined;
    for (NodeId h : hosts)
        joined.append(worldMesh(g, h));
    joined.compact();
    roof::Input in;
    in.host = &joined;
    in.faces = faces;
    in.cover = boundsOf(g, cover);
    in.coverWeights = weightList(cover);
    in.caps = boundsOf(g, caps);
    in.capWeights = weightList(caps);
    const roof::Result r = roof::layout(in, o);
    if (r.regions.empty())
        throw CommandError("roofing: no roof faces: select faces, or give hosts with faces within maxSlope of up");

    const std::string name = a.get<std::string>("name", g.node(hosts[0])->name() + "_Roofing");
    Live l;
    l.nodes.push_back({{"name", "Roofing"}, {"type", nodes::kRoofingType}, {"position", json::array({400, 0})},
                       {"values",
                        {{"weights", weightsOf(cover)}, {"capWeights", weightsOf(caps)}, {"faces", gen::joinNumbers(std::vector<double>(faces.begin(), faces.end()))},
                         {"overlap", o.overlap}, {"sideOverlap", o.sideOverlap}, {"overhang", o.overhang}, {"scale", o.scale},
                         {"missing", o.missing.value_or(-1.0)}, {"lifted", o.lifted.value_or(-1.0)}, {"liftAngle", o.liftAngle}, {"sag", o.sag},
                         {"stagger", o.stagger}, {"ridge", o.ridge}, {"maxSlope", o.maxSlope}}}});
    for (NodeId h : hosts)
        l.info(g, h, "Roofing.host", true);
    l.pieces(g, cover, "Roofing.pieces");
    l.pieces(g, caps, "Roofing.caps");
    l.expose("damage", "float", o.damage, "0 to 1: missing pieces, lifted corners.", "Roofing");
    l.expose("neatness", "float", o.neatness, "0 messy, 1 neat rows.", "Roofing");
    l.expose("seed", "int", int64_t(o.seed), "Another seed gives another roof.", "Roofing");
    json out = finishLive(g, l, "Roofing.geometry", name, "Roof pieces laid in rows over " + g.node(hosts[0])->name() + ".");
    if (a.get<bool>("realize", false))
        out = realizeLive(g, out, name);
    out.update({{"pieces", int64_t(r.placed.size()) - r.caps}, {"caps", r.caps}, {"rows", r.rows}, {"regions", int64_t(r.regions.size())},
                {"missing", r.missing}, {"lifted", r.lifted}, {"style", styleName(style)}, {"kind", kind}, {"used", namesOf(cover)}});
    return out;
}

// ------------------------------------------------------------------------------------------------ windows

/** A material made once by name, so faces with that slot render with it. */
void ensureMaterial(Graph& g, const std::string& name, json flags)
{
    if (g.find(name))
        return;
    flags["args"] = json::array({name});
    try
    {
        ops::execute("createMaterial", flags);
    }
    catch (const std::exception& e)
    {
        log::warn("Weathering: material {} not made: {}", name, e.what());
    }
}

std::string interiorMaterialFor(Graph& g, const std::string& image)
{
    const std::string name = "MI_" + safeName(std::filesystem::path(image).stem().string()) + "Interior";
    ensureMaterial(g, name, {{"baseColorMap", image}, {"roughness", 0.8}});
    return name;
}

json runWindows(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<NodeId> hosts = hostMeshes(g, a, "fitWindows");
    if (hosts.empty())
        throw CommandError("fitWindows: give the walls (meshes, Blockwork blocks, Masonry walls or groups), or select them");
    const Style style = styleArg(a);
    window::Options o;
    if (!window::parseGlass(a.get<std::string>("glass", "intact"), &o.glass))
        throw CommandError("fitWindows: glass is intact, cracked, broken, boarded, none or mixed");
    if (!window::parseInterior(a.get<std::string>("interior", "room"), &o.interior))
        throw CommandError("fitWindows: interior is room, card, mesh or none");
    const bool anyGlass = o.glass != window::Glass::None && o.glass != window::Glass::Boarded;
    const bool mixed = o.glass == window::Glass::Mixed;
    std::vector<Piece> slots[window::RoomMesh];
    slots[window::Frame] = piecesOrDefault(g, a, "frames", gen::role::kFrame, style, "fitWindows");
    o.sill = a.get<bool>("sill", true);
    if (o.sill)
        slots[window::Sill] = piecesOrDefault(g, a, "sills", gen::role::kSill, style, "fitWindows");
    if (anyGlass)
        slots[window::Pane] = piecesOrDefault(g, a, "panes", gen::role::kGlass, style, "fitWindows");
    if (mixed || o.glass == window::Glass::Cracked)
        slots[window::Cracked] = piecesOrDefault(g, a, "cracked", gen::role::kGlassCracked, style, "fitWindows");
    if (mixed || o.glass == window::Glass::Broken)
        slots[window::Broken] = piecesOrDefault(g, a, "broken", gen::role::kGlassBroken, style, "fitWindows");
    if (mixed || o.glass == window::Glass::Boarded)
        slots[window::Board] = piecesOrDefault(g, a, "boards", gen::role::kBoard, style, "fitWindows");
    if (o.interior == window::Interior::Mesh)
    {
        if (!a.has("interiors"))
            throw CommandError("fitWindows: interior='mesh' takes interiors=[...] (meshes, groups or library ids)");
        slots[window::InteriorPiece] = resolvePieces(g, a.flags["interiors"], "fitWindows");
    }
    std::string cardMaterial = "M_WeatheringInteriorCard";
    if (o.interior == window::Interior::Card)
    {
        if (!a.has("interiorImage"))
            throw CommandError("fitWindows: interior='card' takes interiorImage= (an image of a room)");
        cardMaterial = interiorMaterialFor(g, a.get<std::string>("interiorImage"));
    }
    const std::string roomMaterial = style == Style::Toon ? "M_WeatheringInteriorToon" : "M_WeatheringInterior";
    ensureMaterial(g, roomMaterial, {{"baseColor", style == Style::Toon ? json::array({0.32, 0.22, 0.16}) : json::array({0.08, 0.075, 0.07})}, {"roughness", 0.9}});
    ensureMaterial(g, "M_WeatheringGlass", {{"baseColor", json::array({0.05, 0.07, 0.08})}, {"roughness", 0.05}, {"metallic", 0.0}, {"opacity", 0.35}, {"blendMode", "translucent"}});
    ensureMaterial(g, "M_WeatheringGlassToon", {{"baseColor", json::array({0.35, 0.55, 0.65})}, {"roughness", 0.2}, {"opacity", 0.5}, {"blendMode", "translucent"}});

    o.frameInset = a.get<double>("frameInset", 4.0);
    o.frameScale = a.get<double>("frameScale", 1.0);
    o.columns = int(a.get<int64_t>("columns", 1));
    o.rows = int(a.get<int64_t>("rows", 1));
    o.sillProject = a.get<double>("sillProject", 4.0);
    o.damage = a.get<double>("damage", 0.3);
    o.neatness = styled(a, "neatness", style, 0.3, 1.0);
    o.boardOverhang = a.get<double>("boardOverhang", 8.0);
    o.interiorDepth = a.get<double>("interiorDepth", 300.0);
    o.doors = a.get<bool>("doors", false);
    o.seed = uint32_t(a.get<int64_t>("seed", 1));
    window::FindOptions fo;
    fo.minSize = a.get<double>("minSize", 15.0);
    fo.maxThickness = a.get<double>("maxThickness", 150.0);
    fo.flip = a.get<bool>("flip", false);
    g.evaluate();

    gen::Mesh wall;
    for (NodeId h : hosts)
        wall.append(worldMesh(g, h));
    wall.compact();
    const std::vector<window::Opening> openings = window::findOpenings(wall, gen::boundsOf(wall).center(), fo);
    window::Pieces pieces;
    for (int s = 0; s < window::RoomMesh; ++s)
    {
        pieces.slot[s] = boundsOf(g, slots[s]);
        pieces.weights[s] = weightList(slots[s]);
    }
    const window::Result r = window::layout(openings, pieces, o);
    if (r.openings.empty())
        throw CommandError("fitWindows: no openings found" +
                           std::string(r.doorsSkipped ? " (" + std::to_string(r.doorsSkipped) + " reach the ground: doors=True fills them)" : "") +
                           "; carve holes through the walls first");

    const std::string name = a.get<std::string>("name", g.node(hosts[0])->name() + "_Windows");
    Live l;
    l.nodes.push_back({{"name", "Windows"}, {"type", nodes::kWindowsType}, {"position", json::array({400, 0})},
                       {"values",
                        {{"glass", window::glassName(o.glass)}, {"interior", a.get<std::string>("interior", "room")}, {"roomMaterial", roomMaterial},
                         {"cardMaterial", cardMaterial}, {"frameInset", o.frameInset}, {"frameScale", o.frameScale}, {"columns", o.columns},
                         {"rows", o.rows}, {"sill", o.sill}, {"sillProject", o.sillProject}, {"neatness", o.neatness},
                         {"boardOverhang", o.boardOverhang}, {"interiorDepth", o.interiorDepth}, {"doors", o.doors}, {"flip", fo.flip},
                         {"minSize", fo.minSize}, {"maxThickness", fo.maxThickness}}}});
    const char* sockets[] = {"frames", "sills", "panes", "cracked", "broken", "boards", "interiors"};
    const char* weights[] = {"frameWeights", "sillWeights", "paneWeights", "crackedWeights", "brokenWeights", "boardWeights", "interiorWeights"};
    for (int s = 0; s < window::RoomMesh; ++s)
    {
        l.pieces(g, slots[s], std::string("Windows.") + sockets[s]);
        l.nodes[0]["values"][weights[s]] = weightsOf(slots[s]);
    }
    for (NodeId h : hosts)
        l.info(g, h, "Windows.walls", true);
    l.expose("damage", "float", o.damage, "0 to 1: mixed glass breaks more, boards go missing.", "Windows");
    l.expose("seed", "int", int64_t(o.seed), "Another seed picks other pieces and breakage.", "Windows");
    json out = finishLive(g, l, "Windows.geometry", name, "Windows fitted into the openings of " + g.node(hosts[0])->name() + ".");
    if (a.get<bool>("realize", false))
        out = realizeLive(g, out, name);
    json found = json::array();
    for (const window::Opening& op : r.openings)
    {
        const glm::dvec3 c = op.at((op.u0 + op.u1) * 0.5, (op.v0 + op.v1) * 0.5, 0.0);
        found.push_back({{"center", json::array({c.x, c.y, c.z})}, {"width", op.u1 - op.u0}, {"height", op.v1 - op.v0}, {"depth", op.depth},
                         {"normal", json::array({op.N.x, op.N.y, op.N.z})}, {"door", op.door}, {"rectangular", op.rectangular}});
    }
    out.update({{"windows", int64_t(r.openings.size())}, {"openings", found}, {"doorsSkipped", r.doorsSkipped}, {"panes", r.panes},
                {"cracked", r.cracked}, {"broken", r.broken}, {"boarded", r.boarded}, {"style", styleName(style)}, {"cardMaterial", cardMaterial},
                {"used", namesOf(slots[window::Frame])}});
    return out;
}

// ------------------------------------------------------------------------------------------------ wires

void setWorld(Graph& g, NodeId id, const glm::dmat4& m)
{
    json mat = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            mat.push_back(m[c][r]);
    ops::execute("xform", {{"args", {g.node(id)->path()}}, {"worldSpace", true}, {"matrix", mat}});
}

/** A scene instance of a piece placed so its mesh lands at `xf`, parented under group. */
std::string placePiece(Graph& g, const Piece& p, const glm::dmat4& xf, const std::string& group)
{
    const glm::dmat4 meshInRoot = glm::inverse(worldOf(g, p.root)) * worldOf(g, p.mesh);
    const json made = ops::execute("instance", {{"args", {g.node(p.root)->path()}}});
    const NodeId id = g.find(pathOf(made))->id();
    ops::execute("parent", {{"args", {g.node(id)->path(), group}}});
    ops::execute("setAttr", {{"args", {g.node(id)->path() + ".visibility", true}}});
    setWorld(g, id, xf * glm::inverse(meshInRoot));
    return g.node(id)->path();
}

std::vector<glm::dvec3> curveLine(const std::string& curve)
{
    std::vector<glm::dvec3> out;
    const json data = ops::execute("curveData", {{"args", {curve}}, {"worldSpace", true}});
    for (const json& sp : data.is_array() ? data : json::array())
    {
        const curve::Polyline pl = curve::tessellate(curve::splineFromJson(sp));
        for (const glm::dvec3& p : pl.points)
            out.push_back(p);
        if (pl.closed && !pl.points.empty())
            out.push_back(pl.points.front());
        break;
    }
    return out;
}

gen::Bounds boundsInWorld(Graph& g, NodeId mesh) { return gen::boundsOf(worldMesh(g, mesh)); }

json runWires(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::vector<NodeId> curves;
    const std::vector<NodeId> hosts = hostMeshes(g, a, "wires", true, &curves);
    if (hosts.empty() && curves.empty() && !a.has("points"))
        throw CommandError("wires: give poles, buildings or brackets, curves to run along, or points=[[x, y, z], ...]");
    const Style style = styleArg(a);
    g.evaluate();
    const std::vector<Piece> insulators = piecesOrDefault(g, a, "insulators", gen::role::kInsulator, style, "wires");
    const std::vector<Piece> brackets = piecesOrDefault(g, a, "brackets", gen::role::kBracket, style, "wires");
    const bool placePoles = a.get<bool>("placePoles", true) && !curves.empty();
    std::vector<Piece> poles;
    if (placePoles)
        poles = piecesOrDefault(g, a, "poles", gen::role::kPole, style, "wires");
    const gen::Bounds insB = gen::boundsOf(pieceMesh(g, insulators[0]));
    const gen::Bounds brB = gen::boundsOf(pieceMesh(g, brackets[0]));

    wires::Options wo;
    wo.cornerHeight = a.get<double>("cornerHeight", 0.75);
    wo.standoff = a.has("standoff") ? a.get<double>("standoff") : gen::fitPiece(brB, false, true).size.x;
    wo.insulator = insB.size().y;
    wo.maxSpan = a.get<double>("maxSpan", 3000.0);

    std::vector<gen::Bounds> hostBounds;
    for (NodeId h : hosts)
        hostBounds.push_back(boundsInWorld(g, h));
    std::vector<wires::Anchor> anchors = wires::anchorsOn(hostBounds, wo);
    std::vector<std::vector<int>> runs = wires::chain(anchors, wo.maxSpan);
    for (NodeId c : curves)
    {
        const std::vector<glm::dvec3> line = curveLine(g.node(c)->path());
        std::vector<wires::Anchor> along = wires::anchorsAlong(line, a.get<double>("spacing", 2500.0), a.get<double>("height", 700.0) + (placePoles ? wo.insulator : 0.0));
        std::vector<int> run;
        for (wires::Anchor& x : along)
        {
            run.push_back(int(anchors.size()));
            anchors.push_back(x);
        }
        if (run.size() >= 2)
            runs.push_back(run);
    }
    if (a.has("points"))
    {
        std::vector<int> run;
        for (const json& p : a.flags["points"])
        {
            wires::Anchor x;
            x.at = glm::dvec3(p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>());
            run.push_back(int(anchors.size()));
            anchors.push_back(x);
        }
        if (run.size() >= 2)
            runs.push_back(run);
    }
    if (runs.empty())
        throw CommandError("wires: no two anchors close enough to string (maxSpan " + std::to_string(int(wo.maxSpan)) + " cm)");

    // Colliders: buildings (not poles or brackets) given, or colliders=.
    std::vector<NodeId> colliders;
    if (a.has("colliders"))
    {
        for (const json& c : a.flags["colliders"])
        {
            Node* n = g.find(c.get<std::string>());
            if (!n || !n->type().isA("mesh"))
                throw CommandError("wires: collider " + c.dump() + " is not a mesh");
            colliders.push_back(n->id());
        }
    }
    else
        for (size_t k = 0; k < hosts.size(); ++k)
        {
            const glm::dvec3 s = hostBounds[k].size();
            const bool pole = s.y > 2.5 * std::max(s.x, s.z) && std::max(s.x, s.z) < 100.0;
            if (!pole && std::max({s.x, s.y, s.z}) > 60.0)
                colliders.push_back(hosts[k]);
        }

    const std::string name = a.get<std::string>("name", "Wires");
    const std::string group = newGroup(name);
    const NodeId groupId = g.find(group)->id();
    json cables = json::array();
    int spans = 0;
    for (size_t ri = 0; ri < runs.size(); ++ri)
    {
        const std::vector<int>& run = runs[ri];
        json pts = json::array();
        gen::Bounds rb;
        for (int i : run)
        {
            const glm::dvec3 p = anchors[size_t(i)].at;
            pts.push_back(json::array({p.x, p.y, p.z}));
            rb.extend(p);
        }
        spans += int(run.size()) - 1;
        double longest = 0.0;
        for (size_t k = 1; k < run.size(); ++k)
            longest = std::max(longest, glm::length(anchors[size_t(run[k])].at - anchors[size_t(run[k - 1])].at));
        // The four colliders nearest the run.
        std::vector<std::pair<double, NodeId>> near;
        for (NodeId c : colliders)
        {
            const gen::Bounds b = boundsInWorld(g, c);
            const glm::dvec3 d = glm::max(glm::dvec3(0.0), glm::max(b.min - rb.max, rb.min - b.max));
            near.push_back({glm::length(d), c});
        }
        std::ranges::sort(near, [](const auto& x, const auto& y) { return x.first < y.first; });
        json cols = json::array();
        for (size_t k = 0; k < near.size() && k < 4; ++k)
            cols.push_back(g.node(near[k].second)->path());
        json call = {{"points", pts},
                     {"sagMode", "depth"},
                     {"sag", a.get<double>("sag", 40.0)},
                     {"count", a.get<int64_t>("count", 1)},
                     {"spread", a.get<double>("spread", 4.0)},
                     {"sagVariation", a.get<double>("sagVariation", 0.15)},
                     {"seed", a.get<int64_t>("seed", 1) + int64_t(ri)},
                     {"colliders", cols},
                     {"drape", a.get<bool>("drape", true) && !cols.empty()},
                     {"clearance", a.get<double>("radius", style == Style::Toon ? 1.6 : 0.8) + 0.5},
                     {"samples", int64_t(std::clamp(longest / 4.0, 32.0, 1024.0))},
                     {"pipe", true},
                     {"pipeRadius", a.get<double>("radius", style == Style::Toon ? 1.6 : 0.8)},
                     {"pipeSides", style == Style::Toon ? 4 : 6},
                     {"name", safeName(name) + "Cable"}};
        const json made = ops::execute("cable", call);
        for (const json& c : made)
        {
            for (int k : {0, 2, 3})
                if (c[k].is_string())
                    if (Node* n = g.find(c[k].get<std::string>()))
                        ops::execute("parent", {{"args", {n->path(), g.node(groupId)->path()}}});
            cables.push_back({{"curve", g.find(c[0].get<std::string>()) ? json(c[0]) : json()}, {"cable", c[1]}});
        }
    }
    // Curves were reparented: hand back their current paths.
    for (json& c : cables)
        if (c["curve"].is_string())
            if (Node* n = g.find(c["curve"].get<std::string>()))
                c["curve"] = n->path();

    // Fittings.
    const std::string gpath = g.node(groupId)->path();
    int fittings = 0;
    uint32_t counter = 0;
    std::vector<bool> used(anchors.size(), false);
    for (const auto& run : runs)
        for (int i : run)
            used[size_t(i)] = true;
    for (size_t i = 0; i < anchors.size(); ++i)
    {
        if (!used[i])
            continue;
        const wires::Anchor& x = anchors[i];
        const uint32_t id = counter++;
        if (x.kind == wires::Kind::Pole)
        {
            const Piece& ins = insulators[size_t(pickPiece(insulators, unitHash(uint32_t(a.get<int64_t>("seed", 1)), 11, id)))];
            const gen::Fit f = gen::fitPiece(gen::boundsOf(pieceMesh(g, ins)));
            placePiece(g, ins, gen::place(f, x.at - glm::dvec3(0, f.size.y * 0.5, 0), {1, 0, 0}, {0, 1, 0}, {0, 0, 1}), gpath);
            ++fittings;
            if (placePoles && x.host < 0 && !poles.empty())
            {
                const Piece& pole = poles[size_t(pickPiece(poles, unitHash(uint32_t(a.get<int64_t>("seed", 1)), 12, id)))];
                const gen::Fit pf = gen::fitPiece(gen::boundsOf(pieceMesh(g, pole)));
                const double top = x.at.y - f.size.y;
                const double s = std::max(0.05, (top - x.poleBase) / pf.size.y);
                placePiece(g, pole, gen::place(pf, glm::dvec3(x.at.x, x.poleBase + pf.size.y * s * 0.5, x.at.z), {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, glm::dvec3(s)),
                           gpath);
                ++fittings;
            }
        }
        else if (x.kind == wires::Kind::Corner)
        {
            const Piece& br = brackets[size_t(pickPiece(brackets, unitHash(uint32_t(a.get<int64_t>("seed", 1)), 13, id)))];
            const gen::Fit f = gen::fitPiece(gen::boundsOf(pieceMesh(g, br)), false, true);
            const glm::dvec3 z = glm::cross(x.out, glm::dvec3(0, 1, 0));
            placePiece(g, br, gen::place(f, x.at - x.out * (wo.standoff * 0.5), x.out, {0, 1, 0}, z, glm::dvec3(wo.standoff / f.size.x, 1.0, 1.0)), gpath);
            ++fittings;
        }
    }
    g.select({groupId});
    return {{"group", gpath}, {"cables", cables}, {"runs", int64_t(runs.size())}, {"spans", spans}, {"anchors", int64_t(anchors.size())},
            {"fittings", fittings}, {"colliders", int64_t(colliders.size())}, {"style", styleName(style)}};
}

// ------------------------------------------------------------------------------------------------ piles

json runPile(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const Style style = styleArg(a);
    const std::string theme = a.get<std::string>("theme", "rubble");
    std::vector<Piece> items;
    if (a.has("items"))
        items = resolvePieces(g, a.flags["items"], "themedPile");
    else if (theme == "mixed")
    {
        for (auto [r, w] : {std::pair{gen::role::kRubble, 3.0}, std::pair{gen::role::kPlank, 1.0}, std::pair{gen::role::kBrick, 2.0}})
            for (Piece p : piecesOrDefault(g, a, "items", r, style, "themedPile"))
            {
                p.weight *= w;
                items.push_back(p);
            }
    }
    else
    {
        const char* r = theme == "planks" ? gen::role::kPlank : (theme == "bricks" ? gen::role::kBrick : gen::role::kRubble);
        items = piecesOrDefault(g, a, "items", r, style, "themedPile");
    }
    // pileProps picks items evenly, so weights become repeats.
    json names = json::array();
    for (const Piece& p : items)
        for (int k = 0; k < std::clamp(int(std::round(p.weight)), 1, 8); ++k)
            names.push_back(p.name);

    json call = {{"items", names}, {"seed", a.get<int64_t>("seed", 1)}, {"settle", a.get<bool>("settle", true)}, {"realize", a.get<bool>("realize", false)}};
    glm::dvec3 at(0.0);
    double radius = a.get<double>("radius", 60.0);
    if (a.has("at"))
        at = glm::dvec3(a.flags["at"][0].get<double>(), a.flags["at"][1].get<double>(), a.flags["at"][2].get<double>());
    if (!a.positional.empty())
    {
        Node* area = g.find(a.positional[0].get<std::string>());
        if (!area || !area->type().isA("mesh"))
            throw CommandError("themedPile: " + a.positional[0].dump() + " is not a mesh");
        g.evaluate();
        const gen::Bounds b = boundsInWorld(g, area->id());
        if (!a.has("at"))
            at = glm::dvec3(b.center().x, b.max.y, b.center().z);
        if (!a.has("radius"))
            radius = std::max(10.0, std::min(b.size().x, b.size().z) * 0.4);
        call["onto"] = json::array({area->path()});
    }
    const int64_t count = a.has("count") ? a.get<int64_t>("count") : int64_t(std::clamp(radius * radius / 120.0, 6.0, 300.0));
    call["at"] = json::array({at.x, at.y, at.z});
    call["radius"] = radius;
    call["count"] = count;
    const std::string name = safeName(a.get<std::string>("name", "Pile_" + theme));
    call["name"] = name;
    json r = ops::execute("pileProps", call);
    if (!a.get<bool>("realize", false))
    {
        const std::string group = newGroup(name + "_Group");
        const NodeId gid = g.find(group)->id();
        ops::execute("parent", {{"args", {r["host"], group}}});
        r["arrangeGroup"] = r["group"];
        r["group"] = g.node(gid)->path();
        if (Node* h = g.find(r["host"].get<std::string>()); !h)
            for (NodeId c : g.children(gid))
                r["host"] = g.node(c)->path();
    }
    r["theme"] = theme;
    r["style"] = styleName(style);
    r["used"] = namesOf(items);
    return r;
}

// ------------------------------------------------------------------------------------------------ rooftop

json runRooftop(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<NodeId> hosts = hostMeshes(g, a, "rooftopClutter");
    if (hosts.empty())
        throw CommandError("rooftopClutter: give the buildings (meshes, Blockwork blocks or groups), or select them");
    const Style style = styleArg(a);
    const std::vector<Piece> vents = piecesOrDefault(g, a, "vents", gen::role::kVent, style, "rooftopClutter");
    const std::vector<Piece> units = piecesOrDefault(g, a, "acUnits", gen::role::kAcUnit, style, "rooftopClutter");
    const std::vector<Piece> pipes = piecesOrDefault(g, a, "pipes", gen::role::kPipe, style, "rooftopClutter");
    const double railingChance = a.get<double>("railingChance", 0.5), towerChance = a.get<double>("towerChance", 0.35);
    std::vector<Piece> railings, towers;
    if (railingChance > 0.0)
        railings = piecesOrDefault(g, a, "railings", gen::role::kRailing, style, "rooftopClutter");
    if (towerChance > 0.0)
        towers = piecesOrDefault(g, a, "towers", gen::role::kTower, style, "rooftopClutter");
    g.evaluate();

    rooftop::Options o;
    o.slopeMax = a.get<double>("slopeMax", 5.0);
    o.minArea = a.get<double>("minArea", 2.0);
    o.edgeMargin = a.get<double>("edgeMargin", 50.0);
    o.railings = railingChance;
    o.towers = towerChance;
    o.seed = uint32_t(a.get<int64_t>("seed", 1));
    gen::Mesh joined;
    for (NodeId h : hosts)
        joined.append(worldMesh(g, h));
    joined.compact();
    const rooftop::Result r = rooftop::layout(joined, boundsOf(g, railings), weightList(railings), boundsOf(g, towers), weightList(towers), o);
    if (r.tops.empty())
        throw CommandError("rooftopClutter: no flat tops found (upward faces with nothing above, at least minArea m2)");

    const std::string name = a.get<std::string>("name", g.node(hosts[0])->name() + "_Rooftop");
    Live l;
    l.nodes.push_back({{"name", "Rooftop"}, {"type", nodes::kRooftopType}, {"position", json::array({300, 0})},
                       {"values",
                        {{"railingWeights", weightsOf(railings)}, {"towerWeights", weightsOf(towers)}, {"slopeMax", o.slopeMax}, {"minArea", o.minArea},
                         {"edgeMargin", o.edgeMargin}, {"railingChance", o.railings}, {"towerChance", o.towers}}}});
    l.nodes.push_back({{"name", "Scatter"}, {"type", "ScatterProps"}, {"position", json::array({600, 0})},
                       {"values", {{"maxCount", a.get<int64_t>("maxCount", 200)}, {"slopeMax", o.slopeMax + 1.0}, {"mask", rooftop::kFreeAttr},
                                   {"turn", style == Style::Toon ? 0.0 : a.get<double>("turn", 180.0)}}}});
    l.nodes.push_back({{"name", "Join"}, {"type", "JoinGeometry"}, {"position", json::array({900, 0})}});
    l.links.push_back("Rooftop.tops -> Scatter.surface");
    l.links.push_back("Scatter.instances -> Join.geometry");
    l.links.push_back("Rooftop.instances -> Join.geometry");
    for (NodeId h : hosts)
        l.info(g, h, "Rooftop.hosts", true);
    l.pieces(g, railings, "Rooftop.railings");
    l.pieces(g, towers, "Rooftop.towers");
    // Scatter Props picks its items evenly, so weights become repeated links.
    for (const std::vector<Piece>* list : {&vents, &units, &pipes})
        for (const Piece& p : *list)
            for (int k = 0; k < std::clamp(int(std::round(p.weight)), 1, 8); ++k)
                l.info(g, p.mesh, "Scatter.items", false);
    l.expose("seed", "int", int64_t(o.seed), "Another seed gives other edges, corners and clutter.", "Rooftop");
    l.links.push_back("GroupInput.seed -> Scatter.seed");
    l.inputs.push_back({{"name", "density"}, {"type", "float"}, {"default", a.get<double>("density", 0.5)}, {"description", "Clutter per square metre."}});
    l.links.push_back("GroupInput.density -> Scatter.density");
    l.inputs.push_back({{"name", "spacing"}, {"type", "float"}, {"default", a.get<double>("spacing", 1.3)}, {"description", "Footprints kept clear."}});
    l.links.push_back("GroupInput.spacing -> Scatter.spacing");
    json out = finishLive(g, l, "Join.geometry", name, "Vents, AC units, pipes, railings and towers on the flat tops of " + g.node(hosts[0])->name() + ".");
    if (a.get<bool>("realize", false))
        out = realizeLive(g, out, name);
    out.update({{"tops", int64_t(r.tops.size())}, {"railings", r.railings}, {"towers", r.towers}, {"style", styleName(style)}});
    return out;
}

// ------------------------------------------------------------------------------------------------ interiors to Unreal

json runInteriorSend(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::set<std::string> names;
    std::vector<NodeId> meshes = hostMeshes(g, a, "windowInteriorSend");
    g.evaluate();
    for (NodeId m : meshes)
    {
        Node* n = g.node(m);
        const auto mesh = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
        if (mesh)
            for (const std::string& s : mesh->materialNames())
                if (s.rfind("MI_", 0) == 0 && s.size() > 11 && s.ends_with("Interior"))
                    names.insert(s);
    }
    if (names.empty())
        throw CommandError("windowInteriorSend: no interior cards in the given windows (fitWindows interior='card' with interiorImage=)");
    const std::string root = a.get<std::string>("folder", "/Game/Lila/Interiors");
    json textures = json::array(), interiors = json::array();
    for (const std::string& n : names)
    {
        std::string image;
        try
        {
            image = ops::execute("getAttr", {{"args", {n + ".baseColorMap"}}}).get<std::string>();
        }
        catch (const std::exception&)
        {
        }
        if (image.empty())
            throw CommandError("windowInteriorSend: material " + n + " has no base colour image");
        const std::string asset = link::textureAssetName(image, 1001);
        textures.push_back({{"file", image}, {"udim", 1001}, {"map", "baseColor"}});
        interiors.push_back({{"name", n},
                             {"textures", {{"InteriorRoomMap", root + "/Textures/" + asset + "." + asset}}},
                             {"scalars", {{"InteriorDepth", a.get<double>("depth", 0.5)}, {"InteriorTiling", 1.0}}}});
    }
    const json importParams = link::textureImportParams({{"outputs", textures}}, {{"folder", root + "/Textures"}, {"materialMode", "none"}});
    const json request = {{"folder", root + "/Materials"}, {"regenerate", a.get<bool>("regenerateMaster", false)}, {"interiors", interiors}};
    if (a.get<bool>("dryRun", false))
        return {{"request", request}, {"textures", importParams}};
    try
    {
        link::Link::get().ensureConnected();
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string("windowInteriorSend: not connected to Unreal: ") + e.what());
    }
    const json features = link::Link::get().status().value("features", json::array());
    if (!features.is_array() || std::ranges::find(features, json("weatheringInteriors")) == features.end())
        throw CommandError("windowInteriorSend: the LilaLink plugin in this editor is too old for interiors; rebuild it from Lila's ue/LilaLink");
    link::Link::get().waitIdle();
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    if (!client)
        throw CommandError("windowInteriorSend: not connected to Unreal");
    json errors = json::array();
    link::RpcResult t = client->call("importTextures", importParams, link::importCallOptions());
    if (!t.ok)
        errors.push_back("textures: " + t.error);
    link::RpcResult d = client->call("weatheringInteriors", request, link::importCallOptions());
    if (!d.ok)
        errors.push_back("interiors: " + d.error);
    return {{"textures", t.ok ? t.result : json()}, {"interiors", d.ok ? d.result : json()}, {"errors", errors}};
}

// ------------------------------------------------------------------------------------------------ registration

FlagDef realizeFlag() { return {"realize", "rz", T::Bool, false, "Scene objects instead of a live result (the instances turned into objects)."}; }
FlagDef seedFlag() { return {"seed", "s", T::Int, int64_t(1), "Another seed gives another result; the same seed always the same."}; }
FlagDef nameFlag(const char* what) { return {"name", "n", T::String, nullptr, std::string("Name of the group made (default ") + what + ")."}; }

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "roofing",
        .doc = "Cover roofs with sheets, planks or tiles: laid in rows from the eave up, each row overlapping the one below, ridge "
               "caps along ridges, then aged with missing pieces, lifted corners and a sagging middle.\n\n"
               "Covers the selected faces, or every face of the hosts within maxSlope of up (a Blockwork block's top, a house's "
               "roof). Pieces are yours (pieces=, caps=: meshes, groups, sets, library ids, tag:<word>, weighted), else the "
               "placeholders for kind; each is fitted by its bounds (Y out of the roof, its longer side down the slope), so a "
               "chunky toon tile lays the same way as a scanned one. neatness runs from a messy roof (0) to neat stylized rows "
               "(1, the toon default). Live: a mesh whose geometry-nodes group follows the hosts, so extending or carving a block "
               "re-lays it; realize=True makes scene objects. Returns group, mesh, modifier, gnGroup, count, pieces, caps, rows, "
               "regions, missing, lifted, used.",
        .positionalDoc = "roofs: meshes, Blockwork blocks or groups (default the selection)",
        .flags = {{"kind", "k", T::String, "sheets", "Placeholder pieces when pieces= is not given: sheets (corrugated), planks or tiles.", {"sheets", "planks", "tiles"}},
                  piecesFlag("pieces", "pc", "The cover pieces: meshes, groups, sets, library ids, tag:<word>, or {piece, weight}."),
                  piecesFlag("caps", "cp", "Ridge caps, same forms as pieces."),
                  {"faces", "f", T::List, nullptr, "Face indices of the one host to cover (default the face selection, else every upward face)."},
                  styleFlag(),
                  {"neatness", "nt", T::Float, nullptr, "0 a messy roof, 1 neat regular rows (default 0.3, toon 1).", {}, {}, 0.0, 1.0},
                  {"damage", "dm", T::Float, 0.3, "0 to 1: how many pieces are missing or lift, and how far.", {}, {}, 0.0, 1.0},
                  {"missing", "mi", T::Float, nullptr, "Share of pieces left out (default damage x 0.25).", {}, {}, 0.0, 1.0},
                  {"lifted", "li", T::Float, nullptr, "Share of pieces with a lifted corner (default damage x 0.35).", {}, {}, 0.0, 1.0},
                  {"liftAngle", "la", T::Float, 0.0, "How far corners lift, degrees; 0 takes it from damage.", {}, {}, 0.0, 90.0},
                  {"sag", "sg", T::Float, 0.0, "How far the middle of each roof face dips below its edges, cm.", {}, {}, 0.0},
                  {"overlap", "ov", T::Float, 0.15, "Share of a piece's length the row above covers.", {}, {}, 0.0, 0.9},
                  {"sideOverlap", "so", T::Float, 0.08, "Share of a piece's width its neighbour covers.", {}, {}, 0.0, 0.9},
                  {"overhang", "oh", T::Float, 5.0, "How far the first row reaches past the eave, cm.", {}, {}, 0.0},
                  {"stagger", "stg", T::Float, nullptr, "Neat rows: how far alternate rows shift, of a width (default 0.5 for tiles, else 0).", {}, {}, 0.0, 1.0},
                  {"scale", "sc", T::Float, 1.0, "Scale of every piece.", {}, {}, 0.01},
                  {"ridge", "rg", T::Bool, true, "Caps along ridges."},
                  {"maxSlope", "ms", T::Float, 75.0, "Faces picked automatically are at most this steep, degrees.", {}, {}, 0.0, 90.0},
                  seedFlag(), realizeFlag(), nameFlag("<host>_Roofing")},
        .run = runRoofing,
        .examples = {"roofing('Shed', kind='sheets', damage=0.5, sag=4)", "roofing('House', pieces=['MyTile', {'piece': 'MyTileBroken', 'weight': 0.2}], neatness=0.8)",
                     "roofing('Hut', kind='planks', style='toon')"},
    });

    ctx.registerCommand({
        .name = "fitWindows",
        .doc = "Fill the openings in walls with windows: a frame along each side, a sill, glass (intact, cracked, broken, boarded or "
               "mixed) and an interior behind (a generated room, a card with your image, or your own mesh).\n\n"
               "Openings are found in the walls themselves: a Blockwork carve, a Masonry opening, any hole through a wall; arches "
               "get a frame along their outline. Openings reaching the ground are doors, left alone unless doors=True. Pieces are "
               "yours (frames=, sills=, panes=, cracked=, broken=, boards=, interiors=) else placeholders, fitted by their bounds: "
               "frame members along X, sills along X out of the wall along Z, panes and boards X across. Live: carving another "
               "hole adds a window. windowInteriorSend sends card interiors to Unreal as MF_LilaInterior instances. Returns group, "
               "mesh, modifier, gnGroup, count, windows, openings (center, width, height, depth, normal, door), doorsSkipped, panes, "
               "cracked, broken, boarded, cardMaterial, used.",
        .positionalDoc = "walls: meshes, Blockwork blocks, Masonry walls or groups (default the selection)",
        .flags = {{"glass", "gl", T::String, "intact", "intact, cracked, broken, boarded, none or mixed (by damage).", {"intact", "cracked", "broken", "boarded", "none", "mixed"}},
                  {"interior", "in", T::String, "room", "room (generated), card (interiorImage), mesh (interiors) or none.", {"room", "card", "mesh", "none"}},
                  {"interiorImage", "ii", T::String, nullptr, "Image of a room for interior='card' (parallax interior in Unreal)."},
                  piecesFlag("interiors", "ins", "Interior meshes for interior='mesh'."),
                  piecesFlag("frames", "fr", "Frame members, along X."),
                  piecesFlag("sills", "sl", "Sills, along X."),
                  piecesFlag("panes", "pn", "Intact panes."),
                  piecesFlag("cracked", "cr", "Cracked panes."),
                  piecesFlag("broken", "br", "Broken panes."),
                  piecesFlag("boards", "bd", "Boards nailed across."),
                  styleFlag(),
                  {"columns", "co", T::Int, int64_t(1), "Panes across.", {}, {}, 1.0, 16.0},
                  {"rows", "ro", T::Int, int64_t(1), "Panes up.", {}, {}, 1.0, 16.0},
                  {"sill", "si", T::Bool, true, "A sill under each window."},
                  {"sillProject", "sp", T::Float, 4.0, "How far the sill stands out of the wall, cm."},
                  {"frameInset", "fi", T::Float, 4.0, "How far in from the outer face the frame sits, cm.", {}, {}, 0.0},
                  {"frameScale", "fs", T::Float, 1.0, "Scale of frame and sill sections.", {}, {}, 0.01},
                  {"damage", "dm", T::Float, 0.3, "0 to 1: mixed glass breaks more, boards go missing.", {}, {}, 0.0, 1.0},
                  {"neatness", "nt", T::Float, nullptr, "1 lays boards straight and even (default 0.3, toon 1).", {}, {}, 0.0, 1.0},
                  {"boardOverhang", "bo", T::Float, 8.0, "How far boards reach past the opening each side, cm.", {}, {}, 0.0},
                  {"interiorDepth", "id", T::Float, 300.0, "Depth of the room behind the wall, cm.", {}, {}, 10.0},
                  {"doors", "do", T::Bool, false, "Fill openings that reach the ground too."},
                  {"flip", "fl", T::Bool, false, "Outside is the other side of the walls."},
                  {"minSize", "mn", T::Float, 15.0, "Smaller holes are not openings, cm.", {}, {}, 1.0},
                  {"maxThickness", "mt", T::Float, 150.0, "Faces further apart than this are different walls, cm.", {}, {}, 1.0},
                  seedFlag(), realizeFlag(), nameFlag("<wall>_Windows")},
        .run = runWindows,
        .examples = {"fitWindows('Facade', glass='mixed', damage=0.6)", "fitWindows('Shop', interior='card', interiorImage='D:/refs/shop_room.png', columns=2)",
                     "fitWindows('Tower', glass='boarded', style='toon')"},
    });

    ctx.registerCommand({
        .name = "wires",
        .doc = "String wires between things: power and phone lines between poles, along building fronts on wall brackets, or along "
               "a street curve with poles placed under them. Each run is a cable (catenary by sag depth, bundles by count, draped "
               "over the buildings it would pass through) piped to a mesh, with insulators on pole tops and brackets at building "
               "corners.\n\n"
               "Objects are read by their bounds: tall thin ones are poles, small ones wall brackets, others buildings (wired at the "
               "two corners facing the run, standing off by the bracket's length). Curves get poles every spacing. points= strings "
               "given points in order. Anchors are chained along the run, spans no longer than maxSpan. Moving a cable's anchor "
               "curve or a building re-hangs it (cable's own live update). Returns group, cables, runs, spans, anchors, fittings, "
               "colliders.",
        .positionalDoc = "poles, buildings, brackets or curves (default the selection)",
        .flags = {{"points", "p", T::List, nullptr, "[[x, y, z], ...]: anchors strung in order."},
                  piecesFlag("insulators", "is", "Insulators on pole tops."),
                  piecesFlag("brackets", "bk", "Wall brackets, arm along X out of the wall."),
                  piecesFlag("poles", "po", "Poles placed along curves."),
                  styleFlag(),
                  {"sag", "sg", T::Float, 40.0, "How far each span dips below its chord, cm.", {}, {}, 0.0},
                  {"count", "c", T::Int, int64_t(1), "Wires per span (a bundle above 1).", {}, {}, 1.0, 32.0},
                  {"spread", "sd", T::Float, 4.0, "Bundle: how far wires stray at the anchors, cm.", {}, {}, 0.0},
                  {"sagVariation", "sv", T::Float, 0.15, "Bundle: how much each wire's sag varies.", {}, {}, 0.0, 1.0},
                  {"radius", "r", T::Float, nullptr, "Wire radius, cm (default 0.8, toon 1.6).", {}, {}, 0.01},
                  {"drape", "dr", T::Bool, true, "Wires settle onto buildings they would pass through."},
                  nodeFlag({"colliders", "col", T::List, nullptr, "Meshes wires stay out of (default the buildings given); four nearest per run."}, "mesh"),
                  {"maxSpan", "mx", T::Float, 3000.0, "Longest span, cm; further anchors start a new run.", {}, {}, 1.0},
                  {"cornerHeight", "ch", T::Float, 0.75, "Height of corner brackets, of the building's height.", {}, {}, 0.0, 1.0},
                  {"standoff", "st", T::Float, nullptr, "How far corner anchors stand off the wall, cm (default the bracket's length).", {}, {}, 0.0},
                  {"spacing", "spc", T::Float, 2500.0, "Along curves: distance between poles, cm.", {}, {}, 100.0},
                  {"height", "h", T::Float, 700.0, "Along curves: wire height above the curve, cm.", {}, {}, 0.0},
                  {"placePoles", "pp", T::Bool, true, "Along curves: place poles under the anchors."},
                  seedFlag(), nameFlag("Wires")},
        .run = runWires,
        .examples = {"wires(['Pole1', 'Pole2', 'Pole3'], sag=60)", "wires('StreetEdge', spacing=2000, height=750, count=3)",
                     "wires(['ShopA', 'ShopB', 'ShopC'], cornerHeight=0.8, style='toon')"},
    });

    ctx.registerCommand({
        .name = "themedPile",
        .doc = "Heap a themed pile (rubble, planks, bricks or mixed) on a point or an area, through Stagehand's pileProps, so it "
               "settles with physics and Push, keep at rest and the Stagehand send work on it.\n\n"
               "items= takes your own pieces (weighted), else the placeholders for the theme in the style. Given an area mesh, the "
               "pile sits on its top and lands on it. Returns pileProps' result with group (the scene group), theme and used.",
        .positionalDoc = "area mesh the pile sits on (optional)",
        .flags = {{"theme", "th", T::String, "rubble", "rubble, planks, bricks or mixed.", {"rubble", "planks", "bricks", "mixed"}},
                  piecesFlag("items", "it", "Your pieces: meshes, groups, sets, library ids, tag:<word>, or {piece, weight}."),
                  styleFlag(),
                  {"at", "", T::List, nullptr, "Centre of the pile's footprint (default the area's top, else the origin)."},
                  {"radius", "r", T::Float, nullptr, "Footprint radius, cm (default 60, or from the area).", {}, {}, 1.0},
                  {"count", "c", T::Int, nullptr, "How many pieces (default from the radius).", {}, {}, 1.0, 2000.0},
                  {"settle", "st", T::Bool, true, "Let them come to rest with physics."},
                  seedFlag(), realizeFlag(), nameFlag("Pile_<theme>")},
        .run = runPile,
        .examples = {"themedPile(theme='bricks', at=[200, 0, 50], radius=80)", "themedPile('Courtyard', theme='mixed', style='toon')"},
    });

    ctx.registerCommand({
        .name = "rooftopClutter",
        .doc = "Dress flat roof tops: vents, AC units and pipes scattered (a Stagehand Scatter Props arrangement), railings along "
               "some roof edges and a lattice tower on some roofs.\n\n"
               "Only flat tops nothing stands over are used, kept clear of their edges and of towers. Pieces are yours (vents=, "
               "acUnits=, pipes=, railings=, towers=) else placeholders; railings are fitted along each edge, towers stand at a "
               "corner. Live: the arrangement follows the buildings. Returns group, mesh, modifier, gnGroup, count, tops, railings, "
               "towers.",
        .positionalDoc = "buildings: meshes, Blockwork blocks or groups (default the selection)",
        .flags = {piecesFlag("vents", "ve", "Vents."),
                  piecesFlag("acUnits", "ac", "AC units."),
                  piecesFlag("pipes", "pi", "Pipes lying on the roof."),
                  piecesFlag("railings", "ra", "Railing segments, along X."),
                  piecesFlag("towers", "to", "Towers (masts, water towers, antennas)."),
                  styleFlag(),
                  {"density", "de", T::Float, 0.5, "Clutter per square metre.", {}, {}, 0.0},
                  {"spacing", "spc", T::Float, 1.3, "Footprints kept clear, times each piece's own.", {}, {}, 0.0},
                  {"maxCount", "mc", T::Int, int64_t(200), "At most this many scattered.", {}, {}, 0.0},
                  {"railingChance", "rc", T::Float, 0.5, "Share of roof edges with a railing.", {}, {}, 0.0, 1.0},
                  {"towerChance", "tc", T::Float, 0.35, "Chance a roof gets a tower.", {}, {}, 0.0, 1.0},
                  {"edgeMargin", "em", T::Float, 50.0, "Kept clear inside each roof's edge, cm.", {}, {}, 0.0},
                  {"turn", "tu", T::Float, 180.0, "Random turn of scattered pieces, degrees either way (toon: square to the roof).", {}, {}, 0.0, 180.0},
                  {"slopeMax", "sl", T::Float, 5.0, "Steepest face that counts as flat, degrees.", {}, {}, 0.0, 89.0},
                  {"minArea", "ma", T::Float, 2.0, "Smaller flat tops are left alone, square metres.", {}, {}, 0.0},
                  seedFlag(), realizeFlag(), nameFlag("<host>_Rooftop")},
        .run = runRooftop,
        .examples = {"rooftopClutter('Block_12', density=0.8)", "rooftopClutter(['Tower', 'Annex'], vents=['MyVent'], towerChance=1, style='toon')"},
    });

    ctx.registerCommand({
        .name = "curveToTruss",
        .doc = "Build a lattice truss along curves (a mast, pylon, crane jib, girder): a mesh with a geometry-nodes group around the "
               "Curve to Truss node, live as the curves change. Returns group, mesh, modifier, gnGroup.",
        .positionalDoc = "curves (default the selection)",
        .flags = {{"chords", "ch", T::Int, int64_t(4), "Chords round the section.", {}, {}, 3.0, 12.0},
                  {"width", "w", T::Float, 60.0, "Side of the section at the start, cm.", {}, {}, 0.1},
                  {"endWidth", "ew", T::Float, -1.0, "Side at the end, cm; negative keeps width."},
                  {"member", "m", T::Float, 4.0, "Side of each member, cm.", {}, {}, 0.05},
                  {"segment", "sg", T::Float, 80.0, "Distance between levels, cm.", {}, {}, 1.0},
                  {"bracing", "b", T::String, "x", "Diagonals: x (both), zigzag or none.", {"x", "zigzag", "none"}},
                  {"unite", "u", T::Bool, true, "One closed mesh; off leaves overlapping beams (faster)."},
                  nameFlag("<curve>_Truss")},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> curves;
            hostMeshes(g, a, "curveToTruss", true, &curves);
            if (curves.empty())
                throw CommandError("curveToTruss: give curves");
            Live l;
            l.nodes.push_back({{"name", "Truss"}, {"type", nodes::kTrussType}, {"position", json::array({300, 0})},
                               {"props", {{"bracing", a.get<std::string>("bracing", "x")}}},
                               {"values", {{"chords", a.get<int64_t>("chords", 4)}, {"width", a.get<double>("width", 60.0)}, {"endWidth", a.get<double>("endWidth", -1.0)},
                                           {"member", a.get<double>("member", 4.0)}, {"segment", a.get<double>("segment", 80.0)}, {"unite", a.get<bool>("unite", true)}}}});
            l.nodes.push_back({{"name", "Join"}, {"type", "JoinGeometry"}, {"position", json::array({150, 0})}});
            l.links.push_back("Join.geometry -> Truss.curve");
            for (NodeId c : curves)
                l.info(g, c, "Join.geometry", true);
            return finishLive(g, l, "Truss.geometry", a.get<std::string>("name", g.node(curves[0])->name() + "_Truss"), "A lattice truss along curves.");
        },
        .examples = {"curveToTruss('MastLine', chords=3, width=90, endWidth=30, member=5)"},
    });

    ctx.registerCommand({
        .name = "windowInteriorSend",
        .doc = "Send the interior cards of windows to Unreal: each card's room image goes to <folder>/Textures, MF_LilaInterior "
               "and the master M_LilaInterior on it are generated (or rebuilt with regenerateMaster), and each card material "
               "gets its instance MI_<image>Interior, matching the card's material slot so sent window meshes pick it up. "
               "dryRun=True returns the plan without connecting.",
        .positionalDoc = "window meshes made by fitWindows (default the selection)",
        .flags = {{"folder", "f", T::String, "/Game/Lila/Interiors", "Content folder for the textures and instances."},
                  {"depth", "d", T::Float, 0.5, "Room depth as a share of its width, for the parallax.", {}, {}, 0.05, 4.0},
                  {"regenerateMaster", "", T::Bool, false, "Rebuild M_LilaInterior's graph from this version of Lila."},
                  {"dryRun", "", T::Bool, false, "Return the plan without connecting to Unreal."}},
        .undoable = false,
        .run = runInteriorSend,
        .examples = {"windowInteriorSend('Shop_Windows', dryRun=True)"},
    });
}

void registerOptionBoxes()
{
    namespace opt = ops::options;
    const std::vector<std::pair<std::string, std::string>> styles = {{"realistic", "Realistic"}, {"toon", "Toon"}};
    {
        opt::Override o;
        o.order = {"kind", "style", "neatness", "damage", "sag", "missing", "lifted", "liftAngle", "overlap", "sideOverlap", "overhang", "stagger", "scale", "ridge", "pieces", "caps"};
        o.hidden = {"faces", "name"};
        o.shown = {"pieces", "caps"};
        o.sections = {{"missing", "Damage"}, {"lifted", "Damage"}, {"liftAngle", "Damage"}, {"overlap", "Layout"}, {"sideOverlap", "Layout"},
                      {"overhang", "Layout"}, {"stagger", "Layout"}, {"scale", "Layout"}, {"ridge", "Layout"}, {"maxSlope", "Layout"},
                      {"pieces", "Pieces"}, {"caps", "Pieces"}, {"seed", "Advanced"}, {"realize", "Advanced"}};
        o.choices["kind"] = {{"sheets", "Sheets"}, {"planks", "Planks"}, {"tiles", "Tiles"}};
        o.editors["kind"] = "segmented";
        o.editors["style"] = "segmented";
        o.choices["style"] = styles;
        o.labels = {{"sag", "Sag"}, {"liftAngle", "Lift Angle"}, {"caps", "Cap Pieces"}, {"ridge", "Ridge Caps"}, {"realize", "Make Objects"}};
        o.ranges["sag"] = {0.0, 100.0, 0.5, " cm"};
        o.ranges["overhang"] = {0.0, 100.0, 0.5, " cm"};
        o.ranges["liftAngle"] = {0.0, 90.0, 1.0, "°"};
        o.ranges["maxSlope"] = {0.0, 90.0, 1.0, "°"};
        opt::setOverride("roofing", std::move(o));
    }
    {
        opt::Override o;
        o.order = {"glass", "interior", "interiorImage", "style", "damage", "neatness", "columns", "rows", "sill", "doors"};
        o.hidden = {"name"};
        o.shown = {"frames", "sills", "panes", "cracked", "broken", "boards", "interiors"};
        o.sections = {{"sillProject", "Frame"}, {"frameInset", "Frame"}, {"frameScale", "Frame"}, {"boardOverhang", "Frame"},
                      {"interiorDepth", "Interior"}, {"frames", "Pieces"}, {"sills", "Pieces"}, {"panes", "Pieces"}, {"cracked", "Pieces"},
                      {"broken", "Pieces"}, {"boards", "Pieces"}, {"interiors", "Pieces"}, {"flip", "Advanced"}, {"minSize", "Advanced"},
                      {"maxThickness", "Advanced"}, {"seed", "Advanced"}, {"realize", "Advanced"}};
        o.choices["glass"] = {{"intact", "Intact"}, {"cracked", "Cracked"}, {"broken", "Broken"}, {"boarded", "Boarded"}, {"none", "None"}, {"mixed", "Mixed"}};
        o.choices["interior"] = {{"room", "Room"}, {"card", "Card"}, {"mesh", "Mesh"}, {"none", "None"}};
        o.choices["style"] = styles;
        o.editors["interior"] = "segmented";
        o.editors["style"] = "segmented";
        o.enabledWhen["interiorImage"] = {"interior", {"card"}};
        o.enabledWhen["interiors"] = {"interior", {"mesh"}};
        o.enabledWhen["interiorDepth"] = {"interior", {"room", "mesh"}};
        o.labels = {{"interiorImage", "Room Image"}, {"realize", "Make Objects"}, {"interiors", "Interior Meshes"}};
        o.ranges["sillProject"] = {-20.0, 50.0, 0.5, " cm"};
        o.ranges["frameInset"] = {0.0, 100.0, 0.5, " cm"};
        o.ranges["interiorDepth"] = {10.0, 2000.0, 10.0, " cm"};
        o.ranges["boardOverhang"] = {0.0, 60.0, 0.5, " cm"};
        opt::setOverride("fitWindows", std::move(o));
    }
    {
        opt::Override o;
        o.order = {"style", "sag", "count", "spread", "sagVariation", "radius", "drape", "maxSpan", "cornerHeight", "standoff", "spacing", "height", "placePoles"};
        o.hidden = {"points", "name", "colliders"};
        o.shown = {"insulators", "brackets", "poles"};
        o.sections = {{"count", "Bundle"}, {"spread", "Bundle"}, {"sagVariation", "Bundle"}, {"cornerHeight", "Anchors"}, {"standoff", "Anchors"},
                      {"maxSpan", "Anchors"}, {"spacing", "Along Curve"}, {"height", "Along Curve"}, {"placePoles", "Along Curve"},
                      {"insulators", "Pieces"}, {"brackets", "Pieces"}, {"poles", "Pieces"}, {"seed", "Advanced"}};
        o.choices["style"] = styles;
        o.editors["style"] = "segmented";
        o.labels = {{"count", "Wires per Span"}, {"sag", "Sag Depth"}, {"placePoles", "Place Poles"}};
        o.ranges["sag"] = {0.0, 1000.0, 1.0, " cm"};
        o.ranges["spread"] = {0.0, 100.0, 0.5, " cm"};
        o.ranges["radius"] = {0.01, 20.0, 0.1, " cm"};
        o.ranges["maxSpan"] = {100.0, 20000.0, 50.0, " cm"};
        o.ranges["standoff"] = {0.0, 200.0, 1.0, " cm"};
        o.ranges["spacing"] = {100.0, 10000.0, 50.0, " cm"};
        o.ranges["height"] = {0.0, 3000.0, 10.0, " cm"};
        opt::setOverride("wires", std::move(o));
    }
    {
        opt::Override o;
        o.order = {"theme", "style", "radius", "count", "settle", "items"};
        o.hidden = {"name", "at"};
        o.shown = {"items"};
        o.sections = {{"seed", "Advanced"}, {"realize", "Advanced"}};
        o.choices["theme"] = {{"rubble", "Rubble"}, {"planks", "Planks"}, {"bricks", "Bricks"}, {"mixed", "Mixed"}};
        o.editors["theme"] = "segmented";
        o.editors["style"] = "segmented";
        o.choices["style"] = styles;
        o.ranges["radius"] = {1.0, 1000.0, 1.0, " cm"};
        o.labels = {{"realize", "Make Objects"}};
        opt::setOverride("themedPile", std::move(o));
    }
    {
        opt::Override o;
        o.order = {"style", "density", "spacing", "railingChance", "towerChance", "edgeMargin", "turn"};
        o.hidden = {"name"};
        o.shown = {"vents", "acUnits", "pipes", "railings", "towers"};
        o.sections = {{"vents", "Pieces"}, {"acUnits", "Pieces"}, {"pipes", "Pieces"}, {"railings", "Pieces"}, {"towers", "Pieces"},
                      {"maxCount", "Advanced"}, {"slopeMax", "Advanced"}, {"minArea", "Advanced"}, {"seed", "Advanced"}, {"realize", "Advanced"}};
        o.choices["style"] = styles;
        o.editors["style"] = "segmented";
        o.labels = {{"acUnits", "AC Units"}, {"railingChance", "Railings"}, {"towerChance", "Towers"}, {"realize", "Make Objects"}};
        o.ranges["edgeMargin"] = {0.0, 500.0, 1.0, " cm"};
        o.ranges["turn"] = {0.0, 180.0, 1.0, "°"};
        opt::setOverride("rooftopClutter", std::move(o));
    }
    {
        opt::Override o;
        o.hidden = {"name"};
        o.choices["bracing"] = {{"x", "X"}, {"zigzag", "Zigzag"}, {"none", "None"}};
        o.editors["bracing"] = "segmented";
        o.ranges["width"] = {0.1, 2000.0, 1.0, " cm"};
        o.ranges["member"] = {0.05, 100.0, 0.5, " cm"};
        o.ranges["segment"] = {1.0, 2000.0, 5.0, " cm"};
        opt::setOverride("curveToTruss", std::move(o));
    }
}

void registerUi(ext::Context& ctx)
{
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* icon;
        const char* doc;
        const char* python;
        std::vector<std::string> keywords;
    } entries[] = {
        {"weathering.roofing", "Roofing", "roofing", "home", "Cover the selected roofs or faces with sheets, planks or tiles, aged.", "roofing('Shed', damage=0.5)",
         {"messy roof", "roof generator", "shingles", "tiles", "corrugated", "roofing"}},
        {"weathering.windows", "Windows", "fitWindows", "window", "Fit frames, glass and interiors into the openings of the selected walls.",
         "fitWindows('Facade', glass='mixed')", {"window generator", "glazing", "boarded windows", "broken glass", "fit windows"}},
        {"weathering.wires", "Wires", "wires", "line-dashed", "String wires between the selected poles, buildings or along curves.", "wires(['Pole1', 'Pole2'])",
         {"power lines", "cables", "wire generator", "telephone lines", "utility poles"}},
        {"weathering.pile", "Pile", "themedPile", "stack-2", "Heap a rubble, plank or brick pile on the selected area.", "themedPile(theme='rubble')",
         {"rubble pile", "junk pile", "heap", "debris pile"}},
        {"weathering.rooftop", "Rooftop Clutter", "rooftopClutter", "building", "Vents, AC units, pipes, railings and towers on the selected roofs.",
         "rooftopClutter('Block_12')", {"roof clutter", "hvac", "vents", "antenna", "rooftop props"}},
        {"weathering.truss", "Curve to Truss", "curveToTruss", "building-broadcast-tower", "A lattice truss along the selected curves.",
         "curveToTruss('MastLine')", {"lattice tower", "truss", "pylon", "mast", "girder"}},
        {"weathering.interiorSend", "Send Interiors", "windowInteriorSend", "send", "Send window interior cards to Unreal as MF_LilaInterior instances.",
         "windowInteriorSend('Shop_Windows')", {"interior mapping", "fake interior", "parallax interior"}},
    };
    // Menu placement is WeatheringUi.cpp's, so the whole Weathering menu reads in one order.
    for (const auto& e : entries)
        actions::add(ctx, {.id = e.id, .label = e.label, .category = "Weathering", .doc = e.doc, .python = e.python, .command = e.command,
                           .call = json::object(), .icon = e.icon, .keywords = e.keywords});
}
} // namespace generators

bool registerGenerators(ext::Context& ctx)
{
    if (!nodes::registerGeneratorNodes(ctx))
        return false;
    gen::addGeneratorPlaceholders();
    generators::registerCommands(ctx);
    generators::registerOptionBoxes();
    generators::registerUi(ctx);
    return true;
}
} // namespace weathering
