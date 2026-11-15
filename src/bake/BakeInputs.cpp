#include "bake/BakeInputs.h"

#include "bake/BakeCache.h"

#include "bake/BakeCommon.h"
#include "bake/BakeNodes.h"
#include "bake/HighFiles.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Triangulate.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <tbb/parallel_for.h>

#include <format>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace lila::bake
{
namespace
{
using detail::exportedMesh;
using detail::worldOf;

struct NoUndo
{
    bool was = UndoStack::get().enabled();
    NoUndo() { UndoStack::get().setEnabled(false); }
    ~NoUndo() { UndoStack::get().setEnabled(was); }
};

std::span<const glm::vec3> shadingNormals(const geo::Mesh& mesh, std::vector<glm::vec3>& storage)
{
    std::span<const glm::vec3> stored = mesh.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (stored.size() == mesh.cornerCapacity())
        return stored;
    storage = geo::cornerNormals(mesh);
    return storage;
}

struct Xform
{
    glm::dmat4 world;
    glm::dmat3 normal;
    bool mirrored = false;

    explicit Xform(const glm::dmat4& m) : world(m), normal(glm::transpose(glm::inverse(glm::dmat3(m)))), mirrored(glm::determinant(glm::dmat3(m)) < 0.0) {}
    glm::vec3 point(const glm::dvec3& p) const { return glm::vec3(glm::dvec3(world * glm::dvec4(p, 1.0))); }
    glm::vec3 dir(const glm::vec3& n) const
    {
        const glm::dvec3 v = normal * glm::dvec3(n);
        const double l = glm::length(v);
        return l > 0.0 ? glm::vec3(v / l) : glm::vec3(0.0f, 0.0f, 1.0f);
    }
    glm::vec3 vector(const glm::vec3& t) const { return glm::vec3(glm::dmat3(world) * glm::dvec3(t)); }
};

/** Corner order of a soup triangle: mirrored transforms swap corners 1 and 2 so front faces stay counter-clockwise. */
std::array<int, 3> cornerOrder(bool mirrored) { return mirrored ? std::array<int, 3>{0, 2, 1} : std::array<int, 3>{0, 1, 2}; }

void finishBounds(MeshSoup& s)
{
    if (s.positions.empty())
        return;
    s.boundsMin = s.boundsMax = s.positions.front();
    for (const glm::vec3& p : s.positions)
    {
        s.boundsMin = glm::min(s.boundsMin, p);
        s.boundsMax = glm::max(s.boundsMax, p);
    }
}

MeshSoup soupOn(const geo::Mesh& mesh, const geo::Triangulation& tris, std::span<const glm::vec3> normals, const Xform& x, std::string name)
{
    MeshSoup s;
    s.name = std::move(name);
    const size_t n = tris.faces.size();
    s.positions.resize(n * 3);
    s.normals.resize(n * 3);
    s.materials.resize(n);
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    if (uv.size() == mesh.cornerCapacity() && !uv.empty())
        s.uvs.resize(n * 3);
    std::span<const glm::vec4> colors;
    const std::vector<std::string> sets = geo::colors::sets(mesh);
    if (!sets.empty())
        colors = geo::colors::values(mesh, sets.front());
    if (!colors.empty() && colors.size() == mesh.cornerCapacity())
        s.colors.resize(n * 3);
    std::vector<std::span<const glm::vec4>> more;
    for (size_t k = 0; k < sets.size(); ++k)
    {
        s.colorSetNames.push_back(sets[k]);
        if (!k)
            continue;
        std::span<const glm::vec4> v = geo::colors::values(mesh, sets[k]);
        const bool ok = v.size() == mesh.cornerCapacity() && !v.empty();
        more.push_back(ok ? v : std::span<const glm::vec4>());
        s.moreColors.emplace_back(ok ? n * 3 : 0);
    }
    std::span<const int32_t> mat = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);

    // Slot names in id order, so per-triangle ids index them directly.
    int32_t maxId = 0;
    for (int32_t id : mat)
        maxId = std::max(maxId, id);
    const int32_t slots = std::max<int32_t>(int32_t(mesh.materialNames().size()), mat.empty() ? 1 : maxId + 1);
    for (int32_t id = 0; id < slots; ++id)
        s.materialNames.push_back(scene::materials::slotName(mesh, id));

    const auto order = cornerOrder(x.mirrored);
    tbb::parallel_for(size_t(0), n, [&](size_t t) {
        for (int k = 0; k < 3; ++k)
        {
            const geo::Index c = tris.corners[t * 3 + size_t(order[size_t(k)])];
            const size_t o = t * 3 + size_t(k);
            s.positions[o] = x.point(mesh.position(mesh.cornerPoint(c)));
            s.normals[o] = x.dir(normals[c]);
            if (!s.uvs.empty())
                s.uvs[o] = uv[c];
            if (!s.colors.empty())
                s.colors[o] = colors[c];
            for (size_t m = 0; m < more.size(); ++m)
                if (!more[m].empty())
                    s.moreColors[m][o] = more[m][c];
        }
        const geo::Index f = tris.faces[t];
        s.materials[t] = f < mat.size() ? uint32_t(std::max(0, mat[f])) : 0u;
    });
    finishBounds(s);
    return s;
}

/** UV islands per face: faces joined across edges whose two corners carry the same UVs on both sides. */
std::vector<uint32_t> faceIslands(const geo::Mesh& mesh, uint32_t& count)
{
    std::vector<geo::Index> parent(mesh.faceCapacity());
    std::iota(parent.begin(), parent.end(), geo::Index(0));
    auto root = [&](geo::Index f) {
        while (parent[f] != f)
            f = parent[f] = parent[parent[f]];
        return f;
    };
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    auto uvOf = [&](geo::Index c) { return uv.empty() ? glm::vec2(0.0f) : uv[c]; };
    auto same = [](const glm::vec2& a, const glm::vec2& b) { return glm::all(glm::lessThan(glm::abs(a - b), glm::vec2(1e-6f))); };
    for (geo::Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        const geo::Index c0 = mesh.edgeCorner(e);
        if (c0 == geo::kInvalid)
            continue;
        for (geo::Index c1 = mesh.cornerRadial(c0); c1 != c0 && c1 != geo::kInvalid; c1 = mesh.cornerRadial(c1))
        {
            const geo::Index a = mesh.cornerPoint(c0);
            const glm::vec2 a0 = uvOf(c0), b0 = uvOf(mesh.cornerNext(c0));
            const bool aligned = mesh.cornerPoint(c1) == a;
            const glm::vec2 a1 = aligned ? uvOf(c1) : uvOf(mesh.cornerNext(c1));
            const glm::vec2 b1 = aligned ? uvOf(mesh.cornerNext(c1)) : uvOf(c1);
            if (same(a0, a1) && same(b0, b1))
                parent[root(mesh.cornerFace(c0))] = root(mesh.cornerFace(c1));
        }
    }
    std::vector<uint32_t> island(mesh.faceCapacity(), 0);
    std::vector<uint32_t> id(mesh.faceCapacity(), ~0u);
    count = 0;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const geo::Index r = root(f);
        if (id[r] == ~0u)
            id[r] = count++;
        island[f] = id[r];
    }
    return island;
}

scene::Node& meshNode(scene::Graph& g, const std::string& name, const std::string& group, const char* role)
{
    scene::Node* n = nullptr;
    try
    {
        n = g.find(name);
    }
    catch (const std::exception&)
    {
    }
    if (!n || !n->type().isA("mesh"))
        throw std::runtime_error("bake group " + group + ": " + role + " '" + name + "' is not a mesh in the scene");
    return *n;
}

float diagonal(const glm::vec3& lo, const glm::vec3& hi) { return glm::length(hi - lo); }
} // namespace

MeshSoup soupFromMesh(const geo::Mesh& mesh, const glm::dmat4& world, std::string name)
{
    LILA_ZONE();

    const geo::Triangulation tris = geo::triangulate(mesh);
    std::vector<glm::vec3> storage;
    return soupOn(mesh, tris, shadingNormals(mesh, storage), Xform(world), std::move(name));
}

std::vector<glm::ivec2> triangleTiles(const LowInput& low)
{
    const MeshSoup& s = low.soup;
    const size_t n = s.uvs.size() == s.triangleCount() * 3 ? s.triangleCount() : 0;
    std::vector<glm::dvec3> sums(low.islandCount, glm::dvec3(0.0));
    for (size_t t = 0; t < n; ++t)
        if (t < low.islands.size() && low.islands[t] < low.islandCount)
        {
            const glm::vec2* uv = &s.uvs[t * 3];
            const double a = 0.5 * std::abs(double(uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) - double(uv[2].x - uv[0].x) * (uv[1].y - uv[0].y));
            const glm::dvec2 c = glm::dvec2(uv[0] + uv[1] + uv[2]) / 3.0;
            sums[low.islands[t]] += glm::dvec3(c * a, a);
        }
    std::vector<glm::ivec2> out(n);
    for (size_t t = 0; t < n; ++t)
    {
        const glm::vec2* uv = &s.uvs[t * 3];
        glm::dvec2 c = glm::dvec2(uv[0] + uv[1] + uv[2]) / 3.0;
        if (t < low.islands.size() && low.islands[t] < low.islandCount && sums[low.islands[t]].z > 0.0)
            c = glm::dvec2(sums[low.islands[t]]) / sums[low.islands[t]].z;
        out[t] = {int(std::floor(c.x)), int(std::floor(c.y))};
    }
    return out;
}

LowInput lowFromMesh(const geo::Mesh& mesh, const glm::dmat4& world, std::string name, const geo::Mesh* cage, const glm::dmat4& cageWorld)
{
    LILA_ZONE();

    LowInput low;
    const Xform x(world);
    const geo::Triangulation tris = geo::triangulate(mesh);
    std::vector<glm::vec3> storage;
    std::span<const glm::vec3> normals = shadingNormals(mesh, storage);
    low.soup = soupOn(mesh, tris, normals, x, std::move(name));
    const size_t n = tris.faces.size();

    const std::vector<glm::vec4> tangents = geo::cornerTangents(mesh, tris, normals, "uv0");
    const std::vector<glm::vec3> smooth = geo::pointNormals(mesh);
    std::span<const float> offsets = mesh.pointAttrs().get<float>("bakeOffset");
    std::span<const float> skews = mesh.pointAttrs().get<float>("bakeSkew");
    const bool useCage = cage && cage->pointCapacity() == mesh.pointCapacity();
    const Xform cx(cageWorld);
    std::vector<uint32_t> faceIsland = faceIslands(mesh, low.islandCount);

    low.tangents.resize(n * 3, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    low.cageNormals.resize(n * 3);
    low.cageScale.resize(n * 3, 1.0f);
    low.cageSkew.resize(n * 3, 0.0f);
    low.islands.resize(n);
    low.faces.resize(n);
    low.points.resize(n * 3);
    if (useCage)
        low.cagePositions.resize(n * 3);
    const auto order = cornerOrder(x.mirrored);
    tbb::parallel_for(size_t(0), n, [&](size_t t) {
        for (int k = 0; k < 3; ++k)
        {
            const geo::Index c = tris.corners[t * 3 + size_t(order[size_t(k)])];
            const geo::Index p = mesh.cornerPoint(c);
            const size_t o = t * 3 + size_t(k);
            low.points[o] = uint32_t(p);
            if (!tangents.empty())
            {
                const glm::vec4 tg = tangents[c];
                low.tangents[o] = glm::vec4(x.vector(glm::vec3(tg)), x.mirrored ? -tg.w : tg.w);
            }
            low.cageNormals[o] = x.dir(smooth[p]);
            if (p < offsets.size())
                low.cageScale[o] = offsets[p];
            if (p < skews.size())
                low.cageSkew[o] = skews[p];
            if (useCage)
                low.cagePositions[o] = cx.point(cage->position(p));
        }
        low.islands[t] = faceIsland[tris.faces[t]];
        low.faces[t] = uint32_t(tris.faces[t]);
    });
    return low;
}

BakeModifierStack::BakeModifierStack(const json& stack) : mGraph(std::make_unique<scene::Graph>())
{
    if (!stack.is_array())
        return;
    NoUndo noUndo;
    for (const json& entry : stack)
    {
        const std::string kind = entry.is_string() ? entry.get<std::string>() : entry.value("kind", std::string());
        const scene::NodeType* type = scene::types::find(kind + "Modifier");
        if (!type)
        {
            mWarnings.push_back("bake modifier kind '" + kind + "' is not registered; skipped");
            continue;
        }
        scene::Node* n = mGraph->createNode(*type, {}, scene::kNoNode, false);
        if (!scene::modifiers::asModifier(n))
        {
            mWarnings.push_back("'" + kind + "' is not a modifier; skipped");
            mGraph->deleteNode(n->id());
            continue;
        }
        // A high's uvs only place the textures it brings, and blending keeps them where the mesh under the stack has them.
        // Fit Surface would re-lay every island of a mesh 64 times denser: most of a bake's preparation, for uvs nothing
        // reads differently. A stack that names uvs gets what it names.
        if (kind == "subdivision")
            if (const int uvs = type->attrIndex("uvs"); uvs >= 0)
                mGraph->setValue({n->id(), uvs, -1}, int64_t(geo::SubdivideUvs::Blend));
        if (entry.is_object() && entry.contains("params") && entry["params"].is_object())
            for (auto it = entry["params"].begin(); it != entry["params"].end(); ++it)
            {
                const int a = type->attrIndex(it.key());
                if (a < 0)
                {
                    mWarnings.push_back(kind + ": no parameter '" + it.key() + "'");
                    continue;
                }
                try
                {
                    mGraph->setValue({n->id(), a, -1}, scene::fromJson(type->attrs[size_t(a)], it.value()));
                }
                catch (const std::exception& e)
                {
                    mWarnings.push_back(kind + "." + it.key() + ": " + e.what());
                }
            }
        mNodes.push_back(n->id());
    }
}

BakeModifierStack::~BakeModifierStack() = default;

geo::Mesh BakeModifierStack::apply(const geo::Mesh& mesh)
{
    LILA_ZONE();

    NoUndo noUndo;
    scene::MeshPtr cur = std::make_shared<geo::Mesh>(mesh);
    for (scene::NodeId id : mNodes)
    {
        scene::ModifierNode* mod = scene::modifiers::asModifier(mGraph->node(id));
        try
        {
            if (mod->apply)
                cur = std::make_shared<geo::Mesh>(mod->apply(*cur, *mod));
            else
            {
                mGraph->setValue({id, mod->inAttr, -1}, cur);
                mod->compute(scene::EvalContext{});
                const int out = mod->outAttr >= 0 ? mod->outAttr : mod->type().attrIndex("outMesh");
                if (out >= 0 && std::holds_alternative<scene::MeshPtr>(mod->value(out)) && mod->as<scene::MeshPtr>(out))
                    cur = mod->as<scene::MeshPtr>(out);
                if (!mod->error().empty())
                    mWarnings.push_back(mod->type().name + ": " + mod->error());
            }
        }
        catch (const std::exception& e)
        {
            mWarnings.push_back(mod->type().name + ": " + e.what() + " (passed through)");
        }
    }
    return *cur;
}

bool BakeModifierStack::pure() const
{
    for (scene::NodeId id : mNodes)
        if (!scene::modifiers::asModifier(mGraph->node(id))->apply)
            return false;
    return true;
}

geo::Mesh BakeModifierStack::applyPure(const geo::Mesh& mesh, std::vector<std::string>& warnings) const
{
    LILA_ZONE();

    scene::MeshPtr cur = std::make_shared<geo::Mesh>(mesh);
    for (scene::NodeId id : mNodes)
    {
        const scene::ModifierNode* mod = scene::modifiers::asModifier(mGraph->node(id));
        try
        {
            cur = std::make_shared<geo::Mesh>(mod->apply(*cur, *mod));
        }
        catch (const std::exception& e)
        {
            warnings.push_back(mod->type().name + ": " + e.what() + " (passed through)");
        }
    }
    return *cur;
}

json modifierPreset(std::string_view name)
{
    std::string key;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)))
            key += char(std::tolower(static_cast<unsigned char>(c)));
    auto smooth = [](int levels) { return json{{"kind", "subdivision"}, {"params", {{"levels", levels}}}}; };
    if (key == "none" || key.empty())
        return json::array();
    if (key == "smooth" || key == "smoothx2")
        return json::array({smooth(2)});
    if (key.size() == 7 && key.starts_with("smooth") && key[6] >= '1' && key[6] <= '4')
        return json::array({smooth(key[6] - '0')});
    if (key.size() == 8 && key.starts_with("smoothx") && key[7] >= '1' && key[7] <= '4')
        return json::array({smooth(key[7] - '0')});
    if (key == "autobevelsmooth")
    {
        const bool autoBevel = scene::types::find("autoBevelModifier") != nullptr;
        return json::array({json{{"kind", autoBevel ? "autoBevel" : "bevel"}, {"params", json::object()}}, smooth(2)});
    }
    return nullptr;
}

BakeInputs buildInputs(scene::Graph& g, scene::NodeId project, const std::vector<scene::NodeId>& groupIds, bool withHighs)
{
    LILA_ZONE();

    BakeInputs in;
    scene::Node* p = g.node(project);
    if (!p)
        throw std::runtime_error("bake: no project");
    in.project = p->name();
    const std::vector<scene::NodeId> ids = groupIds.empty() ? groupsOf(g, project) : groupIds;
    g.evaluate();

    // Main thread: evaluate meshes and bake stacks. Then the conversions run in parallel.
    struct Job
    {
        scene::MeshPtr mesh;
        glm::dmat4 world;
        std::string name;
        size_t group;
        bool low;
        size_t index;
        scene::MeshPtr cage;
        glm::dmat4 cageWorld{1.0};
        uint32_t sourceTriangles = 0;
        SnapshotKey key;
        std::shared_ptr<const HighInput> cachedHigh;
        std::shared_ptr<const LowInput> cachedLow;
    };
    std::vector<Job> jobs;
    /** A high whose bake stack is all plain mesh functions: run with the others, across the cores, once the jobs are listed. */
    struct StackRun
    {
        size_t job;
        std::shared_ptr<BakeModifierStack> stack;
        std::string group;
        std::vector<std::string> warnings;
    };
    std::vector<StackRun> stackRuns;
    double modifierSeconds = 0.0;
    int cacheHits = 0;
    beginSnapshotTally();
    // A step per mesh, not per group: one group can hold a whole asset, and each mesh runs its export stack here.
    double meshes = 0.0;
    for (scene::NodeId gid : ids)
        if (const scene::Node* gn = g.node(gid); gn && isGroup(*gn))
        {
            const GroupSettings s = groupSettings(*gn);
            meshes += double(s.lows.size());
            if (withHighs)
                for (const HighEntry& h : s.highs)
                    meshes += h.self ? double(s.lows.size()) : 1.0;
        }
    progress::Scope scope("Preparing bake inputs", std::max(meshes, 1.0), progress::Cancellable);
    for (scene::NodeId gid : ids)
    {
        scene::Node* gn = g.node(gid);
        if (!gn || !isGroup(*gn))
            continue;
        GroupInputs gi;
        gi.name = gn->name();
        gi.settings = groupSettings(*gn);
        const size_t gIndex = in.groups.size();
        scene::MeshPtr cage;
        glm::dmat4 cageWorld(1.0);
        if (!gi.settings.cage.empty())
        {
            scene::Node& c = meshNode(g, gi.settings.cage, gi.name, "cage");
            cage = exportedMesh(g, c.id());
            cageWorld = worldOf(g, c.id());
            if (gi.settings.lows.size() != 1)
                in.warnings.push_back("group " + gi.name + ": an explicit cage needs exactly one low; using the auto cage");
        }
        std::vector<std::pair<scene::MeshPtr, glm::dmat4>> lowMeshes;
        for (const std::string& name : gi.settings.lows)
        {
            scene::Node& n = meshNode(g, name, gi.name, "low");
            Job j{exportedMesh(g, n.id()), worldOf(g, n.id()), n.path(), gIndex, true, gi.lows.size()};
            if (cage && gi.settings.lows.size() == 1)
            {
                if (cage->pointCapacity() == j.mesh->pointCapacity())
                {
                    j.cage = cage;
                    j.cageWorld = cageWorld;
                }
                else
                    in.warnings.push_back("group " + gi.name + ": cage " + gi.settings.cage + " doesn't match the low's points; using the auto cage");
            }
            lowMeshes.emplace_back(j.mesh, j.world);
            uint64_t cageKey = 0;
            if (j.cage)
                cageKey = hashBytes(&j.cageWorld, sizeof(j.cageWorld), uint64_t(uintptr_t(j.cage.get())));
            j.key = snapshotKey(j.mesh, j.world, j.name, json(), cageKey);
            j.cachedLow = cachedLow(j.key);
            cacheHits += j.cachedLow ? 1 : 0;
            gi.lows.emplace_back();
            jobs.push_back(std::move(j));
            scope.step();
            scope.checkCancel();
        }
        if (withHighs)
            for (const HighEntry& h : gi.settings.highs)
            {
                std::vector<std::pair<scene::MeshPtr, glm::dmat4>> sources;
                std::vector<std::string> names;
                if (h.self)
                {
                    sources = lowMeshes;
                    for (const std::string& l : gi.settings.lows)
                        names.push_back(l + " (self)");
                }
                else if (!h.file.empty())
                {
                    try
                    {
                        for (const FileMesh& f : loadHighFile(h))
                        {
                            sources.emplace_back(f.mesh, f.world);
                            names.push_back(f.name);
                        }
                    }
                    catch (const std::runtime_error& e)
                    {
                        throw std::runtime_error("bake group " + gi.name + ": " + e.what());
                    }
                }
                else
                {
                    scene::Node& n = meshNode(g, h.node, gi.name, "high");
                    sources.emplace_back(exportedMesh(g, n.id()), worldOf(g, n.id()));
                    names.push_back(n.path());
                }
                const auto stackPtr = std::make_shared<BakeModifierStack>(h.modifiers);
                BakeModifierStack& stack = *stackPtr;
                const bool pure = stack.pure();
                for (size_t k = 0; k < sources.size(); ++k)
                {
                    scene::MeshPtr m = sources[k].first;
                    const SnapshotKey key = snapshotKey(m, sources[k].second, names[k], h.modifiers);
                    std::shared_ptr<const HighInput> cached = m ? cachedHigh(key) : nullptr;
                    const uint32_t before = cached ? cached->sourceTriangles : m ? uint32_t(geo::triangulate(*m).faces.size()) : 0u;
                    const bool later = m && !stack.empty() && !cached && pure;
                    if (later)
                        stackRuns.push_back({jobs.size(), stackPtr, gi.name, {}});
                    else if (m && !stack.empty() && !cached)
                    {
                        // The stack runs here without a step, often for seconds on a dense high.
                        scope.setStatus("Applying bake modifiers: " + names[k]);
                        scope.showNow();
                        const auto t0 = std::chrono::steady_clock::now();
                        m = std::make_shared<geo::Mesh>(stack.apply(*m));
                        modifierSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                        scope.setStatus({});
                    }
                    Job j{m, sources[k].second, names[k], gIndex, false, gi.highs.size()};
                    j.sourceTriangles = before;
                    j.key = key;
                    j.cachedHigh = std::move(cached);
                    cacheHits += j.cachedHigh ? 1 : 0;
                    gi.highs.emplace_back();
                    gi.highNodes.push_back(h.self ? gi.settings.lows[k] : !h.file.empty() ? h.file : names[k]);
                    gi.highSelfLow.push_back(h.self ? int32_t(k) : -1);
                    jobs.push_back(std::move(j));
                    // A stack left for later steps when it has run.
                    if (h.self && !later)
                    {
                        scope.step();
                        scope.checkCancel();
                    }
                }
                if (!h.self && (stackRuns.empty() || stackRuns.back().stack != stackPtr))
                {
                    scope.step();
                    scope.checkCancel();
                }
                for (const std::string& w : stack.warnings())
                    in.warnings.push_back("group " + gi.name + ": " + w);
            }
        if (gi.settings.lows.empty())
            in.warnings.push_back("group " + gi.name + " has no lows");
        else if (withHighs && gi.highs.empty())
            in.warnings.push_back("group " + gi.name + " has no highs");
        in.groups.push_back(std::move(gi));
        scope.checkCancel();
    }

    if (!stackRuns.empty())
    {
        // Each stack is single-threaded for much of its run; a mesh a core finishes an asset's highs several times sooner.
        scope.setStatus("Applying bake modifiers");
        scope.showNow();
        const auto t0 = std::chrono::steady_clock::now();
        tbb::parallel_for(size_t(0), stackRuns.size(), [&](size_t i) {
            if (scope.cancelled())
                return;
            StackRun& r = stackRuns[i];
            Job& j = jobs[r.job];
            j.mesh = std::make_shared<geo::Mesh>(r.stack->applyPure(*j.mesh, r.warnings));
            scope.step();
        });
        modifierSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        scope.setStatus({});
        scope.checkCancel();
        for (const StackRun& r : stackRuns)
            for (const std::string& w : r.warnings)
                in.warnings.push_back("group " + r.group + ": " + w);
    }

    const auto snapshotStarted = std::chrono::steady_clock::now();
    tbb::parallel_for(size_t(0), jobs.size(), [&](size_t i) {
        const Job& j = jobs[i];
        GroupInputs& gi = in.groups[j.group];
        // An abort leaves the rest unread; the check after the loop throws before anything uses them.
        if (!j.mesh || scope.cancelled())
            return;
        if (j.low)
        {
            if (j.cachedLow)
            {
                gi.lows[j.index] = *j.cachedLow;
                return;
            }
            auto low = std::make_shared<LowInput>(lowFromMesh(*j.mesh, j.world, j.name, j.cage.get(), j.cageWorld));
            gi.lows[j.index] = *low;
            cacheLow(j.key, std::move(low));
        }
        else
        {
            if (j.cachedHigh)
            {
                gi.highs[j.index] = *j.cachedHigh;
                return;
            }
            auto h = std::make_shared<HighInput>();
            h->soup = soupFromMesh(*j.mesh, j.world, j.name);
            h->source = j.name;
            h->sourceTriangles = j.sourceTriangles;
            gi.highs[j.index] = *h;
            cacheHigh(j.key, std::move(h));
        }
    });
    scope.checkCancel();
    in.timings["modifiers"] = modifierSeconds;
    in.timings["snapshot"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - snapshotStarted).count();
    in.timings["snapshotCacheHits"] = cacheHits;
    if (const SnapshotTally tally = snapshotTally(); tally.dropped)
    {
        const double gb = double(size_t(1) << 30);
        in.timings["snapshotCacheDropped"] = double(tally.dropped);
        in.timings["snapshotCacheBudget"] = double(tally.budget);
        in.warnings.push_back(std::format("snapshot cache full: {:.1f} GB of these meshes did not fit in its {:.1f} GB and are prepared again on "
                                          "every bake; raise the baker/snapshotCacheGb pref",
                                          double(tally.dropped) / gb, double(tally.budget) / gb));
    }

    for (const GroupInputs& gi : in.groups)
        for (const HighInput& h : gi.highs)
            for (const std::string& m : h.soup.materialNames)
                if (!in.materials.contains(m))
                    in.materials.emplace(m, materialInput(g, m));

    bool anyHigh = false;
    for (GroupInputs& gi : in.groups)
    {
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (const LowInput& l : gi.lows)
            if (l.soup.triangleCount())
            {
                lo = glm::min(lo, l.soup.boundsMin);
                hi = glm::max(hi, l.soup.boundsMax);
            }
        const float diag = lo.x <= hi.x ? diagonal(lo, hi) : 1.0f;
        gi.frontal = gi.settings.frontal > 0.0 ? float(gi.settings.frontal) : 0.03f * diag;
        gi.rear = gi.settings.rear > 0.0 ? float(gi.settings.rear) : gi.frontal;

        bool first = true;
        for (const HighInput& h : gi.highs)
        {
            if (!h.soup.triangleCount())
                continue;
            gi.highMin = first ? h.soup.boundsMin : glm::min(gi.highMin, h.soup.boundsMin);
            gi.highMax = first ? h.soup.boundsMax : glm::max(gi.highMax, h.soup.boundsMax);
            first = false;
        }
        if (first)
            continue;
        in.highMin = anyHigh ? glm::min(in.highMin, gi.highMin) : gi.highMin;
        in.highMax = anyHigh ? glm::max(in.highMax, gi.highMax) : gi.highMax;
        anyHigh = true;
    }
    buildLinks(in);
    return in;
}

namespace
{
/**
 * Whether any triangle of the group's high h has one of the materials. Links are per high, so a high that also has other
 * materials is skipped whole, with a warning.
 */
bool usesMaterial(BakeInputs& in, const GroupInputs& gi, size_t h, const std::vector<std::string>& materials)
{
    if (materials.empty() || h >= gi.highs.size())
        return false;
    const MeshSoup& soup = gi.highs[h].soup;
    std::vector<uint8_t> used(soup.materialNames.size(), 0);
    for (uint32_t id : soup.materials)
        if (id < used.size())
            used[id] = 1;
    bool match = false, other = false;
    for (size_t id = 0; id < used.size(); ++id)
        if (used[id])
            (std::ranges::find(materials, soup.materialNames[id]) != materials.end() ? match : other) = true;
    if (match && other)
    {
        std::string w = "group " + gi.name + ": " + gi.highNodes[h] + " is unlinked whole by material, though only some of its faces have it";
        if (std::ranges::find(in.warnings, w) == in.warnings.end())
            in.warnings.push_back(std::move(w));
    }
    return match;
}
} // namespace

void buildLinks(BakeInputs& in)
{
    uint32_t total = 0;
    for (GroupInputs& gi : in.groups)
    {
        gi.highBase = total;
        total += uint32_t(gi.highs.size());
        gi.links.clear();
        gi.linkWords = 0;
    }
    const uint32_t words = (total + 31) / 32;
    for (GroupInputs& gi : in.groups)
    {
        const size_t lows = gi.lows.size();
        auto set = [&](size_t low, RayKind kind, uint32_t high) {
            if (gi.links.empty())
            {
                gi.linkWords = words;
                gi.links.assign(lows * 2 * words, 0u);
            }
            gi.links[(low * 2 + size_t(kind)) * words + high / 32] |= 1u << (high % 32);
        };
        // A self high is its own low's: the other lows never project from it (they would bake each other's surfaces).
        for (size_t h = 0; h < gi.highSelfLow.size(); ++h)
            if (const int32_t own = gi.highSelfLow[h]; own >= 0 && lows > 1)
                for (size_t l = 0; l < lows; ++l)
                    if (l != size_t(own))
                        set(l, RayKind::Projection, gi.highBase + uint32_t(h));
        for (const Unlink& u : gi.settings.unlinks)
            for (size_t l = 0; l < lows && l < gi.settings.lows.size(); ++l)
            {
                if (!u.lows.empty() && std::ranges::find(u.lows, gi.settings.lows[l]) == u.lows.end())
                    continue;
                for (const GroupInputs& other : in.groups)
                    for (size_t h = 0; h < other.highNodes.size(); ++h)
                    {
                        if (std::ranges::find(u.meshes, other.highNodes[h]) == u.meshes.end() && !usesMaterial(in, other, h, u.materials))
                            continue;
                        if (u.occlusion)
                            set(l, RayKind::Occlusion, other.highBase + uint32_t(h));
                        if (u.projection && &other == &gi)
                            set(l, RayKind::Projection, other.highBase + uint32_t(h));
                    }
            }
    }
}
} // namespace lila::bake
