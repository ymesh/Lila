#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/LiveSession.h"
#include "uv/Uv.h"
#include "uv/UvCommands.h"
#include "uv/UvEdit.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>

namespace lila::uv
{
namespace
{
using scene::ComponentMode;
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::Node;
using scene::components::Ids;

/** Solve iterations for islands a relax leaves with slivers on the export: twice a plain solve's, as those are slow to open. */
constexpr int kRepairIterations = 60;

std::string setArg(const Args& a)
{
    return a.has("uvSet") ? a.get<std::string>("uvSet") : currentUvSet();
}

FlagDef setFlag()
{
    return {"uvSet", "", ArgType::String, nullptr, "UV set; the current one (uvSetCurrent) when not given."};
}

/** Every face of the uv islands (split along seams too) that hold any of the faces. All faces when faces is empty. */
std::vector<Index> islandFacesOf(const geo::Mesh& m, std::span<const Index> faces, std::string_view set)
{
    std::vector<uint8_t> seams = seamFlags(m);
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = set;
    co.extraSeams = seams;
    std::vector<std::vector<Index>> islands = islandFaces(m, {}, co);
    std::vector<Index> out;
    if (faces.empty())
    {
        for (auto& is : islands)
            out.insert(out.end(), is.begin(), is.end());
    }
    else
    {
        std::vector<uint8_t> want(m.faceCapacity(), 0);
        for (Index f : faces)
            if (f < want.size())
                want[f] = 1;
        for (auto& is : islands)
            if (std::any_of(is.begin(), is.end(), [&](Index f) { return want[f] != 0; }))
                out.insert(out.end(), is.begin(), is.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Index> facesOfEdges(const geo::Mesh& m, std::span<const Index> edges)
{
    std::vector<Index> out;
    for (Index e : edges)
        if (m.edgeValid(e))
            m.forEachEdgeCorner(e, [&](Index c) { out.push_back(m.cornerFace(c)); });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct RelaxStats
{
    size_t islands = 0;
    int iterations = 0;
    size_t folded = 0;
    bool settled = true;
    // Some island was relaxed on the exported mesh.
    bool exported = false;

    std::string warning;

    json toJson() const
    {
        json j = {{"islands", islands}, {"iterations", iterations}, {"folded", folded}, {"settled", settled}};
        if (!warning.empty())
            j["finalMesh"] = {{"method", "cage"}, {"warning", warning}};
        return j;
    }
};

/** Relaxes the islands holding the faces (whole islands; all when faces is empty) with the live solver, synchronously. */
RelaxStats relaxIslands(geo::Mesh& m, std::span<const Index> faces, std::string_view set, const LiveOptions& o, int iterations,
                        bool flatten = false, bool usePins = true, const Stack& stack = {})
{
    LILA_ZONE();

    RelaxStats st;
    if (m.cornerAttrs().get<glm::vec2>(set).empty() || iterations <= 0)
        return st;
    std::vector<Index> scope = islandFacesOf(m, faces, set);
    if (scope.empty())
        return st;
    std::vector<LiveIsland> islands = liveIslands(m, scope, set, o);
    if (!stack.empty())
        if (FinalMap map = attachFinal(islands, m, set, stack); !map.warning.empty())
            st.warning = map.warning;
    std::vector<glm::ivec2> tiles;
    for (const LiveIsland& is : islands)
        tiles.push_back(glm::ivec2(glm::floor(is.chart().uvCentroid())));
    std::vector<int> iters(islands.size(), 0);
    tbb::parallel_for(size_t(0), islands.size(), size_t(1), [&](size_t i) {
        LiveIsland& is = islands[i];
        if (!usePins)
            for (Index v = 0; v < is.vertexCount(); ++v)
                is.setPinned(v, false);
        if (flatten)
            is.flatten();
        iters[i] = is.solve(iterations);
    });
    for (size_t i = 0; i < islands.size(); ++i)
    {
        st.iterations = std::max(st.iterations, iters[i]);
        st.folded += islands[i].folds();
        st.settled &= islands[i].settled() && iters[i] < iterations;
    }
    st.islands = islands.size();
    // Islands relaxed on the exported mesh can grow; they stay in their UDIM tile.
    for (size_t i = 0; i < islands.size(); ++i)
        if (islands[i].hasFinal() && std::ranges::none_of(islands[i].chart().pinned, [](uint8_t p) { return p != 0; }))
        {
            Chart c = islands[i].chart();
            keepInUdimTile(c, tiles[i]);
            for (Index v = 0; v < c.vertexCount(); ++v)
                if (c.uv[v] != islands[i].chart().uv[v])
                    islands[i].setUv(v, c.uv[v]);
        }
    writeLiveIslands(m, islands, set, false);
    st.exported = std::ranges::any_of(islands, [](const LiveIsland& is) { return is.hasFinal(); });
    return st;
}

/**
 * The relax reads a fit's respacing as it was; where the new uvs respace the border into slivers, those islands solve
 * again on the export.
 */
void repairRelaxed(geo::Mesh& m, std::span<const Index> faces, std::string_view set, bool usePins, const Stack& stack)
{
    FinalSolveOptions fo;
    fo.usePins = usePins;
    fo.iterations = kRepairIterations;
    repairFinal(m, islandFacesOf(m, faces, set), set, stack, fo);
}

LiveOptions optionsArgs(const Args& a)
{
    LiveOptions o;
    o.balance = std::clamp(a.get<double>("balance", 0.0), -1.0, 1.0);
    o.boundarySmooth = std::clamp(a.get<double>("boundarySmooth", 0.0), 0.0, 1.0);
    o.slimSteps = std::clamp(a.get<int>("slim", 0), 0, 100);
    o.keepScale = a.get<bool>("keepScale", true);
    return o;
}

std::vector<FlagDef> optionFlags()
{
    using T = ArgType;
    return {{"balance", "", T::Float, 0.0, "Stretch against compression, -1..1: positive tolerates stretch and fixes compression, negative the reverse.", {}, {}, -1.0, 1.0},
            {"boundarySmooth", "", T::Float, 0.0, "Pull of the borders towards a smooth outline, 0..1.", {}, {}, 0.0, 1.0},
            {"slim", "", T::Int, 0, "SLIM iterations once settled, for a fold-free low-distortion finish.", {}, {}, 0.0, 100.0},
            {"keepScale", "", T::Bool, true, "Keep each island's size; off solves at 3D scale."}};
}

ComponentMode editKind()
{
    ComponentMode k = scene::components::mode();
    return k == ComponentMode::Object ? ComponentMode::Face : k;
}

/** Explicit edges (edges= flag) on one named mesh (the first object argument or selected mesh), as one undo step. */
json editEdges(const char* label, const Args& a, const std::function<json(geo::Mesh&, const std::vector<Index>&)>& fn)
{
    Graph& g = Graph::current();
    Node* n = nullptr;
    for (const std::string& name : a.objects())
        if (Node* c = g.find(name); c && c->type().isA("mesh"))
        {
            n = c;
            break;
        }
    if (!n)
        throw CommandError(std::string(label) + ": name a mesh, or select one, for edges=");
    std::vector<Index> edges;
    for (const json& e : a.flags["edges"])
        edges.push_back(e.get<Index>());
    UndoChunk chunk(label);
    scene::PlugRef base = uvEditPlug(*n);
    geo::Mesh mesh = *std::get<MeshPtr>(g.storedValue(base));
    json r = fn(mesh, withMirrorEdges(*n, mesh, edges));
    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return r;
}
} // namespace

void registerLiveCommands()
{
    using T = ArgType;
    live::installAutoCommit();

    ops::registerCommand({
        .name = "uvLiveCommit",
        .doc = "Applies the result of a live uv drag, brush stroke or relax from the UV editor as one undo step. Only the live tools call it.",
        .flags = {{"live", "", T::Int, nullptr, "Token of the staged result."}},
        .run = [](const Args& a) -> json {
            scene::NodeId node = scene::kNoNode;
            MeshPtr mesh = live::takeStaged(uint64_t(a.get<int64_t>("live", 0)), node);
            Graph& g = Graph::current();
            Node* n = mesh ? g.node(node) : nullptr;
            if (!n)
                throw CommandError("uvLiveCommit: nothing staged under that token (it is only for the live uv tools)");
            g.setValue(uvEditPlug(*n), mesh);
            return n->path();
        },
    });

    std::vector<FlagDef> relaxFlags = {setFlag(),
                                       {"iterations", "", T::Int, 200, "Most local/global iterations; it stops once settled.", {}, {}, 1.0, 100000.0},
                                       {"flatten", "", T::Bool, false, "Solve from scratch first (LSCM/SLIM), keeping size and placement."},
                                       {"pins", "", T::Bool, true, "Respect pinned uvs (uvPin)."},
                                       finalMeshFlag()};
    for (const FlagDef& f : optionFlags())
        relaxFlags.push_back(f);
    ops::registerCommand({
        .name = "uvLiveRelax",
        .doc = "Relax the selected faces' uv islands with the live solver (ARAP with a stretch/compress balance, stiffness and pins), as "
               "the UV editor's Relax does, but synchronously. Never folds a triangle that was not folded.",
        .flags = relaxFlags,
        .run = [](const Args& a) -> json {
            LiveOptions o = optionsArgs(a);
            int iterations = std::clamp(a.get<int>("iterations", 200), 1, 100000);
            bool flatten = a.get<bool>("flatten", false), pins = a.get<bool>("pins", true), final = a.get<bool>("finalMesh", true);
            std::string set = setArg(a);
            return editUvMeshes("UV Live Relax", ComponentMode::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                const Stack stack = final ? exportStack(n) : Stack{};
                RelaxStats st;
                withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&] {
                    st = relaxIslands(m, f, set, o, iterations, flatten, pins, stack);
                    return json();
                });
                // After the symmetry pass, which rewrites mirrored islands the repair has to see.
                if (st.exported)
                    withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&] {
                        repairRelaxed(m, f, set, pins, stack);
                        return json();
                    });
                if (!st.warning.empty())
                    log::warn("{}: {}", n.name(), st.warning);
                return st.toJson();
            });
        },
    });

    auto pinCommand = [](const char* name, const char* label, bool on) {
        std::vector<FlagDef> flags = {setFlag()};
        if (!on)
            flags.push_back({"all", "", T::Bool, false, "Unpin every uv of the meshes."});
        ops::registerCommand({
            .name = name,
            .doc = on ? "Pin the uvs of the selected components so the live solver, unwrap and relax keep them in place."
                      : "Unpin the uvs of the selected components (all=True: of the whole meshes).",
            .flags = flags,
            .run = [label, on](const Args& a) -> json {
                std::string set = setArg(a);
                bool all = !on && a.get<bool>("all", false);
                ComponentMode kind = editKind();
                return editUvMeshes(label, kind, [&](geo::Mesh& m, const Ids& ids, Node& n) -> json {
                    std::span<bool> pin = m.cornerAttrs().getOrAdd<bool>("uvPin");
                    if (all || (!on && ids.empty() && scene::components::mode() == ComponentMode::Object))
                    {
                        std::fill(pin.begin(), pin.end(), false);
                        return {{"corners", 0}};
                    }
                    if (ids.empty())
                        throw CommandError(std::string(on ? "uvPin" : "uvUnpin") + ": select uv components");
                    std::vector<Index> corners = withMirrorCorners(n, m, selectionCorners(m, n.id(), kind, ids, set));
                    for (Index c : corners)
                        pin[c] = on;
                    return {{"corners", corners.size()}};
                });
            },
        });
    };
    pinCommand("uvPin", "UV Pin", true);
    pinCommand("uvUnpin", "UV Unpin", false);

    ops::registerCommand({
        .name = "uvStiffness",
        .doc = "Set how stiff the selected components' uvs are for the live solver: 1 normal, up to 20 rigid, down to 0.05 floppy.",
        .flags = {setFlag(), {"value", "", T::Float, 1.0, "Stiffness, 0.05..20.", {}, {}, 0.05, 20.0}},
        .run = [](const Args& a) -> json {
            std::string set = setArg(a);
            float k = float(std::clamp(a.get<double>("value", 1.0), 0.05, 20.0));
            ComponentMode kind = editKind();
            return editUvMeshes("UV Stiffness", kind, [&](geo::Mesh& m, const Ids& ids, Node& n) -> json {
                std::span<float> st = m.cornerAttrs().getOrAdd<float>("uvStiffness");
                std::vector<Index> corners = withMirrorCorners(n, m, selectionCorners(m, n.id(), kind, ids, set));
                for (Index c : corners)
                    st[c] = k;
                return {{"corners", corners.size()}};
            });
        },
    });

    ops::registerCommand({
        .name = "uvCut",
        .doc = "Cut the uvs along the selected edges (or edges= on the named mesh): marks them as seams, splits the uvs there and relaxes "
               "the affected islands so the cut opens.",
        .positionalDoc = "[mesh]",
        .flags = {setFlag(),
                  {"edges", "", T::List, nullptr, "Edge ids to cut instead of the selection (with the mesh named)."},
                  {"relax", "", T::Bool, true, "Relax the affected islands afterwards."},
                  {"iterations", "", T::Int, 40, "Relax iterations.", {}, {}, 0.0, 100000.0},
                  {"balance", "", T::Float, 0.0, "Stretch against compression for the relax, -1..1.", {}, {}, -1.0, 1.0}},
        .run = [](const Args& a) -> json {
            std::string set = setArg(a);
            bool relax = a.get<bool>("relax", true);
            int iterations = std::clamp(a.get<int>("iterations", 40), 0, 100000);
            LiveOptions o;
            o.balance = std::clamp(a.get<double>("balance", 0.0), -1.0, 1.0);
            auto cut = [&](geo::Mesh& m, const std::vector<Index>& edges) -> json {
                std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
                size_t n = 0;
                for (Index e : edges)
                    if (m.edgeValid(e) && !m.isBoundaryEdge(e))
                    {
                        seam[e] = true;
                        ++n;
                    }
                std::vector<Index> faces = facesOfEdges(m, edges);
                RelaxStats st = relax ? relaxIslands(m, faces, set, o, iterations) : RelaxStats{};
                json j = st.toJson();
                j["cut"] = n;
                if (!relax && !m.cornerAttrs().get<glm::vec2>(set).empty())
                    j["islands"] = liveIslands(m, islandFacesOf(m, faces, set), set).size();
                return j;
            };
            if (a.has("edges"))
                return editEdges("UV Cut", a, cut);
            return editUvMeshes("UV Cut", ComponentMode::Edge, [&](geo::Mesh& m, const Ids& e, Node& n) -> json {
                return cut(m, withMirrorEdges(n, m, e));
            }, false);
        },
    });

    ops::registerCommand({
        .name = "uvSew",
        .doc = "Sew the uvs along the selected edges (or edges= on the named mesh): unmarks the seams and welds the two sides. With move "
               "(move and sew) the smaller island first travels to meet the other; then the joined island relaxes.",
        .positionalDoc = "[mesh]",
        .flags = {setFlag(),
                  {"edges", "", T::List, nullptr, "Edge ids to sew instead of the selection (with the mesh named)."},
                  {"move", "", T::Bool, true, "Move the smaller island onto the other before welding."},
                  {"relax", "", T::Bool, true, "Relax the joined island afterwards."},
                  {"iterations", "", T::Int, 40, "Relax iterations.", {}, {}, 0.0, 100000.0},
                  {"balance", "", T::Float, 0.0, "Stretch against compression for the relax, -1..1.", {}, {}, -1.0, 1.0}},
        .run = [](const Args& a) -> json {
            std::string set = setArg(a);
            bool move = a.get<bool>("move", true), relax = a.get<bool>("relax", true);
            int iterations = std::clamp(a.get<int>("iterations", 40), 0, 100000);
            LiveOptions o;
            o.balance = std::clamp(a.get<double>("balance", 0.0), -1.0, 1.0);
            auto sew = [&](geo::Mesh& m, const std::vector<Index>& edges) -> json {
                json j = sewUvEdges(m, edges, set, move);
                std::vector<Index> faces = facesOfEdges(m, edges);
                RelaxStats st = relax ? relaxIslands(m, faces, set, o, iterations) : RelaxStats{};
                json r = st.toJson();
                r["sewn"] = j.value("sewn", 0);
                if (!relax && !m.cornerAttrs().get<glm::vec2>(set).empty())
                    r["islands"] = liveIslands(m, islandFacesOf(m, faces, set), set).size();
                return r;
            };
            if (a.has("edges"))
                return editEdges("UV Sew", a, sew);
            return editUvMeshes("UV Sew", ComponentMode::Edge, [&](geo::Mesh& m, const Ids& e, Node& n) -> json {
                return sew(m, withMirrorEdges(n, m, e));
            }, false);
        },
    });
}
} // namespace lila::uv
