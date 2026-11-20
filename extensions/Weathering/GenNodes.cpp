// The generators' geometry nodes: Roofing, Windows and Rooftop lay pieces linked into them out over hosts linked in
// (world space), as instances; Curve to Truss builds a lattice truss along curves. The commands wire them into live
// groups with Object Info nodes, so editing a host or a piece rebuilds the result.

#include "GenNodes.h"

#include "RoofLayout.h"
#include "RooftopLayout.h"
#include "Truss.h"
#include "WindowLayout.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Log.h>
#include <lila/Profile.h>

namespace weathering::nodes
{
using lila::gn::ExecContext;
using lila::gn::Geometry;
using lila::gn::GeometryPtr;
using Socket = lila::gn::SocketType;

namespace
{
lila::gn::SocketDecl in(std::string id, Socket t, lila::gn::json def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    lila::gn::SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    s.max = hi;
    return s;
}

lila::gn::SocketDecl geo(std::string id, std::string doc, bool multi = false)
{
    return {.id = std::move(id), .type = Socket::Geometry, .multi = multi, .doc = std::move(doc)};
}

/** The pieces linked into a socket: each realized to one mesh, as a reference, with its bounds. */
struct Linked
{
    std::vector<GeometryPtr> refs;
    std::vector<Bounds> bounds;
};

Linked linked(ExecContext& c, std::string_view id)
{
    Linked out;
    for (const GeometryPtr& g : c.geometries(id))
    {
        if (!g)
            continue;
        Mesh m = lila::gn::toOutputMesh(*g);
        if (m.numFaces() == 0)
            continue;
        out.bounds.push_back(gen::boundsOf(m));
        out.refs.push_back(lila::gn::meshGeometry(std::move(m)));
    }
    return out;
}

/** Every geometry linked into a socket as one world-space mesh. */
Mesh joined(ExecContext& c, std::string_view id)
{
    Mesh out;
    for (const GeometryPtr& g : c.geometries(id))
        if (g)
            out.append(lila::gn::toOutputMesh(*g));
    out.compact();
    return out;
}

struct Builder
{
    std::shared_ptr<lila::gn::Instances> inst = std::make_shared<lila::gn::Instances>();
    std::vector<int> offset;

    int addSlot(const Linked& l)
    {
        offset.push_back(int(inst->references.size()));
        for (const GeometryPtr& r : l.refs)
            inst->references.push_back(r);
        return int(offset.size()) - 1;
    }
    int addRef(GeometryPtr g)
    {
        offset.push_back(int(inst->references.size()));
        inst->references.push_back(std::move(g));
        return int(offset.size()) - 1;
    }
    void add(const glm::dmat4& xf, int slot, int piece) { inst->add(xf, offset[size_t(slot)] + piece); }
    GeometryPtr geometry()
    {
        Geometry g;
        g.instances = inst;
        return lila::gn::makeGeometry(std::move(g));
    }
};

void roofingExec(ExecContext& c)
{
    LILA_ZONE();

    const Mesh host = joined(c, "host");
    const Linked cover = linked(c, "pieces");
    const Linked caps = linked(c, "caps");
    if (cover.refs.empty())
        throw lila::gn::ExecError(c.node.name + ": link roof pieces (sheets, planks or tiles) into pieces");
    roof::Input in;
    in.host = &host;
    for (double f : gen::parseNumbers(c.s("faces")))
        if (f >= 0.0)
            in.faces.push_back(Index(f));
    in.cover = cover.bounds;
    in.coverWeights = gen::parseNumbers(c.s("weights"));
    in.caps = caps.bounds;
    in.capWeights = gen::parseNumbers(c.s("capWeights"));
    roof::Options o;
    o.overlap = c.f("overlap");
    o.sideOverlap = c.f("sideOverlap");
    o.overhang = c.f("overhang");
    o.scale = std::max(0.01, c.f("scale"));
    o.damage = c.f("damage");
    if (c.f("missing") >= 0.0)
        o.missing = c.f("missing");
    if (c.f("lifted") >= 0.0)
        o.lifted = c.f("lifted");
    o.liftAngle = c.f("liftAngle");
    o.sag = c.f("sag");
    o.neatness = c.f("neatness");
    o.stagger = c.f("stagger");
    o.ridge = c.b("ridge");
    o.maxSlope = c.f("maxSlope");
    o.seed = uint32_t(c.i("seed"));
    const roof::Result r = roof::layout(in, o);
    Builder b;
    b.addSlot(cover);
    b.addSlot(caps);
    for (const roof::Placed& p : r.placed)
        b.add(p.xf, p.slot, p.piece);
    auto& attrs = b.inst->attrs;
    auto slot = attrs.getOrAdd<int32_t>("slot");
    auto row = attrs.getOrAdd<int32_t>("row");
    auto lifted = attrs.getOrAdd<int32_t>("lifted");
    auto random = attrs.getOrAdd<float>("random");
    for (size_t k = 0; k < r.placed.size(); ++k)
    {
        slot[k] = r.placed[k].slot;
        row[k] = r.placed[k].row;
        lifted[k] = r.placed[k].lifted ? 1 : 0;
        random[k] = r.placed[k].random;
    }
    c.out("geometry", b.geometry());
}

void windowsExec(ExecContext& c)
{
    LILA_ZONE();

    const Mesh wall = joined(c, "walls");
    window::FindOptions fo;
    fo.minSize = c.f("minSize");
    fo.maxThickness = c.f("maxThickness");
    fo.flip = c.b("flip");
    const std::vector<window::Opening> openings = window::findOpenings(wall, gen::boundsOf(wall).center(), fo);

    const char* sockets[window::SlotCount] = {"frames", "sills", "panes", "cracked", "broken", "boards", "interiors", nullptr, nullptr};
    const char* weights[window::SlotCount] = {"frameWeights", "sillWeights", "paneWeights", "crackedWeights", "brokenWeights", "boardWeights", "interiorWeights", nullptr, nullptr};
    Linked slots[window::SlotCount];
    window::Pieces pieces;
    for (int s = 0; s < window::RoomMesh; ++s)
    {
        slots[s] = linked(c, sockets[s]);
        pieces.slot[s] = slots[s].bounds;
        pieces.weights[s] = gen::parseNumbers(c.s(weights[s]));
    }
    window::Options o;
    o.frameInset = c.f("frameInset");
    o.frameScale = c.f("frameScale");
    o.columns = int(c.i("columns"));
    o.rows = int(c.i("rows"));
    o.sill = c.b("sill");
    o.sillProject = c.f("sillProject");
    if (!window::parseGlass(c.s("glass"), &o.glass))
        throw lila::gn::ExecError(c.node.name + ": glass is intact, cracked, broken, boarded, none or mixed");
    if (!window::parseInterior(c.s("interior"), &o.interior))
        throw lila::gn::ExecError(c.node.name + ": interior is room, card, mesh or none");
    o.damage = c.f("damage");
    o.neatness = c.f("neatness");
    o.boardOverhang = c.f("boardOverhang");
    o.interiorDepth = c.f("interiorDepth");
    o.doors = c.b("doors");
    o.seed = uint32_t(c.i("seed"));
    const window::Result r = window::layout(openings, pieces, o);

    Builder b;
    for (int s = 0; s < window::RoomMesh; ++s)
        b.addSlot(slots[s]);
    b.addRef(lila::gn::meshGeometry(window::unitRoom(c.s("roomMaterial"))));
    b.addRef(lila::gn::meshGeometry(window::unitCard(c.s("cardMaterial"))));
    for (const window::Placed& p : r.placed)
        b.add(p.xf, p.slot, p.piece);
    auto slot = b.inst->attrs.getOrAdd<int32_t>("slot");
    auto win = b.inst->attrs.getOrAdd<int32_t>("window");
    auto random = b.inst->attrs.getOrAdd<float>("random");
    for (size_t k = 0; k < r.placed.size(); ++k)
    {
        slot[k] = r.placed[k].slot;
        win[k] = r.placed[k].window;
        random[k] = r.placed[k].random;
    }
    c.out("geometry", b.geometry());
}

void rooftopExec(ExecContext& c)
{
    LILA_ZONE();

    const Mesh host = joined(c, "hosts");
    const Linked railings = linked(c, "railings");
    const Linked towers = linked(c, "towers");
    rooftop::Options o;
    o.slopeMax = c.f("slopeMax");
    o.minArea = c.f("minArea");
    o.edgeMargin = c.f("edgeMargin");
    o.railings = c.f("railingChance");
    o.towers = c.f("towerChance");
    o.cell = c.f("cell");
    o.seed = uint32_t(c.i("seed"));
    const std::vector<double> rw = gen::parseNumbers(c.s("railingWeights")), tw = gen::parseNumbers(c.s("towerWeights"));
    rooftop::Result r = rooftop::layout(host, railings.bounds, rw, towers.bounds, tw, o);
    Builder b;
    b.addSlot(railings);
    b.addSlot(towers);
    for (const rooftop::Placed& p : r.placed)
        b.add(p.xf, p.slot, p.piece);
    c.out("instances", b.geometry());
    c.out("tops", lila::gn::meshGeometry(std::move(r.surface)));
}

void trussExec(ExecContext& c)
{
    LILA_ZONE();

    const GeometryPtr g = c.geometry("curve");
    std::vector<std::vector<glm::dvec3>> lines;
    if (g->curves)
    {
        const lila::gn::CurveSet ev = g->curves->evaluated();
        for (size_t k = 0; k < ev.numCurves(); ++k)
        {
            const auto pts = ev.curvePoints(k);
            std::vector<glm::dvec3> line(pts.begin(), pts.end());
            if (ev.cyclic[k] && !line.empty())
                line.push_back(line.front());
            lines.push_back(std::move(line));
        }
    }
    if (g->mesh)
    {
        const Mesh& m = *g->mesh;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && m.edgeFaceCount(e) == 0)
                lines.push_back({m.position(m.edgePoint(e, 0)), m.position(m.edgePoint(e, 1))});
    }
    truss::Options o;
    o.chords = int(c.i("chords"));
    o.width = c.f("width");
    o.endWidth = c.f("endWidth");
    o.member = c.f("member");
    o.segment = c.f("segment");
    if (!truss::parseBracing(c.prop("bracing"), &o.bracing))
        o.bracing = truss::Bracing::X;
    o.rings = c.b("rings");
    o.unite = c.b("unite");
    truss::Result r = truss::build(lines, o);
    if (!r.warning.empty())
        lila::log::warn("{}: {}", c.node.name, r.warning);
    c.out("geometry", lila::gn::meshGeometry(std::move(r.mesh)));
}

std::string weightsDoc(const char* what) { return std::string("How often each of the ") + what + " is picked, in link order, comma separated (missing ones are 1)."; }
} // namespace

bool registerGeneratorNodes(lila::ext::Context& ctx)
{
    std::vector<lila::gn::NodeDecl> decls;
    decls.push_back(
        {.type = "WeatheringRoofing",
         .label = "Roofing",
         .category = "Weathering",
         .doc = "Lays the linked pieces (sheets, planks, tiles) in overlapping rows over a host's roof faces, from the eave up, with "
                "ridge caps along ridges, then ages them: missing pieces, lifted corners, a sagging middle. Pieces are fitted by "
                "their bounds: Y out of the roof, the longer horizontal side down the slope. Instances carry slot (0 cover, 1 cap), "
                "row, lifted and random.",
         .sockets = {.inputs = {geo("host", "The roof (world space): a building, a Blockwork block, a roof mesh.", true),
                                geo("pieces", "The cover pieces laid in rows.", true),
                                geo("caps", "Ridge caps; none links leaves ridges bare.", true),
                                in("weights", Socket::String, "", weightsDoc("pieces")),
                                in("capWeights", Socket::String, "", weightsDoc("caps")),
                                in("faces", Socket::String, "", "Face indices of the host to cover, comma separated; empty covers every face within maxSlope of up."),
                                in("overlap", Socket::Float, 0.15, "How much of a piece's length the next row covers.", 0.0, 0.9),
                                in("sideOverlap", Socket::Float, 0.08, "How much of a piece's width its neighbour covers.", 0.0, 0.9),
                                in("overhang", Socket::Float, 5.0, "How far the first row reaches past the eave, cm.", 0.0),
                                in("scale", Socket::Float, 1.0, "Scale of every piece.", 0.01),
                                in("damage", Socket::Float, 0.3, "0 to 1: missing pieces, lifted corners and how far they lift.", 0.0, 1.0),
                                in("missing", Socket::Float, -1.0, "Share of pieces left out; negative takes it from damage."),
                                in("lifted", Socket::Float, -1.0, "Share of pieces with a lifted corner; negative takes it from damage."),
                                in("liftAngle", Socket::Float, 0.0, "How far corners lift, degrees; 0 takes it from damage.", 0.0, 90.0),
                                in("sag", Socket::Float, 0.0, "How far the middle of each face dips below its edges, cm.", 0.0),
                                in("neatness", Socket::Float, 0.3, "0 a messy roof, 1 neat regular rows.", 0.0, 1.0),
                                in("stagger", Socket::Float, 0.5, "Neat rows: how far alternate rows shift, of a piece's width.", 0.0, 1.0),
                                in("ridge", Socket::Bool, true, "Caps along ridges."),
                                in("maxSlope", Socket::Float, 75.0, "Faces picked when none are given: at most this steep, degrees.", 0.0, 90.0),
                                in("seed", Socket::Int, 1, "Another seed gives another roof.")},
                     .outputs = {geo("geometry", "The roof pieces, as instances.")}},
         .exec = roofingExec});
    decls.push_back(
        {.type = "WeatheringWindows",
         .label = "Windows",
         .category = "Weathering",
         .doc = "Finds the openings in the linked walls (Blockwork carves, Masonry openings, holes in any wall) and fits each with a "
                "frame along its sides, a sill, glass (intact, cracked, broken, boarded or mixed) and an interior behind it (a "
                "generated room, a card or a linked mesh). Pieces are fitted by their bounds. Instances carry slot (0 frame, 1 sill, "
                "2 pane, 3 cracked, 4 broken, 5 board, 6 interior mesh, 7 room, 8 card), window and random.",
         .sockets = {.inputs = {geo("walls", "The walls (world space).", true),
                                geo("frames", "Frame members, along X.", true),
                                geo("sills", "Sills, along X, out of the wall along Z.", true),
                                geo("panes", "Intact panes, X across and Y up.", true),
                                geo("cracked", "Cracked panes.", true),
                                geo("broken", "Broken panes.", true),
                                geo("boards", "Boards nailed across, along X.", true),
                                geo("interiors", "Interior meshes, for interior mesh.", true),
                                in("frameWeights", Socket::String, "", weightsDoc("frames")),
                                in("sillWeights", Socket::String, "", weightsDoc("sills")),
                                in("paneWeights", Socket::String, "", weightsDoc("panes")),
                                in("crackedWeights", Socket::String, "", weightsDoc("cracked panes")),
                                in("brokenWeights", Socket::String, "", weightsDoc("broken panes")),
                                in("boardWeights", Socket::String, "", weightsDoc("boards")),
                                in("interiorWeights", Socket::String, "", weightsDoc("interiors")),
                                in("glass", Socket::String, "intact", "intact, cracked, broken, boarded, none or mixed."),
                                in("interior", Socket::String, "room", "room (generated), card, mesh or none."),
                                in("roomMaterial", Socket::String, "M_WeatheringInterior", "Material slot of the generated rooms."),
                                in("cardMaterial", Socket::String, "M_WeatheringInteriorCard", "Material slot of the interior cards."),
                                in("frameInset", Socket::Float, 4.0, "How far in from the wall's outer face the frame sits, cm.", 0.0),
                                in("frameScale", Socket::Float, 1.0, "Scale of the frame and sill sections.", 0.01),
                                in("columns", Socket::Int, 1, "Panes across.", 1.0, 16.0),
                                in("rows", Socket::Int, 1, "Panes up.", 1.0, 16.0),
                                in("sill", Socket::Bool, true, "A sill under windows."),
                                in("sillProject", Socket::Float, 4.0, "How far the sill stands out of the wall, cm."),
                                in("damage", Socket::Float, 0.3, "0 to 1: mixed glass breaks more, boards go missing.", 0.0, 1.0),
                                in("neatness", Socket::Float, 0.3, "1 lays boards straight and even.", 0.0, 1.0),
                                in("boardOverhang", Socket::Float, 8.0, "How far boards reach past the opening each side, cm.", 0.0),
                                in("interiorDepth", Socket::Float, 300.0, "Depth of the room behind the wall, cm.", 10.0),
                                in("doors", Socket::Bool, false, "Fill openings that reach the bottom of the wall too."),
                                in("flip", Socket::Bool, false, "Outside is the other side of the walls."),
                                in("minSize", Socket::Float, 15.0, "Smaller holes are not openings, cm.", 1.0),
                                in("maxThickness", Socket::Float, 150.0, "Faces further apart than this are different walls, cm.", 1.0),
                                in("seed", Socket::Int, 1, "Another seed picks other pieces and other breakage.")},
                     .outputs = {geo("geometry", "The windows, as instances.")}},
         .exec = windowsExec});
    decls.push_back(
        {.type = "WeatheringRooftop",
         .label = "Rooftop",
         .category = "Weathering",
         .doc = "The flat tops of the linked buildings (upward faces nothing stands over), with railings along some of their edges "
                "and a tower on some. tops is a gridded surface with the point float rooftopFree (0 near edges and towers), the mask "
                "Scatter Props takes to place vents, AC units and pipes on it.",
         .sockets = {.inputs = {geo("hosts", "The buildings (world space).", true),
                                geo("railings", "Railing segments along X.", true),
                                geo("towers", "Towers, standing Y up.", true),
                                in("railingWeights", Socket::String, "", weightsDoc("railings")),
                                in("towerWeights", Socket::String, "", weightsDoc("towers")),
                                in("slopeMax", Socket::Float, 5.0, "Steepest face that counts as flat, degrees.", 0.0, 89.0),
                                in("minArea", Socket::Float, 2.0, "Smaller flat tops are left alone, square metres.", 0.0),
                                in("edgeMargin", Socket::Float, 50.0, "Kept clear inside each top's edge, cm.", 0.0),
                                in("railingChance", Socket::Float, 0.5, "Share of edges that get a railing.", 0.0, 1.0),
                                in("towerChance", Socket::Float, 0.35, "Chance a top gets a tower.", 0.0, 1.0),
                                in("cell", Socket::Float, 40.0, "Grid of the tops surface, cm.", 5.0),
                                in("seed", Socket::Int, 1, "Another seed picks other edges and corners.")},
                     .outputs = {geo("tops", "The flat tops, gridded, with rooftopFree."), geo("instances", "Railings and towers.")}},
         .exec = rooftopExec});
    decls.push_back(
        {.type = "CurveToTruss",
         .label = "Curve to Truss",
         .category = "Curve",
         .doc = "A lattice truss along each curve (or loose edge): chords along it, rings at each level, bracing on every face, each "
                "member a square beam, united into one closed mesh. Masts, pylons, crane jibs, girders, stylized towers.",
         .props = {{.id = "bracing", .options = {"x", "zigzag", "none"}, .defaultValue = "x", .doc = "Diagonals on each face: both, alternating or none."}},
         .sockets = {.inputs = {geo("curve", "Curves or loose edges."),
                                in("chords", Socket::Int, 4, "Chords round the section.", 3.0, 12.0),
                                in("width", Socket::Float, 60.0, "Side of the section at the start, cm.", 0.1),
                                in("endWidth", Socket::Float, -1.0, "Side at the end, cm; negative keeps width."),
                                in("member", Socket::Float, 4.0, "Side of each member, cm.", 0.05),
                                in("segment", Socket::Float, 80.0, "Distance between levels, cm.", 1.0),
                                in("rings", Socket::Bool, true, "Members round every level, not only the ends."),
                                in("unite", Socket::Bool, true, "One closed mesh; off leaves overlapping beams (faster).")},
                     .outputs = {geo("geometry", "The truss.")}},
         .exec = trussExec});
    for (lila::gn::NodeDecl& d : decls)
        if (!lila::gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace weathering::nodes
