// Level > Settlement, whose Create lays out a settlement in each selected outline mesh or a square one on the ground
// below, and the Level Toolkit's Settlement section.

#include "Settlement.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>

using namespace lila;
using namespace lila::ui;

namespace settlement
{
namespace
{
const geo::Mesh* meshAt(scene::Graph& g, const scene::Node& n, int plug)
{
    const scene::PlugValue v = g.value({n.id(), plug, -1});
    const scene::MeshPtr* m = std::get_if<scene::MeshPtr>(&v);
    return m && *m ? m->get() : nullptr;
}

/** A site mesh: one authored with no faces, holding an outline as loose edges or a centre as loose points. */
bool isSiteMesh(scene::Graph& g, const scene::Node& n)
{
    const geo::Mesh* in = meshAt(g, n, scene::MeshNode::kInMesh);
    return in && in->numFaces() == 0 && in->numPoints() > 0;
}

/** World-space bounds of the node's input or output mesh, from its local bounds' corners. */
geo::Bounds worldBounds(scene::Graph& g, const scene::Node& n, int plug = scene::MeshNode::kOutMesh)
{
    geo::Bounds out;
    const geo::Mesh* m = meshAt(g, n, plug);
    if (!m || m->numPoints() == 0)
        return out;
    const geo::Bounds b = m->bounds();
    const scene::PlugValue xv = g.value({n.id(), scene::Transform::kWorldMatrix, -1});
    const glm::dmat4* xf = std::get_if<glm::dmat4>(&xv);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 p((k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z);
        out.extend(xf ? glm::dvec3(*xf * glm::dvec4(p, 1.0)) : p);
    }
    return out;
}

/** The ground a settlement centred at (x, z) stands on: the widest visible faced mesh spanning that point, or none. */
std::string groundNameUnder(scene::Graph& g, double x, double z)
{
    std::string best;
    double bestArea = 0.0;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || isSiteMesh(g, *n))
            continue;
        if (const scene::PlugValue vis = g.value({id, scene::Transform::kVisibility, -1}); std::get_if<bool>(&vis) && !std::get<bool>(vis))
            continue;
        const geo::Bounds b = worldBounds(g, *n);
        if (!b.valid() || x < b.min.x || x > b.max.x || z < b.min.z || z > b.max.z)
            continue;
        if (const double area = (b.max.x - b.min.x) * (b.max.z - b.min.z); area > bestArea)
        {
            bestArea = area;
            best = n->path();
        }
    }
    return best;
}

/**
 * The ground from the selection: the first selected faced mesh, else whatever is under the selected sites or the
 * origin. Selected site meshes (outlines) go to sites.
 */
std::string groundFromSelection(scene::Graph& g, std::vector<std::string>* sites)
{
    std::string ground;
    double cx = 0.0, cz = 0.0;
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (isSiteMesh(g, *n))
        {
            sites->push_back(n->path());
            if (const geo::Bounds b = worldBounds(g, *n, scene::MeshNode::kInMesh); b.valid())
            {
                cx = b.center().x;
                cz = b.center().z;
            }
        }
        else if (const geo::Bounds b = worldBounds(g, *n); ground.empty() && b.valid() && b.size().x > 0.0 && b.size().z > 0.0)
            ground = n->path();
    }
    return ground.empty() ? groundNameUnder(g, cx, cz) : ground;
}

/**
 * Create from the selection: selected site meshes each get a settlement in their outline; with none, one square
 * settlement on the ground's best site for the profile (settlementSuggest), or at the origin on flat ground.
 */
void createFromSelection()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> sites;
    const std::string ground = groundFromSelection(g, &sites);
    for (const std::string& s : sites)
        ops::execute("settlementCreate", {{"args", {s}}, {"ground", ground}}, CallSource::UI);
    if (!sites.empty())
        return;
    nlohmann::json flags = {{"ground", ground}};
    if (!ground.empty())
    {
        const nlohmann::json best = ops::execute("settlementSuggest", {{"ground", ground}, {"count", 1}});
        if (best.is_array() && !best.empty())
        {
            // The 400 m square is kept on the ground: a site near its edge would lose the far half.
            const geo::Bounds b = worldBounds(g, *g.find(ground));
            const double half = 0.5 * 40000.0; // settlementCreate's default size
            auto inside = [&](double v, double lo, double hi) { return lo + half > hi - half ? 0.5 * (lo + hi) : std::clamp(v, lo + half, hi - half); };
            const nlohmann::json& p = best[0]["position"];
            flags["attractors"] = nlohmann::json::array(
                {nlohmann::json::array({inside(p[0].get<double>(), b.min.x, b.max.x), inside(p[2].get<double>(), b.min.z, b.max.z)})});
        }
    }
    ops::execute("settlementCreate", flags, CallSource::UI);
}

/** Settle Region with its option box settings, on the selected ground or the one under the origin unless the box names one. */
void regionFromSelection()
{
    nlohmann::json call = nlohmann::json::object();
    if (const nlohmann::json set = ops::options::values("settlementRegion"); !set.contains("ground") || set["ground"] == "")
    {
        std::vector<std::string> sites;
        call["ground"] = groundFromSelection(scene::Graph::current(), &sites);
    }
    actions::runWithOptions("settlementRegion", call);
}

void warnOnThrow(void (*f)())
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}
} // namespace

void registerUi(ext::Context& ctx)
{
    actions::add(ctx, {.id = "settlement.create",
                       .label = "Settlement",
                       .category = "Create",
                       .run = [] { warnOnThrow(createFromSelection); },
                       .doc = "Lay out streets, lots and building envelopes inside each selected outline mesh, or in a 400 m "
                              "square on the best site of the selected ground (a terrain or any mesh), else of whatever is under "
                              "the origin.",
                       .python = "settlementCreate(profile='medievalTown', size=40000)"});
    actions::add(ctx, {.id = "settlement.region", .label = "Settle Region", .category = "Settlement",
                       .run = [] { warnOnThrow(regionFromSelection); },
                       .doc = "Place cities, towns and villages over the selected ground, else whatever is under the origin, "
                              "joined by roads.",
                       .python = "settlementRegion(ground='Terrain')",
                       .command = "settlementRegion",
                       .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "settlement.export", .label = "Export Settlement Layout", .category = "Settlement",
                       .doc = "Write the selected settlement's streets, lots and buildings as JSON.", .python = "settlementExport('Town', path='town.json')",
                       .command = "settlementExport", .call = nlohmann::json::object()});

    levelToolkit::add(ctx, {.id = "settlement",
                            .title = "Settlement",
                            .items = {{"settlement.create", "building-community", "Create", "Town"}, {"settlement.region", "map-2", "Region", "Region"},
                                      {"settlement.export", "file-export", "Export"}},
                            .order = 20, .docs = "settlement"});
    menus::addItem(ctx, {.id = "settlement.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Settlement",
                         .before = levelToolkit::kDressSection});
    for (const char* a : {"settlement.create", "settlement.region", "settlement.export"})
        menus::addItem(ctx, {.id = std::string("settlement.menu.") + a, .menu = "Level/Settlement", .action = a});
}
} // namespace settlement
