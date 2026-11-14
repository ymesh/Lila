#include "uv/UvCommands.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/Align.h"
#include "uv/Bff.h"
#include "uv/Flow.h"
#include "uv/OptCuts.h"
#include "uv/Optimize.h"
#include "uv/SymmetryUv.h"
#include "uv/Uv.h"
#include "uv/UvEdit.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lila::uv
{
namespace
{
using scene::ComponentMode;
using scene::Node;
using scene::components::Ids;

std::string uvSetOf(const Args& a)
{
    return a.has("uvSet") ? a.get<std::string>("uvSet") : currentUvSet();
}

std::vector<Chart> uvIslands(const geo::Mesh& m, std::span<const Index> faces, const std::string& set)
{
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = set;
    return buildCharts(m, faces, co);
}

std::vector<uint8_t> edgeMask(const geo::Mesh& m, const Ids& edges)
{
    std::vector<uint8_t> mask(m.edgeCapacity(), 0);
    for (Index e : edges)
        if (e < mask.size())
            mask[e] = 1;
    return mask;
}

/** Faces of every uv island that has one of the edges. */
std::vector<Index> islandFacesOfEdges(const geo::Mesh& m, const Ids& edges, const std::string& set)
{
    std::vector<Index> faces;
    for (Index e : edges)
        if (m.edgeValid(e))
            m.forEachEdgeCorner(e, [&](Index c) { faces.push_back(m.cornerFace(c)); });
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = set;
    return islandsOfFaces(m, faces, co);
}

OptimizeMode modeArg(const Args& a)
{
    std::string mode = a.get<std::string>("mode", "blend");
    if (mode == "conformal")
        return OptimizeMode::Conformal;
    if (mode == "authalic")
        return OptimizeMode::Authalic;
    if (mode == "isometric")
        return OptimizeMode::Isometric;
    if (mode == "blend")
        return OptimizeMode::Blend;
    throw CommandError("mode must be conformal, authalic, blend or isometric");
}

std::vector<FlagDef> modeFlags()
{
    using T = ArgType;
    return {{"mode", "", T::String, "blend",
             "conformal keeps angles, authalic keeps areas, blend mixes them by mix, isometric is SLIM (symmetric Dirichlet).",
             {"conformal", "authalic", "blend", "isometric"}},
            {"mix", "", T::Float, 0.5, "Blend only: 0 keeps angles, 1 keeps areas.", {}, {}, 0.0, 1.0},
            {"iterations", "", T::Int, 20, "Solver iterations.", {}, {}, 1.0, 500.0}};
}

OptimizeOptions optimizeArgs(const Args& a)
{
    OptimizeOptions o;
    o.mode = modeArg(a);
    o.mix = std::clamp(a.get<double>("mix", 0.5), 0.0, 1.0);
    o.iterations = std::clamp(a.get<int>("iterations", 20), 1, 500);
    o.usePins = a.get<bool>("pins", true);
    return o;
}

/** The selected points of a mesh node when vertices are being picked (Polygon corners). */
std::vector<Index> selectedPoints(const Node& n)
{
    if (scene::components::mode() != ComponentMode::Vertex)
        return {};
    const Ids& ids = scene::components::selected(n.id(), ComponentMode::Vertex);
    return {ids.begin(), ids.end()};
}

/** The exported-mesh solve matching an optimise mode. */
FinalSolveOptions finalOptions(const OptimizeOptions& o)
{
    FinalSolveOptions f;
    f.mode = o.mode;
    f.mix = o.mix;
    f.iterations = std::max(o.iterations, 200);
    f.usePins = o.usePins;
    return f;
}

bool usableUvs(const Chart& ch)
{
    double a = std::abs(ch.areaUv());
    return std::isfinite(a) && a > 1e-14 * std::max(1.0, ch.area3d()) && foldedTriangles(ch) == 0;
}
} // namespace

void registerOptimizeCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    ops::registerCommand({
        .name = "uvBff",
        .doc = "Boundary First Flattening of the selected faces' islands: conformal, with the boundary chosen first. free keeps the boundary "
               "lengths (least area distortion), disk makes a circle, rectangle straight sides between four corners (the selected vertices, "
               "else the sharpest), polygon one straight side per selected corner. cones adds automatic cone points that cut the island "
               "open to cut area distortion. Islands keep their place unless pack=True or they had no uvs.",
        .flags = {uvSetFlag(),
                  {"boundary", "", T::String, "free", "free, disk, rectangle or polygon.", {"free", "disk", "rectangle", "polygon"}},
                  {"cones", "", T::Int, 0, "Cone points placed automatically (free boundary only); each adds a short seam.", {}, {}, 0.0, 64.0},
                  {"coneSource", "", T::String, "stretch",
                   "stretch: where flattening stretches most. flow: where the surface's flow turns (caps and saddles), most curved first; "
                   "with cones 0, every one that bends enough.",
                   {"stretch", "flow"}},
                  {"exactLengths", "", T::Bool, false, "free boundary: bend rather than stretch to close it, so its lengths are exact."},
                  {"pack", "", T::Bool, false, "Pack the islands into 0-1 afterwards."},
                  finalMeshFlag()},
        .run = [](const Args& a) -> json {
            const bool final = a.get<bool>("finalMesh", true);
            std::string boundary = a.get<std::string>("boundary", "free");
            BffOptions bo;
            if (boundary == "free")
                bo.boundary = BffBoundary::MinimalArea;
            else if (boundary == "disk")
                bo.boundary = BffBoundary::Disk;
            else if (boundary == "rectangle" || boundary == "polygon")
                bo.boundary = BffBoundary::Polygon;
            else
                throw CommandError("uvBff: boundary must be free, disk, rectangle or polygon");
            bo.autoCones = std::clamp(a.get<int>("cones", 0), 0, 64);
            const std::string coneSource = a.get<std::string>("coneSource", "stretch");
            if (coneSource != "stretch" && coneSource != "flow")
                throw CommandError("uvBff: coneSource must be stretch or flow");
            const bool flowSource = coneSource == "flow" && bo.boundary == BffBoundary::MinimalArea;
            const int flowCount = bo.autoCones;
            if (flowSource)
                bo.autoCones = 0;
            bo.exactLengths = a.get<bool>("exactLengths", false);
            const bool pack = a.get<bool>("pack", false), rectangle = boundary == "rectangle";
            std::string set = uvSetOf(a);
            return editUvMeshes("UV BFF", K::Face, symmetricUvEdit(set, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                LILA_ZONE_N("uvBff");
                std::vector<Index> faces = liveFaces(m, f);
                Index opened = cutToDisks(m, faces);
                ChartOptions co;
                co.uvSet = set;
                std::vector<Chart> charts = buildCharts(m, faces, co);
                std::vector<Index> corners = selectedPoints(n);
                std::atomic<size_t> failed{0}, cones{0};
                std::vector<uint8_t> place(charts.size(), 0);
                tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
                    Chart& ch = charts[i];
                    const bool had = usableUvs(ch);
                    const glm::dvec2 c0 = ch.uvCentroid();
                    const double a0 = std::abs(ch.areaUv());
                    BffOptions o = bo;
                    for (Index v = 0; v < ch.vertexCount(); ++v)
                        if (std::find(corners.begin(), corners.end(), ch.vertexPoint[v]) != corners.end())
                            o.corners.push_back(v);
                    if (rectangle && o.corners.size() != 4)
                        o.corners.clear();
                    if (flowSource)
                        o.cones = flowConeVertices(ch, flowCount);
                    Chart trial = ch;
                    BffResult r = bff(trial, o);
                    if (!r.ok)
                    {
                        ++failed;
                        return;
                    }
                    ch = std::move(trial);
                    cones += r.cones.size();
                    if (had && !pack)
                        fitChart(ch, c0, a0);
                    else
                        place[i] = 1;
                });
                std::vector<Chart> toPack;
                for (size_t i = 0; i < charts.size(); ++i)
                    if (place[i])
                        toPack.push_back(std::move(charts[i]));
                if (!toPack.empty())
                    packCharts(toPack, {});
                for (size_t i = 0, k = 0; i < charts.size(); ++i)
                    if (place[i])
                        charts[i] = std::move(toPack[k++]);
                writeCharts(m, charts, set);
                if (cones > 0)
                    markSeamsFromUvs(m, faces, set);
                json out = {{"islands", charts.size()}, {"failed", failed.load()}, {"cones", cones.load()}, {"seamsAdded", opened}};
                const PackOptions packing;
                if (json fm = finishOnExport(m, faces, set, n, final, pack ? &packing : nullptr); !fm.is_null())
                    out["finalMesh"] = std::move(fm);
                return out;
            }));
        },
    });

    ops::registerCommand({
        .name = "uvFlowUnwrap",
        .doc = "Unwrap the selected faces' islands so U and V follow the surface's flow (its curvature, creases and borders): panels come "
               "out as rectangles and bevels as straight strips, ready for trim sheets. Each island is cut open from the points its flow "
               "turns round; one that would fold or overlap itself that way is unwrapped for least stretch instead and counted in fellBack. Islands keep "
               "their place unless pack=True or they had no uvs.",
        .flags = {uvSetFlag(),
                  {"straighten", "", T::Bool, true, "Lay border and crease edges that run nearly along U or V exactly along it."},
                  {"straightAngle", "", T::Float, 12.0, "How close to U or V an edge must run to be straightened, degrees.", {}, {}, 0.0, 45.0},
                  {"pack", "", T::Bool, false, "Pack the islands into 0-1 afterwards."},
                  finalMeshFlag()},
        .run = [](const Args& a) -> json {
            const bool final = a.get<bool>("finalMesh", true);
            FlowUnwrapOptions fo;
            fo.straighten = a.get<bool>("straighten", true);
            fo.straightAngle = std::clamp(a.get<double>("straightAngle", 12.0), 0.0, 45.0);
            const bool pack = a.get<bool>("pack", false);
            std::string set = uvSetOf(a);
            return editUvMeshes("UV Flow Unwrap", K::Face, symmetricUvEdit(set, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                LILA_ZONE_N("uvFlowUnwrap");
                std::vector<Index> faces = liveFaces(m, f);
                Index opened = cutToDisks(m, faces);
                ChartOptions co;
                co.uvSet = set;
                std::vector<Chart> charts = buildCharts(m, faces, co);
                std::atomic<size_t> fellBack{0}, darts{0}, straightened{0}, repaired{0}, folded{0};
                std::vector<uint8_t> place(charts.size(), 0);
                tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
                    Chart& ch = charts[i];
                    const bool had = usableUvs(ch);
                    const glm::dvec2 c0 = ch.uvCentroid();
                    const double a0 = std::abs(ch.areaUv());
                    Chart trial = ch;
                    const FlowUnwrapResult r = flowUnwrap(trial, fo);
                    if (r.ok && r.folded == 0 && !chartOverlaps(trial))
                    {
                        ch = std::move(trial);
                        darts += r.darts;
                        straightened += r.straightened;
                        repaired += r.repaired;
                    }
                    else
                    {
                        ++fellBack;
                        UnwrapOptions uo;
                        uo.usePins = false;
                        if (!unwrapChart(ch, uo).ok)
                            ++folded;
                    }
                    if (had && !pack)
                        fitChart(ch, c0, a0);
                    else
                        place[i] = 1;
                });
                std::vector<Chart> toPack;
                for (size_t i = 0; i < charts.size(); ++i)
                    if (place[i])
                        toPack.push_back(std::move(charts[i]));
                if (!toPack.empty())
                    packCharts(toPack, {});
                for (size_t i = 0, k = 0; i < charts.size(); ++i)
                    if (place[i])
                        charts[i] = std::move(toPack[k++]);
                writeCharts(m, charts, set);
                if (darts > 0)
                    markSeamsFromUvs(m, faces, set);
                json out = {{"islands", charts.size()},       {"darts", darts.load()},       {"straightened", straightened.load()},
                            {"repaired", repaired.load()},    {"fellBack", fellBack.load()}, {"failed", folded.load()},
                            {"seamsAdded", opened}};
                if (fellBack > 0)
                    out["note"] = std::to_string(fellBack.load()) + " island(s) couldn't follow their flow without folding or overlapping and were unwrapped for least stretch instead";
                const PackOptions packing;
                if (json fm = finishOnExport(m, faces, set, n, final, pack ? &packing : nullptr); !fm.is_null())
                    out["finalMesh"] = std::move(fm);
                return out;
            }));
        },
    });

    ops::registerCommand({
        .name = "uvOptimize",
        .doc = "Optimise the selected uv islands from where they are: conformal (keep angles), authalic (keep areas), blend (both, weighted by "
               "mix) or isometric (SLIM). Islands keep their place, size and pins, and never fold.",
        .flags = [] {
            std::vector<FlagDef> f{uvSetFlag()};
            for (FlagDef& d : modeFlags())
                f.push_back(std::move(d));
            f.push_back({"pins", "", ArgType::Bool, true, "Respect pinned uvs (corner attribute uvPin)."});
            f.push_back(finalMeshFlag());
            return f;
        }(),
        .run = [](const Args& a) -> json {
            OptimizeOptions o = optimizeArgs(a);
            std::string set = uvSetOf(a);
            const bool final = a.get<bool>("finalMesh", true);
            return editUvMeshes("UV Optimize", K::Face, [&](geo::Mesh& mesh, const Ids& faces, Node& node) -> json {
                const std::span<const glm::vec2> was = mesh.cornerAttrs().get<glm::vec2>(set);
                const std::vector<glm::vec2> before(was.begin(), was.end());
                std::shared_ptr<const FinalMap> map;
                json result = symmetricUvEdit(set, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                    if (m.cornerAttrs().get<glm::vec2>(set).empty())
                        throw CommandError("uvOptimize: no " + set + " uvs; unwrap first");
                    std::vector<Chart> charts = uvIslands(m, f, set);
                    std::vector<OptimizeResult> res(charts.size());
                    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) { res[i] = optimizeChart(charts[i], o); });
                    writeCharts(m, charts, set);
                    double area[2] = {0.0, 0.0}, angle[2] = {0.0, 0.0}, w = 0.0;
                    size_t failed = 0;
                    for (size_t i = 0; i < charts.size(); ++i)
                    {
                        if (!res[i].ok)
                        {
                            ++failed;
                            continue;
                        }
                        double a3 = charts[i].area3d();
                        area[0] += a3 * res[i].before.area;
                        area[1] += a3 * res[i].after.area;
                        angle[0] += a3 * res[i].before.angle;
                        angle[1] += a3 * res[i].after.angle;
                        w += a3;
                    }
                    w = std::max(w, 1e-300);
                    json out = {{"islands", charts.size()},
                                {"failed", failed},
                                {"areaDistortion", {area[0] / w, area[1] / w}},
                                {"angleDistortion", {angle[0] / w, angle[1] / w}}};
                    if (json fm = refineOnExport(m, f, set, n, finalOptions(o), final, &map); !fm.is_null())
                        out["finalMesh"] = std::move(fm);
                    return out;
                })(mesh, faces, node);
                // After the symmetry pass, which rewrites mirrored islands: islands the solve grew into a neighbour move off it.
                if (map)
                    keepIslandsApart(mesh, *map, set, before, sharedTextureTriangles(node, mesh, set));
                return result;
            });
        },
    });

    ops::registerCommand({
        .name = "uvMosaic",
        .doc = "Split the selected uv islands that cannot flatten below threshold along short, hidden, creased cuts, then re-solve every piece with "
               "the optimise mode. Pieces keep the island's scale; the largest stays in place, the rest line up beside it.",
        .flags = [] {
            std::vector<FlagDef> f{uvSetFlag(),
                                   {"threshold", "", ArgType::Float, 0.08, "Largest stretch (symmetric Dirichlet excess) an island may keep.", {}, {}, 0.0},
                                   {"visibility", "", ArgType::Bool, true, "Hide cuts where they are least visible."}};
            for (FlagDef& d : modeFlags())
                f.push_back(std::move(d));
            f.push_back(finalMeshFlag());
            return f;
        }(),
        .run = [](const Args& a) -> json {
            const bool final = a.get<bool>("finalMesh", true);
            MosaicOptions o;
            o.threshold = std::max(0.0, a.get<double>("threshold", 0.08));
            o.visibility = a.get<bool>("visibility", true);
            o.optimize = optimizeArgs(a);
            std::string set = uvSetOf(a);
            return editUvMeshes("UV Mosaic", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvMosaic: no " + set + " uvs; unwrap first");
                MosaicResult r = mosaicFaces(m, f, set, o);
                json out = {{"islandsBefore", r.islandsBefore}, {"islandsAfter", r.islandsAfter}, {"split", r.split}};
                if (json fm = refineOnExport(m, f, set, n, finalOptions(o.optimize), final); !fm.is_null())
                    out["finalMesh"] = std::move(fm);
                return out;
            });
        },
    });

    ops::registerCommand({
        .name = "uvUnwrapSymmetric",
        .doc = "Symmetry-aware unwrap of the selected faces using the mesh's symmetry pairs: islands on one side are solved and mirrored onto "
               "their partners (flip lays the mirror image beside, stack shares the same uvs), islands across the plane come out exactly "
               "mirror symmetric. Seams are mirrored first so islands pair up. With radial symmetry, one wedge's faces are unwrapped and "
               "every other wedge's copies stack on them.",
        .flags = {uvSetFlag(),
                  {"axis", "", T::String, "auto", "x, y, z, or auto: the mesh's symmetry axis, else x.", {"auto", "x", "y", "z"}},
                  {"mode", "", T::String, "flip", "flip or stack.", {"flip", "stack"}},
                  {"method", "", T::String, "slim", "slim, conformal or bff.", {"slim", "conformal", "bff"}},
                  {"mirrorSeams", "", T::Bool, true, "Copy seams across the plane first."},
                  {"pack", "", T::Bool, false, "Pack into 0-1 afterwards."}},
        .run = [](const Args& a) -> json {
            SymmetryUvOptions o;
            std::string axis = a.get<std::string>("axis", "auto");
            std::string mode = a.get<std::string>("mode", "flip");
            std::string method = a.get<std::string>("method", "slim");
            o.mode = mode == "stack" ? MirrorUvMode::Stack : MirrorUvMode::Flip;
            o.unwrap.method = method == "conformal" ? UnwrapMethod::Conformal : method == "bff" ? UnwrapMethod::Bff : UnwrapMethod::Slim;
            o.mirrorSeams = a.get<bool>("mirrorSeams", true);
            o.pack = a.get<bool>("pack", false);
            std::string set = uvSetOf(a);
            return editUvMeshes("UV Unwrap Symmetric", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                if (const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n); axis == "auto" && spec && spec->radial)
                {
                    // Radial: one wedge's faces are unwrapped and every other wedge's copies stack on them.
                    geo::SymmetryMap map = geo::SymmetryMap::persistent(m, *spec->radial, std::max(1e-4, glm::length(m.bounds().size()) * 1e-5));
                    std::vector<Index> faces(f.begin(), f.end());
                    if (faces.empty())
                        for (Index i = 0; i < m.faceCapacity(); ++i)
                            if (m.faceValid(i))
                                faces.push_back(i);
                    const int drive = radialFaceWedge(m, map, faces);
                    std::vector<Index> wedge;
                    for (Index i : faces)
                        if (m.faceValid(i) && radialFaceWedge(m, map, std::vector<Index>{i}) == drive)
                            wedge.push_back(i);
                    UnwrapFacesOptions uo;
                    uo.unwrap = o.unwrap;
                    uo.pack = o.pack;
                    UnwrapFacesResult r = unwrapFaces(m, wedge, set, uo);
                    const size_t stacked = stackRadialUvs(m, map, set, drive, wedge);
                    return {{"islands", r.islands}, {"stacked", stacked}, {"seamsAdded", r.seamsAdded}, {"axis", "radial"}};
                }
                SymmetryUvOptions oo = o;
                if (axis == "x" || axis == "y" || axis == "z")
                    oo.axis = axis[0] - 'x';
                else
                {
                    int64_t stored = n.as<int64_t>(scene::MeshNode::kSymmetry);
                    oo.axis = stored >= 1 && stored <= 3 ? int(stored - 1) : 0;
                }
                oo.offset = scene::symmetryOffset(n, oo.axis);
                SymmetryUvResult r = unwrapSymmetric(m, f, set, oo);
                return {{"islands", r.islands},
                        {"pairs", r.pairs},
                        {"acrossPlane", r.selfSymmetric},
                        {"unpaired", r.asymmetric},
                        {"seamsAdded", r.seamsAdded},
                        {"axis", std::string(1, char('x' + oo.axis))}};
            });
        },
    });

    ops::registerCommand({
        .name = "uvStackRadial",
        .doc = "Stack a radially symmetric mesh's uvs: one wedge's faces give their uvs to their copies in every other wedge, so all the "
               "copies share one patch of texture (mirrored wedges as mirror-image islands). Needs radial symmetry on the mesh.",
        .flags = {uvSetFlag(), {"wedge", "", T::Int, -1, "The wedge whose uvs are kept; -1: the one most of the selected faces are in, else 0.", {}, {}, -1.0}},
        .run = [](const Args& a) -> json {
            std::string set = uvSetOf(a);
            const int wedge = a.get<int>("wedge");
            return editUvMeshes("UV Stack Radial", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
                if (!spec || !spec->radial)
                    throw CommandError("uvStackRadial: " + n.path() + " has no radial symmetry");
                geo::SymmetryMap map = geo::SymmetryMap::persistent(m, *spec->radial, std::max(1e-4, glm::length(m.bounds().size()) * 1e-5));
                if (wedge >= map.order())
                    throw CommandError(std::format("uvStackRadial: {} has wedges 0 to {}", n.path(), map.order() - 1));
                const int drive = wedge >= 0 ? wedge : f.empty() ? 0 : radialFaceWedge(m, map, f);
                return stackRadialUvs(m, map, set, drive);
            });
        },
    });

    ops::registerCommand({
        .name = "uvStraightenEdges",
        .doc = "Straighten the selected edge chains in uv (edge loops, borders, seam sides): each goes onto its best-fit line with its uv length "
               "kept, and the rest of its island is re-solved around it.",
        .flags = {uvSetFlag(), {"iterations", "", T::Int, 12, "SLIM iterations for the re-solve.", {}, {}, 0.0, 200.0}},
        .run = [](const Args& a) -> json {
            std::string set = uvSetOf(a);
            int iterations = std::clamp(a.get<int>("iterations", 12), 0, 200);
            return editUvMeshes("UV Straighten Edges", K::Edge, symmetricUvEdit(set, [&](geo::Mesh& m, const Ids& e, Node&) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvStraightenEdges: no " + set + " uvs; unwrap first");
                std::vector<uint8_t> sel = edgeMask(m, e);
                std::vector<Chart> charts = uvIslands(m, islandFacesOfEdges(m, e, set), set);
                std::atomic<size_t> chains{0};
                tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) { chains += straightenEdges(charts[i], m, sel, iterations); });
                writeCharts(m, charts, set);
                return {{"chains", chains.load()}};
            }), false);
        },
    });

    ops::registerCommand({
        .name = "uvAlignToEdge",
        .doc = "Rotate each island with selected edges so their direction lies along U or V: axis=u or v aligns an island side, nearest "
               "orients the island by the edge.",
        .flags = {uvSetFlag(), {"axis", "", T::String, "nearest", "u, v, or nearest.", {"nearest", "u", "v"}}},
        .run = [](const Args& a) -> json {
            std::string set = uvSetOf(a);
            std::string axis = a.get<std::string>("axis", "nearest");
            AlignAxis ax = axis == "u" ? AlignAxis::U : axis == "v" ? AlignAxis::V : AlignAxis::Nearest;
            return editUvMeshes("UV Align To Edge", K::Edge, symmetricUvEdit(set, [&](geo::Mesh& m, const Ids& e, Node&) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvAlignToEdge: no " + set + " uvs; unwrap first");
                std::vector<uint8_t> sel = edgeMask(m, e);
                std::vector<Chart> charts = uvIslands(m, islandFacesOfEdges(m, e, set), set);
                size_t turned = 0;
                for (Chart& ch : charts)
                    turned += alignToEdges(ch, m, sel, ax) ? 1 : 0;
                writeCharts(m, charts, set);
                return {{"islands", turned}};
            }), false);
        },
    });
}
} // namespace lila::uv
