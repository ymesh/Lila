#include "ui/PreviewQuality.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "render/Displacement.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "tex/ShadingChannels.h"
#include "tex/TextureSet.h"
#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/Overlay.h"
#include "ui/TexCompare.h"
#include "ui/TexLive.h"
#include "ui/TextureGraphUi.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportTool.h"

#include <algorithm>
#include <list>
#include <map>
#include <unordered_map>

namespace lila::ui::previewQuality
{
namespace
{
using T = ArgType;
using json = nlohmann::json;

// ---------------------------------------------------------------- displacement preview

/** The attr's value as a bool or double on a node, nullopt when the node lacks it. */
template <class V> std::optional<V> attrValue(const scene::Node& n, const char* name)
{
    const int a = n.type().attrIndex(name);
    if (a < 0)
        return std::nullopt;
    const scene::PlugValue& v = n.value(a);
    if (const V* x = std::get_if<V>(&v))
        return *x;
    if constexpr (std::is_same_v<V, double>)
        if (const int64_t* i = std::get_if<int64_t>(&v))
            return double(*i);
    return std::nullopt;
}

bool displacementOn(const scene::Node& n) { return attrValue<bool>(n, kDisplacementAttr).value_or(false); }
double detailOf(const scene::Node& n) { return std::clamp(attrValue<double>(n, kDetailAttr).value_or(6.0), 1.0, 64.0); }

struct Tessellated
{
    std::weak_ptr<const render::RenderMesh> source;
    std::shared_ptr<const render::RenderMesh> mesh;
    uint64_t lastFrame = 0;
};

struct DisplacementCache
{
    std::map<std::pair<uint64_t, int>, Tessellated> meshes; // by source key and level
    /** Per set name: the height images it was made from and the map over them. */
    std::unordered_map<std::string, std::pair<std::vector<render::GpuImage*>, render::TexturePtr>> maps; // by set|channel
    std::unordered_map<uint64_t, std::string> status;
    /** Per set, the height copy size the user was last told about. */
    std::unordered_map<std::string, int> copyNoted;
    uint64_t frame = 0;
};

DisplacementCache& cache()
{
    static DisplacementCache c;
    return c;
}

std::shared_ptr<const render::RenderMesh> tessellated(const std::shared_ptr<const render::RenderMesh>& source, int level)
{
    DisplacementCache& c = cache();
    Tessellated& t = c.meshes[{source->key, level}];
    if (!t.mesh || t.source.expired())
    {
        t.source = source;
        t.mesh = render::displacement::tessellate(*source, level);
    }
    t.lastFrame = c.frame;
    return t.mesh;
}

void dropStaleTessellations()
{
    DisplacementCache& c = cache();
    std::erase_if(c.meshes, [&](const auto& e) { return e.second.source.expired() || e.second.lastFrame + 120 < c.frame; });
}

/** The set bound to a material node: the one whose material setting names it, else the set of the material's own name. */
std::string setFor(scene::Graph& g, const scene::Node& material)
{
    std::string set = tex::setForMaterial(g, material.name());
    if (set.empty() && tex::findSet(g, material.name()))
        set = material.name();
    return set;
}

/** A set's settings, read again only when the scene changed. Null when unreadable. */
const tex::SetSettings* settingsOf(scene::Graph& g, const std::string& set)
{
    struct Entry
    {
        uint64_t version = 0;
        std::optional<tex::SetSettings> settings;
    };
    static std::unordered_map<std::string, Entry> entries;
    Entry& e = entries[set];
    if (e.version != g.version() || e.version == 0)
    {
        e.version = g.version();
        e.settings.reset();
        if (scene::Node* node = tex::findSet(g, set))
            try
            {
                e.settings = tex::readSettings(*node);
            }
            catch (const std::exception&)
            {
            }
    }
    return e.settings ? &*e.settings : nullptr;
}

/** A live set channel over the set's tiles as one map (a UDIM set when it has several), null when it isn't evaluated whole. */
render::TexturePtr channelMap(const std::string& set, const tex::SetSettings& settings, const std::string& channel)
{
    std::vector<render::GpuImage*> raw;
    std::vector<std::pair<int, render::GpuImageRef>> images;
    for (const tex::Tile& t : settings.tiles)
        if (render::GpuImageRef img = texLive::channelImage(set, channel, t.udim))
        {
            raw.push_back(img.get());
            images.emplace_back(t.udim, std::move(img));
        }
    if (images.empty())
        return nullptr;
    auto& entry = cache().maps[set + "|" + channel];
    if (entry.first == raw && entry.second)
        return entry.second;
    entry.first = raw;
    if (images.size() == 1 && images.front().first == 1001)
        entry.second = images.front().second->asTexture();
    else
    {
        auto t = std::make_shared<render::TextureImage>();
        t->width = images.front().second->width();
        t->height = images.front().second->height();
        t->format = images.front().second->format();
        t->srgb = false;
        for (auto& [udim, img] : images)
            t->tiles.push_back({udim, img->asTexture()});
        std::ranges::sort(t->tiles, {}, &render::UdimTile::udim);
        entry.second = t;
    }
    return entry.second;
}

/** Statuses that clear by themselves within a few frames, so they aren't logged as warnings. */
constexpr const char* kWaiting = "waiting for ";

/** The live height map of a set and its height depth, null (with why) when it isn't there. */
render::TexturePtr heightMap(scene::Graph& g, const std::string& set, std::string& why, float& depth)
{
    const tex::SetSettings* settings = settingsOf(g, set);
    if (!settings)
    {
        why = "no texture set";
        return nullptr;
    }
    if (!settings->channel("height"))
    {
        why = "no height channel";
        return nullptr;
    }
    depth = float(settings->heightDepth);
    render::TexturePtr t = channelMap(set, *settings, "height");
    if (t)
        return t;
    if (texLive::heightCopy(set))
        why = std::string(kWaiting) + "the paged set's height copy";
    else if (!texLive::isLive(set))
        why = std::string(kWaiting) + "the texture set to evaluate";
    else
        why = "height not evaluated";
    return t;
}

/**
 * Gives default lit materials whose texture set has shading channels (tex/ShadingChannels.h) the Extended model with
 * those channels as maps, for the viewport and the path-traced view.
 */
void applyShadingChannels(render::FrameState& s)
{
    scene::Graph& g = scene::Graph::current();

    bool any = false;
    for (const render::MeshDraw& d : s.meshes)
        if (!d.materials.empty() && d.node)
        {
            any = true;
            break;
        }
    if (!any)
        return;
    std::vector<render::MeshDraw>* draws = nullptr;
    for (size_t i = 0; i < s.meshes.size(); ++i)
    {
        const render::MeshDraw& cd = s.meshes[i];
        if (cd.materials.empty() || !cd.node || !(cd.flags & render::DrawShaded))
            continue;
        const scene::Node* n = g.node(scene::NodeId(cd.node));
        const int outMesh = n ? n->type().attrIndex("outMesh") : -1;
        const scene::MeshPtr* mesh = outMesh >= 0 ? std::get_if<scene::MeshPtr>(&n->value(outMesh)) : nullptr;
        if (!mesh || !*mesh)
            continue;
        for (size_t slot = 0; slot < cd.materials.size(); ++slot)
        {
            if (cd.materials[slot].style.model != render::ShadingModel::DefaultLit)
                continue;
            const scene::Node* material = scene::materials::find(g, scene::materials::slotName(**mesh, int32_t(slot)));
            const std::string set = material ? setFor(g, *material) : std::string();
            const tex::SetSettings* settings = set.empty() ? nullptr : settingsOf(g, set);
            if (!settings)
                continue;
            render::ExtendedShading ext;
            bool has = false;
            const auto list = tex::shading::channels();
            for (size_t k = 0; k < list.size() && k < render::kExtendedMaps; ++k)
                if (settings->channel(list[k].name))
                {
                    has = true;
                    ext.maps[k] = channelMap(set, *settings, list[k].name);
                }
            if (!has)
                continue;
            if (!draws)
                draws = &s.meshes.edit();
            render::Material& m = (*draws)[i].materials[slot];
            m.ext = std::move(ext);
            m.style.model = render::ShadingModel::Extended;
        }
    }
}

/** Swaps the meshes of displaced draws for their tessellations and gives their materials the set's height. */
void drawDisplacement(render::FrameState& s, OverlayPainter&, const ViewInfo& view)
{
    applyShadingChannels(s);
    DisplacementCache& c = cache();
    ++c.frame;
    if (c.frame % 60 == 0)
        dropStaleTessellations();
    scene::Graph& g = scene::Graph::current();
    std::vector<size_t> picked;
    for (size_t i = 0; i < s.meshes.size(); ++i)
    {
        const render::MeshDraw& d = s.meshes[i];
        if (!d.node || !d.mesh || !(d.flags & render::DrawShaded))
            continue;
        if (const scene::Node* n = g.node(scene::NodeId(d.node)); n && displacementOn(*n))
            picked.push_back(i);
    }
    if (picked.empty())
    {
        c.status.clear();
        return;
    }
    std::unordered_map<uint64_t, std::string> status;
    std::vector<render::MeshDraw>& draws = s.meshes.edit();
    for (size_t i : picked)
    {
        render::MeshDraw& d = draws[i];
        const scene::Node& n = *g.node(scene::NodeId(d.node));
        const int outMesh = n.type().attrIndex("outMesh");
        const scene::MeshPtr* mesh = outMesh >= 0 ? std::get_if<scene::MeshPtr>(&n.value(outMesh)) : nullptr;
        if (!mesh || !*mesh)
        {
            status[d.node] = "not a mesh";
            continue;
        }
        if (d.materials.empty())
        {
            status[d.node] = "materials hidden";
            continue;
        }
        std::string why = "no texture set";
        std::string copyNote;
        bool any = false;
        for (size_t slot = 0; slot < d.materials.size(); ++slot)
        {
            const scene::Node* material = scene::materials::find(g, scene::materials::slotName(**mesh, int32_t(slot)));
            if (!material)
                continue;
            const std::string set = setFor(g, *material);
            if (set.empty())
                continue;
            float depth = 0.0f;
            render::TexturePtr height = heightMap(g, set, why, depth);
            if (!height)
                continue;
            if (std::optional<texLive::HeightCopy> copy = texLive::heightCopy(set); copy && copy->size < copy->full)
            {
                copyNote = ", height at " + std::to_string(copy->size) + " of " + std::to_string(copy->full) + " px";
                // Once per set and size: the preview is coarser than the set, which users should know.
                if (c.copyNoted[set] != copy->size)
                {
                    c.copyNoted[set] = copy->size;
                    viewportToast::showActive(QString("Displacement Preview: height at %1 of %2 px (paged set)").arg(copy->size).arg(copy->full), 4000);
                }
            }
            render::Material& m = d.materials[slot];
            m.heightMap = std::move(height);
            m.displacementDepth = depth;
            m.displacementMid = 0.5f;
            any = true;
        }
        if (!any)
        {
            status[d.node] = why;
            continue;
        }
        const int level = render::displacement::levelFor(*d.mesh, d.world, s.view, s.proj, view.height, detailOf(n));
        d.mesh = tessellated(d.mesh, level);
        status[d.node] = "displaced (level " + std::to_string(level) + ", " + std::to_string(d.mesh->indices.size() / 3) + " triangles" + copyNote + ")";
    }
    for (auto& [node, text] : status)
        if (c.status[node] != text && !text.starts_with("displaced") && !text.starts_with(kWaiting))
            log::warn("Displacement preview, {}: {}", g.node(scene::NodeId(node)) ? g.node(scene::NodeId(node))->name() : "?", text);
    c.status = std::move(status);
}

scene::Node& meshArg(scene::Graph& g, const std::string& name, const char* command)
{
    scene::Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(command) + ": no object '" + name + "'");
    if (n->type().attrIndex("outMesh") < 0)
        throw CommandError(std::string(command) + ": '" + name + "' is not a mesh");
    return *n;
}

void ensureAttr(const scene::Node& n, const char* name, const json& addFlags)
{
    if (n.type().attrIndex(name) >= 0)
        return;
    json call = addFlags;
    call["args"] = json::array({n.path()});
    call["longName"] = name;
    ops::execute("addAttr", call);
}

void registerDisplacement()
{
    ops::registerCommand({
        .name = "displacementPreview",
        .doc = "Turn displacement preview on or off for meshes: in the viewport they subdivide and move by their texture sets' height "
               "channel (Height Depth from each set's settings), updating live as height is painted. With no flags, reports each "
               "mesh's state. Returns [{object, enabled, detail, status}].",
        .positionalDoc = "objects",
        .flags =
            {
                {"enable", "e", T::Bool, nullptr, "On or off."},
                {"detail", "d", T::Float, nullptr, "Target edge length in screen pixels: smaller subdivides more.", {}, {}, 1.0, 64.0},
            },
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("displacementPreview: select a mesh or name one");
            json out = json::array();
            for (const std::string& o : objects)
            {
                scene::Node& n = meshArg(g, o, "displacementPreview");
                if (a.has("enable"))
                {
                    ensureAttr(n, kDisplacementAttr,
                               {{"attributeType", "bool"}, {"category", "Display"}, {"niceName", "Displacement Preview"},
                                {"doc", "Subdivide and displace by the texture set's height in the viewport."}});
                    ops::execute("setAttr", {{"args", {n.path() + "." + kDisplacementAttr, a.get<bool>("enable")}}});
                }
                if (a.has("detail"))
                {
                    ensureAttr(n, kDetailAttr,
                               {{"attributeType", "double"}, {"category", "Display"}, {"niceName", "Displacement Detail"}, {"defaultValue", 6.0},
                                {"minValue", 1.0}, {"maxValue", 64.0}, {"doc", "Target edge length in screen pixels."}});
                    ops::execute("setAttr", {{"args", {n.path() + "." + kDetailAttr, a.get<double>("detail")}}});
                }
                const bool on = displacementOn(n);
                out.push_back({{"object", n.path()},
                               {"enabled", on},
                               {"detail", detailOf(n)},
                               {"status", on ? displacementStatus(uint64_t(n.id())) : std::string("off")}});
            }
            viewportTools::requestRedraw();
            return out;
        },
        .examples = {"displacementPreview(\"rock\", enable=True)", "displacementPreview(\"rock\", detail=3)"},
    });
    viewportHooks::addDrawer("lila.displacementPreview", drawDisplacement);
}

void registerDisplacementActions()
{
    actions::add({.id = "texture.displacementPreview",
                  .label = "Displacement Preview",
                  .category = "Texture",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          json sel = json::array();
                          bool anyOff = false;
                          for (scene::NodeId id : g.selection())
                              if (const scene::Node* n = g.node(id); n && n->type().attrIndex("outMesh") >= 0)
                              {
                                  sel.push_back(n->path());
                                  anyOff = anyOff || !displacementOn(*n);
                              }
                          if (sel.empty())
                          {
                              log::warn("Displacement Preview: select a mesh");
                              return;
                          }
                          ops::execute("displacementPreview", {{"args", sel}, {"enable", anyOff}}, CallSource::UI);
                      },
                  .doc = "Subdivide the selected meshes in the viewport and move them by their texture set's height, live while you paint.",
                  .python = "displacementPreview(enable=True)",
                  .checked =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          for (scene::NodeId id : g.selection())
                              if (const scene::Node* n = g.node(id); n && displacementOn(*n))
                                  return true;
                          return false;
                      },
                  .icon = "mountain",
                  .keywords = {"displace", "tessellation", "height", "parallax", "relief"}});
}

// ---------------------------------------------------------------- path-traced view

constexpr const char* kPtOn = "/viewport/pathTrace/enabled";
constexpr const char* kPtBounces = "/viewport/pathTrace/bounces";
constexpr const char* kPtSamples = "/viewport/pathTrace/samples";
constexpr const char* kPtDenoise = "/viewport/pathTrace/denoise";

render::FrameState::PathTrace pathTraceSettings()
{
    const PrefStore& p = PrefStore::get();
    render::FrameState::PathTrace t;
    t.on = p.value<bool>("general", kPtOn, false);
    t.bounces = std::clamp(p.value<int>("general", kPtBounces, 4), 1, 32);
    t.maxSamples = std::clamp(p.value<int>("general", kPtSamples, 1024), 1, 65536);
    t.denoise = p.value<bool>("general", kPtDenoise, true);
    return t;
}

bool canTrace()
{
    render::Renderer& r = render::Renderer::get();
    return r.running() && r.gpuInfo().rayTracing;
}

void drawPathTrace(render::FrameState& s, OverlayPainter& painter, const ViewInfo& view)
{
    s.pathTrace = pathTraceSettings();
    if (s.pathTrace.on && !canTrace())
    {
        s.pathTrace.on = false;
        const QString text = "Path Traced needs a GPU with ray tracing: showing the shaded view";
        painter.label(QPointF(view.width * 0.5 - OverlayPainter::textWidth(text) * 0.5, 40.0), text, {1.0f, 0.75f, 0.35f, 1.0f});
    }
}

json pathTraceState()
{
    const render::FrameState::PathTrace t = pathTraceSettings();
    return {{"enabled", t.on}, {"bounces", t.bounces}, {"samples", t.maxSamples}, {"denoise", t.denoise}, {"supported", canTrace()}};
}

void registerPathTrace()
{
    ops::registerCommand({
        .name = "pathTracedView",
        .doc = "Show every viewport path traced: light bounces through the scene (environment, lights, materials and texture sets) "
               "and the image refines while nothing changes, starting over on any camera, scene or paint change. Needs a GPU with "
               "ray tracing; without one the views stay shaded and say so. With no flags, reports the settings. Returns "
               "{enabled, bounces, samples, denoise, supported}.",
        .flags =
            {
                {"enable", "e", T::Bool, nullptr, "On or off."},
                {"bounces", "b", T::Int, nullptr, "Most surfaces a path bounces off.", {}, {}, 1.0, 32.0},
                {"samples", "s", T::Int, nullptr, "Samples per pixel the image refines to, then stops.", {}, {}, 1.0, 65536.0},
                {"denoise", "d", T::Bool, nullptr, "Smooth the noise of the first samples, guided by the surfaces' colours, normals and depth."},
            },
        .undoable = false,
        .run = [](const Args& a) -> json {
            PrefStore& p = PrefStore::get();
            if (a.has("enable"))
            {
                p.set("general", kPtOn, a.get<bool>("enable"));
                if (a.get<bool>("enable") && !canTrace())
                    log::warn("pathTracedView: this GPU can't trace rays; the viewports stay shaded");
            }
            if (a.has("bounces"))
                p.set("general", kPtBounces, a.get<int>("bounces"));
            if (a.has("samples"))
                p.set("general", kPtSamples, a.get<int>("samples"));
            if (a.has("denoise"))
                p.set("general", kPtDenoise, a.get<bool>("denoise"));
            viewportTools::requestRedraw();
            return pathTraceState();
        },
        .examples = {"pathTracedView(enable=True)", "pathTracedView(bounces=8, samples=4096)"},
    });
    viewportHooks::addDrawer("lila.pathTracedView", drawPathTrace);
}

void registerPathTraceActions()
{
    actions::add({.id = "viewport.pathTraced",
                  .label = "Path Traced",
                  .category = "Display",
                  .run =
                      [] {
                          const bool on = !pathTraceSettings().on;
                          ops::execute("pathTracedView", {{"enable", on}}, CallSource::UI);
                          if (on && !canTrace())
                              viewportToast::showActive("Path Traced needs a GPU with ray tracing", 4000, viewportToast::Kind::Warning);
                      },
                  .doc = "Path trace the viewports: true reflections, shadows and bounced light that refine while the view holds still.",
                  .python = "pathTracedView(enable=True)",
                  .checked = [] { return pathTraceSettings().on; },
                  .icon = "sun",
                  .keywords = {"path tracing", "ray tracing", "raytrace", "render preview", "progressive", "global illumination", "gi", "iray",
                               "cycles"}});
    actions::add({.id = "viewport.pathTracedDenoise",
                  .label = "Path Traced Denoise",
                  .category = "Display",
                  .run = [] { ops::execute("pathTracedView", {{"denoise", !pathTraceSettings().denoise}}, CallSource::UI); },
                  .doc = "Smooth the path-traced view's early noise; it fades out as samples come in.",
                  .python = "pathTracedView(denoise=False)",
                  .checked = [] { return pathTraceSettings().denoise; },
                  .keywords = {"noise", "denoiser"}});
}
} // namespace

std::string displacementStatus(uint64_t node)
{
    auto it = cache().status.find(node);
    return it != cache().status.end() ? it->second : std::string("waiting for a frame");
}

void registerCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerDisplacement();
    registerPathTrace();
    texCompare::registerCommands();
}

void registerShadingChannelActions()
{
    actions::add({.id = "texture.addShadingChannels",
                  .label = "Add Shading Channels",
                  .category = "Texture",
                  .run =
                      [] {
                          const std::string set = currentTextureSet();
                          if (set.empty())
                          {
                              log::warn("Add Shading Channels: no texture set");
                              return;
                          }
                          ops::execute("texAddShadingChannels", {{"args", {set}}}, CallSource::UI);
                      },
                  .doc = "Give the current texture set sheen, coat, subsurface, anisotropy, transmission, IOR and specular channels: paint or "
                         "fill them like any other, and the viewport and exports use them.",
                  .python = "texAddShadingChannels(\"RockTex\")",
                  .icon = "layers-subtract",
                  .keywords = {"sheen", "fuzz", "clearcoat", "coat", "subsurface", "sss", "anisotropy", "transmission", "ior", "specular color",
                               "openpbr"}});
}

void registerUi()
{
    registerCommands();
    registerShadingChannelActions();
    texCompare::registerActions();
    registerDisplacementActions();
    menus::addActions("Texture", {"", "texture.addShadingChannels", "texture.displacementPreview", "viewport.pathTraced"});
    registerPathTraceActions();
    menus::addActions("Display", {"", "viewport.pathTraced", "viewport.pathTracedDenoise", "texture.displacementPreview"});
}
} // namespace lila::ui::previewQuality
