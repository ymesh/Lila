#include "ui/QuickMaskOverlay.h"

#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "render/FrameState.h"
#include "render/PaintGpu.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "tex/PaintStrokes.h"
#include "ops/Command.h"
#include "tex/TextureSet.h"
#include "ui/TexturePaintTool.h"
#include "ui/ViewportTool.h"

#include <QColor>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lila::ui::quickMaskOverlay
{
namespace
{
namespace gp = render::paint;

/** Tint images are capped at this size: enough to read the mask's edges, cheap to rebuild while it is painted. */
constexpr int kMaxSide = 1024;

struct Cache
{
    std::string set;
    scene::NodeId id = scene::kNoNode;
    uint64_t paint = 0;
    Style style;
    render::TexturePtr image;
    uint64_t version = 0;
    const void* source = nullptr;
};

Cache& cache()
{
    static Cache c;
    return c;
}

/** Bumped by every paint change, so the tint knows when to read the buffer again. */
uint64_t& paintChanges()
{
    static uint64_t n = 1;
    static const bool listening = [] {
        gp::addChangeListener([](scene::NodeId) {
            ++paintChanges();
            viewportTools::requestRedraw();
        });
        PrefStore::get().onChanged([](const std::string&) { viewportTools::requestRedraw(); });
        return true;
    }();
    (void)listening;
    return n;
}

/** Tint draws the last frame added. */
size_t gLastDraws = 0;

bool sameStyle(const Style& a, const Style& b) { return a.color == b.color && a.opacity == b.opacity; }

/** The set a material slot draws, empty for none. */
std::string slotSet(scene::Graph& g, const geo::Mesh& mesh, size_t slot)
{
    const scene::Node* material = scene::materials::find(g, scene::materials::slotName(mesh, int32_t(slot)));
    if (!material)
        return {};
    std::string set = tex::setForMaterial(g, material->name());
    if (set.empty() && tex::findSet(g, material->name()))
        set = material->name();
    return set;
}
} // namespace

Style style()
{
    const PrefStore& p = PrefStore::get();
    Style s;
    QColor c(QString::fromStdString(p.value<std::string>("general", kColorPref, kDefaultColor)));
    if (!c.isValid())
        c = QColor(kDefaultColor);
    s.color = glm::vec3(float(c.redF()), float(c.greenF()), float(c.blueF()));
    s.opacity = float(std::clamp(p.value<double>("general", kOpacityPref, kDefaultOpacity), 0.0, 1.0));
    s.shown = p.value<bool>("general", kShownPref, true);
    return s;
}

void setShown(bool on)
{
    PrefStore::get().set("general", kShownPref, on);
    viewportTools::requestRedraw();
}

render::TexturePtr tint(const std::string& setName, uint64_t* version)
{
    LILA_ZONE();
    Cache& c = cache();
    const uint64_t paint = paintChanges();
    scene::Graph& g = scene::Graph::current();
    scene::Node* set = setName.empty() ? nullptr : tex::findSet(g, setName);
    const Style st = style();
    auto done = [&](render::TexturePtr img) {
        if (version)
            *version = c.version;
        return img;
    };
    if (!set || !tex::paint::readPaint(*set)->buffer(kBuffer))
    {
        if (c.image)
        {
            c.image.reset();
            ++c.version;
        }
        c.set.clear();
        return done(nullptr);
    }
    if (c.image && c.set == setName && c.id == set->id() && c.paint == paint && sameStyle(c.style, st))
        return done(c.image);
    c.set = setName;
    c.id = set->id();
    c.paint = paint;
    c.style = st;
    ++c.version;
    c.image.reset();
    // A quick mask with nothing painted limits nothing, so it shows nothing.
    const render::GpuImageRef mask = gp::channelImage(set->id(), kBuffer, tex::paint::kMaskChannel);
    if (!mask)
        return done(nullptr);
    // A mask image made by this request fills from the pages on the main queue: read it again after that.
    if (mask.get() != c.source)
    {
        c.source = mask.get();
        mainq::post([] {
            ++paintChanges();
            viewportTools::requestRedraw();
        });
    }
    int w = mask->width(), h = mask->height();
    const int side = std::max(std::max(w, h), 1);
    const double scale = side > kMaxSide ? double(kMaxSide) / double(side) : 1.0;
    w = std::max(int(std::lround(w * scale)), 1);
    h = std::max(int(std::lround(h * scale)), 1);
    const std::vector<uint8_t> covered = gp::viewImage(mask, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), w, h, false, true);
    auto img = std::make_shared<render::TextureImage>();
    img->width = w;
    img->height = h;
    img->format = render::TextureFormat::RGBA8;
    img->srgb = true;
    img->rgba.resize(size_t(w) * size_t(h) * 4);
    const uint8_t r = uint8_t(std::lround(st.color.r * 255.0f)), gr = uint8_t(std::lround(st.color.g * 255.0f)),
                  b = uint8_t(std::lround(st.color.b * 255.0f));
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
    {
        const float out = 1.0f - float(covered[i * 4 + 3]) / 255.0f;
        img->rgba[i * 4] = r;
        img->rgba[i * 4 + 1] = gr;
        img->rgba[i * 4 + 2] = b;
        img->rgba[i * 4 + 3] = uint8_t(std::lround(std::clamp(out * st.opacity, 0.0f, 1.0f) * 255.0f));
    }
    c.image = std::move(img);
    return done(c.image);
}

QImage tintImage(const std::string& set)
{
    const render::TexturePtr t = tint(set);
    if (!t || t->width <= 0)
        return {};
    QImage q(t->width, t->height, QImage::Format_RGBA8888);
    for (int y = 0; y < t->height; ++y)
        std::memcpy(q.scanLine(y), &t->rgba[size_t(y) * size_t(t->width) * 4], size_t(t->width) * 4);
    return q;
}

void addDraws(render::FrameState& fs, const std::string& setName)
{
    LILA_ZONE();
    gLastDraws = 0;
    if (setName.empty() || !style().shown)
        return;
    const render::TexturePtr image = tint(setName);
    if (!image)
        return;
    scene::Graph& g = scene::Graph::current();
    std::vector<render::MeshDraw> extra;
    for (const render::MeshDraw& d : fs.meshes)
    {
        if (!d.node || !d.mesh || !(d.flags & render::DrawShaded))
            continue;
        const scene::Node* n = g.node(scene::NodeId(d.node));
        const int outMesh = n ? n->type().attrIndex("outMesh") : -1;
        const scene::MeshPtr* mesh = outMesh >= 0 ? std::get_if<scene::MeshPtr>(&n->value(outMesh)) : nullptr;
        if (!mesh || !*mesh)
            continue;
        const size_t slots = std::max<size_t>((*mesh)->materialNames().size(), 1);
        std::vector<render::Material> mats(slots);
        bool any = false;
        for (size_t slot = 0; slot < slots; ++slot)
        {
            render::Material& m = mats[slot];
            m.style.model = render::ShadingModel::Unlit;
            m.keepBaseColor = true;
            m.flags = render::MatTranslucent | render::MatTwoSided;
            if (slot < d.materials.size())
                m.flags |= d.materials[slot].flags & render::MatUv1;
            if (slotSet(g, **mesh, slot) != setName)
            {
                m.opacity = 0.0f;
                m.baseColor = glm::vec3(0.0f);
                continue;
            }
            any = true;
            m.baseColor = glm::vec3(1.0f);
            m.baseColorMap = image;
            m.flags |= render::MatBaseColorMap;
        }
        if (!any)
            continue;
        render::MeshDraw t;
        t.mesh = d.mesh;
        t.world = d.world;
        t.instances = d.instances;
        t.materials = std::move(mats);
        t.flags = render::DrawShaded;
        t.lightingChannels = d.lightingChannels;
        extra.push_back(std::move(t));
    }
    gLastDraws = extra.size();
    for (render::MeshDraw& t : extra)
        fs.meshes.push_back(std::move(t));
}

void registerCommands()
{
    ops::registerCommand({
        .name = "texQuickMaskOverlay",
        .doc = "Show or hide the Quick Mask overlay, the tint over what the quick mask leaves out (show=), and report it: {shown, set "
               "(the set the quick mask limits, empty when it holds nothing), tint {width, height, version, tinted (the share of its texels tinted)} or None, draws (tinted mesh "
               "draws in the last viewport frame)}. Colour and opacity are the general prefs /texture/quickMaskColor and /texture/quickMaskOpacity.",
        .flags = {{"show", "s", ArgType::Bool, nullptr, "Show the overlay, or hide it."}},
        .undoable = false,
        .run = [](const Args& a) -> nlohmann::json {
            if (a.has("show"))
                setShown(a.get<bool>("show"));
            const std::string set = quickMaskSet();
            uint64_t version = 0;
            const render::TexturePtr t = set.empty() ? nullptr : tint(set, &version);
            nlohmann::json out = {{"shown", style().shown}, {"set", set}, {"draws", gLastDraws}, {"tint", nullptr}};
            if (t)
            {
                size_t tinted = 0;
                for (size_t i = 3; i < t->rgba.size(); i += 4)
                    tinted += t->rgba[i] > 0 ? 1 : 0;
                out["tint"] = {{"width", t->width}, {"height", t->height}, {"version", version},
                               {"tinted", double(tinted) / double(std::max<size_t>(t->rgba.size() / 4, 1))}};
            }
            return out;
        },
    });
}
} // namespace lila::ui::quickMaskOverlay
