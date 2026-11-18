#include "ui/TexCompare.h"

#include "core/Log.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "tex/Snapshots.h"
#include "tex/TextureSet.h"
#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/Overlay.h"
#include "ui/TexPreview.h"
#include "ui/TextureGraphUi.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <map>

namespace lila::ui::texCompare
{
namespace
{
using T = ArgType;
namespace snaps = tex::snapshots;

struct State
{
    bool on = false;
    std::string set;
    std::string snapshot;
    std::string against = "live"; // or another snapshot's name
    double wipe = 0.5;
    bool dragging = false;
};

State& state()
{
    static State s;
    return s;
}

/** A snapshot's channels as viewport maps (base colour, normal, ORM packed, emissive), made once per snapshot. */
struct Maps
{
    render::TexturePtr baseColor, normal, orm, emissive;
    bool occlusion = false, roughness = false, metallic = false;
};

render::TexturePtr rgba8(const image::Image& img, bool srgb, const std::function<glm::vec4(const float*)>& texel)
{
    if (img.width <= 0)
        return nullptr;
    auto t = std::make_shared<render::TextureImage>();
    t->width = img.width;
    t->height = img.height;
    t->format = render::TextureFormat::RGBA8;
    t->srgb = srgb;
    std::vector<float> rgba(size_t(img.width) * img.height * 4);
    for (size_t i = 0; i < size_t(img.width) * img.height; ++i)
    {
        const glm::vec4 v = texel(&img.rgba[i * 4]);
        rgba[i * 4] = v.r;
        rgba[i * 4 + 1] = v.g;
        rgba[i * 4 + 2] = v.b;
        rgba[i * 4 + 3] = v.a;
    }
    render::encodeTexels(render::TextureFormat::RGBA8, srgb, rgba, t->rgba);
    return t;
}

const Maps& mapsOf(const snaps::Snapshot& s)
{
    static std::map<uint64_t, Maps> cache;
    if (cache.size() > 8)
        cache.clear();
    auto [it, added] = cache.try_emplace(s.id);
    if (!added)
        return it->second;
    Maps& m = it->second;
    const image::Image base = snaps::image(s, "baseColor"), opacity = snaps::image(s, "opacity");
    m.baseColor = rgba8(base, true, [&](const float* p) {
        const size_t i = size_t(p - base.rgba.data());
        return glm::vec4(p[0], p[1], p[2], opacity.width == base.width && opacity.height == base.height ? opacity.rgba[i] : 1.0f);
    });
    m.normal = rgba8(snaps::image(s, "normal"), false, [](const float* p) { return glm::vec4(p[0], p[1], p[2], 1.0f); });
    m.emissive = rgba8(snaps::image(s, "emissive"), true, [](const float* p) { return glm::vec4(p[0], p[1], p[2], 1.0f); });
    const image::Image ao = snaps::image(s, "ao"), rough = snaps::image(s, "roughness"), metal = snaps::image(s, "metallic");
    const image::Image& any = ao.width ? ao : rough.width ? rough : metal;
    m.occlusion = ao.width > 0;
    m.roughness = rough.width > 0;
    m.metallic = metal.width > 0;
    if (any.width > 0)
        m.orm = rgba8(any, false, [&](const float* p) {
            const size_t i = size_t(p - any.rgba.data());
            auto at = [&](const image::Image& im, float fallback) { return im.width == any.width && im.height == any.height ? im.rgba[i] : fallback; };
            return glm::vec4(at(ao, 1.0f), at(rough, 0.5f), at(metal, 0.0f), 1.0f);
        });
    return m;
}

/** Puts the snapshot's maps on every material slot drawing the compared set. */
void applySnapshot(render::FrameState& s, const snaps::Snapshot& snap)
{
    scene::Graph& g = scene::Graph::current();
    const Maps& maps = mapsOf(snap);
    std::vector<render::MeshDraw>* draws = nullptr;
    for (size_t i = 0; i < s.meshes.size(); ++i)
    {
        const render::MeshDraw& d = s.meshes[i];
        if (d.materials.empty() || !d.node)
            continue;
        const scene::Node* n = g.node(scene::NodeId(d.node));
        const int outMesh = n ? n->type().attrIndex("outMesh") : -1;
        const scene::MeshPtr* mesh = outMesh >= 0 ? std::get_if<scene::MeshPtr>(&n->value(outMesh)) : nullptr;
        if (!mesh || !*mesh)
            continue;
        for (size_t slot = 0; slot < d.materials.size(); ++slot)
        {
            const scene::Node* material = scene::materials::find(g, scene::materials::slotName(**mesh, int32_t(slot)));
            if (!material)
                continue;
            std::string set = tex::setForMaterial(g, material->name());
            if (set.empty() && tex::findSet(g, material->name()))
                set = material->name();
            if (set != snap.set)
                continue;
            if (!draws)
                draws = &s.meshes.edit();
            render::Material& m = (*draws)[i].materials[slot];
            if (maps.baseColor)
            {
                m.baseColorMap = maps.baseColor;
                m.flags |= render::MatBaseColorMap;
            }
            if (maps.normal)
            {
                m.normalMap = maps.normal;
                m.flags |= render::MatNormalMap | render::MatNormalGreenUp;
            }
            if (maps.orm)
            {
                m.ormMap = maps.orm;
                m.flags &= ~uint32_t(render::MatOcclusionMap | render::MatRoughnessMap | render::MatMetallicMap);
                m.flags |= (maps.occlusion ? uint32_t(render::MatOcclusionMap) : 0u) | (maps.roughness ? uint32_t(render::MatRoughnessMap) : 0u) |
                           (maps.metallic ? uint32_t(render::MatMetallicMap) : 0u);
            }
            if (maps.emissive)
            {
                m.emissiveMap = maps.emissive;
                m.flags |= render::MatEmissiveMap;
            }
            m.ext = {};
            m.heightMap.reset();
        }
    }
}

std::shared_ptr<const snaps::Snapshot> snapshotOf(const std::string& name)
{
    const State& st = state();
    return name.empty() || name == "live" ? nullptr : snaps::find(st.set, name);
}

/** The left side's picture: the frame drawn with the snapshot's maps, kept while the view and scene hold. */
render::TexturePtr leftImage(const render::FrameState& frame, const snaps::Snapshot& snap, int w, int h)
{
    struct Cached
    {
        glm::dmat4 view{0.0}, proj{0.0};
        int w = 0, h = 0;
        uint64_t snapshot = 0, scene = 0;
        render::TexturePtr image;
    };
    static Cached cache;
    const uint64_t scene = scene::Graph::current().version();
    if (cache.image && cache.view == frame.view && cache.proj == frame.proj && cache.w == w && cache.h == h && cache.snapshot == snap.id &&
        cache.scene == scene)
        return cache.image;
    render::FrameState c = frame;
    applySnapshot(c, snap);
    c.pathTrace.on = false;
    c.imagePlanes.clear();
    c.overlay.clear();
    c.overlayTris.clear();
    c.accumulate = 1;
    std::vector<uint8_t> bgra;
    std::string err;
    if (!render::Renderer::get().renderOffscreen(c, w, h, bgra, &err))
    {
        log::warn("Compare: {}", err);
        return nullptr;
    }
    auto t = std::make_shared<render::TextureImage>();
    t->width = w;
    t->height = h;
    t->format = render::TextureFormat::RGBA8;
    t->srgb = true;
    t->rgba = std::move(bgra);
    for (size_t i = 0; i + 3 < t->rgba.size(); i += 4)
        std::swap(t->rgba[i], t->rgba[i + 2]);
    cache = {frame.view, frame.proj, w, h, snap.id, scene, t};
    return t;
}

void drawCompare(render::FrameState& s, OverlayPainter& painter, const ViewInfo& view)
{
    State& st = state();
    if (!st.on)
        return;
    const std::shared_ptr<const snaps::Snapshot> left = snapshotOf(st.snapshot), right = snapshotOf(st.against);
    if (!left)
        return;
    if (right)
        applySnapshot(s, *right);
    const int w = std::max(int(view.width), 16), h = std::max(int(view.height), 16);
    render::TexturePtr image = leftImage(s, *left, w, h);
    const double wipe = std::clamp(st.wipe, 0.0, 1.0);
    if (image)
    {
        render::ImagePlaneDraw d;
        d.screen = true;
        const double x = -1.0 + 2.0 * wipe;
        d.corners[0] = {-1.0, -1.0, 0.0};
        d.corners[1] = {x, -1.0, 0.0};
        d.corners[2] = {x, 1.0, 0.0};
        d.corners[3] = {-1.0, 1.0, 0.0};
        d.uv = glm::vec4(0.0f, 0.0f, float(wipe), 1.0f);
        d.image = image;
        d.depth = render::ImagePlaneDraw::Depth::Front;
        s.imagePlanes.push_back(std::move(d));
    }
    const double x = wipe * view.width;
    const glm::vec4 line(1.0f, 1.0f, 1.0f, 0.9f);
    painter.line(QPointF(x, 0.0), QPointF(x, view.height), st.dragging ? 3.0f : 2.0f, line);
    painter.label(QPointF(std::max(8.0, x - 8.0 - OverlayPainter::textWidth(QString::fromStdString(left->name))), 28.0), QString::fromStdString(left->name));
    painter.label(QPointF(std::min(view.width - 60.0, x + 8.0), 28.0), right ? QString::fromStdString(right->name) : QString("Live"));
}

json describe()
{
    const State& st = state();
    return {{"enabled", st.on}, {"set", st.set}, {"snapshot", st.snapshot}, {"against", st.against}, {"wipe", st.wipe}};
}
} // namespace

void registerCommands()
{
    ops::registerCommand({
        .name = "texCompare",
        .doc = "Wipe-compare a texture set snapshot (texSnapshot) against the live set or another snapshot, in the 3D viewports and "
               "the 2D View: the snapshot shows left of the wipe line, drag the line to move it. With no flags, reports the state. "
               "Returns {enabled, set, snapshot, against, wipe}.",
        .positionalDoc = "texture set",
        .flags = {{"enable", "e", T::Bool, nullptr, "On or off."},
                  {"snapshot", "s", T::String, nullptr, "The snapshot on the left; the newest when left out."},
                  {"against", "a", T::String, nullptr, "live, or another snapshot's name, on the right."},
                  {"wipe", "w", T::Float, nullptr, "Where the line sits across the view, 0 (all live) to 1 (all snapshot).", {}, {}, 0.0, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            State& st = state();
            if (!a.positional.empty() && a.positional[0].is_string())
                st.set = a.positional[0].get<std::string>();
            if (a.has("enable") && a.get<bool>("enable"))
            {
                if (st.set.empty())
                    st.set = currentTextureSet();
                if (st.set.empty() || !tex::findSet(scene::Graph::current(), st.set))
                    throw CommandError("texCompare: name a texture set");
            }
            if (a.has("snapshot"))
                st.snapshot = a.get<std::string>("snapshot");
            if (a.has("against"))
                st.against = a.get<std::string>("against");
            if (a.has("wipe"))
                st.wipe = a.get<double>("wipe");
            if (a.has("enable"))
                st.on = a.get<bool>("enable");
            if (st.on)
            {
                if (st.snapshot.empty() || !tex::snapshots::find(st.set, st.snapshot))
                {
                    const auto all = tex::snapshots::list(st.set);
                    if (all.empty())
                    {
                        st.on = false;
                        throw CommandError("texCompare: " + st.set + " has no snapshot; take one with texSnapshot");
                    }
                    st.snapshot = all.back()->name;
                }
                if (st.against != "live" && !tex::snapshots::find(st.set, st.against))
                    throw CommandError("texCompare: no snapshot " + st.against);
            }
            viewportTools::requestRedraw();
            return describe();
        },
        .examples = {"texSnapshot(\"RockTex\", name=\"before\")\ntexCompare(\"RockTex\", enable=True, snapshot=\"before\", wipe=0.5)"},
    });
    viewportHooks::addDrawer("lila.texCompare", drawCompare);
    viewportHooks::setDragHook("lila.texCompare",
                               {.press =
                                    [](QMouseEvent* e, const ViewInfo& view) {
                                        State& st = state();
                                        if (!st.on || e->button() != Qt::LeftButton || std::abs(e->position().x() - st.wipe * view.width) > 6.0)
                                            return false;
                                        st.dragging = true;
                                        return true;
                                    },
                                .move =
                                    [](QMouseEvent* e, const ViewInfo& view) {
                                        State& st = state();
                                        st.wipe = std::clamp(e->position().x() / std::max(view.width, 1.0), 0.0, 1.0);
                                        viewportTools::requestRedraw();
                                    },
                                .release =
                                    [](QMouseEvent*, const ViewInfo&) {
                                        state().dragging = false;
                                        viewportTools::requestRedraw();
                                    }});
}

void registerActions()
{
    actions::add({.id = "texture.snapshot",
                  .label = "Take Snapshot",
                  .category = "Texture",
                  .run =
                      [] {
                          const std::string set = currentTextureSet();
                          if (set.empty())
                          {
                              log::warn("Take Snapshot: no texture set");
                              return;
                          }
                          ops::execute("texSnapshot", {{"args", {set}}}, CallSource::UI);
                      },
                  .doc = "Keep the current texture set as it looks now, to wipe-compare later work against.",
                  .python = "texSnapshot(\"RockTex\")",
                  .icon = "camera",
                  .keywords = {"snapshot", "before after", "a/b", "history", "compare"}});
    actions::add({.id = "texture.compare",
                  .label = "Compare with Snapshot",
                  .category = "Texture",
                  .run =
                      [] {
                          const bool on = !state().on;
                          try
                          {
                              ops::execute("texCompare", {{"args", {currentTextureSet()}}, {"enable", on}}, CallSource::UI);
                          }
                          catch (const std::exception& e)
                          {
                              log::warn("{}", e.what());
                          }
                      },
                  .doc = "Wipe between the newest snapshot (left) and the live texture set (right); drag the line to move it.",
                  .python = "texCompare(\"RockTex\", enable=True)",
                  .checked = [] { return state().on; },
                  .icon = "columns",
                  .keywords = {"wipe", "before after", "a/b", "split", "diff"}});
    menus::addActions("Texture", {"", "texture.snapshot", "texture.compare"});
}

void paint2D(QPainter& p, const QRectF& imageRect)
{
    const State& st = state();
    if (!st.on)
        return;
    const texture2d::Source src = texture2d::current();
    if (src.set != st.set || src.channel.empty())
        return;
    const std::shared_ptr<const snaps::Snapshot> left = snaps::find(st.set, st.snapshot);
    if (!left)
        return;
    static std::map<std::pair<uint64_t, std::string>, QImage> images;
    auto key = std::make_pair(left->id, src.channel);
    auto it = images.find(key);
    if (it == images.end())
    {
        if (images.size() > 16)
            images.clear();
        const image::Image img = snaps::image(*left, src.channel);
        if (img.width <= 0)
            return;
        std::string type = "color";
        if (scene::Node* set = tex::findSet(scene::Graph::current(), st.set))
            if (const tex::SetSettings s = tex::readSettings(*set); const tex::Channel* c = s.channel(src.channel))
                type = c->type == tex::PortType::Gray ? "gray" : "color";
        it = images.emplace(key, texPreview::toQImage(img, type, texPreview::isColor(type, src.channel))).first;
    }
    const double x = imageRect.left() + imageRect.width() * std::clamp(st.wipe, 0.0, 1.0);
    p.save();
    p.setClipRect(QRectF(imageRect.left(), imageRect.top(), x - imageRect.left(), imageRect.height()));
    p.drawImage(imageRect, it->second);
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 230), 2.0));
    p.drawLine(QPointF(x, imageRect.top()), QPointF(x, imageRect.bottom()));
}
} // namespace lila::ui::texCompare
