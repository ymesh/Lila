#include "ui/BakeCageView.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "bake/Arrange.h"
#include "bake/Bake.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/Cage.h"
#include "bake/CagePreview.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Triangulate.h"
#include "render/GpuImage.h"
#include "render/MeshMaps.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/MaterialDraw.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <set>

namespace lila::ui::bakeCage
{
namespace
{
using scene::kNoNode;
using scene::NodeId;
using Clock = std::chrono::steady_clock;

scene::Node* findNode(scene::Graph& g, const std::string& name)
{
    if (name.empty())
        return nullptr;
    try
    {
        return g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

scene::MeshPtr exported(scene::Graph& g, NodeId id)
{
    scene::MeshPtr m = scene::modifiers::exportMesh(g, id);
    if (!m)
        if (const scene::PlugValue v = g.value({id, scene::MeshNode::kOutMesh, -1}); std::holds_alternative<scene::MeshPtr>(v))
            m = std::get<scene::MeshPtr>(v);
    return m;
}

glm::dmat4 displayWorld(scene::Graph& g, NodeId id)
{
    return bake::arrange::displayWorld(g, id, g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
}

/** The shell of one low (or of an editable cage), cached until its inputs change. */
struct Shell
{
    scene::MeshPtr mesh;
    glm::dmat4 world{0.0};
    double frontal = -1.0;
    double offset = -1.0;
    uint64_t workingVersion = 0;
    std::vector<render::LineVertex> tris;
    std::vector<render::LineVertex> wire;
};

/** Preview maps of one low: GPU images the renderer samples, written in place by each new preview. */
struct PreviewMaps
{
    render::GpuImageRef normal, error;
    render::TexturePtr normalTexture, errorTexture;
};

/** Preview textures of one group as the renderer takes them. */
struct PreviewTextures
{
    uint64_t generation = 0;
    std::map<std::string, PreviewMaps> byMesh;
    bool greenUp = false;
};

struct State
{
    std::set<NodeId> shown;
    NodeId painting = kNoNode;
    bool live = false;
    std::map<NodeId, std::vector<float>> working;
    uint64_t workingVersion = 1;
    std::map<NodeId, Shell> shells;
    std::map<NodeId, PreviewTextures> textures;
    /** Lows of the displayed groups, to their group. */
    std::map<NodeId, NodeId> lowGroup;
    /** Nodes whose change dirties a displayed group: groups, lows, scene highs, cages. */
    std::map<NodeId, NodeId> watched;
    /** Scene highs of the painted group, hidden in the viewport so the low and its cage show. */
    std::set<NodeId> paintHighs;
    std::deque<NodeId> queue;
    Clock::time_point lastRequest{};
    QTimer* debounce = nullptr;
    int graphListener = 0;
    bool listening = false;
    /** GPU previews: per project, the groups its cage preview set bakes. */
    std::map<NodeId, std::vector<NodeId>> liveGroups;
    int mapListener = 0;
    std::vector<std::pair<QPointer<QObject>, std::function<void()>>> changed;
};

State& state()
{
    static State s;
    return s;
}

void notifyChanged()
{
    auto& cbs = state().changed;
    std::erase_if(cbs, [](const auto& c) { return !c.first; });
    for (const auto& c : std::vector(cbs))
        if (c.first)
            c.second();
}

std::set<NodeId> displayed()
{
    State& st = state();
    std::set<NodeId> out = st.shown;
    if (st.painting != kNoNode)
        out.insert(st.painting);
    return out;
}

void rebuildWatch()
{
    State& st = state();
    scene::Graph& g = scene::Graph::current();
    st.lowGroup.clear();
    st.watched.clear();
    st.paintHighs.clear();
    for (NodeId gid : displayed())
    {
        scene::Node* grp = g.node(gid);
        if (!grp || !bake::isGroup(*grp))
            continue;
        st.watched[gid] = gid;
        const bake::GroupSettings s = bake::groupSettings(*grp);
        for (const std::string& l : s.lows)
            if (scene::Node* n = findNode(g, l))
            {
                st.lowGroup[n->id()] = gid;
                st.watched[n->id()] = gid;
            }
        for (const bake::HighEntry& h : s.highs)
            if (scene::Node* n = findNode(g, h.node))
            {
                st.watched[n->id()] = gid;
                if (gid == st.painting && !h.self && !st.lowGroup.contains(n->id()))
                    st.paintHighs.insert(n->id());
            }
        if (scene::Node* c = findNode(g, s.cage))
            st.watched[c->id()] = gid;
    }
}

/** Previews bake through the GPU's live mesh maps when there is a GPU: per stroke only the touched tiles, never read back. */
bool gpuPreview() { return bake::preferredBackend() != "cpu"; }

/** The project's tangent normal map settings (its convention), else the default. */
bake::MapSettings normalMapOf(const bake::ProjectSettings& ps)
{
    for (const bake::MapSettings& m : ps.maps)
        if (m.map == "normal" && m.enabled)
            return m;
    bake::MapSettings m;
    m.map = "normal";
    return m;
}

/** GPU previews: one cage preview set per project over its displayed groups while live preview is on. Main thread. */
void syncLiveSets()
{
    State& st = state();
    if (!gpuPreview())
        return;
    scene::Graph& g = scene::Graph::current();
    std::map<NodeId, std::vector<NodeId>> want;
    if (st.live)
        for (NodeId gid : displayed())
            if (scene::Node* grp = g.node(gid); grp && bake::isGroup(*grp))
                if (scene::Node* p = bake::groupProject(g, *grp))
                    want[p->id()].push_back(gid);
    for (const auto& [pid, groups] : st.liveGroups)
        if (!want.contains(pid))
            render::disableLiveMaps(pid, render::kCagePreviewSet);
    std::map<NodeId, std::vector<NodeId>> enabled;
    for (const auto& [pid, groups] : want)
    {
        if (auto it = st.liveGroups.find(pid); it != st.liveGroups.end() && it->second == groups && render::liveMapsEnabled(pid, render::kCagePreviewSet))
        {
            enabled[pid] = groups;
            continue;
        }
        bake::MapSettings error;
        error.map = "error";
        render::LiveMapsOptions o;
        o.mapSettings = {normalMapOf(bake::projectSettings(*g.node(pid))), error};
        o.groups = groups;
        o.resolution = 512;
        o.supersampling = 1;
        o.padding = 4;
        try
        {
            render::enableLiveMaps(g, pid, o, render::kCagePreviewSet);
            enabled[pid] = groups;
            for (const auto& [low, values] : st.working)
                if (scene::Node* n = g.node(low); n && st.lowGroup.contains(low) && std::ranges::find(groups, st.lowGroup[low]) != groups.end())
                    render::setLiveCageOffsets(pid, render::kCagePreviewSet, n->path(), values);
        }
        catch (const std::exception& e)
        {
            log::warn("Cage preview of {}: {}", g.node(pid)->name(), e.what());
        }
    }
    st.liveGroups = std::move(enabled);
    if (!st.mapListener)
        st.mapListener = render::addMeshMapSetListener([](const render::MeshMapChange& c) {
            if (c.set == render::kCagePreviewSet)
                viewportTools::requestRedraw();
        });
}

/** The project of a displayed group's low, or null. */
scene::Node* projectOfLow(scene::Graph& g, NodeId low)
{
    State& st = state();
    auto it = st.lowGroup.find(low);
    scene::Node* grp = it != st.lowGroup.end() ? g.node(it->second) : nullptr;
    return grp && bake::isGroup(*grp) ? bake::groupProject(g, *grp) : nullptr;
}

/** Starts the next queued preview when none runs. */
void pump()
{
    State& st = state();
    if (st.live && gpuPreview())
    {
        st.queue.clear();
        syncLiveSets();
        return;
    }
    if (!st.live || bake::cagePreviewBusy())
        return;
    scene::Graph& g = scene::Graph::current();
    while (!st.queue.empty())
    {
        const NodeId gid = st.queue.front();
        st.queue.pop_front();
        scene::Node* grp = g.node(gid);
        if (!grp || !bake::isGroup(*grp) || !displayed().contains(gid))
            continue;
        bake::CagePreviewOptions o;
        for (const auto& [low, values] : st.working)
            if (scene::Node* n = g.node(low); n && st.lowGroup.contains(low) && st.lowGroup[low] == gid)
                o.offsets[n->path()] = values;
        try
        {
            bake::requestCagePreview(g, gid, o);
            st.lastRequest = Clock::now();
            return;
        }
        catch (const std::exception& e)
        {
            log::warn("Cage preview of {}: {}", grp->name(), e.what());
        }
    }
}

void enqueue(NodeId gid)
{
    State& st = state();
    if (std::ranges::find(st.queue, gid) == st.queue.end())
        st.queue.push_back(gid);
}

void schedule(NodeId gid)
{
    State& st = state();
    if (!st.live)
        return;
    enqueue(gid);
    if (!st.debounce)
    {
        st.debounce = new QTimer();
        st.debounce->setSingleShot(true);
        st.debounce->setInterval(200);
        QObject::connect(st.debounce, &QTimer::timeout, [] { pump(); });
    }
    st.debounce->start();
}

void listen()
{
    State& st = state();
    if (st.listening)
        return;
    st.listening = true;
    st.graphListener = scene::Graph::current().addListener([](scene::Change c, NodeId id) {
        State& st = state();
        if (c == scene::Change::Selection)
            return;
        if (c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Renamed || c == scene::Change::Reset ||
            c == scene::Change::Cleared)
        {
            if (c == scene::Change::Cleared || c == scene::Change::Reset)
            {
                scene::Graph& g = scene::Graph::current();
                std::erase_if(st.shown, [&](NodeId gid) { return !g.node(gid); });
                if (st.painting != kNoNode && !g.node(st.painting))
                    st.painting = kNoNode;
                std::erase_if(st.liveGroups, [&](const auto& kv) { return !g.node(kv.first); });
            }
            rebuildWatch();
            for (NodeId gid : displayed())
                schedule(gid);
            return;
        }
        if (auto it = st.watched.find(id); it != st.watched.end())
        {
            if (id == it->second)
                rebuildWatch();
            schedule(it->second);
        }
    });
    bake::addCagePreviewListener([](const std::shared_ptr<const bake::CagePreview>&) {
        viewportTools::requestRedraw();
        pump();
    });
}

/** Writes a preview map into image (made anew when missing or resized). srgb: the values are sRGB-encoded colours. */
render::TexturePtr upload(render::GpuImageRef& image, const bake::CagePreviewTexture& t, bool srgb)
{
    if (t.width < 1 || t.height < 1 || t.value.size() != size_t(t.width) * size_t(t.height))
        return nullptr;
    if (!image || image->width() != t.width || image->height() != t.height)
        image = render::GpuImage::create({t.width, t.height, render::TextureFormat::RGBA16F, false, true, "cage preview " + t.map});
    std::vector<float> rgba(t.value.size() * 4);
    for (size_t i = 0; i < t.value.size(); ++i)
        for (int k = 0; k < 4; ++k)
        {
            float v = std::clamp(t.value[i][k], 0.0f, 1.0f);
            if (srgb && k < 3)
                v = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            rgba[i * 4 + size_t(k)] = v;
        }
    image->updateRgba(0, 0, t.width, t.height, rgba);
    return image->asTexture();
}

const PreviewTextures* texturesOf(NodeId gid)
{
    State& st = state();
    const auto pv = bake::latestCagePreview(gid);
    if (!pv)
        return nullptr;
    PreviewTextures& pt = st.textures[gid];
    if (pt.generation != pv->generation)
    {
        pt.generation = pv->generation;
        std::map<std::string, PreviewMaps> next;
        for (const bake::CagePreviewTexture& t : pv->textures)
        {
            if (t.map != "normal")
                continue;
            PreviewMaps maps;
            if (auto old = pt.byMesh.find(t.mesh); old != pt.byMesh.end())
                maps = std::move(old->second);
            const bake::CagePreviewTexture* err = pv->find(t.mesh, "error");
            maps.normalTexture = upload(maps.normal, t, false);
            maps.errorTexture = err ? upload(maps.error, *err, true) : nullptr;
            next[t.mesh] = std::move(maps);
            pt.greenUp = t.greenUp;
        }
        pt.byMesh = std::move(next);
    }
    return &pt;
}

/** GPU previews: the low's normal and error images from its project's cage preview set. */
bool livePreviewMaterials(NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& out)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(id);
    scene::Node* p = n ? projectOfLow(g, id) : nullptr;
    if (!p)
        return false;
    const render::MeshMapInfo* normal = nullptr;
    const render::MeshMapInfo* error = nullptr;
    const std::vector<render::MeshMapInfo> maps = render::meshMapSet(p->id(), render::kCagePreviewSet);
    for (const render::MeshMapInfo& m : maps)
        if (m.low == n->path())
        {
            const render::MeshMapInfo*& slot = m.map == "normal" ? normal : error;
            if (!slot || m.udim < slot->udim)
                slot = &m;
        }
    if (!normal)
        return false;
    const bool greenUp = normalMapOf(bake::projectSettings(*p)).options.value("greenUp", false);
    out = drawMaterials(mesh);
    if (out.empty())
        out.resize(1);
    for (render::Material& m : out)
    {
        m.normalMap = normal->image->asTexture();
        m.flags |= render::MatNormalMap;
        if (greenUp)
            m.flags |= render::MatNormalGreenUp;
        else
            m.flags &= ~uint32_t(render::MatNormalGreenUp);
        if (error)
        {
            m.emissiveMap = error->image->asTexture();
            m.emissive = glm::vec3(1.0f);
            m.flags |= render::MatEmissiveMap;
        }
    }
    return true;
}

bool previewMaterials(NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& out)
{
    State& st = state();
    if (!st.live)
        return false;
    if (gpuPreview())
        return st.lowGroup.contains(id) && livePreviewMaterials(id, mesh, out);
    auto it = st.lowGroup.find(id);
    if (it == st.lowGroup.end())
        return false;
    const PreviewTextures* pt = texturesOf(it->second);
    scene::Node* n = scene::Graph::current().node(id);
    if (!pt || !n)
        return false;
    auto tex = pt->byMesh.find(n->path());
    if (tex == pt->byMesh.end() || !tex->second.normalTexture)
        return false;
    out = drawMaterials(mesh);
    if (out.empty())
        out.resize(1);
    for (render::Material& m : out)
    {
        m.normalMap = tex->second.normalTexture;
        m.flags |= render::MatNormalMap;
        if (pt->greenUp)
            m.flags |= render::MatNormalGreenUp;
        else
            m.flags &= ~uint32_t(render::MatNormalGreenUp);
        if (tex->second.errorTexture)
        {
            m.emissiveMap = tex->second.errorTexture;
            m.emissive = glm::vec3(1.0f);
            m.flags |= render::MatEmissiveMap;
        }
    }
    return true;
}

/** Shell triangles and wire for a mesh whose points sit at pts (world). */
void buildShell(Shell& sh, const geo::Mesh& m, const std::vector<glm::dvec3>& pts)
{
    const glm::vec4 fill(0.5f, 0.5f, 0.5f, 0.22f), line(0.62f, 0.62f, 0.62f, 0.75f);
    const geo::Triangulation tris = geo::triangulate(m);
    sh.tris.resize(tris.corners.size());
    tbb::parallel_for(size_t(0), tris.corners.size(), [&](size_t c) { sh.tris[c] = {glm::vec3(pts[m.cornerPoint(tris.corners[c])]), fill}; });
    sh.wire.clear();
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            sh.wire.push_back({glm::dvec3(pts[m.edgePoint(e, 0)]), line});
            sh.wire.push_back({glm::dvec3(pts[m.edgePoint(e, 1)]), line});
        }
}

/** The group's frontal as bakes resolve it: set, else the last preview's measurement, else the size fallback. */
double frontalOf(scene::Graph& g, NodeId gid, const bake::GroupSettings& s)
{
    if (s.frontal > 0.0)
        return s.frontal;
    if (const auto pv = bake::latestCagePreview(gid); pv && pv->stats.measured && pv->stats.frontal > 0.0)
        return pv->stats.frontal;
    if (scene::Node* grp = g.node(gid); grp && gpuPreview())
        if (scene::Node* p = bake::groupProject(g, *grp))
        {
            // No live session yet has no "last"; json::value throws on anything but an object.
            const json status = render::liveMapsStatus(p->id(), render::kCagePreviewSet);
            const json* last = status.contains("last") && status["last"].is_object() ? &status["last"] : nullptr;
            const json* groups = last && last->contains("groups") && (*last)["groups"].is_array() ? &(*last)["groups"] : nullptr;
            for (const json& gs : groups ? *groups : json::array())
                if (gs.is_object() && gs.value("group", std::string()) == grp->name() && gs.value("measured", false))
                    if (const auto fr = gs.find("frontal"); fr != gs.end() && fr->is_number() && fr->get<double>() > 0.0)
                        return fr->get<double>();
        }
    return bake::groupFrontal(g, s);
}

void drawGroup(render::FrameState& f, scene::Graph& g, NodeId gid)
{
    LILA_ZONE();

    State& st = state();
    scene::Node* grp = g.node(gid);
    if (!grp || !bake::isGroup(*grp))
        return;
    const bake::GroupSettings s = bake::groupSettings(*grp);
    auto pushShell = [&](const Shell& sh) {
        f.tris3d.insert(f.tris3d.end(), sh.tris.begin(), sh.tris.end());
        f.lines.insert(f.lines.end(), sh.wire.begin(), sh.wire.end());
    };
    if (scene::Node* c = findNode(g, s.cage); c && c->type().isA("mesh"))
    {
        scene::MeshPtr m = exported(g, c->id());
        if (!m)
            return;
        Shell& sh = st.shells[c->id()];
        const glm::dmat4 w = displayWorld(g, c->id());
        if (sh.mesh != m || sh.world != w)
        {
            sh.mesh = m;
            sh.world = w;
            std::vector<glm::dvec3> pts(m->pointCapacity());
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                pts[p] = glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
            buildShell(sh, *m, pts);
        }
        pushShell(sh);
        return;
    }
    const double frontal = frontalOf(g, gid, s);
    for (const std::string& l : s.lows)
    {
        scene::Node* n = findNode(g, l);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr m = exported(g, n->id());
        if (!m)
            continue;
        Shell& sh = st.shells[n->id()];
        const glm::dmat4 w = displayWorld(g, n->id());
        auto work = st.working.find(n->id());
        const uint64_t version = work != st.working.end() ? st.workingVersion : 0;
        if (sh.mesh != m || sh.world != w || sh.frontal != frontal || sh.offset != s.cageOffset || sh.workingVersion != version)
        {
            sh.mesh = m;
            sh.world = w;
            sh.frontal = frontal;
            sh.offset = s.cageOffset;
            sh.workingVersion = version;
            const std::vector<float> values = work != st.working.end() && work->second.size() == m->pointCapacity()
                                                  ? work->second
                                                  : bake::pointValues(*m, *bake::paintAttr(bake::kOffsetAttr));
            buildShell(sh, *m, bake::autoCagePoints(*m, w, frontal, s.cageOffset, values));
        }
        pushShell(sh);
    }
}

void toggleShown()
{
    scene::Graph& g = scene::Graph::current();
    const std::vector<NodeId> groups = selectedGroups(g);
    if (groups.empty())
    {
        log::warn("Show Bake Cage: select a bake group or its low");
        return;
    }
    const bool on = !std::ranges::all_of(groups, [](NodeId id) { return shown(id); });
    for (NodeId id : groups)
        setShown(id, on);
}
} // namespace

scene::Node* groupOfMesh(scene::Graph& g, NodeId mesh)
{
    scene::Node* m = g.node(mesh);
    if (!m)
        return nullptr;
    const std::string path = m->path();
    for (NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && bake::isGroup(*n))
        {
            const bake::GroupSettings s = bake::groupSettings(*n);
            if (std::ranges::find(s.lows, path) != s.lows.end() || s.cage == path)
                return n;
        }
    return nullptr;
}

std::vector<NodeId> selectedGroups(scene::Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        scene::Node* grp = n && bake::isGroup(*n) ? n : n && n->type().isA("mesh") ? groupOfMesh(g, id) : nullptr;
        if (grp && std::ranges::find(out, grp->id()) == out.end())
            out.push_back(grp->id());
    }
    return out;
}

bool shown(NodeId group) { return state().shown.contains(group); }

void setShown(NodeId group, bool on)
{
    State& st = state();
    listen();
    if (on)
        st.shown.insert(group);
    else
    {
        st.shown.erase(group);
        st.textures.erase(group);
    }
    rebuildWatch();
    if (st.live && !on)
        syncLiveSets();
    if (on)
        schedule(group);
    viewportTools::requestRedraw();
    notifyChanged();
}

bool livePreview() { return state().live; }

void setLivePreview(bool on)
{
    State& st = state();
    listen();
    st.live = on;
    if (on)
        for (NodeId gid : displayed())
            schedule(gid);
    else
    {
        st.queue.clear();
        bake::cancelCagePreview(false);
        syncLiveSets();
    }
    viewportTools::requestRedraw();
    notifyChanged();
}

void onChanged(QObject* owner, std::function<void()> fn)
{
    state().changed.emplace_back(owner, std::move(fn));
}

void setWorkingOffsets(NodeId low, std::span<const float> values, bool rebake)
{
    State& st = state();
    st.working[low].assign(values.begin(), values.end());
    ++st.workingVersion;
    if (!rebake || !st.live)
        return;
    auto it = st.lowGroup.find(low);
    if (it == st.lowGroup.end())
        return;
    if (gpuPreview())
    {
        // The live set rebakes the stroke's tiles on its own schedule.
        scene::Graph& g = scene::Graph::current();
        if (scene::Node* p = projectOfLow(g, low))
            render::setLiveCageOffsets(p->id(), render::kCagePreviewSet, g.node(low)->path(), std::vector<float>(values.begin(), values.end()));
        return;
    }
    // Throttled: a preview at most every 150 ms, and never queued behind a running one mid-stroke.
    if (bake::cagePreviewBusy() || Clock::now() - st.lastRequest < std::chrono::milliseconds(150))
        return;
    enqueue(it->second);
    pump();
}

void clearWorkingOffsets(NodeId low)
{
    State& st = state();
    if (st.working.erase(low))
    {
        ++st.workingVersion;
        scene::Graph& g = scene::Graph::current();
        if (scene::Node* p = gpuPreview() ? projectOfLow(g, low) : nullptr)
            render::setLiveCageOffsets(p->id(), render::kCagePreviewSet, g.node(low)->path(), {});
    }
}

void setPainting(NodeId group)
{
    State& st = state();
    if (st.painting == group)
        return;
    listen();
    st.painting = group;
    rebuildWatch();
    if (st.live && group == kNoNode)
        syncLiveSets();
    if (group != kNoNode)
        schedule(group);
    viewportTools::requestRedraw();
}

void registerBakeCageView()
{
    viewportHooks::addDrawer("lila.bakeCage", [](render::FrameState& f, OverlayPainter&, const ViewInfo&) {
        if (state().shown.empty() && state().painting == kNoNode)
            return;
        scene::Graph& g = scene::Graph::current();
        for (NodeId gid : displayed())
            drawGroup(f, g, gid);
    });
    viewportHooks::setMaterialHook("lila.bakeCagePreview", previewMaterials);
    viewportHooks::setHideHook("lila.bakeCagePaint", [](NodeId id) { return state().paintHighs.contains(id); });
    bake::addBakeListener([](const bake::BakeEvent& e) {
        if (e.kind != bake::BakeEvent::Kind::Started)
            return;
        QTimer::singleShot(0, qApp, [] {
            for (NodeId gid : std::set<NodeId>(state().shown))
                setShown(gid, false);
        });
    });

    actions::add({.id = "bake.showCage",
                  .label = "Show Bake Cage",
                  .category = "Bake",
                  .run = toggleShown,
                  .doc = "Toggles the selected bake groups' cage in the viewport: a translucent shell with its wire where rays start "
                         "(the editable cage, or the auto cage with its painted offsets).",
                  .python = "lila.ui.run_action(\"bake.showCage\")"});
    actions::add({.id = "bake.livePreview",
                  .label = "Live Bake Preview",
                  .category = "Bake",
                  .run = [] { setLivePreview(!livePreview()); },
                  .doc = "Toggles live preview bakes: shown and painted groups rebake a low resolution normal map in memory as their "
                         "cage changes, drawn on the lows with missed rays glowing red. Nothing is written.",
                  .python = "lila.ui.run_action(\"bake.livePreview\")"});
    actions::add({.id = "bake.autoPaintCage",
                  .label = "Auto Paint Cage",
                  .category = "Bake",
                  .doc = "Paints the selected group's cage offsets so every ray starts just outside the high and meets it, and sets "
                         "its frontal and rear distances. One undo step.",
                  .command = "bakeAutoPaintCage",
                  .call = json::object()});
}
} // namespace lila::ui::bakeCage
