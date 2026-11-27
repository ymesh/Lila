// The cliff nodes: Terrain Cliffs (the ground under cliffs and walls, in the terrain's chain), Terrain Cliff Wall (a wall
// mesh following a curve), Terrain Cliff Scatter (tiered scatter on cliff meshes) and Terrain Cliff Kit (kit pieces fitted
// along a cliff).

#include "CliffScene.h"
#include "GroundCover.h"
#include "Stamps.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Overlap.h>
#include <lila/Profile.h>

#include <glm/gtc/matrix_transform.hpp>

#include <map>
#include <mutex>

using namespace lila;

namespace terrain::cliffs
{
using json = nlohmann::json;

namespace
{
std::mutex gGroundMutex;
std::map<uint64_t, std::shared_ptr<const Heightfield>> gGround;

json parseJson(const std::string& text, json fallback)
{
    if (text.empty())
        return fallback;
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? fallback : j;
}

gn::GeometryPtr withHeightfield(const gn::GeometryPtr& g, Heightfield h)
{
    gn::Geometry out = *g;
    out.heightfield = std::make_shared<const Heightfield>(std::move(h));
    return gn::makeGeometry(std::move(out));
}

/** The terrain heightfield an Object Info node sees, in the terrain's own space, and its world matrix. */
bool terrainOf(const gn::ExecContext& c, const std::string& info, Heightfield& h, glm::dmat4& world)
{
    auto it = c.env.objects.find(info);
    if (it == c.env.objects.end() || !it->second.mesh)
        return false;
    h = heightfieldFromGridMesh(*it->second.mesh);
    world = it->second.matrix;
    return !h.empty();
}

/** Every curve of an Object Info node as polylines in space toLocal maps its own space into. */
std::vector<std::vector<glm::dvec2>> curveLines(const gn::ExecContext& c, const std::string& info, const glm::dmat4& toLocal)
{
    auto it = c.env.objects.find(info);
    if (it == c.env.objects.end() || !it->second.curves)
        return {};
    return footprintFromCurves(*it->second.curves, toLocal * it->second.matrix).lines;
}

glm::dmat4 standOn(const glm::dvec3& at, const glm::dvec3& n, double align, double turn, double scale)
{
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const glm::dvec3 target = glm::normalize(glm::mix(up, n, std::clamp(align, 0.0, 1.0)));
    const glm::dvec3 axis = glm::cross(up, target);
    const double s = glm::length(axis), cosA = glm::dot(up, target);
    const glm::dmat4 tilt = s < 1e-9 ? glm::dmat4(1.0) : glm::rotate(glm::dmat4(1.0), std::atan2(s, cosA), axis / s);
    return glm::translate(glm::dmat4(1.0), at) * glm::rotate(glm::dmat4(1.0), turn, target) * tilt * glm::scale(glm::dmat4(1.0), glm::dvec3(scale));
}

std::mutex gResultMutex;
std::map<std::pair<uint64_t, std::string>, json> gResults;

void publishResult(uint64_t owner, const std::string& node, json r)
{
    std::lock_guard lock(gResultMutex);
    gResults[{owner, node}] = std::move(r);
}

// ---------------------------------------------------------------------------------------------------- Terrain Cliffs

gn::NodeDecl cliffsDecl()
{
    gn::NodeDecl d;
    d.type = kCliffsType;
    d.label = "Terrain Cliffs";
    d.category = "Terrain";
    d.doc = "The ground under cliff meshes: a proud cliff sets the ground back under its mesh so the mesh stands in front, an "
            "inset cliff opens holes the mesh fills, and an extruded wall raises a plateau behind it and cuts the base in front. "
            "Writes masks cliff, cliffTalus (below cliffs, for scree) and cliffTop (along their tops). terrainCliffMesh and "
            "terrainCliffExtrude write it.";
    d.props = {{.id = "cliffs", .defaultValue = "[]", .doc = "Cliffs, JSON: [{name, kind, mesh, mode, rings, depth, recess, blend, margin, talusWidth}] in the heightfield's own space."},
               {.id = "walls", .defaultValue = "[]", .doc = "Walls, JSON: [{name, object (an Object Info node), height, plateau, cut, falloff, flip, stamp}]."}};
    d.sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        gn::GeometryPtr g = c.geometry("heightfield");
        const Heightfield* in = heightfieldOf(*g);
        if (!in)
            throw gn::ExecError(c.node.name + ": the geometry has no heightfield");
        publishGround(c.env.owner, std::make_shared<const Heightfield>(*in));
        std::vector<GroundEdit> edits;
        for (const json& e : parseJson(c.prop("cliffs"), json::array()))
            if (e.is_object() && e.contains("rings") && e.value("kind", std::string("region")) == "region")
                edits.push_back(GroundEdit::fromJson(e));
        std::vector<WallStamp> walls;
        const glm::dmat4 toLocal = glm::inverse(c.env.ownerMatrix);
        for (const json& w : parseJson(c.prop("walls"), json::array()))
        {
            if (!w.is_object() || !w.value("stamp", true))
                continue;
            for (auto& line : curveLines(c, w.value("object", std::string()), toLocal))
            {
                WallStamp s;
                s.name = w.value("name", std::string());
                s.line = std::move(line);
                s.height = w.value("height", s.height);
                s.plateau = w.value("plateau", s.plateau);
                s.cut = w.value("cut", s.cut);
                s.falloff = w.value("falloff", s.falloff);
                s.flip = w.value("flip", false);
                walls.push_back(std::move(s));
            }
        }
        if (edits.empty() && walls.empty())
        {
            c.out("heightfield", g);
            return;
        }
        Heightfield h = *in;
        const GroundResult r = applyGroundEdits(h, edits, walls);
        publishResult(c.env.owner, c.node.name, {{"recessed", r.recessed}, {"holes", r.holes}, {"stamped", r.stamped}});
        c.out("heightfield", withHeightfield(g, std::move(h)));
    };
    return d;
}

// ---------------------------------------------------------------------------------------------------- Terrain Cliff Wall

gn::NodeDecl wallDecl()
{
    gn::NodeDecl d;
    d.type = "TerrainCliffWall";
    d.label = "Terrain Cliff Wall";
    d.category = "Terrain";
    d.doc = "A cliff wall along a curve: from the ground at its base up to height (or the plateau a stamped terrain raised "
            "behind it), shaped by a profile, strata and noise, with the masks every cliff mesh carries. Follows the curve and "
            "the terrain live.";
    d.props = {{.id = "curve", .defaultValue = "", .doc = "The Object Info node of the curve."},
               {.id = "terrain", .defaultValue = "", .doc = "The Object Info node of the terrain; empty stands the wall on Y = 0."},
               {.id = "settings", .defaultValue = "{}", .doc = "The wall, JSON: {height, profile, custom, batter, steps, sink, detail, cell, flip, shape: {...}}."}};
    d.sockets = {.outputs = {{.id = "geometry", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        const json s = parseJson(c.prop("settings"), json::object());
        Heightfield ground;
        glm::dmat4 terrainWorld(1.0);
        const bool haveGround = terrainOf(c, c.prop("terrain"), ground, terrainWorld);
        const glm::dmat4 toLocal = haveGround ? glm::inverse(terrainWorld) : glm::dmat4(1.0);
        ExtrudeParams p;
        p.height = s.value("height", p.height) / (haveGround ? glm::length(glm::dvec3(terrainWorld[1])) : 1.0);
        Profile prof = Profile::Vertical;
        if (parseProfile(s.value("profile", std::string("vertical")), &prof))
            p.profile = prof;
        for (const json& q : s.value("custom", json::array()))
            if (q.is_array() && q.size() >= 2)
                p.custom.push_back({q[0].get<double>(), q[1].get<double>()});
        p.batter = s.value("batter", p.batter);
        p.steps = s.value("steps", p.steps);
        p.sink = s.value("sink", p.sink);
        p.detail = s.value("detail", p.detail);
        p.cell = haveGround ? ground.cell : s.value("cell", p.cell);
        const json shape = s.value("shape", json::object());
        p.mesh.blend = shape.value("blend", 200.0);
        p.mesh.uvScale = shape.value("uvScale", p.mesh.uvScale);
        Shape& sh = p.mesh.shape;
        sh.depth = shape.value("depth", sh.depth);
        sh.strataHeight = shape.value("strataHeight", sh.strataHeight);
        sh.strataJitter = shape.value("strataJitter", sh.strataJitter);
        sh.undercut = shape.value("undercut", sh.undercut);
        sh.ledges = shape.value("ledges", sh.ledges);
        sh.noise = shape.value("noise", sh.noise);
        sh.noiseSize = shape.value("noiseSize", sh.noiseSize);
        sh.overhang = shape.value("overhang", sh.overhang);
        sh.seed = shape.value("seed", sh.seed);
        const double dip = shape.value("strataDip", 0.0) * 3.141592653589793 / 180.0;
        sh.strata = glm::dvec3(0.0, std::cos(dip), std::sin(dip));
        const auto lines = curveLines(c, c.prop("curve"), toLocal);
        if (lines.empty())
        {
            c.out("geometry", gn::makeGeometry({}));
            return;
        }
        geo::Mesh joined;
        json stats = json::array();
        for (const auto& line : lines)
        {
            CliffMesh w = extrudeWall(line, haveGround ? &ground : nullptr, p, s.value("flip", false));
            geo::Mesh m = toMesh(w.mesh);
            gn::transformMesh(m, glm::inverse(c.env.ownerMatrix) * terrainWorld);
            joined.append(m);
            stats.push_back({{"vertices", w.stats.vertices}, {"triangles", w.stats.triangles}, {"layers", w.stats.layers},
                             {"baseClearance", w.stats.baseClearance}, {"warnings", w.stats.warnings}});
        }
        joined.compact();
        publishResult(c.env.owner, c.node.name, {{"walls", stats}});
        gn::Geometry out;
        out.mesh = std::make_shared<const geo::Mesh>(std::move(joined));
        c.out("geometry", gn::makeGeometry(std::move(out)));
    };
    return d;
}

// ---------------------------------------------------------------------------------------------------- Terrain Cliff Scatter

std::vector<Tier> tiersFrom(const json& j)
{
    std::vector<Tier> out;
    for (const json& t : j.is_array() ? j : json::array())
    {
        Tier tier;
        tier.name = t.value("name", std::string("tier") + std::to_string(out.size() + 1));
        tier.grid = std::max(10.0, t.value("grid", tier.grid));
        tier.density = std::max(0.0, t.value("density", tier.density));
        tier.mask = t.value("mask", tier.mask);
        tier.align = t.value("align", tier.align);
        if (t.contains("scale") && t["scale"].is_array() && t["scale"].size() == 2)
        {
            tier.scaleMin = t["scale"][0].get<double>();
            tier.scaleMax = t["scale"][1].get<double>();
        }
        for (const json& i : t.value("items", json::array()))
            tier.items.push_back(i.get<std::string>());
        out.push_back(std::move(tier));
    }
    return out;
}

gn::NodeDecl scatterDecl()
{
    gn::NodeDecl d;
    d.type = "TerrainCliffScatter";
    d.label = "Terrain Cliff Scatter";
    d.category = "Terrain";
    d.doc = "Scatter on cliff meshes by tiers, each on its own grid (big rocks coarse, debris fine) with a seed per cell, so an "
            "edit only reshuffles the cells it touches. Pieces go where a tier's mask (a cliff mesh attribute: scatter, ledge, "
            "talus, upFacing) is high. Instances carry tier, item, slope and downhill.";
    d.props = {{.id = "tiers", .defaultValue = "[]", .doc = "Tiers, JSON: [{name, grid, density, mask, align, scale, items}]."},
               {.id = "sources", .defaultValue = "[]", .doc = "Object Info nodes of the cliff meshes, JSON list."}};
    d.sockets = {.inputs = {{.id = "seed", .type = gn::SocketType::Int}}, .outputs = {{.id = "instances", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        const std::vector<Tier> tiers = tiersFrom(parseJson(c.prop("tiers"), json::array()));
        const uint32_t seed = uint32_t(c.i("seed"));
        auto inst = std::make_shared<gn::Instances>();
        std::map<std::string, int> refOf;
        std::vector<ScatterPoint> all;
        for (const json& src : parseJson(c.prop("sources"), json::array()))
        {
            auto it = c.env.objects.find(src.get<std::string>());
            if (it == c.env.objects.end() || !it->second.mesh)
                continue;
            geo::Mesh world = *it->second.mesh;
            gn::transformMesh(world, it->second.matrix);
            const std::vector<ScatterPoint> pts = scatterTiers(fromMesh(world), tiers, seed, {0.0, 0.0});
            all.insert(all.end(), pts.begin(), pts.end());
        }
        const glm::dmat4 toOwner = glm::inverse(c.env.ownerMatrix);
        std::vector<const ScatterPoint*> kept;
        for (const ScatterPoint& p : all)
        {
            const Tier& tier = tiers[size_t(p.tier)];
            if (tier.items.empty())
                continue;
            const std::string item = tier.items[size_t(p.item)];
            auto [at, fresh] = refOf.emplace(item, int(inst->references.size()));
            if (fresh)
            {
                auto it = c.env.objects.find("Item_" + item);
                gn::Geometry ref;
                if (it != c.env.objects.end())
                    ref.mesh = it->second.mesh;
                inst->references.push_back(gn::makeGeometry(std::move(ref)));
            }
            inst->add(toOwner * standOn(p.position, p.normal, tier.align, p.turn, p.scale), at->second);
            kept.push_back(&p);
        }
        auto tierAttr = inst->attrs.getOrAdd<int32_t>("tier");
        auto itemAttr = inst->attrs.getOrAdd<int32_t>("item");
        auto slopeAttr = inst->attrs.getOrAdd<float>("slope");
        auto downAttr = inst->attrs.getOrAdd<glm::vec3>("downhill");
        for (size_t k = 0; k < kept.size(); ++k)
        {
            tierAttr[k] = kept[k]->tier;
            itemAttr[k] = kept[k]->item;
            slopeAttr[k] = float(kept[k]->slope);
            downAttr[k] = glm::vec3(kept[k]->downhill);
        }
        json counts = json::object();
        for (const ScatterPoint* p : kept)
            counts[tiers[size_t(p->tier)].name] = counts.value(tiers[size_t(p->tier)].name, 0) + 1;
        publishResult(c.env.owner, c.node.name, {{"count", kept.size()}, {"perTier", counts}});
        gn::Geometry out;
        out.instances = std::move(inst);
        c.out("instances", gn::makeGeometry(std::move(out)));
    };
    return d;
}

// ---------------------------------------------------------------------------------------------------- Terrain Cliff Kit

gn::NodeDecl kitDecl()
{
    gn::NodeDecl d;
    d.type = "TerrainCliffKit";
    d.label = "Terrain Cliff Kit";
    d.category = "Terrain";
    d.doc = "Kit pieces (rock and cliff meshes) fitted along a cliff region: facing out, scaled to its height, overlapped and "
            "turned to hide repeats, base sunk, floating and over-buried pieces dropped, gaps filled with smaller pieces. "
            "Instances carry piece and filler.";
    d.props = {{.id = "terrain", .defaultValue = "", .doc = "The Object Info node of the terrain."},
               {.id = "region", .defaultValue = "{}", .doc = "The region, JSON: {rings, low, high} in the terrain's own space."},
               {.id = "pieces", .defaultValue = "[]", .doc = "Piece names, JSON list; each is an Object Info node Item_<name>."},
               {.id = "settings", .defaultValue = "{}", .doc = "Fitting, JSON: {overlap, rotate, scaleJitter, sink, floatTolerance, maxPenetration, gap, seed}."}};
    d.sockets = {.outputs = {{.id = "instances", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        Heightfield ground;
        glm::dmat4 world(1.0);
        auto inst = std::make_shared<gn::Instances>();
        if (!terrainOf(c, c.prop("terrain"), ground, world))
            throw gn::ExecError(c.node.name + ": no terrain");
        const json rj = parseJson(c.prop("region"), json::object());
        Region r;
        r.rings = ringsFrom(rj.value("rings", json::array()));
        r.low = rj.value("low", 0.0);
        r.high = rj.value("high", 1000.0);
        const json sj = parseJson(c.prop("settings"), json::object());
        KitParams p;
        p.overlap = sj.value("overlap", p.overlap);
        p.rotate = sj.value("rotate", p.rotate);
        p.scaleJitter = sj.value("scaleJitter", p.scaleJitter);
        p.sink = sj.value("sink", p.sink);
        p.floatTolerance = sj.value("floatTolerance", p.floatTolerance) / glm::length(glm::dvec3(world[1]));
        p.maxPenetration = sj.value("maxPenetration", p.maxPenetration);
        p.gap = sj.value("gap", p.gap) / glm::length(glm::dvec3(world[0]));
        p.seed = sj.value("seed", p.seed);
        std::vector<KitPiece> kit;
        std::vector<std::string> names;
        for (const json& n : parseJson(c.prop("pieces"), json::array()))
        {
            auto it = c.env.objects.find("Item_" + n.get<std::string>());
            if (it == c.env.objects.end() || !it->second.mesh || it->second.mesh->numPoints() == 0)
                continue;
            const geo::Mesh& m = *it->second.mesh;
            KitPiece piece;
            piece.name = n.get<std::string>();
            const geo::Bounds b = m.bounds();
            piece.lo = b.min;
            piece.size = b.max - b.min;
            const geo::Index step = std::max<geo::Index>(1, m.numPoints() / 256);
            for (geo::Index v = 0; v < m.pointCapacity(); v += step)
                if (m.pointValid(v))
                    piece.samples.push_back(m.position(v));
            kit.push_back(std::move(piece));
            gn::Geometry ref;
            ref.mesh = it->second.mesh;
            inst->references.push_back(gn::makeGeometry(std::move(ref)));
            names.push_back(n.get<std::string>());
        }
        // Over-buried pieces by Stagehand's physics when it is loaded, on the ground round the region only.
        std::string check = "samples";
        std::unique_ptr<lila::overlap::Session> session;
        if (!r.rings.empty())
        {
            glm::dvec2 lo(1e300), hi(-1e300);
            for (const auto& ring : r.rings)
                for (const glm::dvec2& q : ring)
                {
                    lo = glm::min(lo, q);
                    hi = glm::max(hi, q);
                }
            const double margin = 2.0 * std::max(1.0, r.high - r.low) + 4.0 * ground.cell;
            const int x0 = std::max(0, int(std::floor((lo.x - margin - ground.origin.x) / ground.cell)));
            const int z0 = std::max(0, int(std::floor((lo.y - margin - ground.origin.y) / ground.cell)));
            const int x1 = std::min(ground.nx - 1, int(std::ceil((hi.x + margin - ground.origin.x) / ground.cell)));
            const int z1 = std::min(ground.nz - 1, int(std::ceil((hi.y + margin - ground.origin.y) / ground.cell)));
            if (x1 > x0 && z1 > z0)
            {
                Heightfield crop = lila::geo::makeHeightfield(x1 - x0 + 1, z1 - z0 + 1, ground.cell, ground.samplePos(x0, z0));
                for (int z = z0; z <= z1; ++z)
                    for (int x = x0; x <= x1; ++x)
                        crop.heights[crop.index(x - x0, z - z0)] = ground.at(x, z);
                std::string label;
                session = lila::overlap::open(crop, &label);
                if (session)
                {
                    check = label;
                    p.depthOf = [&session](std::span<const glm::dvec3> hull, const glm::dvec3& out) { return session->depth(hull, out); };
                }
            }
        }
        const KitResult res = fitKit(ground, r, kit, p);
        const glm::dmat4 toOwner = glm::inverse(c.env.ownerMatrix) * world;
        for (const KitPlacement& k : res.placed)
            inst->add(toOwner * k.m, k.piece);
        auto pieceAttr = inst->attrs.getOrAdd<int32_t>("piece");
        auto fillerAttr = inst->attrs.getOrAdd<int32_t>("filler");
        for (size_t k = 0; k < res.placed.size(); ++k)
        {
            pieceAttr[k] = res.placed[k].piece;
            fillerAttr[k] = res.placed[k].filler ? 1 : 0;
        }
        publishResult(c.env.owner, c.node.name,
                      {{"placed", res.placed.size()}, {"fillers", res.fillers}, {"rejectedFloating", res.rejectedFloating},
                       {"rejectedPenetrating", res.rejectedPenetrating}, {"coverage", res.coverage}, {"pieces", names},
                       {"penetrationCheck", check},
                       {"warnings", check == "samples" ? json::array({"Stagehand isn't loaded, so over-buried pieces were judged by how many of their sample points sit under the ground"})
                                                       : json::array()}});
        gn::Geometry out;
        out.instances = std::move(inst);
        c.out("instances", gn::makeGeometry(std::move(out)));
    };
    return d;
}
} // namespace

void publishGround(uint64_t modifier, std::shared_ptr<const Heightfield> h)
{
    std::lock_guard lock(gGroundMutex);
    gGround[modifier] = std::move(h);
}

void forgetGoneCliffs()
{
    const lila::scene::Graph& g = lila::scene::Graph::current();
    {
        std::lock_guard lock(gGroundMutex);
        std::erase_if(gGround, [&](const auto& e) { return !g.node(lila::scene::NodeId(e.first)); });
    }
    std::lock_guard lock(gResultMutex);
    std::erase_if(gResults, [&](const auto& e) { return !g.node(lila::scene::NodeId(e.first.first)); });
}

std::shared_ptr<const Heightfield> publishedGround(uint64_t modifier)
{
    std::lock_guard lock(gGroundMutex);
    auto it = gGround.find(modifier);
    return it == gGround.end() ? nullptr : it->second;
}

json cliffNodeResult(uint64_t owner, const std::string& node)
{
    std::lock_guard lock(gResultMutex);
    auto it = gResults.find({owner, node});
    return it == gResults.end() ? json() : it->second;
}

bool registerCliffNodes(ext::Context& ctx)
{
    for (gn::NodeDecl d : {cliffsDecl(), wallDecl(), scatterDecl(), kitDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace terrain::cliffs
