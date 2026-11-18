#include "ui/TexturePaintTool.h"

// Before Qt: its slots macro breaks tex/Plan.h.
#include "render/PaintGpu.h"
#include "tex/BrushPresets.h"
#include "tex/Library.h"
#include "tex/MaterialFromMaps.h"
#include "tex/PaintBuffer.h"
#include "tex/PaintParticles.h"
#include "tex/PatchFill.h"
#include "tex/ShadingChannels.h"
#include "tex/PaintSymmetry.h"
#include "tex/TextureSet.h"

#include "brush/BrushUi.h"
#include "brush/Symmetry.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Symmetry.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/GpuImage.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/ColorPopup.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/QuickMaskOverlay.h"
#include "ui/SculptTool.h"
#include "ui/SpinScrub.h"
#include "sculpt/SculptSession.h"
#include "ui/TexLive.h"
#include "ui/TextureGraphUi.h"
#include "ui/TextureSetPanel.h"
#include "ui/MaterialThumbnails.h"
#include "ui/TexturePicker.h"
#include "ui/UiCommon.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/HotkeyBindings.h"
#include "ui/MaterialDraw.h"
#include "ui/ViewportToast.h"
#include "ui/FileDialog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QDateTime>
#include <QUrl>
#include <QProcess>
#include <QFileSystemWatcher>
#include <QDesktopServices>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <filesystem>
#include <functional>
#include <set>

namespace lila::ui
{
namespace
{
using scene::kNoNode;
using scene::NodeId;
namespace tp = tex::paint;
namespace rp = render::paint;

constexpr const char* kTexturePaintPrefix = "/texturePaint";
constexpr const char* kDimWirePref = "/texturePaint/dimWire";
constexpr const char* kPaintSymmetryPref = "/texturePaint/symmetry";
constexpr const char* kShownSetOnlyPref = "/texturePaint/shownSetOnly";

void ensureTexturePaint();
/** Base colour, roughness, metallic, height and emissive: always in the brush page. The rest show when the set has them. */
constexpr size_t kMainChannels = 5;

struct ChannelValue
{
    const char* name;
    const char* label;
    bool color;
    bool on;
    glm::vec3 value;
};

/** The tools, in the panel's order: what a stroke (or a click) does. */
struct ToolKind
{
    const char* id;
    const char* label;
    const char* tip;
    const char* icon;
};
constexpr ToolKind kTools[] = {
    {"paint", "Paint", "Paints the checked channels (or a library material's).", "brush"},
    {"direction", "Direction", "Writes the stroke's direction: a normal tilt, or a flow map in another channel.", "direction-sign"},
    {"normal", "Normal", "Sculpts the normal with RNM: bumps from the tip, or a normal map stamp.", "ripple"},
    {"smudge", "Smudge", "Drags the paint along the stroke.", "hand-finger"},
    {"blur", "Blur", "Softens the paint.", "blur"},
    {"sharpen", "Sharpen", "Crispens the paint.", "focus-2"},
    {"clone", "Clone", "Copies from the clone source: Ctrl+Alt+click sets it.", "copy"},
    {"heal", "Heal", "Clones detail, keeping the colour around it: Ctrl+Alt+click sets the source.", "bandage"},
    {"path", "Path", "Click points on the surface (drag to curve), Enter paints along them; drag a point afterwards to repaint.", "ease-in-control-point"},
    {"reveal", "Reveal", "Paints a picture in through the brush, pinned to the screen or laid on the uvs.", "photo-scan"},
    {"gradient", "Gradient", "Drag from where it paints fully to where it fades out: linear, radial, reflected or angle.", "transition-right"},
    {"select", "Selection", "Draw a lasso, polygon or rectangle to mask the active layer: Shift adds, Ctrl subtracts.", "lasso"},
    {"pick", "Picker", "Click the mesh to take every channel's value there; Shift+click keeps it as a swatch.", "color-picker"},
    {"fill", "Fill", "Click to fill what the Fill mode picks (the highlight shows it); drag a box to fill everything in it; hold Tab and drag to fill each one the cursor passes over.", "bucket-droplet"},
    {"project", "Projection", "Paint on the screen-space paint buffer, then Enter projects it into every visible texel.", "photo"},
};

/** What a fill click (or a drag's box) fills, in the panel's order. */
struct FillMode
{
    const char* id;
    const char* icon;
    const char* tip;
};
constexpr FillMode kFillModes[] = {
    {"triangle", "triangle", "Triangle: the triangle under the cursor"},
    {"polygon", "square", "Polygon: the face under the cursor"},
    {"island", "puzzle", "UV Island: the uv island under the cursor"},
    {"material", "palette", "Material: every face with the material under the cursor"},
    {"mesh", "cube", "Mesh: the whole painted surface of the mesh"},
    {"selection", "pointer", "Selection: the selected faces"},
};

struct TexturePaintSettings
{
    brush::BrushSettings stroke = [] {
        brush::BrushSettings b;
        b.radius = 40.0;
        b.strength = 1.0;
        b.spacing = 0.1;
        b.falloff = "smooth";
        return b;
    }();
    std::string tool = "paint";
    /** The five every set has, then the ones a set may add (opacity, AO, shading channels), shown when the set has them. */
    std::vector<ChannelValue> channels = [] {
        std::vector<ChannelValue> out = {{"baseColor", "Base Color", true, true, {0.8f, 0.1f, 0.08f}},
                                         {"roughness", "Roughness", false, false, glm::vec3(0.5f)},
                                         {"metallic", "Metallic", false, false, glm::vec3(0.0f)},
                                         {"height", "Height", false, false, glm::vec3(1.0f)},
                                         {"emissive", "Emissive", true, false, glm::vec3(1.0f)},
                                         {"opacity", "Opacity", false, false, glm::vec3(1.0f)},
                                         {"ao", "AO", false, false, glm::vec3(1.0f)}};
        // Coat normal and anisotropy direction are directions: the Normal and Direction tools paint those.
        for (const tex::shading::ChannelInfo& c : tex::shading::channels())
            if (std::string_view(c.name) != "coatNormal" && std::string_view(c.name) != "anisotropyDirection")
                out.push_back({c.name, c.label, c.type == tex::PortType::Color, false, glm::vec3(c.fallback[0], c.fallback[1], c.fallback[2])});
        return out;
    }();
    tp::Blend blend = tp::Blend::Normal;
    float tilt = 0.5f;
    std::string flowChannel = "normal";
    bool occlusion = true;
    float filterRadius = 4.0f;
    float amount = 1.0f;
    std::string normalMode = "raise";
    float intensity = 1.0f;
    std::string stampImage;
    std::string fillBy = "island";
    /** What the stroke paints: "values" (the channel values), "material" (a library material) or "texture" (an image). */
    std::string source = "values";
    /** Library material painted through the stroke (material brush). */
    std::string material;
    /** Image painted through the stroke, sampled in uv. */
    std::string sourceImage;
    /** Material or texture tiles per uv unit. */
    float materialScale = 1.0f;
    /**
     * Varied tiling of the material or texture: hex cells each take it at a random offset and turn, blended so the repeat
     * does not show. Turn is how far a cell may turn (1: a half turn), blend the seams' contrast, brightness a per-cell spread.
     */
    bool varied = false;
    float variedTurn = 0.5f, variedBlend = 0.5f, variedBrightness = 0.0f;
    /** The preset last applied, for the brush row and the library; empty once none was. */
    std::string presetId;
    json tip;
    /** drips, rain, sparks, or empty. */
    std::string particles;
    json particleSpec = json::object();
    // Projection buffer.
    std::string bufferBrush = "paint";
    float bufferScale = 1.0f;
    bool autoProject = false;
    float facingFade = 0.2f;
    bool transform = false;
    bool warp = false;
    /** The stencil every stroke and the buffer paint through; its placement is in buffer pixels. */
    tp::Stencil stencil;
    std::string stencilPath;
    /** "mask" or "color". */
    std::string stencilMode = "mask";
    /** Bumped when the stencil's picture or mode changes, so the tool remakes what it derived from it. */
    uint64_t stencilVersion = 0;
    /** The picker reads the active layer's own paint instead of the whole set. */
    bool pickLayer = false;
    /** The tool the picker hands back to after a pick. */
    std::string pickReturn = "paint";
    /** Picked channel values kept for later, newest last. */
    std::vector<std::map<std::string, glm::vec3>> swatches;
    /** Selection tool: lasso, polygon or rect. */
    std::string selectMode = "lasso";
    /** Quick Mask on: strokes paint the quick mask instead of the layer. */
    bool quickMask = false;
    /** The quick mask strokes are limited by once it is off (a held PaintImage key); empty for none. */
    std::string quickLimit;
    /** Face groups whose faces strokes leave alone. */
    std::set<std::string> lockedGroups;
    /** Reveal: the picture painted through, and whether it sits on the screen or on the uvs. */
    std::string revealImage;
    std::string revealSpace = "screen";
    /** Mirror strokes in uv space too: off, u or v, across the middle of uv space or of each island. */
    std::string uvMirror = "off";
    bool uvMirrorPerIsland = false;
    /** Brush limits (Payload::limits): where on the surface the brush may paint. */
    json limits = json::object();
    /** Gradient tool: linear, radial, reflected or angle, and whether it dithers. */
    std::string gradientType = "linear";
    bool gradientDither = true;
};

constexpr const char* kQuickMaskBuffer = quickMaskOverlay::kBuffer;

TexturePaintSettings& texturePaintState()
{
    static TexturePaintSettings s = [] {
        TexturePaintSettings out;
        out.stroke.loadPrefs(kTexturePaintPrefix);
        return out;
    }();
    return s;
}

void saveTexturePaintSettings() { texturePaintState().stroke.savePrefs(kTexturePaintPrefix); }

/** Sets the stencil from a file; empty clears it. Throws std::runtime_error when the file can't be read. */
void setStencilFile(const std::string& path)
{
    TexturePaintSettings& s = texturePaintState();
    s.stencil = {};
    s.stencilPath.clear();
    ++s.stencilVersion;
    if (path.empty())
        return;
    std::string err;
    image::Image img = image::read(paths::fromUtf8(path), &err);
    if (img.width <= 0)
        throw std::runtime_error("stencil " + path + ": " + err);
    s.stencil.image = std::move(img);
    s.stencilPath = path;
}

bool filterTool(const std::string& t) { return t == "smudge" || t == "blur" || t == "sharpen" || t == "clone" || t == "heal"; }

tp::Payload payloadFor(const TexturePaintSettings& s, bool erase)
{
    tp::Payload p;
    p.kind = s.tool == "project" ? "project" : (s.tool == "path" || s.tool == "pick" || s.tool == "select" || s.tool == "reveal") ? "paint" : s.tool == "gradient" ? "fill" : s.tool;
    for (const ChannelValue& c : s.channels)
        if (c.on)
            p.channels[c.name] = c.color ? c.value : glm::vec3(c.value.x);
    if (s.tool == "direction")
    {
        p.channels.clear();
        p.directionChannel = s.flowChannel;
    }
    if (s.tool == "normal" && (s.normalMode == "raise" || s.normalMode == "lower"))
    {
        // Raised and sunken shapes are height: dabs blend towards one level, so a stroke is one flat plate (no rake), its
        // edge the brush's falloff, and the set turns the height into normals.
        p.kind = "paint";
        p.channels.clear();
        const float h = std::abs(s.intensity);
        p.channels["height"] = glm::vec3(s.normalMode == "lower" ? -h : h);
    }
    else if (s.tool == "normal")
    {
        p.channels.clear();
        p.directionChannel = "normal";
        p.normalMode = s.normalMode;
        p.stampImage = s.stampImage;
        p.intensity = s.intensity;
    }
    p.blend = erase ? tp::Blend::Erase : s.blend;
    p.tilt = s.tilt;
    p.filterRadius = s.filterRadius;
    p.amount = s.amount;
    p.facingFade = s.facingFade;
    p.fillBy = s.fillBy;
    const bool sourced = s.tool == "paint" || s.tool == "fill" || s.tool == "project" || s.tool == "path";
    // Reveal on the uvs: the picture is the stroke's source, sampled where each texel's uv falls.
    if (s.tool == "reveal" && s.revealSpace == "uv" && !s.revealImage.empty())
    {
        p.source = {{"node", "layerImage"}, {"values", {{"file", s.revealImage}}}, {"resolution", 2048}, {"scale", s.materialScale}};
        if (!p.channels.contains("baseColor"))
            p.channels["baseColor"] = glm::vec3(1.0f);
    }
    // A material or texture keeps one size on the surface, its real-world size, however dense each part's uvs are.
    if (sourced && s.source == "material" && !s.material.empty())
    {
        const std::optional<tex::library::Entry> e = tex::library::find(s.material);
        p.source = {{"library", s.material}, {"resolution", 512}, {"scale", s.materialScale}, {"space", "object"}, {"size", e ? e->meta.scaleCm : 100.0}};
    }
    else if (sourced && s.source == "texture" && !s.sourceImage.empty())
        p.source = {{"node", "layerImage"}, {"values", {{"file", s.sourceImage}}}, {"resolution", 1024}, {"scale", s.materialScale}, {"space", "object"}, {"size", 100.0}};
    if (s.varied && p.source.is_object())
        p.source["tiling"] = {{"rotate", s.variedTurn}, {"contrast", s.variedBlend}, {"jitter", s.variedBrightness}};
    if (!s.tip.is_null())
        p.tip = s.tip;
    if (!s.quickMask)
        p.limit = s.quickLimit;
    if (!s.limits.empty())
        p.limits = s.limits;
    // Quick Mask: the stroke paints the quick mask's coverage, nothing else.
    if (s.quickMask && s.tool != "project")
    {
        p.channels.clear();
        p.mask = true;
        p.limit.clear();
    }
    return p;
}

/**
 * A gradient fill over the whole mesh from start to end (screen pixels of view, or uv): the channel values at full
 * strength at the start, fading out to the end (reversed with reverse).
 */
json gradientPayload(const TexturePaintSettings& s, const json& start, const json& end, const std::string& space, bool reverse)
{
    tp::Payload p = payloadFor(s, false);
    p.kind = "fill";
    json pay = p.toJson();
    pay.erase("triangles");
    pay["fillBy"] = "mesh";
    pay["gradient"] = {{"type", s.gradientType},
                       {"space", space},
                       {"start", start},
                       {"end", end},
                       {"dither", s.gradientDither},
                       {"stops", {{{"position", 0.0}, {"amount", reverse ? 0.0 : 1.0}}, {{"position", 1.0}, {"amount", reverse ? 1.0 : 0.0}}}}};
    return pay;
}

} // namespace

/** The set the Texture Set dock shows, else the one the graph or 2D View shows. */
std::string shownTextureSet()
{
    if (TextureSetPanel* p = TextureSetPanel::active())
        return p->textureSet();
    return currentTextureSet();
}

std::string texturePaintSymmetry() { return PrefStore::get().value<std::string>("general", kPaintSymmetryPref, "off"); }

void texturePaintSymmetryFor(const scene::Node& mesh, const geo::Mesh* surface, tex::paint::PaintSymmetry& out)
{
    brush::BrushSettings b = texturePaintState().stroke;
    const std::string s = texturePaintSymmetry();
    if (s == "x" || s == "y" || s == "z")
    {
        const int axis = s == "x" ? 0 : s == "y" ? 1 : 2;
        b.mirror = true;
        out = tp::brushSymmetry(b, surface, axis, scene::symmetryOffset(mesh, axis));
        return;
    }
    // Mesh: whatever symmetry the mesh has, radial included. Off: none, whatever the mesh has.
    b.mirror = s == "mesh" && scene::symmetrySpec(mesh).has_value();
    out = tp::brushSymmetry(b, surface, scene::paintSymmetrySpec(mesh));
}

namespace
{
/** The brush's uv mirror on top of its spatial symmetry. */
void addUvMirror(tp::PaintSymmetry& sym)
{
    const TexturePaintSettings& s = texturePaintState();
    if (s.uvMirror == "off")
        return;
    tp::UvMirror m;
    m.axis = s.uvMirror == "v" ? 1 : 0;
    m.perIsland = s.uvMirrorPerIsland;
    sym.uvMirror = m;
}

/** Triangles of the locked face groups on this mesh, for a stroke's payload. */
std::vector<uint32_t> lockedTriangles(const scene::Node& set, const std::string& meshPath, const tex::paint::PaintMesh& mesh)
{
    std::vector<uint32_t> out;
    const json groups = tex::readSettings(set).faceGroups;
    for (const std::string& name : texturePaintState().lockedGroups)
        if (const json g = groups.value(name, json()); g.is_object() && g.value("mesh", std::string()) == meshPath)
        {
            std::vector<uint32_t> t = mesh.ofFaces(g.value("faces", std::vector<uint32_t>()));
            out.insert(out.end(), t.begin(), t.end());
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/** Applies a preset over the settings: only what it names changes. */
void applyPreset(TexturePaintSettings& s, const tex::brushes::Preset& p)
{
    s.stroke = brush::BrushSettings::fromJson(p.brush, s.stroke);
    const json& pay = p.payload;
    const std::string kind = pay.value("kind", std::string("paint"));
    // A plain paint preset is a brush shape: a stroke tool keeps it. A preset made for one tool switches to that tool.
    static constexpr std::string_view kStrokeTools[] = {"paint", "direction", "normal", "smudge", "blur", "sharpen", "clone", "heal", "path", "reveal"};
    const bool keepTool = kind == "paint" && std::ranges::find(kStrokeTools, std::string_view(s.tool)) != std::end(kStrokeTools);
    if (!keepTool)
        s.tool = kind;
    if (pay.contains("blend"))
        s.blend = tp::blendFromName(pay["blend"].get<std::string>()).value_or(tp::Blend::Normal);
    else
        s.blend = tp::Blend::Normal;
    if (pay.contains("channels") && pay["channels"].is_object() && (kind == "paint" || kind == "fill"))
    {
        for (ChannelValue& c : s.channels)
            c.on = pay["channels"].contains(c.name);
        for (ChannelValue& c : s.channels)
            if (c.on)
            {
                const json& v = pay["channels"][c.name];
                c.value = v.is_array() ? glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()) : glm::vec3(v.get<float>());
            }
    }
    s.filterRadius = pay.value("filterRadius", 4.0f);
    s.amount = pay.value("amount", 1.0f);
    s.tilt = pay.value("tilt", 0.5f);
    s.flowChannel = pay.value("channel", std::string("normal"));
    s.normalMode = pay.value("normalMode", std::string("raise"));
    s.intensity = pay.value("intensity", 1.0f);
    s.fillBy = pay.value("fillBy", std::string("island"));
    s.material = pay.contains("source") ? pay["source"].value("library", std::string()) : std::string();
    s.materialScale = pay.contains("source") ? pay["source"].value("scale", 1.0f) : 1.0f;
    s.source = s.material.empty() ? "values" : "material";
    {
        const json tiling = pay.contains("source") ? pay["source"].value("tiling", json()) : json();
        s.varied = tiling.is_object();
        s.variedTurn = s.varied ? tiling.value("rotate", 0.5f) : 0.5f;
        s.variedBlend = s.varied ? tiling.value("contrast", 0.5f) : 0.5f;
        s.variedBrightness = s.varied ? tiling.value("jitter", 0.0f) : 0.0f;
    }
    s.presetId = p.id;
    s.tip = pay.contains("tip") ? pay["tip"] : json();
    s.limits = pay.contains("limits") && pay["limits"].is_object() ? pay["limits"] : json::object();
    s.particles = p.particles.is_object() ? p.particles.value("kind", std::string()) : std::string();
    s.particleSpec = p.particles.is_object() ? p.particles : json::object();
}

/** Applies picked or kept values to the brush's channels; the channels it has no value for keep theirs. */
void applyChannelValues(const std::map<std::string, glm::vec3>& values)
{
    for (ChannelValue& c : texturePaintState().channels)
        if (auto it = values.find(c.name); it != values.end())
            c.value = c.color ? it->second : glm::vec3(it->second.x);
}

float toDisplay(float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f); }
float fromDisplay(float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 2.2f); }
/** The texture set the material of a face of the mesh uses; null when it uses none. */
scene::Node* setForFace(scene::Graph& g, const geo::Mesh& mesh, geo::Index face)
{
    std::span<const int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    const std::string slot = scene::materials::slotName(mesh, !ids.empty() && face < ids.size() ? ids[face] : 0);
    const std::string set = tex::setForMaterial(g, slot);
    return set.empty() ? nullptr : tex::findSet(g, set);
}

/** A number field; a 0 to 1 amount gets the slider fill every bounded amount in Lila has. */
QDoubleSpinBox* settingSpin(QWidget* parent, double lo, double hi, double step, int decimals, const QString& tip = {})
{
    auto* s = lo >= 0.0 && hi <= 1.0 ? new controls::SliderField(parent) : new QDoubleSpinBox(parent);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setToolTip(tip);
    return s;
}

void setQuickMask(bool on);
void clearQuickMask();

/** Set while the Strokes list waits for a click on the mesh: the next press picks a stroke instead of painting. */
bool gPickingStroke = false;
std::function<void(const json&)> gStrokePicked;

/**
 * The active paint layer's strokes, newest first: pick one from the list or the viewport, then change its colour, size or
 * strength, move it up or down the layer's order, or delete it. Each change re-rasterises the layer from its records.
 */
class StrokeList final : public QWidget
{
public:
    explicit StrokeList(QWidget* parent) : QWidget(parent)
    {
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(4);
        mList = new QListWidget(this);
        mList->setObjectName("strokeList");
        mList->setSelectionMode(QAbstractItemView::SingleSelection);
        mList->setMaximumHeight(140);
        col->addWidget(mList);
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        mColor = new QPushButton(this);
        mColor->setObjectName("strokeColor");
        mColor->setToolTip("Colour: the stroke's colour channel");
        mColor->setFixedWidth(36);
        mSize = settingSpin(this, 1.0, 1000.0, 5.0, 0, "Size: the stroke's dabs scaled, in percent");
        mSize->setObjectName("strokeSize");
        mSize->setSuffix(" %");
        mStrength = settingSpin(this, 0.0, 1.0, 0.05, 2, "Strength: the stroke's brush strength");
        mStrength->setObjectName("strokeStrength");
        row->addWidget(mColor);
        row->addWidget(mSize, 1);
        row->addWidget(mStrength, 1);
        col->addLayout(row);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(2);
        mPick = toolIcon("pointer", "Pick: click a stroke on the mesh to select it");
        mPick->setCheckable(true);
        mUp = toolIcon("arrow-up", "Up: paint this stroke later, over the ones after it");
        mDown = toolIcon("arrow-down", "Down: paint this stroke earlier, under the ones before it");
        mDelete = toolIcon("trash", "Delete the stroke");
        for (QToolButton* b : {mPick, mUp, mDown, mDelete})
            buttons->addWidget(b);
        buttons->addStretch(1);
        col->addLayout(buttons);

        connect(mList, &QListWidget::currentRowChanged, this, [this] { syncEditors(); });
        connect(mPick, &QToolButton::toggled, this, [this](bool on) {
            gPickingStroke = on;
            gStrokePicked = on ? [this](const json& r) {
                mPick->setChecked(false);
                if (r.is_object())
                    select(r.value("id", uint64_t(0)));
            } : std::function<void(const json&)>();
        });
        connect(mColor, &QPushButton::clicked, this, [this] {
            const json* st = current();
            if (!st)
                return;
            glm::vec3 c(1.0f);
            if (const json& ch = (*st)["channels"]; ch.contains("baseColor"))
                c = {ch["baseColor"][0].get<float>(), ch["baseColor"][1].get<float>(), ch["baseColor"][2].get<float>()};
            const QColor picked = colorPopup::getColor(QColor::fromRgbF(toDisplay(c.r), toDisplay(c.g), toDisplay(c.b)), mColor, "Stroke Colour");
            if (picked.isValid())
                edit({{"color", {fromDisplay(float(picked.redF())), fromDisplay(float(picked.greenF())), fromDisplay(float(picked.blueF()))}}});
        });
        connect(mSize, &QDoubleSpinBox::editingFinished, this, [this] {
            if (mSize->value() != 100.0)
                edit({{"sizeScale", mSize->value() / 100.0}});
        });
        connect(mStrength, &QDoubleSpinBox::editingFinished, this, [this] {
            if (const json* st = current(); st && std::abs((*st)["strength"].get<double>() - mStrength->value()) > 1e-6)
                edit({{"strength", mStrength->value()}});
        });
        connect(mUp, &QToolButton::clicked, this, [this] { move(+1); });
        connect(mDown, &QToolButton::clicked, this, [this] { move(-1); });
        connect(mDelete, &QToolButton::clicked, this, [this] {
            if (const json* st = current())
                run("texDeleteStrokes", {{"strokes", {(*st)["id"]}}});
        });
        mPoll.setInterval(400);
        connect(&mPoll, &QTimer::timeout, this, [this] { refresh(); });
        mPoll.start();
    }

    ~StrokeList() override
    {
        if (gPickingStroke)
            gPickingStroke = false;
        gStrokePicked = nullptr;
    }

    void refresh(bool force = false)
    {
        if (!isVisible() && !force)
            return;
        const std::string set = currentTextureSet();
        scene::Node* n = set.empty() ? nullptr : tex::findSet(scene::Graph::current(), set);
        std::string layer = n ? textureSets::activeLayer(set) : std::string();
        // With no layer picked in the Texture Set dock, the one the brush paints.
        if (n && layer.empty())
            try
            {
                layer = rp::targetLayer(*n, "", false);
            }
            catch (const std::exception&)
            {
                layer.clear();
            }
        tex::paint::PaintPtr paint = n ? tex::paint::readPaint(*n) : nullptr;
        if (!force && set == mSet && layer == mLayer && paint == mPaint)
            return;
        mSet = set;
        mLayer = layer;
        mPaint = std::move(paint);
        const uint64_t keep = current() ? (*current())["id"].get<uint64_t>() : 0;
        mStrokes = json::array();
        if (n && !layer.empty())
            try
            {
                mStrokes = ops::execute("texStrokes", {{"args", {set, layer}}}, CallSource::UI);
            }
            catch (const std::exception&)
            {
                mStrokes = json::array();
            }
        const QSignalBlocker block(mList);
        mList->clear();
        for (size_t i = mStrokes.size(); i-- > 0;)
        {
            const json& st = mStrokes[i];
            QString text = QString("%1  %2").arg(st["index"].get<int>() + 1).arg(QString::fromStdString(kindLabel(st["kind"].get<std::string>())));
            if (st.value("path", false))
                text += "  (path)";
            auto* item = new QListWidgetItem(text, mList);
            item->setData(Qt::UserRole, int(i));
            if (const json& ch = st["channels"]; ch.contains("baseColor"))
            {
                QPixmap px(12, 12);
                px.fill(QColor::fromRgbF(toDisplay(ch["baseColor"][0].get<float>()), toDisplay(ch["baseColor"][1].get<float>()), toDisplay(ch["baseColor"][2].get<float>())));
                item->setIcon(QIcon(px));
            }
        }
        if (mStrokes.empty())
            new QListWidgetItem(layer.empty() ? "No paint layer selected" : "No strokes", mList);
        select(keep);
        syncEditors();
    }

protected:
    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        QTimer::singleShot(0, this, [this] { refresh(); });
    }

private:
    static std::string kindLabel(const std::string& kind)
    {
        std::string out = kind;
        if (!out.empty())
            out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
        return out;
    }

    QToolButton* toolIcon(const char* name, const char* tip)
    {
        auto* b = new QToolButton(this);
        b->setIcon(icon(name));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        return b;
    }

    const json* current() const
    {
        const QListWidgetItem* item = mList->currentItem();
        if (!item || !item->data(Qt::UserRole).isValid())
            return nullptr;
        const int i = item->data(Qt::UserRole).toInt();
        return i >= 0 && size_t(i) < mStrokes.size() ? &mStrokes[size_t(i)] : nullptr;
    }

    void select(uint64_t id)
    {
        for (int r = 0; r < mList->count(); ++r)
            if (const QVariant v = mList->item(r)->data(Qt::UserRole); v.isValid() && mStrokes[size_t(v.toInt())]["id"].get<uint64_t>() == id)
            {
                mList->setCurrentRow(r);
                return;
            }
    }

    void syncEditors()
    {
        const json* st = current();
        for (QWidget* w : std::initializer_list<QWidget*>{mColor, mSize, mStrength, mUp, mDown, mDelete})
            w->setEnabled(st != nullptr);
        const QSignalBlocker b1(mSize), b2(mStrength);
        mSize->setValue(100.0);
        mStrength->setValue(st ? (*st)["strength"].get<double>() : 1.0);
        QColor c = palette().button().color();
        if (st)
            if (const json& ch = (*st)["channels"]; ch.contains("baseColor"))
                c = QColor::fromRgbF(toDisplay(ch["baseColor"][0].get<float>()), toDisplay(ch["baseColor"][1].get<float>()), toDisplay(ch["baseColor"][2].get<float>()));
        mColor->setStyleSheet(QString("QPushButton { background: %1; }").arg(c.name()));
        mColor->setEnabled(st && (*st)["channels"].contains("baseColor"));
    }

    void edit(json flags)
    {
        const json* st = current();
        if (!st)
            return;
        flags["stroke"] = (*st)["id"];
        if (json r = run("texEditStroke", flags); r.is_object())
        {
            refresh(true);
            select(r.value("id", uint64_t(0)));
        }
    }

    void move(int by)
    {
        const json* st = current();
        if (!st)
            return;
        const int to = (*st)["index"].get<int>() + by;
        if (to < 0 || size_t(to) >= mStrokes.size())
            return;
        const uint64_t id = (*st)["id"].get<uint64_t>();
        run("texMoveStroke", {{"stroke", id}, {"index", to}});
        select(id);
    }

    json run(const char* cmd, json flags)
    {
        flags["args"] = {mSet, mLayer};
        try
        {
            json r = ops::execute(cmd, flags, CallSource::UI);
            refresh(true);
            return r;
        }
        catch (const std::exception& e)
        {
            log::warn("Strokes: {}", e.what());
            return nullptr;
        }
    }

    QListWidget* mList = nullptr;
    QPushButton* mColor = nullptr;
    QDoubleSpinBox* mSize = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QToolButton *mPick = nullptr, *mUp = nullptr, *mDown = nullptr, *mDelete = nullptr;
    QTimer mPoll;
    std::string mSet, mLayer;
    tex::paint::PaintPtr mPaint;
    json mStrokes = json::array();
};

/** A combo that runs a callback before its list opens, so a list drawn from the scene is current when read. */
class FreshCombo final : public QComboBox
{
public:
    using QComboBox::QComboBox;
    std::function<void()> beforePopup;
    /** Opens in place of the plain list when set. */
    std::function<void()> popup;
    void showPopup() override
    {
        if (beforePopup)
            beforePopup();
        if (popup)
            popup();
        else
            QComboBox::showPopup();
    }
};

/**
 * The brush material picker: the combo's materials searchable, under their category with a thumbnail, scene sets
 * under Scene. Thumbnails not rendered yet render in the background for the next opening.
 */
void pickBrushMaterial(QComboBox* combo)
{
    texturePicker::Options o;
    o.title = "Material";
    o.favourites = "brushMaterials";
    o.current = combo->currentData().toString().toStdString();
    o.projectTextures = false;
    o.none = false;
    o.browse = false;
    std::vector<texturePicker::Item> items;
    for (int i = 0; i < combo->count(); ++i)
    {
        const std::string id = combo->itemData(i).toString().toStdString();
        texturePicker::Item it;
        it.value = id;
        QString group = "Scene";
        it.name = combo->itemText(i);
        if (id.rfind("scene/", 0) == 0)
            it.name.remove(0, int(std::string_view("Scene: ").size()));
        else if (std::optional<tex::library::Entry> e = tex::library::find(id))
        {
            const std::string& cat = e->meta.category;
            group = cat.empty() ? QString("Other") : QString::fromStdString(cat.substr(0, cat.find('/')));
        }
        it.group = group;
        it.file = materialThumbs::cachedFile(id);
        if (it.file.empty())
            materialThumbs::renderAsync(id, {}, [](const materialThumbs::Result&, const std::string&) {});
        items.push_back(std::move(it));
    }
    o.items = [items] { return items; };
    QPointer<QComboBox> self(combo);
    texturePicker::open(combo, std::move(o), [self](const std::string& value) {
        if (!self)
            return;
        if (const int at = self->findData(QString::fromStdString(value)); at >= 0)
            self->setCurrentIndex(at);
    });
}

class TexturePaintPanel final : public QWidget
{
public:
    std::function<void()> onChanged, onNewLayer, onCommit, onClearBuffer, onLoadImage, onLoadStencil, onClearStencil, onImageEditor, onPatchFill;
    std::function<void(const std::string&)> onSaveView, onRecallView;

    explicit TexturePaintPanel(QWidget* parent) : QWidget(parent)
    {
        setObjectName("texturePaintPage");
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(4, 2, 4, 4);
        col->setSpacing(6);
        auto* head = new QHBoxLayout;
        head->setSpacing(4);
        mBrush = new QToolButton(this);
        mBrush->setObjectName("brushPreset");
        controls::setRole(mBrush, "row");
        mBrush->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mBrush->setIconSize(QSize(16, 16));
        mBrush->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mBrush->setToolTip("Brush: pick another in the library's Brushes");
        connect(mBrush, &QToolButton::clicked, this, [] { textureSets::showLibrary("brushes"); });
        head->addWidget(mBrush, 1);
        auto* more = new QToolButton(this);
        more->setObjectName("brushMore");
        more->setIcon(icon("dots"));
        more->setAutoRaise(true);
        more->setPopupMode(QToolButton::InstantPopup);
        more->setToolTip("More: paint layer and preset commands");
        more->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; }");
        auto* moreMenu = new QMenu(more);
        moreMenu->addAction("New Paint Layer", this, [this] { call(onNewLayer); });
        moreMenu->addSeparator();
        moreMenu->addAction("Rasterize Layer", this, [] { actions::run(json("tex.rasterizeLayer")); });
        moreMenu->addAction("Flatten Layer", this, [] { actions::run(json("tex.flattenLayer")); });
        moreMenu->addAction("Clear Layer", this, [] { actions::run(json("tex.clearLayer")); });
        moreMenu->addSeparator();
        QAction* wire = moreMenu->addAction("Wireframe While Painting\tShift+F", this, [] { actions::run(json("tex.paintWire")); });
        wire->setCheckable(true);
        connect(moreMenu, &QMenu::aboutToShow, wire, [wire] { wire->setChecked(texturePaintDimWire()); });
        moreMenu->addSeparator();
        moreMenu->addAction("Save Brush Preset...", this, [this] { savePresetAs(); });
        moreMenu->addAction("Reset Brush", this, [] { actions::run(json("tex.resetBrush")); });
        moreMenu->addAction("Image Editor...", this, [this] {
            const QString exe = ui::fileDialog::getOpenFileName(this, "Image Editor", {}, "Programs (*.exe);;All files (*)");
            if (!exe.isEmpty())
                PrefStore::get().set("general", "/texturePaint/imageEditor", exe.toStdString());
        });
        more->setMenu(moreMenu);
        head->addWidget(more);
        col->addLayout(head);
        mTarget = new QLabel(this);
        mTarget->setWordWrap(true);
        controls::setRole(mTarget, "dim");
        col->addWidget(mTarget);
        auto* form = new QFormLayout;
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        std::vector<QWidget*> tiles;
        for (const ToolKind& t : kTools)
        {
            auto* b = new QToolButton(this);
            b->setObjectName(QString("tool:") + t.id);
            b->setIcon(icon(t.icon));
            b->setIconSize(QSize(18, 18));
            b->setToolButtonStyle(Qt::ToolButtonIconOnly);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setMinimumWidth(24);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setToolTip(QString("%1: %2").arg(t.label, t.tip));
            controls::setRole(b, "tile");
            const std::string id = t.id;
            connect(b, &QToolButton::clicked, this, [this, id] {
                if (id == "pick" && texturePaintState().tool != "pick")
                    texturePaintState().pickReturn = texturePaintState().tool;
                texturePaintState().tool = id;
                syncTools();
                changed();
                ensureTexturePaint();
            });
            mTools.push_back(b);
            tiles.push_back(b);
        }
        // Two icon rows: the brush row above names the current tool, the tooltips name the rest.
        col->addWidget(controls::grid(tiles, int(std::size(kTools) + 1) / 2, this));
        auto* symRow = new QHBoxLayout;
        auto* symLabel = new QLabel("Symmetry", this);
        controls::setRole(symLabel, "dim");
        symRow->addWidget(symLabel);
        mSymmetry = new controls::Segmented(this);
        mSymmetry->setObjectName("paintSymmetry");
        // Painting's own symmetry: the mesh's setting (for modelling and UVs) only applies with Mesh.
        mSymmetry->add("Off", {}, "No symmetry, whatever the mesh's is", 0);
        mSymmetry->add("X", {}, "Mirror strokes across X", 1);
        mSymmetry->add("Y", {}, "Mirror strokes across Y", 2);
        mSymmetry->add("Z", {}, "Mirror strokes across Z", 3);
        mSymmetry->add("Mesh", {}, "The mesh's own symmetry, radial included", 4);
        mSymmetry->setToolTip("Symmetry: how strokes mirror. Separate from the mesh's symmetry unless Mesh is picked");
        symRow->addWidget(mSymmetry, 1);
        col->addLayout(symRow);
        auto* setsRow = new QHBoxLayout;
        auto* setsLabel = new QLabel("Sets", this);
        controls::setRole(setsLabel, "dim");
        setsRow->addWidget(setsLabel);
        mSets = new controls::Segmented(this);
        mSets->setObjectName("paintSets");
        mSets->add("All", {}, "Strokes paint every texture set under the brush", 0);
        mSets->add("Shown", {}, "Strokes paint only the texture set shown in the Texture Set dock", 1);
        mSets->setCurrent(PrefStore::get().value<bool>("general", kShownSetOnlyPref, false) ? 1 : 0);
        connect(mSets, &controls::Segmented::picked, this, [](int id) { PrefStore::get().set("general", kShownSetOnlyPref, id == 1); });
        setsRow->addWidget(mSets, 1);
        col->addLayout(setsRow);
        connect(mSymmetry, &controls::Segmented::picked, this, [](int id) {
            static constexpr std::array<const char*, 5> kNames = {"off", "x", "y", "z", "mesh"};
            PrefStore::get().set("general", kPaintSymmetryPref, std::string(kNames[size_t(std::clamp(id, 0, 4))]));
        });
        mSource = new controls::Segmented(this);
        mSource->setObjectName("brushSource");
        mSource->add("Values", {}, "Paint the channel values below", 0);
        mSource->add("Material", {}, "Paint a library material's channels through the stroke", 1);
        mSource->add("Texture", {}, "Paint an image through the stroke: pick one in the library's Textures", 2);
        connect(mSource, &controls::Segmented::picked, this, [this](int id) {
            texturePaintState().source = id == 1 ? "material" : id == 2 ? "texture" : "values";
            if (id == 1 && texturePaintState().material.empty())
                texturePaintState().material = mMaterial->currentData().toString().toStdString();
            if (id == 2 && texturePaintState().sourceImage.empty())
                textureSets::showLibrary("textures");
            changed();
        });
        mSourceRow = new QWidget(this);
        auto* sourceBox = new QHBoxLayout(mSourceRow);
        sourceBox->setContentsMargins(0, 0, 0, 0);
        sourceBox->addWidget(mSource, 1);
        col->addWidget(mSourceRow);
        auto* extraBox = new QWidget(this);
        auto* extraForm = new QFormLayout(extraBox);
        extraForm->setContentsMargins(0, 0, 0, 0);
        extraForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        for (size_t i = 0; i < texturePaintState().channels.size(); ++i)
        {
            ChannelValue& c = texturePaintState().channels[i];
            QFormLayout* rowForm = i < kMainChannels ? form : extraForm;
            // The label in the right-aligned column like every row; the box leads the field so the boxes line up.
            auto* on = new QCheckBox(this);
            on->setObjectName(QString("channel:") + c.name);
            on->setToolTip(QString("%1: paint this channel").arg(c.label));
            mOn.push_back(on);
            auto* field = new QWidget(this);
            auto* fieldRow = new QHBoxLayout(field);
            fieldRow->setContentsMargins(0, 0, 0, 0);
            fieldRow->setSpacing(6);
            fieldRow->addWidget(on);
            connect(on, &QCheckBox::toggled, this, [this, i](bool v) {
                texturePaintState().channels[i].on = v;
                changed();
            });
            if (c.color)
            {
                auto* swatch = new QPushButton(this);
                swatch->setFixedSize(44, 20);
                swatch->setToolTip("Pick the colour this channel paints");
                auto* bright = new colorPopup::ValueSlider(this);
                bright->setToolTip("Brightness");
                bright->dragged = [this, i](const glm::dvec3& lin) {
                    texturePaintState().channels[i].value = glm::vec3(glm::clamp(lin, 0.0, 1.0));
                    sync();
                    changed();
                };
                fieldRow->addWidget(colorPopup::withEyedropper(swatch, [this, i](const glm::dvec3& c) {
                    const glm::dvec3 s = colorMath::toSrgb(glm::clamp(c, 0.0, 1.0));
                    texturePaintState().channels[i].value = {fromDisplay(float(s.x)), fromDisplay(float(s.y)), fromDisplay(float(s.z))};
                    sync();
                    changed();
                }, bright), 1);
                mChannelRow.push_back(addRow(rowForm, c.label, field));
                mSwatch.push_back(swatch);
                mBright.push_back(bright);
                mValue.push_back(nullptr);
                connect(swatch, &QPushButton::clicked, this, [this, i, swatch] {
                    ChannelValue& cv = texturePaintState().channels[i];
                    const QColor start = QColor::fromRgbF(toDisplay(cv.value.r), toDisplay(cv.value.g), toDisplay(cv.value.b));
                    auto apply = [this, i](const QColor& picked) {
                        texturePaintState().channels[i].value = {fromDisplay(float(picked.redF())), fromDisplay(float(picked.greenF())), fromDisplay(float(picked.blueF()))};
                        sync();
                        changed();
                    };
                    if (const QColor picked = colorPopup::getColor(start, swatch, QString("%1 Colour").arg(cv.label), false, apply); picked.isValid())
                        apply(picked);
                });
            }
            else
            {
                // IOR shows as the index itself; the channel stores (ior - 1) / 2.
                const bool ior = std::string_view(c.name) == "ior";
                auto* v = ior ? settingSpin(this, 1.0, 3.0, 0.05, 3, "Index of refraction: 1.5 for glass and plastic, 1.33 for water")
                              : settingSpin(this, 0.0, 1.0, 0.05, 3);
                fieldRow->addWidget(v, 1);
                mChannelRow.push_back(addRow(rowForm, c.label, field));
                mValue.push_back(v);
                mSwatch.push_back(nullptr);
                mBright.push_back(nullptr);
                connect(v, &QDoubleSpinBox::valueChanged, this, [this, i, ior](double x) {
                    texturePaintState().channels[i].value = glm::vec3(ior ? tex::shading::channelFromIor(float(x)) : float(x));
                    changed();
                });
            }
        }
        mMoreChannels = controls::section("More Channels", extraBox, this, "/texturePaint/moreChannels", true);
        mMoreChannels->setObjectName("moreChannels");
        form->addRow(mMoreChannels);
        setSetChannels({});
        auto* materials = new FreshCombo(this);
        materials->beforePopup = [this] { refreshMaterials(); };
        materials->popup = [materials] { pickBrushMaterial(materials); };
        mMaterial = materials;
        mMaterial->setObjectName("brushMaterial");
        mMaterial->setToolTip("The material the stroke paints: one from the library, or a texture set of this scene (Scene)");
        mMaterial->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mMaterial->setMinimumContentsLength(8);
        mMaterial->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        refreshMaterials();
        auto* editMaterial = new QToolButton(this);
        editMaterial->setObjectName("editBrushMaterial");
        editMaterial->setIcon(icon("node-editor"));
        editMaterial->setAutoRaise(true);
        editMaterial->setToolTip("Open the material's nodes: a scene texture set opens in the Texture Graph, a library material is "
                                 "copied into the scene first");
        auto* newMaterial = new QToolButton(this);
        newMaterial->setObjectName("newBrushMaterial");
        newMaterial->setIcon(icon("plus"));
        newMaterial->setAutoRaise(true);
        newMaterial->setToolTip("New brush material: a texture set of its own, opened in the Texture Graph to build from nodes");
        auto* materialRow = new QHBoxLayout;
        materialRow->setSpacing(2);
        materialRow->addWidget(mMaterial, 1);
        materialRow->addWidget(editMaterial);
        materialRow->addWidget(newMaterial);
        mMaterialRow = addRow(form, "Material", materialRow);
        connect(editMaterial, &QToolButton::clicked, this, [this] { editBrushMaterial(); });
        connect(newMaterial, &QToolButton::clicked, this, [this] { newBrushMaterial(); });
        mTexture = new QToolButton(this);
        mTexture->setObjectName("brushTexture");
        controls::setRole(mTexture, "row");
        mTexture->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mTexture->setIcon(icon("photo"));
        mTexture->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mTexture->setToolTip("The image the stroke paints: pick another in the library's Textures");
        connect(mTexture, &QToolButton::clicked, this, [] { textureSets::showLibrary("textures"); });
        mTextureRow = addRow(form, "Texture", mTexture);
        mMaterialScale = settingSpin(this, 0.05, 64.0, 0.25, 2, "Repeats over the material's own size: 2 paints it at half size. The same on every part, whatever its UVs");
        mTiling = new controls::Segmented(this);
        mTiling->setObjectName("sourceTiling");
        mTiling->add("Repeat", {}, "Repeat: the material or texture tiles as it is", 0);
        mTiling->add("Varied", {}, "Varied: hex cells each take it at a random offset and turn, blended so the repeat does not show", 1);
        auto* scaleRow = new QHBoxLayout;
        scaleRow->addWidget(mMaterialScale, 1);
        scaleRow->addWidget(mTiling);
        mScaleRow = addRow(form, "Scale", scaleRow);
        connect(mTiling, &controls::Segmented::picked, this, [this](int id) {
            texturePaintState().varied = id == 1;
            sync();
            changed();
        });
        mVariedTurn = settingSpin(this, 0.0, 1.0, 0.05, 2, "How far each cell turns: 1 is up to a half turn either way");
        mVariedBlend = settingSpin(this, 0.0, 0.95, 0.05, 2, "Seam contrast: low blends cells softly, high cuts between them");
        mVariedBrightness = settingSpin(this, 0.0, 1.0, 0.05, 2, "Brightness spread of Base Color from cell to cell");
        mVariedTurnRow = addRow(form, "Turn", mVariedTurn);
        mVariedBlendRow = addRow(form, "Seams", mVariedBlend);
        mVariedBrightRow = addRow(form, "Brightness", mVariedBrightness);
        connect(mVariedTurn, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().variedTurn, float(v)); });
        connect(mVariedBlend, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().variedBlend, float(v)); });
        connect(mVariedBrightness, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().variedBrightness, float(v)); });
        mBlend = new QComboBox(this);
        mBlend->addItems({"Normal", "Erase", "Add", "Subtract", "Multiply"});
        form->addRow("Blend", mBlend);

        // Per tool.
        mTilt = settingSpin(this, 0.0, 1.0, 0.05, 2, "Direction: how far the normal leans along the stroke");
        mFlow = new QComboBox(this);
        mFlow->setEditable(true);
        mFlow->addItems({"normal", "strokeDir"});
        mFlow->setToolTip("Direction: the channel written; any channel but normal gets a flow map");
        auto* dirRow = new QHBoxLayout;
        dirRow->addWidget(mTilt);
        dirRow->addWidget(mFlow, 1);
        mDirRow = addRow(form, "Direction", dirRow);
        mNormalMode = new QComboBox(this);
        mNormalMode->addItem("Raise", "raise");
        mNormalMode->addItem("Lower", "lower");
        mNormalMode->addItem("Bump", "bump");
        mNormalMode->addItem("Stamp", "stamp");
        mNormalMode->setToolTip("Raise and Lower paint a flat raised or sunken shape (in the set's height, made into normals), "
                                "its edge the brush's falloff. Bump presses a bump per dab; Stamp lays a normal map");
        mIntensity = settingSpin(this, -4.0, 4.0, 0.1, 2, "Raise and Lower: how high, in height units (1 is the set's Height Depth). "
                                                         "Bump and Stamp: strength; negative digs");
        auto* normalRow = new QHBoxLayout;
        normalRow->addWidget(mNormalMode);
        normalRow->addWidget(mIntensity);
        mNormalRow = addRow(form, "Normal", normalRow);
        mStamp = new QLineEdit(this);
        mStamp->setPlaceholderText("normal map file");
        auto* browseStamp = new QPushButton("...", this);
        browseStamp->setFixedWidth(28);
        auto* stampRow = new QHBoxLayout;
        stampRow->addWidget(mStamp, 1);
        stampRow->addWidget(browseStamp);
        mStampRow = addRow(form, "Stamp", stampRow);
        mFilterRadius = settingSpin(this, 0.5, 64.0, 0.5, 1, "Blur, sharpen and heal: kernel radius in texels");
        mAmount = settingSpin(this, 0.0, 8.0, 0.1, 2, "Smudge: reach in brush radii; sharpen: strength");
        auto* filterRow = new QHBoxLayout;
        filterRow->addWidget(mFilterRadius);
        filterRow->addWidget(mAmount);
        mFilterRow = addRow(form, "Filter", filterRow);
        mFillBy = new controls::Segmented(this);
        mFillBy->setObjectName("fillBy");
        for (size_t f = 0; f < std::size(kFillModes); ++f)
            mFillBy->add({}, kFillModes[f].icon, kFillModes[f].tip, int(f));
        mFillRow = addRow(form, "Fill", mFillBy);
        mParticles = new QComboBox(this);
        mParticles->addItem("None", QString());
        mParticles->addItem("Drips", "drips");
        mParticles->addItem("Rain", "rain");
        mParticles->addItem("Sparks", "sparks");
        mParticles->addItem("Rust Run", "rustRun");
        mParticles->addItem("Leak", "leak");
        mParticles->addItem("Rain Streaks", "rainStreaks");
        mParticles->addItem("Dust Settle", "dustSettle");
        mParticles->addItem("Burn", "burn");
        mParticles->addItem("Scorch", "scorch");
        mParticles->setToolTip("Particles each dab emits, painting as they move over the surface");
        mParticleRow = addRow(form, "Particles", mParticles);
        mPickLayer = new QCheckBox("Layer only", this);
        mPickLayer->setObjectName("pickLayer");
        mPickLayer->setToolTip("Pick the active layer's own paint instead of the whole texture set");
        mPickRow = addRow(form, "Pick", mPickLayer);
        mReveal = new QToolButton(this);
        mReveal->setObjectName("revealImage");
        controls::setRole(mReveal, "row");
        mReveal->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mReveal->setIcon(icon("photo"));
        mReveal->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mReveal->setToolTip("The picture the brush reveals: click to pick another");
        mRevealSpace = new controls::Segmented(this);
        mRevealSpace->setObjectName("revealSpace");
        mRevealSpace->add("Screen", {}, "Screen: the picture is pinned to the view; paint it onto whatever is under it", 0);
        mRevealSpace->add("UV", {}, "UV: the picture lies on the uvs, tiled by Scale", 1);
        auto* revealClear = new QToolButton(this);
        revealClear->setObjectName("revealClear");
        revealClear->setIcon(icon("x"));
        revealClear->setAutoRaise(true);
        revealClear->setToolTip("No reveal picture");
        connect(revealClear, &QToolButton::clicked, this, [this] {
            texturePaintState().revealImage.clear();
            sync();
            changed();
        });
        auto* revealRow = new QHBoxLayout;
        revealRow->setSpacing(2);
        revealRow->addWidget(mReveal, 1);
        revealRow->addWidget(mRevealSpace);
        revealRow->addWidget(revealClear);
        mRevealRow = addRow(form, "Reveal", revealRow);
        connect(mReveal, &QToolButton::clicked, this, [this] {
            const QString f = ui::fileDialog::getOpenFileName(this, "Reveal Picture", {}, "Images (*.png *.jpg *.jpeg *.exr *.tga *.tif *.tiff *.psd)");
            if (!f.isEmpty())
            {
                texturePaintState().revealImage = f.toStdString();
                sync();
                changed();
            }
        });
        connect(mRevealSpace, &controls::Segmented::picked, this, [this](int id) {
            texturePaintState().revealSpace = id == 1 ? "uv" : "screen";
            changed();
        });
        mSelectMode = new controls::Segmented(this);
        mSelectMode->setObjectName("selectMode");
        mSelectMode->add({}, "lasso", "Lasso: drag around what to select", 0);
        mSelectMode->add({}, "lasso-polygon", "Polygon: click corners, Enter or double-click closes it", 1);
        mSelectMode->add({}, "square", "Rectangle: drag a box", 2);
        auto* selectRow = new QHBoxLayout;
        selectRow->addWidget(mSelectMode, 1);
        selectRow->addWidget(controls::push("tex.patchFill", this, "Patch Fill"));
        mSelectRow = addRow(form, "Shape", selectRow);
        mGradient = new controls::Segmented(this);
        mGradient->setObjectName("gradientType");
        mGradient->add("Linear", {}, "Linear: fades along the drag", 0);
        mGradient->add("Radial", {}, "Radial: fades out from the press", 1);
        mGradient->add("Reflected", {}, "Reflected: fades both ways from the press", 2);
        mGradient->add("Angle", {}, "Angle: sweeps round the press", 3);
        mGradientDither = new QCheckBox("Dither", this);
        mGradientDither->setToolTip("Break up banding with a fixed half-step noise");
        auto* gradRow = new QHBoxLayout;
        gradRow->addWidget(mGradient, 1);
        gradRow->addWidget(mGradientDither);
        mGradientRow = addRow(form, "Gradient", gradRow);
        connect(mGradient, &controls::Segmented::picked, this, [this](int id) {
            static const char* kTypes[] = {"linear", "radial", "reflected", "angle"};
            texturePaintState().gradientType = kTypes[std::clamp(id, 0, 3)];
            changed();
        });
        connect(mGradientDither, &QCheckBox::toggled, this, [this](bool on) { set(texturePaintState().gradientDither, on); });
        connect(mSelectMode, &controls::Segmented::picked, this, [this](int id) {
            texturePaintState().selectMode = id == 1 ? "polygon" : id == 2 ? "rect" : "lasso";
            changed();
        });
        mQuickMask = new QToolButton(this);
        mQuickMask->setObjectName("quickMask");
        mQuickMask->setText("Quick Mask");
        mQuickMask->setCheckable(true);
        mQuickMask->setToolTip("Quick Mask: while on, strokes paint a temporary mask; once off, every stroke stays inside it");
        auto* clearQuick = new QToolButton(this);
        clearQuick->setObjectName("clearQuickMask");
        clearQuick->setIcon(icon("x"));
        clearQuick->setAutoRaise(true);
        clearQuick->setToolTip("Clear the quick mask: strokes paint everywhere again");
        mQuickOverlay = new QToolButton(this);
        mQuickOverlay->setObjectName("quickMaskOverlay");
        mQuickOverlay->setIcon(icon("eye"));
        mQuickOverlay->setCheckable(true);
        mQuickOverlay->setAutoRaise(true);
        mQuickOverlay->setChecked(quickMaskOverlay::style().shown);
        mQuickOverlay->setToolTip("Overlay: tint what the quick mask leaves out (colour and opacity in Preferences > General > Textures)");
        connect(mQuickOverlay, &QToolButton::toggled, this, [](bool on) { quickMaskOverlay::setShown(on); });
        // The action, the command and Preferences flip it too.
        const int overlayListener = PrefStore::get().onChanged([button = QPointer<QToolButton>(mQuickOverlay)](const std::string&) {
            if (button && button->isChecked() != quickMaskOverlay::style().shown)
            {
                const QSignalBlocker block(button.data());
                button->setChecked(quickMaskOverlay::style().shown);
            }
        });
        connect(mQuickOverlay, &QObject::destroyed, [overlayListener] { PrefStore::get().removeOnChanged(overlayListener); });
        auto* quickRow = new QHBoxLayout;
        quickRow->addWidget(mQuickMask, 1);
        quickRow->addWidget(mQuickOverlay);
        quickRow->addWidget(clearQuick);
        addRow(form, "Limit", quickRow);
        connect(mQuickMask, &QToolButton::toggled, this, [this](bool on) {
            setQuickMask(on);
            changed();
        });
        connect(clearQuick, &QToolButton::clicked, this, [this] {
            clearQuickMask();
            sync();
            changed();
        });
        mGroups = new QComboBox(this);
        mGroups->setObjectName("faceGroups");
        mGroups->setToolTip("Face groups: named face sets of the texture set");
        auto* groupMenu = new QToolButton(this);
        groupMenu->setIcon(icon("dots"));
        groupMenu->setAutoRaise(true);
        groupMenu->setPopupMode(QToolButton::InstantPopup);
        groupMenu->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; }");
        groupMenu->setToolTip("Face group commands");
        auto* gm = new QMenu(groupMenu);
        gm->addAction("New Group from Selected Faces...", this, [this] { newGroup(); });
        gm->addAction("Mask Active Layer to Group", this, [this] { maskToGroup(); });
        gm->addAction("Fill Group", this, [this] { fillGroup(); });
        gm->addAction("Remove Group", this, [this] { removeGroup(); });
        groupMenu->setMenu(gm);
        mLock = new QToolButton(this);
        mLock->setObjectName("lockGroup");
        mLock->setIcon(icon("lock"));
        mLock->setCheckable(true);
        mLock->setToolTip("Lock: strokes and fills leave this group's faces alone");
        auto* groupRow = new QHBoxLayout;
        groupRow->addWidget(mGroups, 1);
        groupRow->addWidget(mLock);
        groupRow->addWidget(groupMenu);
        addRow(form, "Group", groupRow);
        connect(mLock, &QToolButton::toggled, this, [this](bool on) {
            const std::string g = mGroups->currentText().toStdString();
            if (g.empty())
                return;
            if (on)
                texturePaintState().lockedGroups.insert(g);
            else
                texturePaintState().lockedGroups.erase(g);
            changed();
        });
        connect(mGroups, &QComboBox::currentIndexChanged, this, [this](int) {
            const QSignalBlocker b(mLock);
            mLock->setChecked(texturePaintState().lockedGroups.contains(mGroups->currentText().toStdString()));
        });
        connect(mPickLayer, &QCheckBox::toggled, this, [this](bool v) { set(texturePaintState().pickLayer, v); });
        mSwatchBar = new QWidget(this);
        mSwatchBar->setObjectName("swatches");
        mSwatchLayout = new QHBoxLayout(mSwatchBar);
        mSwatchLayout->setContentsMargins(0, 0, 0, 0);
        mSwatchLayout->setSpacing(2);
        mSwatchRow = addRow(form, "Swatches", mSwatchBar);

        // Projection buffer.
        mBufferBrush = new QComboBox(this);
        for (const char* b : {"paint", "erase", "blur", "smudge", "clone"})
            mBufferBrush->addItem(QString(b).replace(0, 1, QString(b).at(0).toUpper()), b);
        mBufferBrush->setToolTip("What LMB does on the buffer (clone: Ctrl+Alt+click sets the screen source)");
        mBufferScale = settingSpin(this, 0.5, 2.0, 0.25, 2, "Buffer pixels per screen pixel");
        auto* bufRow = new QHBoxLayout;
        bufRow->addWidget(mBufferBrush, 1);
        bufRow->addWidget(mBufferScale);
        mBufferRow = addRow(form, "Buffer", bufRow);
        mTransform = new QCheckBox("Transform", this);
        mTransform->setToolTip("LMB drag moves the buffer, Shift rotates, Ctrl scales");
        mWarp = new QCheckBox("Warp", this);
        mWarp->setToolTip("Drag the grid's points to warp the buffer");
        mAuto = new QCheckBox("Auto project", this);
        mAuto->setToolTip("Project the buffer whenever the camera moves");
        auto* bufFlags = new QHBoxLayout;
        bufFlags->addWidget(mTransform);
        bufFlags->addWidget(mWarp);
        bufFlags->addWidget(mAuto);
        mBufferFlags = addRow(form, "", bufFlags);
        mFacing = settingSpin(this, 0.0, 1.0, 0.05, 2, "Fade the projection toward surfaces seen edge-on");
        mFacingRow = addRow(form, "Edge fade", mFacing);
        auto* bufButtons = new QHBoxLayout;
        auto* commit = new QPushButton("Project", this);
        commit->setToolTip("Project the buffer into the layer (Enter)");
        auto* clearBuf = new QPushButton("Clear", this);
        auto* image = new QPushButton("Image...", this);
        image->setToolTip("Lay a picture on the buffer to project (decals, photo detail)");
        auto* editor = new QPushButton("Edit...", this);
        editor->setObjectName("imageEditor");
        editor->setToolTip("Edit in Image Editor: open what the view shows in your image editor; each save projects back from this view");
        auto* patch = new QPushButton("Patch Fill", this);
        patch->setObjectName("patchFill");
        patch->setToolTip("Patch Fill: paint over what to remove, then rebuild it from what the view shows around it; Project lays it on");
        for (QPushButton* b : {commit, clearBuf, image, editor, patch})
            bufButtons->addWidget(b);
        mBufferButtons = addRow(form, "", bufButtons);
        connect(editor, &QPushButton::clicked, this, [this] { call(onImageEditor); });
        connect(patch, &QPushButton::clicked, this, [this] { call(onPatchFill); });
        mViews = new QComboBox(this);
        mViews->setObjectName("projectionViews");
        mViews->setToolTip("Saved projection views: pick one to look and project from it again");
        auto* saveView = new QToolButton(this);
        saveView->setIcon(icon("camera-plus"));
        saveView->setAutoRaise(true);
        saveView->setToolTip("Save this view for projecting from it again");
        auto* viewsRow = new QHBoxLayout;
        viewsRow->addWidget(mViews, 1);
        viewsRow->addWidget(saveView);
        mViewsRow = addRow(form, "View", viewsRow);
        connect(saveView, &QToolButton::clicked, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "Save Projection View", "Name", QLineEdit::Normal, "view", &ok);
            if (ok && !name.trimmed().isEmpty() && onSaveView)
                onSaveView(name.trimmed().toStdString());
        });
        connect(mViews, &QComboBox::activated, this, [this](int i) {
            if (i > 0 && onRecallView)
                onRecallView(mViews->itemText(i).toStdString());
        });
        connect(commit, &QPushButton::clicked, this, [this] { call(onCommit); });
        connect(clearBuf, &QPushButton::clicked, this, [this] { call(onClearBuffer); });
        connect(image, &QPushButton::clicked, this, [this] { call(onLoadImage); });

        // Stencil: every tool paints through it.
        mStencilPick = new QToolButton(this);
        mStencilPick->setObjectName("stencilPick");
        controls::setRole(mStencilPick, "row");
        mStencilPick->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mStencilPick->setIcon(icon("photo"));
        mStencilPick->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mStencilPick->setToolTip("Stencil: a picture pinned to the screen that strokes paint through. Hold S and drag to move it, "
                                 "with Shift to rotate (add Ctrl to snap to 15 degrees), with Ctrl to scale");
        auto* stencilClear = new QToolButton(this);
        stencilClear->setObjectName("stencilClear");
        stencilClear->setIcon(icon("x"));
        stencilClear->setAutoRaise(true);
        stencilClear->setToolTip("No stencil");
        auto* stencilRow = new QHBoxLayout;
        stencilRow->setSpacing(2);
        stencilRow->addWidget(mStencilPick, 1);
        stencilRow->addWidget(stencilClear);
        addRow(form, "Stencil", stencilRow);
        mStencilMode = new controls::Segmented(this);
        mStencilMode->setObjectName("stencilMode");
        mStencilMode->add("Mask", {}, "Mask: the picture's brightness sets how much each stroke paints", 0);
        mStencilMode->add("Colour", {}, "Colour: strokes paint the picture's colour into Base Color, its alpha as the amount", 1);
        mStencilInvert = new QCheckBox("Invert", this);
        mStencilInvert->setToolTip("Paint where the stencil is dark instead");
        mStencilTile = new QCheckBox("Tile", this);
        mStencilTile->setToolTip("Repeat the picture past its edges");
        auto* stencilOpts = new QHBoxLayout;
        stencilOpts->addWidget(mStencilMode, 1);
        stencilOpts->addWidget(mStencilInvert);
        stencilOpts->addWidget(mStencilTile);
        mStencilOptions = addRow(form, "", stencilOpts);
        connect(mStencilPick, &QToolButton::clicked, this, [this] { call(onLoadStencil); });
        connect(stencilClear, &QToolButton::clicked, this, [this] { call(onClearStencil); });
        connect(mStencilMode, &controls::Segmented::picked, this, [this](int id) {
            texturePaintState().stencilMode = id == 1 ? "color" : "mask";
            ++texturePaintState().stencilVersion;
            changed();
        });
        connect(mStencilInvert, &QCheckBox::toggled, this, [this](bool v) { set(texturePaintState().stencil.invert, v); });
        connect(mStencilTile, &QCheckBox::toggled, this, [this](bool v) { set(texturePaintState().stencil.tile, v); });

        // Limits: map ranges the brush stays inside, a face group, and a fade on surfaces turned away.
        struct LimitRow
        {
            const char* key;
            const char* label;
            const char* tip;
        };
        const LimitRow limitRows[] = {{"cavity", "Cavity", "Paint only where the cavity map is in this range"},
                                      {"ao", "AO", "Paint only where the ambient occlusion map is in this range"},
                                      {"height", "Height", "Paint only where the set's height is in this range"}};
        for (const LimitRow& lr : limitRows)
        {
            auto* on = new QCheckBox(this);
            on->setObjectName(QString("limit:") + lr.key);
            on->setToolTip(lr.tip);
            auto* lo = settingSpin(this, 0.0, 1.0, 0.05, 2, "From");
            auto* hi = settingSpin(this, 0.0, 1.0, 0.05, 2, "To");
            auto* row = new QHBoxLayout;
            row->addWidget(on);
            row->addWidget(lo, 1);
            row->addWidget(hi, 1);
            mLimitRows.push_back(addRow(form, lr.label, row));
            const std::string key = lr.key;
            auto apply = [this, key, on, lo, hi] {
                json& l = texturePaintState().limits;
                if (on->isChecked())
                    l[key] = {lo->value(), hi->value()};
                else
                    l.erase(key);
                lo->setEnabled(on->isChecked());
                hi->setEnabled(on->isChecked());
                changed();
            };
            connect(on, &QCheckBox::toggled, this, apply);
            connect(lo, &QDoubleSpinBox::valueChanged, this, apply);
            connect(hi, &QDoubleSpinBox::valueChanged, this, apply);
            mLimitSync.push_back([key, on, lo, hi] {
                const json& l = texturePaintState().limits;
                const QSignalBlocker b1(on), b2(lo), b3(hi);
                on->setChecked(l.contains(key));
                lo->setValue(l.contains(key) ? l[key][0].get<double>() : 0.0);
                hi->setValue(l.contains(key) ? l[key][1].get<double>() : 1.0);
                lo->setEnabled(on->isChecked());
                hi->setEnabled(on->isChecked());
            });
        }
        mLimitFacing = settingSpin(this, 0.0, 1.0, 0.05, 2, "Fade out surfaces turned away from the view; 0 paints them all");
        mLimitRows.push_back(addRow(form, "Facing", mLimitFacing));
        connect(mLimitFacing, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (v > 0.0)
                texturePaintState().limits["facing"] = v;
            else
                texturePaintState().limits.erase("facing");
            changed();
        });
        mLimitGroup = new QComboBox(this);
        mLimitGroup->setObjectName("limitGroup");
        mLimitGroup->setToolTip("Paint only the faces of this face group");
        mLimitRows.push_back(addRow(form, "Only group", mLimitGroup));
        connect(mLimitGroup, &QComboBox::activated, this, [this](int i) {
            if (i <= 0)
                texturePaintState().limits.erase("group");
            else
                texturePaintState().limits["group"] = mLimitGroup->itemText(i).toStdString();
            changed();
        });

        mRadius = new QSpinBox(this);
        mRadius->setRange(1, 2000);
        mRadius->setSuffix(" px");
        mRadius->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow("Radius", mRadius);
        mStrength = settingSpin(this, 0.0, 1.0, 0.05, 3);
        form->addRow("Strength", mStrength);
        mFalloff = new QComboBox(this);
        for (const char* f : {"smooth", "sphere", "root", "sharp", "linear", "constant", "bell"})
            mFalloff->addItem(QString(f).replace(0, 1, QString(f).at(0).toUpper()), f);
        form->addRow("Falloff", mFalloff);
        mOcclusion = new QCheckBox("Only what the view sees", this);
        mOcclusion->setToolTip("Leave surfaces hidden from the view unpainted (the painted mesh only)");
        form->addRow(mOcclusion);
        mUvMirror = new controls::Segmented(this);
        mUvMirror->setObjectName("uvMirror");
        mUvMirror->add("Off", {}, "No uv mirror", 0);
        mUvMirror->add("U", {}, "Mirror strokes across u: left and right halves of a symmetric unwrap", 1);
        mUvMirror->add("V", {}, "Mirror strokes across v: top and bottom halves", 2);
        mUvIsland = new QCheckBox("Per island", this);
        mUvIsland->setToolTip("Mirror across each uv island's own middle instead of the middle of uv space");
        auto* uvRow = new QHBoxLayout;
        uvRow->addWidget(mUvMirror, 1);
        uvRow->addWidget(mUvIsland);
        addRow(form, "UV mirror", uvRow);
        connect(mUvMirror, &controls::Segmented::picked, this, [this](int id) {
            texturePaintState().uvMirror = id == 1 ? "u" : id == 2 ? "v" : "off";
            changed();
        });
        connect(mUvIsland, &QCheckBox::toggled, this, [this](bool on) { set(texturePaintState().uvMirrorPerIsland, on); });
        col->addLayout(form);
        mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return texturePaintState().stroke; }, [this] { changed(); });
        col->addWidget(mStrokeOptions);
        col->addWidget(controls::section("Strokes", new StrokeList(this), this, "/texturePaint/collapsed", false));

        connect(browseStamp, &QPushButton::clicked, this, [this] {
            const QString f = ui::fileDialog::getOpenFileName(this, "Normal Map Stamp", mStamp->text(), "Images (*.png *.exr *.tga *.tif *.tiff *.psd)");
            if (!f.isEmpty())
                mStamp->setText(f);
            texturePaintState().stampImage = mStamp->text().toStdString();
            changed();
        });

        connect(mMaterial, &QComboBox::currentIndexChanged, this, [this](int i) {
            texturePaintState().material = mMaterial->itemData(i).toString().toStdString();
            changed();
        });
        connect(mMaterialScale, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().materialScale, float(v)); });
        connect(mBlend, &QComboBox::currentIndexChanged, this, [this](int i) {
            texturePaintState().blend = tp::Blend(std::clamp(i, 0, 4));
            changed();
        });
        connect(mTilt, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().tilt, float(v)); });
        connect(mFlow, &QComboBox::currentTextChanged, this, [this](const QString& t) {
            texturePaintState().flowChannel = t.isEmpty() ? "normal" : t.toStdString();
            changed();
        });
        connect(mNormalMode, &QComboBox::currentIndexChanged, this, [this](int i) {
            texturePaintState().normalMode = mNormalMode->itemData(i).toString().toStdString();
            changed();
        });
        connect(mIntensity, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().intensity, float(v)); });
        connect(mStamp, &QLineEdit::editingFinished, this, [this] {
            texturePaintState().stampImage = mStamp->text().toStdString();
            changed();
        });
        connect(mFilterRadius, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().filterRadius, float(v)); });
        connect(mAmount, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().amount, float(v)); });
        connect(mFillBy, &controls::Segmented::picked, this, [this](int i) {
            texturePaintState().fillBy = kFillModes[std::clamp(i, 0, int(std::size(kFillModes)) - 1)].id;
            changed();
        });
        connect(mParticles, &QComboBox::currentIndexChanged, this, [this](int i) {
            texturePaintState().particles = mParticles->itemData(i).toString().toStdString();
            changed();
        });
        connect(mBufferBrush, &QComboBox::currentIndexChanged, this, [this](int i) {
            texturePaintState().bufferBrush = mBufferBrush->itemData(i).toString().toStdString();
            changed();
        });
        connect(mBufferScale, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().bufferScale, float(v)); });
        connect(mTransform, &QCheckBox::toggled, this, [this](bool v) {
            texturePaintState().transform = v;
            if (v)
                texturePaintState().warp = false;
            sync();
            changed();
        });
        connect(mWarp, &QCheckBox::toggled, this, [this](bool v) {
            texturePaintState().warp = v;
            if (v)
                texturePaintState().transform = false;
            sync();
            changed();
        });
        connect(mAuto, &QCheckBox::toggled, this, [this](bool v) {
            texturePaintState().autoProject = v;
            changed();
        });
        connect(mFacing, &QDoubleSpinBox::valueChanged, this, [this](double v) { set(texturePaintState().facingFade, float(v)); });
        connect(mRadius, &QSpinBox::valueChanged, this, [this](int v) {
            texturePaintState().stroke.radius = v;
            changed();
        });
        connect(mStrength, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            texturePaintState().stroke.strength = v;
            changed();
        });
        connect(mFalloff, &QComboBox::currentIndexChanged, this, [this](int i) {
            texturePaintState().stroke.falloff = mFalloff->itemData(i).toString().toStdString();
            changed();
        });
        connect(mOcclusion, &QCheckBox::toggled, this, [this](bool v) {
            texturePaintState().occlusion = v;
            changed();
        });
        recordDefaults();
        sync();
    }

    /** Gives each setting field the built-in value as its default, for middle-click reset and Reset All. */
    void recordDefaults()
    {
        TexturePaintSettings& s = texturePaintState();
        TexturePaintSettings kept = std::move(s);
        s = TexturePaintSettings{};
        sync();
        // The Strokes list shows a picked stroke's values, which have no default.
        auto setting = [this](QWidget* w) {
            if (w->property(kDefaultProperty).isValid())
                return false;
            for (QWidget* p = w->parentWidget(); p && p != this; p = p->parentWidget())
                if (dynamic_cast<StrokeList*>(p))
                    return false;
            return true;
        };
        for (QDoubleSpinBox* box : findChildren<QDoubleSpinBox*>())
            if (setting(box))
                setSpinDefault(box, box->value());
        for (QSpinBox* box : findChildren<QSpinBox*>())
            if (setting(box))
                setSpinDefault(box, box->value());
        for (QCheckBox* box : findChildren<QCheckBox*>())
            if (setting(box))
                setCheckDefault(box, box->isChecked());
        for (QComboBox* box : {mBlend, mNormalMode, mFalloff, mParticles, mBufferBrush})
            if (setting(box))
                setComboDefault(box, box->currentIndex());
        s = std::move(kept);
    }

    void setTarget(const QString& text) { mTarget->setText(text); }

    /** Shows the More Channels rows of the channels the set under the brush has; none when there is no set. */
    void setSetChannels(const std::set<std::string>& names)
    {
        bool any = false;
        for (size_t i = kMainChannels; i < mChannelRow.size(); ++i)
        {
            const bool has = names.contains(texturePaintState().channels[i].name);
            showRow(mChannelRow[i], has);
            any |= has;
        }
        if (mMoreChannels)
            mMoreChannels->setVisible(any);
    }


    void syncSymmetry()
    {
        const std::string s = texturePaintSymmetry();
        const QSignalBlocker block(mSymmetry);
        mSymmetry->setCurrent(s == "x" ? 1 : s == "y" ? 2 : s == "z" ? 3 : s == "mesh" ? 4 : 0);
    }

    void syncTools()
    {
        for (size_t i = 0; i < mTools.size(); ++i)
            mTools[i]->setChecked(texturePaintState().tool == kTools[i].id);
    }

    void sync()
    {
        const TexturePaintSettings& s = texturePaintState();
        syncTools();
        const QSignalBlocker b1(mBlend), b2(mTilt), b3(mRadius), b4(mStrength), b5(mFalloff), b6(mOcclusion);
        const QSignalBlocker b8(mMaterial), b9(mMaterialScale), b10(mFlow), b11(mNormalMode), b12(mIntensity), b13(mStamp), b14(mFilterRadius);
        const QSignalBlocker b15(mAmount), b17(mParticles), b18(mBufferBrush), b19(mBufferScale), b20(mTransform), b21(mWarp);
        const QSignalBlocker b22(mAuto), b23(mFacing), b24(mVariedTurn), b25(mVariedBlend), b26(mVariedBrightness);
        mBlend->setCurrentIndex(int(s.blend));
        mTilt->setValue(s.tilt);
        mFlow->setCurrentText(QString::fromStdString(s.flowChannel));
        mNormalMode->setCurrentIndex(std::max(0, mNormalMode->findData(QString::fromStdString(s.normalMode))));
        mIntensity->setValue(s.intensity);
        mStamp->setText(QString::fromStdString(s.stampImage));
        mFilterRadius->setValue(s.filterRadius);
        mAmount->setValue(s.amount);
        for (size_t f = 0; f < std::size(kFillModes); ++f)
            if (s.fillBy == kFillModes[f].id)
                mFillBy->setCurrent(int(f));
        mParticles->setCurrentIndex(std::max(0, mParticles->findData(QString::fromStdString(s.particles))));
        mMaterial->setCurrentIndex(std::max(0, mMaterial->findData(QString::fromStdString(s.material))));
        mTiling->setCurrent(s.varied ? 1 : 0);
        mVariedTurn->setValue(s.variedTurn);
        mVariedBlend->setValue(s.variedBlend);
        mVariedBrightness->setValue(s.variedBrightness);
        mMaterialScale->setValue(s.materialScale);
        mBufferBrush->setCurrentIndex(std::max(0, mBufferBrush->findData(QString::fromStdString(s.bufferBrush))));
        mBufferScale->setValue(s.bufferScale);
        mTransform->setChecked(s.transform);
        mWarp->setChecked(s.warp);
        mAuto->setChecked(s.autoProject);
        mFacing->setValue(s.facingFade);
        mRadius->setValue(int(s.stroke.radius));
        mStrength->setValue(s.stroke.strength);
        mFalloff->setCurrentIndex(std::max(0, mFalloff->findData(QString::fromStdString(s.stroke.falloff))));
        mOcclusion->setChecked(s.occlusion);
        mUvMirror->setCurrent(s.uvMirror == "u" ? 1 : s.uvMirror == "v" ? 2 : 0);
        {
            const QSignalBlocker bu(mUvIsland);
            mUvIsland->setChecked(s.uvMirrorPerIsland);
        }
        syncSymmetry();
        mSource->setCurrent(s.source == "material" ? 1 : s.source == "texture" ? 2 : 0);
        mTexture->setText(s.sourceImage.empty() ? QString("None") : QFileInfo(QString::fromStdString(s.sourceImage)).completeBaseName());
        mStencilPick->setText(!s.stencil.active() ? QString("None")
                              : s.stencilPath.empty() ? QString("Picture")
                                                      : QFileInfo(QString::fromStdString(s.stencilPath)).completeBaseName());
        mStencilMode->setCurrent(s.stencilMode == "color" ? 1 : 0);
        {
            const QSignalBlocker bp(mPickLayer);
            mPickLayer->setChecked(s.pickLayer);
        }
        syncSwatches();
        for (auto& fn : mLimitSync)
            fn();
        {
            const QSignalBlocker bf(mLimitFacing), bg(mLimitGroup);
            mLimitFacing->setValue(s.limits.value("facing", 0.0));
            mLimitGroup->clear();
            mLimitGroup->addItem("Any");
            const std::string setName = currentTextureSet();
            if (scene::Node* set = setName.empty() ? nullptr : tex::findSet(scene::Graph::current(), setName))
                for (const auto& [name, g] : tex::readSettings(*set).faceGroups.items())
                    mLimitGroup->addItem(QString::fromStdString(name));
            mLimitGroup->setCurrentText(QString::fromStdString(s.limits.value("group", std::string("Any"))));
        }
        {
            const QSignalBlocker bv(mViews);
            mViews->clear();
            mViews->addItem("Current");
            const std::string setName = currentTextureSet();
            if (scene::Node* set = setName.empty() ? nullptr : tex::findSet(scene::Graph::current(), setName))
                for (const auto& [name, v] : tex::readSettings(*set).projectionViews.items())
                    mViews->addItem(QString::fromStdString(name));
        }
        mSelectMode->setCurrent(s.selectMode == "polygon" ? 1 : s.selectMode == "rect" ? 2 : 0);
        mReveal->setText(s.revealImage.empty() ? QString("None") : QFileInfo(QString::fromStdString(s.revealImage)).completeBaseName());
        mRevealSpace->setCurrent(s.revealSpace == "uv" ? 1 : 0);
        mGradient->setCurrent(s.gradientType == "radial" ? 1 : s.gradientType == "reflected" ? 2 : s.gradientType == "angle" ? 3 : 0);
        {
            const QSignalBlocker bd(mGradientDither);
            mGradientDither->setChecked(s.gradientDither);
        }
        {
            const QSignalBlocker bq(mQuickMask), bo(mQuickOverlay), bg(mGroups), bl(mLock);
            mQuickMask->setChecked(s.quickMask);
            mQuickOverlay->setChecked(quickMaskOverlay::style().shown);
            const QString keep = mGroups->currentText();
            mGroups->clear();
            const std::string setName = currentTextureSet();
            if (scene::Node* set = setName.empty() ? nullptr : tex::findSet(scene::Graph::current(), setName))
                for (const auto& [name, g] : tex::readSettings(*set).faceGroups.items())
                    mGroups->addItem(QString::fromStdString(name));
            mGroups->setCurrentText(keep);
            mLock->setChecked(s.lockedGroups.contains(mGroups->currentText().toStdString()));
            mLock->setEnabled(mGroups->count() > 0);
        }
        {
            const QSignalBlocker bi(mStencilInvert), bt(mStencilTile);
            mStencilInvert->setChecked(s.stencil.invert);
            mStencilTile->setChecked(s.stencil.tile);
        }
        const ToolKind* kind = &kTools[0];
        for (const ToolKind& t : kTools)
            if (s.tool == t.id)
                kind = &t;
        mBrush->setIcon(icon(kind->icon));
        std::optional<tex::brushes::Preset> preset = s.presetId.empty() ? std::nullopt : tex::brushes::find(s.presetId);
        mBrush->setText(preset ? QString::fromStdString(preset->name) : QString(kind->label));
        for (size_t i = 0; i < s.channels.size(); ++i)
        {
            const QSignalBlocker b(mOn[i]);
            mOn[i]->setChecked(s.channels[i].on);
            if (mValue[i])
            {
                const QSignalBlocker bv(mValue[i]);
                const bool ior = std::string_view(s.channels[i].name) == "ior";
                mValue[i]->setValue(ior ? tex::shading::iorFromChannel(s.channels[i].value.x) : s.channels[i].value.x);
            }
            if (mSwatch[i])
            {
                const glm::vec3 v = s.channels[i].value;
                // The swatch shows the painted colour (data), not chrome.
                mSwatch[i]->setStyleSheet(QString("background-color: %1; border: 1px solid #888;")
                                              .arg(QColor::fromRgbF(toDisplay(v.r), toDisplay(v.g), toDisplay(v.b)).name()));
                if (mBright[i])
                    mBright[i]->setColor(glm::dvec3(v));
            }
        }
        showForTool();
        if (mStrokeOptions)
            mStrokeOptions->sync();
    }

private:
    QWidget* addRow(QFormLayout* form, const QString& label, QLayout* content)
    {
        auto* w = new QWidget(this);
        content->setContentsMargins(0, 0, 0, 0);
        w->setLayout(content);
        form->addRow(label, w);
        mLabels[w] = form->labelForField(w);
        return w;
    }
    QWidget* addRow(QFormLayout* form, const QString& label, QWidget* content)
    {
        auto* box = new QHBoxLayout;
        box->addWidget(content);
        return addRow(form, label, box);
    }

    void showRow(QWidget* row, bool on)
    {
        row->setVisible(on);
        if (QWidget* l = mLabels[row])
            l->setVisible(on);
    }

    void showForTool()
    {
        const std::string& t = texturePaintState().tool;
        const bool values = t == "paint" || t == "fill" || t == "project" || t == "path";
        const std::string& source = texturePaintState().source;
        for (QCheckBox* on : mOn)
            on->setEnabled(values || filterTool(t));
        // The channel values only paint with the Values source; the others say which channels they write.
        for (QWidget* w : mValue)
            if (w)
                w->setEnabled(values && source == "values");
        for (QWidget* w : mSwatch)
            if (w)
                w->setEnabled(values && source == "values");
        for (QWidget* w : mBright)
            if (w)
                w->setEnabled(values && source == "values");
        showRow(mSourceRow, values);
        showRow(mMaterialRow, values && source == "material");
        showRow(mTextureRow, values && source == "texture");
        showRow(mScaleRow, values && source != "values");
        const bool varied = values && source != "values" && texturePaintState().varied;
        showRow(mVariedTurnRow, varied);
        showRow(mVariedBlendRow, varied);
        showRow(mVariedBrightRow, varied);
        showRow(mDirRow, t == "direction");
        showRow(mNormalRow, t == "normal");
        showRow(mStampRow, t == "normal" && texturePaintState().normalMode == "stamp");
        showRow(mFilterRow, filterTool(t));
        showRow(mFillRow, t == "fill");
        showRow(mParticleRow, t == "paint");
        showRow(mPickRow, t == "pick");
        showRow(mRevealRow, t == "reveal");
        showRow(mSelectRow, t == "select");
        showRow(mGradientRow, t == "gradient");
        showRow(mSwatchRow, !texturePaintState().swatches.empty());
        for (QWidget* w : {mBufferRow, mBufferFlags, mFacingRow, mBufferButtons, mViewsRow})
            showRow(w, t == "project");
        showRow(mStencilOptions, texturePaintState().stencil.active());
    }

    void newGroup()
    {
        const std::string set = currentTextureSet();
        if (set.empty())
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "New Face Group", "Name", QLineEdit::Normal, "group", &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        try
        {
            ops::execute("texFaceGroup", {{"args", {set, name.trimmed().toStdString()}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        sync();
        mGroups->setCurrentText(name.trimmed());
    }

    void maskToGroup()
    {
        const std::string set = currentTextureSet(), group = mGroups->currentText().toStdString();
        const std::string layer = textureSets::activeLayer(set);
        if (set.empty() || group.empty() || layer.empty())
        {
            log::warn("Texture paint: pick a face group and select a layer first");
            return;
        }
        try
        {
            ops::execute("texSelectionMask", {{"args", {set, layer}}, {"group", group}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
    }

    void fillGroup()
    {
        const std::string set = currentTextureSet(), group = mGroups->currentText().toStdString();
        if (set.empty() || group.empty())
            return;
        tp::Payload p = payloadFor(texturePaintState(), false);
        p.kind = "fill";
        json payload = p.toJson();
        payload.erase("triangles");
        payload["fillBy"] = "group";
        payload["group"] = group;
        const std::string layer = textureSets::activeLayer(set);
        try
        {
            ops::execute("texPaintStroke", {{"args", {set, layer}}, {"stroke", {{"brush", texturePaintState().stroke.toJson()}, {"payload", payload}}}},
                         CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
    }

    void removeGroup()
    {
        const std::string set = currentTextureSet(), group = mGroups->currentText().toStdString();
        if (set.empty() || group.empty())
            return;
        texturePaintState().lockedGroups.erase(group);
        ops::execute("texFaceGroup", {{"args", {set, group}}, {"remove", true}}, CallSource::UI);
        sync();
    }

    /** One small square per kept swatch, showing its colour (or its first value as gray); a click paints with it. */
    void syncSwatches()
    {
        while (QLayoutItem* it = mSwatchLayout->takeAt(0))
        {
            delete it->widget();
            delete it;
        }
        const auto& swatches = texturePaintState().swatches;
        for (size_t i = 0; i < swatches.size(); ++i)
        {
            const auto& sw = swatches[i];
            glm::vec3 c = sw.contains("baseColor") ? sw.at("baseColor") : sw.empty() ? glm::vec3(0.5f) : glm::vec3(sw.begin()->second.x);
            auto* b = new QToolButton(mSwatchBar);
            b->setObjectName(QString("swatch:%1").arg(i));
            b->setFixedSize(18, 18);
            b->setProperty("lilaRawColors", true);
            b->setStyleSheet(QString("background-color: %1; border: 1px solid #888;").arg(QColor::fromRgbF(toDisplay(c.r), toDisplay(c.g), toDisplay(c.b)).name()));
            QStringList tip;
            for (const auto& [name, v] : sw)
                tip << QString("%1 %2").arg(QString::fromStdString(name)).arg(v.x, 0, 'f', 3);
            tip << "Click to paint with it, right-click to remove";
            b->setToolTip(tip.join(QChar('\n')));
            connect(b, &QToolButton::clicked, this, [this, i] {
                if (i < texturePaintState().swatches.size())
                    applyChannelValues(texturePaintState().swatches[i]);
                sync();
                changed();
            });
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QToolButton::customContextMenuRequested, this, [this, i] {
                auto& all = texturePaintState().swatches;
                if (i < all.size())
                    all.erase(all.begin() + std::ptrdiff_t(i));
                QTimer::singleShot(0, this, [this] { sync(); });
            });
            mSwatchLayout->addWidget(b);
        }
        mSwatchLayout->addStretch(1);
    }

    /** The material list: the library's, then the scene's texture sets. Rebuilt only when the ids change. */
    void refreshMaterials()
    {
        QStringList ids;
        std::vector<std::pair<QString, QString>> items;
        for (const tex::library::Entry& e : tex::library::all())
        {
            const QString name = QString::fromStdString(e.meta.name);
            items.emplace_back(e.tier == "scene" ? "Scene: " + name : name, QString::fromStdString(e.id));
            ids << items.back().second;
        }
        QStringList have;
        for (int i = 0; i < mMaterial->count(); ++i)
            have << mMaterial->itemData(i).toString();
        if (have == ids)
            return;
        const QSignalBlocker block(mMaterial);
        const QString current = QString::fromStdString(texturePaintState().material);
        mMaterial->clear();
        for (const auto& [label, id] : items)
            mMaterial->addItem(label, id);
        mMaterial->setCurrentIndex(std::max(0, mMaterial->findData(current)));
    }

    /** Picks a material as the brush's source and shows the Material source. */
    void useMaterial(const std::string& id)
    {
        TexturePaintSettings& s = texturePaintState();
        s.material = id;
        s.source = "material";
        refreshMaterials();
        sync();
        changed();
    }

    /** Creates a texture set named after base (made a valid, unused name). Returns its name. Throws. */
    static std::string createSet(const std::string& base)
    {
        std::string name;
        for (char c : base)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                name += c;
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
            name = "Brush" + name;
        for (int i = 1;; ++i)
        {
            const std::string tryName = i == 1 ? name : name + std::to_string(i);
            if (scene::Graph::current().find(tryName) == nullptr)
                return ops::execute("texCreateSet", {{"args", {tryName}}}, CallSource::UI).get<std::string>();
        }
    }

    /** A new texture set to build a brush material in from nodes, opened in the Texture Graph and picked as the source. */
    void newBrushMaterial()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "New Brush Material", "Name", QLineEdit::Normal, "BrushMaterial", &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        try
        {
            const std::string made = createSet(name.toStdString());
            useMaterial("scene/" + made);
            openTextureSet(made);
        }
        catch (const std::exception& e)
        {
            log::error("New brush material: {}", e.what());
        }
    }

    /** Opens the brush's material in the Texture Graph; a library material is copied into a scene set to edit. */
    void editBrushMaterial()
    {
        const std::string id = texturePaintState().material;
        if (id.empty())
            return;
        if (id.rfind("scene/", 0) == 0)
        {
            openTextureSet(id.substr(6));
            return;
        }
        std::optional<tex::library::Entry> e = tex::library::find(id);
        if (!e)
        {
            log::warn("Brush material {} isn't in the library any more", id);
            return;
        }
        try
        {
            UndoChunk chunk("Edit Brush Material");
            const std::string made = createSet(e->meta.name);
            ops::execute("applyLibraryMaterial", {{"args", {id}}, {"set", made}, {"scale", false}}, CallSource::UI);
            log::info("Copied {} into the scene as {} to edit its nodes; the brush paints the copy", e->meta.name, made);
            useMaterial("scene/" + made);
            openTextureSet(made);
        }
        catch (const std::exception& ex)
        {
            log::error("Edit brush material: {}", ex.what());
        }
    }

    template <class T> void set(T& field, T v)
    {
        field = v;
        changed();
    }

    void call(const std::function<void()>& fn)
    {
        if (fn)
            fn();
    }

    void changed()
    {
        saveTexturePaintSettings();
        showForTool();
        if (onChanged)
            onChanged();
    }

    void savePresetAs()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save Brush Preset", "Name", QLineEdit::Normal, {}, &ok);
        if (!ok || name.isEmpty())
            return;
        const TexturePaintSettings& s = texturePaintState();
        json payload = payloadFor(s, false).toJson();
        payload.erase("triangles");
        json call = {{"args", {name.toStdString()}}, {"brush", s.stroke.toJson()}, {"payload", payload}, {"category", "User"}};
        if (!s.particles.empty())
        {
            json spec = s.particleSpec;
            spec["kind"] = s.particles;
            call["particles"] = spec;
        }
        try
        {
            ops::execute("texSaveBrushPreset", call, CallSource::UI);
            texturePaintState().presetId = name.toStdString();
            textureSets::refreshLibrary();
            sync();
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
    }

    QLabel* mTarget = nullptr;
    QToolButton* mBrush = nullptr;
    controls::Segmented* mSource = nullptr;
    QToolButton* mTexture = nullptr;
    QWidget *mSourceRow = nullptr, *mMaterialRow = nullptr, *mTextureRow = nullptr, *mScaleRow = nullptr;
    controls::Segmented* mTiling = nullptr;
    QDoubleSpinBox *mVariedTurn = nullptr, *mVariedBlend = nullptr, *mVariedBrightness = nullptr;
    QWidget *mVariedTurnRow = nullptr, *mVariedBlendRow = nullptr, *mVariedBrightRow = nullptr;
    std::vector<QToolButton*> mTools;
    std::vector<QCheckBox*> mOn;
    /** Each channel's row; past kMainChannels they sit under More Channels and show when the set has the channel. */
    std::vector<QWidget*> mChannelRow;
    QWidget* mMoreChannels = nullptr;
    std::vector<QDoubleSpinBox*> mValue;
    std::vector<QPushButton*> mSwatch;
    std::vector<colorPopup::ValueSlider*> mBright;
    QComboBox* mMaterial = nullptr;
    QDoubleSpinBox* mMaterialScale = nullptr;
    QComboBox* mBlend = nullptr;
    QDoubleSpinBox* mTilt = nullptr;
    QComboBox* mFlow = nullptr;
    QComboBox* mNormalMode = nullptr;
    QDoubleSpinBox* mIntensity = nullptr;
    QLineEdit* mStamp = nullptr;
    QDoubleSpinBox* mFilterRadius = nullptr;
    QDoubleSpinBox* mAmount = nullptr;
    controls::Segmented* mFillBy = nullptr;
    QComboBox* mParticles = nullptr;
    QComboBox* mBufferBrush = nullptr;
    QDoubleSpinBox* mBufferScale = nullptr;
    QCheckBox* mTransform = nullptr;
    QCheckBox* mWarp = nullptr;
    QCheckBox* mAuto = nullptr;
    QDoubleSpinBox* mFacing = nullptr;
    QWidget *mDirRow = nullptr, *mNormalRow = nullptr, *mStampRow = nullptr, *mFilterRow = nullptr, *mFillRow = nullptr, *mParticleRow = nullptr;
    QWidget *mBufferRow = nullptr, *mBufferFlags = nullptr, *mFacingRow = nullptr, *mBufferButtons = nullptr;
    QToolButton* mStencilPick = nullptr;
    controls::Segmented* mStencilMode = nullptr;
    QCheckBox *mStencilInvert = nullptr, *mStencilTile = nullptr;
    QWidget* mStencilOptions = nullptr;
    QComboBox* mViews = nullptr;
    std::vector<QWidget*> mLimitRows;
    std::vector<std::function<void()>> mLimitSync;
    QDoubleSpinBox* mLimitFacing = nullptr;
    QComboBox* mLimitGroup = nullptr;
    QWidget* mViewsRow = nullptr;
    QCheckBox* mPickLayer = nullptr;
    controls::Segmented* mSelectMode = nullptr;
    QToolButton* mReveal = nullptr;
    controls::Segmented* mRevealSpace = nullptr;
    QWidget* mRevealRow = nullptr;
    controls::Segmented* mGradient = nullptr;
    QCheckBox* mGradientDither = nullptr;
    QWidget* mGradientRow = nullptr;
    QWidget* mSelectRow = nullptr;
    QToolButton *mQuickMask = nullptr, *mQuickOverlay = nullptr, *mLock = nullptr;
    QComboBox* mGroups = nullptr;
    QWidget *mPickRow = nullptr, *mSwatchRow = nullptr, *mSwatchBar = nullptr;
    QHBoxLayout* mSwatchLayout = nullptr;
    std::map<QWidget*, QWidget*> mLabels;
    QSpinBox* mRadius = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QComboBox* mFalloff = nullptr;
    QCheckBox* mOcclusion = nullptr;
    controls::Segmented* mSymmetry = nullptr;
    controls::Segmented* mSets = nullptr;
    controls::Segmented* mUvMirror = nullptr;
    QCheckBox* mUvIsland = nullptr;
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
};

/** The Brush page the Texture Set panel hosts; the tool wires its buttons while it runs. */
QPointer<TexturePaintPanel> gPage;

TexturePaintPanel* brushPage()
{
    if (!gPage)
        gPage = new TexturePaintPanel(nullptr);
    return gPage;
}

void ensureTexturePaint()
{
    if (!viewportTools::active() || std::string(viewportTools::active()->name()) != "texturePaint")
        startTexturePaint();
}

void brushChanged()
{
    saveTexturePaintSettings();
    if (gPage)
        gPage->sync();
    viewportTools::requestRedraw();
}

/**
 * Picks the channel values of a set at where ({uv} or {mesh, position}) into the brush, or with add into a new swatch.
 * A pick hands the tool back to the one used before the picker.
 */
/**
 * Quick Mask on: strokes paint the quick mask. Off: what it holds limits every later stroke (until cleared), so painting
 * stays inside it.
 */
void setQuickMask(bool on)
{
    TexturePaintSettings& s = texturePaintState();
    s.quickMask = on;
    if (on)
        return;
    s.quickLimit.clear();
    const std::string setName = currentTextureSet();
    scene::Node* set = setName.empty() ? nullptr : tex::findSet(scene::Graph::current(), setName);
    if (!set || !tex::paint::readPaint(*set)->buffer(kQuickMaskBuffer))
        return;
    const image::Image m = rp::readChannel(set->id(), kQuickMaskBuffer, tex::paint::kMaskChannel);
    if (m.width <= 0)
        return;
    auto img = tex::paint::PaintImage::fromRgba(m.width, m.height, m.rgba);
    tex::paint::holdImage(img);
    s.quickLimit = img->key();
}

void clearQuickMask()
{
    TexturePaintSettings& s = texturePaintState();
    s.quickLimit.clear();
    s.quickMask = false;
    const std::string setName = currentTextureSet();
    if (scene::Node* set = setName.empty() ? nullptr : tex::findSet(scene::Graph::current(), setName))
        if (tex::paint::readPaint(*set)->buffer(kQuickMaskBuffer))
            ops::execute("texClearLayer", {{"args", {setName, kQuickMaskBuffer}}}, CallSource::UI);
}

void pickChannels(const std::string& set, json where, bool add)
{
    TexturePaintSettings& s = texturePaintState();
    where["args"] = {set};
    where["source"] = s.pickLayer ? "layer" : "set";
    if (s.pickLayer)
        where["layer"] = textureSets::activeLayer(set);
    json r;
    try
    {
        r = ops::execute("texSampleChannels", where, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("Texture paint: {}", e.what());
        return;
    }
    std::map<std::string, glm::vec3> values;
    for (const auto& [name, v] : r["channels"].items())
        values[name] = v.is_array() ? glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()) : glm::vec3(v.get<float>());
    if (values.empty())
    {
        log::info("Texture paint: the layer has no paint there to pick");
        return;
    }
    if (add)
    {
        s.swatches.push_back(std::move(values));
        if (s.swatches.size() > 16)
            s.swatches.erase(s.swatches.begin());
    }
    else
    {
        applyChannelValues(values);
        s.tool = s.pickReturn.empty() || s.pickReturn == "pick" ? std::string("paint") : s.pickReturn;
    }
    brushChanged();
}


/** What the brush is over: a mesh, the texture set its face's material binds, and the hit in the mesh's object space. */
struct Target
{
    NodeId mesh = kNoNode;
    std::string meshPath;
    NodeId set = kNoNode;
    std::string setName;
    glm::dmat4 world{1.0};
    std::shared_ptr<const tex::paint::PaintMesh> paint;
    geo::BvhHit hit;
};

/** The view the paint tool last drew in, so Paint Along Curve can size its dabs as the brush shows them. */
std::optional<ViewInfo> gLastPaintView;

/** The last layer painted per set, so the panel and the actions know it. */
std::map<NodeId, std::string> gLayers;

/** A view as the JSON texPaintStroke takes (world space). */
json viewJson(const ViewInfo& v)
{
    const glm::dmat4 vp = v.viewProj();
    json m = json::array();
    for (int i = 0; i < 16; ++i)
        m.push_back((&vp[0][0])[i]);
    // lookAt goes along so a saved view can put the viewport camera back; viewProj is what projects.
    glm::dvec3 o, d;
    v.ray(QPointF(v.width * 0.5, v.height * 0.5), o, d);
    const glm::dvec3 at = v.eye + glm::normalize(d) * 100.0;
    return {{"viewProj", m}, {"eye", {v.eye.x, v.eye.y, v.eye.z}}, {"lookAt", {at.x, at.y, at.z}}, {"width", v.width}, {"height", v.height}};
}

/** The toast for a stroke while textures are off, naming the key that turns them on when one does. */
QString texturesOffText()
{
    const hotkeys::Snapshot keys = hotkeys::snapshot();
    for (const hotkeys::Binding& b : keys.bindings)
        if ((b.context == "viewport" || b.context == "global") && hotkeys::targets(b, "viewport.textured"))
            return "Textures off: press " + hotkeys::displayText(b.chord) + " to see paint";
    return "Textures off: Display > Textured to see paint";
}

/** The stroke view of a mesh (its object space). */
tp::StrokeView strokeView(const ViewInfo& view, const glm::dmat4& world)
{
    tp::StrokeView v;
    const glm::dmat4 inv = glm::inverse(world);
    v.objectToClip = glm::mat4(view.viewProj() * world);
    v.eye = glm::vec3(inv * glm::dvec4(view.eye, 1.0));
    glm::dvec3 o, d;
    view.ray(QPointF(view.width * 0.5, view.height * 0.5), o, d);
    v.direction = glm::vec3(glm::normalize(glm::dvec3(inv * glm::dvec4(d, 0.0))));
    v.width = std::max(1, int(view.width));
    v.height = std::max(1, int(view.height));
    return v;
}

bool samePaintView(const ViewInfo& a, const ViewInfo& b) { return a.view == b.view && a.proj == b.proj && a.width == b.width && a.height == b.height; }

class TexturePaintTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "texturePaint"; }
    // The brush ring and its centre dot are the cursor.
    std::optional<QCursor> cursor() const override
    {
        const TexturePaintSettings& st = texturePaintState();
        const bool ring = st.tool != "fill" && !(st.tool == "project" && (st.transform || st.warp));
        return ring ? std::optional<QCursor>(QCursor(Qt::BlankCursor)) : std::nullopt;
    }

    void activate() override
    {
        rp::prewarm();
        warmTargets();
        mAlive = std::make_shared<bool>(true);
        watchTab();
        mChannelsDirty = true;
        mSettingsListener = scene::Graph::current().addListener([this](scene::Change change, NodeId id) {
            if (change == scene::Change::Value && id == mChannelsSet)
            {
                mChannelsDirty = true;
                QTimer::singleShot(0, [alive = std::weak_ptr<bool>(mAlive), this] {
                    if (alive.lock())
                        updatePanel();
                });
            }
        });
        // A tapped Shift+F toggles the wire, as in Sculpt.
        mControls.setStrengthTap([] {
            actions::run(json("tex.paintWire"));
            viewportToast::showActive(texturePaintDimWire() ? "Wireframe on" : "Wireframe off", 1000);
        });
        mControls.activate();
        mActiveListener = textureSets::addActiveLayerListener([this](const std::string& setName, const std::string& layer) {
            if (scene::Node* set = tex::findSet(scene::Graph::current(), setName))
            {
                if (layer.empty())
                    gLayers.erase(set->id());
                else
                    gLayers[set->id()] = layer;
            }
            updatePanel();
            viewportTools::requestRedraw();
        });
        textureSets::showBrushPage(true);
        mPanel = brushPage();
        mPanel->onChanged = [] { viewportTools::requestRedraw(); };
        mPanel->onNewLayer = [this] { newLayer(); };
        mPanel->onCommit = [this] { commitBuffer(); };
        mPanel->onClearBuffer = [this] {
            mBuffer.clear();
            viewportTools::requestRedraw();
        };
        mPanel->onLoadImage = [this] { loadBufferImage(); };
        mPanel->onImageEditor = [this] { editInImageEditor(); };
        mPanel->onPatchFill = [this] { patchFillBuffer(); };
        mPanel->onSaveView = [this](const std::string& name) { saveView(name); };
        mPanel->onRecallView = [this](const std::string& name) { recallView(name); };
        mPanel->onLoadStencil = [this] { loadStencil(); };
        mPanel->onClearStencil = [this] {
            setStencilFile({});
            mPanel->sync();
            viewportTools::requestRedraw();
        };
        mPanel->sync();
        updatePanel();
    }

    void deactivate() override
    {
        if (mStroking)
            cancelStroke();
        mKeyWatch.reset();
        mTabWatch.reset();
        scene::Graph::current().removeListener(mSettingsListener);
        mSettingsListener = 0;
        mTabDown = false;
        endSweep();
        mStencilKey = false;
        mStencilDrag = StencilDrag::None;
        mControls.deactivate();
        textureSets::removeActiveLayerListener(mActiveListener);
        mActiveListener = 0;
        mAlive.reset();
        if (mPanel)
        {
            for (std::function<void()>* fn : {&mPanel->onNewLayer, &mPanel->onCommit, &mPanel->onClearBuffer, &mPanel->onLoadImage,
                                              &mPanel->onLoadStencil, &mPanel->onClearStencil, &mPanel->onImageEditor, &mPanel->onPatchFill})
                *fn = nullptr;
            mPanel->onSaveView = nullptr;
            mPanel->onRecallView = nullptr;
            mPanel->setTarget({});
        }
        textureSets::showBrushPage(false);
    }

    bool ownsBrushRadius() const override { return true; }
    bool showsSelection(scene::NodeId) const override { return texturePaintShowsWire(); }
    bool dimsSelection(scene::NodeId) const override { return texturePaintDimWire(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (stencilPress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (gPickingStroke)
        {
            if (std::optional<Target> t = pick(e->position(), view); t && t->set != kNoNode)
                pickStroke(*t);
            return true;
        }
        const std::string& tool = texturePaintState().tool;
        if (tool == "project")
            return bufferPress(e, view);
        if (tool == "path")
            return pathPress(e, view);
        if (tool == "select")
            return selectPress(e, view);
        if (tool == "gradient")
        {
            mGradientFrom = mGradientTo = e->position();
            mGradientDrag = true;
            return true;
        }
        if (tool == "pick")
        {
            if (std::optional<Target> t = pick(e->position(), view); t && t->set != kNoNode)
                pickChannels(t->setName, {{"mesh", t->meshPath}, {"position", {t->hit.point.x, t->hit.point.y, t->hit.point.z}}},
                             e->modifiers() & Qt::ShiftModifier);
            return true;
        }
        std::optional<Target> t = pick(e->position(), view);
        if (!t || t->set == kNoNode)
            return true;
        const bool source = (e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::AltModifier);
        if (source && (tool == "clone" || tool == "heal"))
        {
            mCloneUv = t->paint->uv(t->hit.triangle, float(t->hit.bary.y), float(t->hit.bary.z));
            mCloneMesh = t->mesh;
            mCloneSourceDab = {uint32_t(t->hit.triangle), float(t->hit.bary.y), float(t->hit.bary.z)};
            viewportTools::requestRedraw();
            return true;
        }
        if (tool == "fill" && mTabDown)
        {
            // One undo step for the whole sweep.
            UndoStack::get().openChunk("Fill Sweep");
            mSweeping = true;
            mSwept.clear();
            sweepFill(*t);
            return true;
        }
        if (tool == "fill")
        {
            // The fill waits for the release: a drag instead boxes every region in it.
            mFillPress = *t;
            mMarqueeFrom = mMarqueeTo = e->position();
            mMarquee = false;
            return true;
        }
        beginStroke(*t, e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mStencilDrag != StencilDrag::None)
        {
            stencilMove(e, view);
            return true;
        }
        if (mPathDrag >= 0)
        {
            pathMove(e, view);
            return true;
        }
        if (mSelecting)
        {
            selectMove(e);
            return true;
        }
        if (mGradientDrag)
        {
            mGradientTo = e->position();
            viewportTools::requestRedraw();
            return true;
        }
        if (mBufferDrag != BufferDrag::None)
        {
            bufferMove(e, view);
            return true;
        }
        if (mStroking)
        {
            mStroke.move(mControls.input(e));
            flush();
            return true;
        }
        if (mSweeping && (e->buttons() & Qt::LeftButton))
        {
            mOver = pick(e->position(), view);
            if (mOver && mOver->set != kNoNode)
                sweepFill(*mOver);
            updateFillHover();
            viewportTools::requestRedraw();
            return true;
        }
        if (mFillPress && (e->buttons() & Qt::LeftButton))
        {
            mMarqueeTo = e->position();
            mMarquee = mMarquee || (mMarqueeTo - mMarqueeFrom).manhattanLength() > 4;
            viewportTools::requestRedraw();
            return true;
        }
        if (e->buttons() == Qt::NoButton)
        {
            mOver = pick(mHover, view);
            if (mOver && mOver->set != kNoNode && mOver->set != mWarmedSet)
                if (scene::Node* set = scene::Graph::current().node(mOver->set))
                {
                    mWarmedSet = set->id();
                    warmGraph(*set);
                }
            updateFillHover();
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (mSweeping)
        {
            endSweep();
            return true;
        }
        if (mStencilDrag != StencilDrag::None)
        {
            stencilMove(e, view);
            mStencilDrag = StencilDrag::None;
            return true;
        }
        if (mPathDrag >= 0)
        {
            pathRelease();
            return true;
        }
        if (mSelecting)
        {
            finishSelection(view);
            return true;
        }
        if (mGradientDrag)
        {
            mGradientDrag = false;
            paintGradient(e->modifiers() & Qt::ControlModifier, view);
            return true;
        }
        if (mBufferDrag != BufferDrag::None)
        {
            bufferMove(e, view);
            if (mBufferDrag == BufferDrag::Paint)
            {
                mSampler.end();
                bufferSamples();
            }
            mBufferDrag = BufferDrag::None;
            viewportTools::requestRedraw();
            return true;
        }
        if (mFillPress)
        {
            const Target t = *mFillPress;
            mFillPress.reset();
            if (mMarquee)
                fillBox(t, QRectF(mMarqueeFrom, mMarqueeTo).normalized(), view);
            else
                fill(t);
            mMarquee = false;
            mFillKey.clear();
            updateFillHover();
            viewportTools::requestRedraw();
            return true;
        }
        if (mStroking)
        {
            endStroke();
            // mOver stops following the mouse while a button is down; left alone, the ring is drawn back at the stroke's start.
            mHover = e->position();
            mOver = pick(mHover, view);
        }
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        // S is an app hotkey too; with a stencil it is the stencil's while the tool is active.
        if (e.key() == Qt::Key_S && !(e.modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && texturePaintState().stencil.active())
            return holdStencilKey(e);
        return false;
    }

    /** S down: drags place the stencil until it comes up (watched app-wide, as tools see no key releases). */
    bool holdStencilKey(const QKeyEvent& e)
    {
        if (e.isAutoRepeat() || mStencilKey)
            return true;
        mStencilKey = true;
        struct Watch final : QObject
        {
            std::function<void()> released;
            bool eventFilter(QObject*, QEvent* ev) override
            {
                if (ev->type() == QEvent::KeyRelease)
                    if (auto* k = static_cast<QKeyEvent*>(ev); k->key() == Qt::Key_S && !k->isAutoRepeat())
                        released();
                return false;
            }
        };
        auto watch = std::make_unique<Watch>();
        watch->released = [this] {
            mStencilKey = false;
            QTimer::singleShot(0, [alive = std::weak_ptr<bool>(mAlive), this] {
                if (alive.lock())
                    mKeyWatch.reset();
            });
        };
        qApp->installEventFilter(watch.get());
        mKeyWatch = std::move(watch);
        return true;
    }

    bool stencilPress(QMouseEvent* e)
    {
        if (!mStencilKey || e->button() != Qt::LeftButton || !texturePaintState().stencil.active())
            return false;
        mStencilDrag = (e->modifiers() & Qt::ShiftModifier) ? StencilDrag::Rotate : (e->modifiers() & Qt::ControlModifier) ? StencilDrag::Scale : StencilDrag::Move;
        mStencilFrom = e->position();
        mStencilStart = texturePaintState().stencil.place;
        return true;
    }

    void stencilMove(QMouseEvent* e, const ViewInfo& view)
    {
        TexturePaintSettings& s = texturePaintState();
        tp::Placement& place = s.stencil.place;
        place = mStencilStart;
        const QPointF d = e->position() - mStencilFrom;
        if (mStencilDrag == StencilDrag::Move)
            place.offset += glm::vec2(float(d.x()), float(d.y())) * s.bufferScale;
        else if (mStencilDrag == StencilDrag::Scale)
            place.scale = std::clamp(mStencilStart.scale * std::exp(float(d.x() - d.y()) / 200.0f), 0.02f, 50.0f);
        else
        {
            // About the stencil's centre on the screen.
            const glm::vec2 c = glm::vec2(float(view.width), float(view.height)) * 0.5f + mStencilStart.offset / s.bufferScale;
            const glm::vec2 a(float(mStencilFrom.x()) - c.x, float(mStencilFrom.y()) - c.y), b(float(e->position().x()) - c.x, float(e->position().y()) - c.y);
            place.rotation = mStencilStart.rotation + (std::atan2(b.y, b.x) - std::atan2(a.y, a.x));
            // The drag's kind is set at the press, so Ctrl is free here: it snaps the angle to 15 degree steps.
            if (e->modifiers() & Qt::ControlModifier)
            {
                const float step = glm::radians(15.0f);
                place.rotation = std::round(place.rotation / step) * step;
            }
        }
        viewportTools::requestRedraw();
    }

    // ------------------------------------------------------------------------------------------------ selection shapes

    bool selectPress(QMouseEvent* e, const ViewInfo& view)
    {
        const std::string& mode = texturePaintState().selectMode;
        mSelectMods = e->modifiers();
        if (mode == "polygon")
        {
            // Clicking near the first corner closes the polygon too.
            if (mSelect.size() >= 3 && QLineF(mSelect.front(), e->position()).length() < 8.0)
            {
                finishSelection(view);
                return true;
            }
            mSelect.push_back(e->position());
            viewportTools::requestRedraw();
            return true;
        }
        mSelect = {e->position()};
        mSelecting = true;
        return true;
    }

    void selectMove(QMouseEvent* e)
    {
        if (texturePaintState().selectMode == "rect")
            mSelect = {mSelect.front(), e->position()};
        else if (QLineF(mSelect.back(), e->position()).length() > 2.0)
            mSelect.push_back(e->position());
        viewportTools::requestRedraw();
    }

    /** Masks the active layer with the shape drawn: plain replaces, Shift adds, Ctrl subtracts. */
    void finishSelection(const ViewInfo& view)
    {
        mSelecting = false;
        std::vector<QPointF> pts = std::exchange(mSelect, {});
        const std::string set = currentTextureSet();
        const std::string layer = set.empty() ? std::string() : textureSets::activeLayer(set);
        if (layer.empty())
        {
            log::warn("Texture paint: select a layer in the Texture Set dock to mask");
            return;
        }
        json call = {{"args", {set, layer}},
                     {"view", viewJson(view)},
                     {"mode", (mSelectMods & Qt::ShiftModifier) ? "add" : (mSelectMods & Qt::ControlModifier) ? "subtract" : "replace"},
                     {"occlusion", texturePaintState().occlusion}};
        if (texturePaintState().selectMode == "rect")
        {
            if (pts.size() < 2)
                return;
            call["rect"] = {pts[0].x(), pts[0].y(), pts[1].x(), pts[1].y()};
        }
        else
        {
            if (pts.size() < 3)
                return;
            json list = json::array();
            for (const QPointF& q : pts)
                list.push_back({q.x(), q.y()});
            call["points"] = list;
        }
        try
        {
            ops::execute("texSelectionMask", call, CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::warn("Texture paint: {}", ex.what());
        }
        viewportTools::requestRedraw();
    }

    /** Fills the mesh under the drag's start with a gradient along the drag, into the layer strokes go to. */
    void paintGradient(bool reverse, const ViewInfo& view)
    {
        if (QLineF(mGradientFrom, mGradientTo).length() < 3.0)
            return;
        std::optional<Target> t = pick(mGradientFrom, view);
        if (!t || t->set == kNoNode)
            t = mOver;
        scene::Node* set = t ? scene::Graph::current().node(t->set) : nullptr;
        if (!set)
        {
            log::warn("Texture paint: start the gradient over a mesh with a texture set");
            return;
        }
        const std::string layer = texturePaintState().quickMask ? std::string(kQuickMaskBuffer) : layerFor(*set);
        json pay = gradientPayload(texturePaintState(), {mGradientFrom.x(), mGradientFrom.y()}, {mGradientTo.x(), mGradientTo.y()}, "screen", reverse);
        if (std::vector<uint32_t> locked = lockedTriangles(*set, t->meshPath, *t->paint); !locked.empty())
            pay["locked"] = locked;
        try
        {
            ops::execute("texPaintStroke",
                         {{"args", {set->name(), layer}},
                          {"stroke", {{"mesh", t->meshPath}, {"brush", texturePaintState().stroke.toJson()}, {"payload", pay}, {"view", viewJson(view)}}}},
                         CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::warn("Texture paint: {}", ex.what());
        }
        viewportTools::requestRedraw();
    }

    void drawSelection(OverlayPainter& p)
    {
        if (mGradientDrag)
        {
            p.line(mGradientFrom, mGradientTo, 1.5f, p.ink(0.9f));
            p.disc(mGradientFrom, 4.0f, p.ink(0.9f));
            p.disc(mGradientTo, 3.0f, p.ink(0.6f));
        }
        if (mSelect.empty())
            return;
        std::vector<QPointF> pts = mSelect;
        if (texturePaintState().selectMode == "rect" && pts.size() == 2)
            pts = {pts[0], {pts[1].x(), pts[0].y()}, pts[1], {pts[0].x(), pts[1].y()}};
        const glm::vec4 ink = p.ink(0.9f);
        for (size_t i = 1; i < pts.size(); ++i)
            p.line(pts[i - 1], pts[i], 1.5f, ink);
        if (pts.size() > 2 && texturePaintState().selectMode != "polygon")
            p.line(pts.back(), pts.front(), 1.0f, p.ink(0.6f));
        if (texturePaintState().selectMode == "polygon")
            p.line(pts.back(), mHover, 1.0f, p.ink(0.6f));
    }

    // ------------------------------------------------------------------------------------------------ path strokes

    /** The path's points as texPaintPath takes them: handles only where a drag set them, the rest smooth. */
    json pathPointsJson() const
    {
        json pts = json::array();
        for (size_t i = 0; i < mPath.pts.size(); ++i)
        {
            const glm::dvec3& p = mPath.pts[i];
            json o = {{"position", {p.x, p.y, p.z}}};
            if (mPath.manual[i])
            {
                o["in"] = {mPath.in[i].x, mPath.in[i].y, mPath.in[i].z};
                o["out"] = {mPath.out[i].x, mPath.out[i].y, mPath.out[i].z};
            }
            pts.push_back(o);
        }
        return pts;
    }

    /** The path's Bezier on the screen, sampled. Smooth handles as the command makes them. */
    std::vector<QPointF> pathOnScreen(const ViewInfo& view) const
    {
        std::vector<QPointF> out;
        const size_t n = mPath.pts.size();
        auto handles = [&](size_t i, glm::dvec3& in, glm::dvec3& o) {
            if (mPath.manual[i])
            {
                in = mPath.in[i];
                o = mPath.out[i];
                return;
            }
            const glm::dvec3 prev = mPath.pts[i > 0 ? i - 1 : i], next = mPath.pts[i + 1 < n ? i + 1 : i];
            const glm::dvec3 t = (next - prev) * (i > 0 && i + 1 < n ? 0.5 : 1.0);
            in = mPath.pts[i] - t / 3.0;
            o = mPath.pts[i] + t / 3.0;
        };
        for (size_t i = 0; i + 1 < n; ++i)
        {
            glm::dvec3 ai, ao, bi, bo;
            handles(i, ai, ao);
            handles(i + 1, bi, bo);
            for (int k = 0; k <= 24; ++k)
            {
                const double t = k / 24.0, s = 1.0 - t;
                const glm::dvec3 p = mPath.pts[i] * (s * s * s) + ao * (3 * s * s * t) + bi * (3 * s * t * t) + mPath.pts[i + 1] * (t * t * t);
                if (auto q = view.project(glm::dvec3(mPath.world * glm::dvec4(p, 1.0))))
                    out.push_back(*q);
            }
        }
        return out;
    }

    std::optional<QPointF> pathPointOnScreen(const ViewInfo& view, const glm::dvec3& p) const
    {
        return view.project(glm::dvec3(mPath.world * glm::dvec4(p, 1.0)));
    }

    bool pathPress(QMouseEvent* e, const ViewInfo& view)
    {
        for (size_t i = 0; i < mPath.pts.size(); ++i)
            if (auto q = pathPointOnScreen(view, mPath.pts[i]); q && QLineF(*q, e->position()).length() < 8.0)
            {
                mPathDrag = int(i);
                mPathHandle = false;
                mPathMoved = false;
                return true;
            }
        std::optional<Target> t = pick(e->position(), view);
        if (!t || t->set == kNoNode)
            return true;
        if (mPath.stroke != 0 || mPath.set != t->set || mPath.mesh != t->mesh)
        {
            mPath = {};
            mPath.set = t->set;
            mPath.mesh = t->mesh;
            mPath.meshPath = t->meshPath;
            mPath.world = t->world;
            mPath.view = strokeView(view, t->world);
        }
        mPath.pts.push_back(t->hit.point);
        mPath.in.push_back(t->hit.point);
        mPath.out.push_back(t->hit.point);
        mPath.manual.push_back(false);
        mPathDrag = int(mPath.pts.size()) - 1;
        mPathHandle = true;
        mPathMoved = false;
        viewportTools::requestRedraw();
        return true;
    }

    void pathMove(QMouseEvent* e, const ViewInfo& view)
    {
        std::optional<Target> t = pick(e->position(), view);
        if (!t || t->mesh != mPath.mesh || mPathDrag < 0 || size_t(mPathDrag) >= mPath.pts.size())
            return;
        const size_t i = size_t(mPathDrag);
        if (mPathHandle)
        {
            // A drag past a few pixels pulls the handle out; a plain click leaves the point smooth.
            const auto at = pathPointOnScreen(view, mPath.pts[i]);
            if (at && QLineF(*at, e->position()).length() < 4.0)
                return;
            mPath.out[i] = t->hit.point;
            mPath.in[i] = mPath.pts[i] * 2.0 - t->hit.point;
            mPath.manual[i] = true;
        }
        else
        {
            const glm::dvec3 d = t->hit.point - mPath.pts[i];
            mPath.pts[i] += d;
            mPath.in[i] += d;
            mPath.out[i] += d;
        }
        mPathMoved = true;
        viewportTools::requestRedraw();
    }

    void pathRelease()
    {
        const bool edit = mPathMoved && mPath.stroke != 0;
        mPathDrag = -1;
        if (!edit)
            return;
        scene::Node* set = scene::Graph::current().node(mPath.set);
        if (!set)
            return;
        try
        {
            json r = ops::execute("texEditPath", {{"args", {set->name(), mPath.layer}}, {"stroke", mPath.stroke}, {"points", pathPointsJson()}},
                                  CallSource::UI);
            mPath.stroke = r.value("stroke", mPath.stroke);
            mPath.ids.push_back(mPath.stroke);
        }
        catch (const std::exception& ex)
        {
            log::warn("Texture paint: {}", ex.what());
        }
    }

    /** Paints the placed path as one stroke; it stays editable until another path starts. */
    void commitPath()
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(mPath.set);
        if (!set || mPath.stroke != 0 || mPath.pts.size() < 2)
            return;
        mPath.layer = layerFor(*set);
        if (mPath.layer.empty())
            return;
        const TexturePaintSettings& s = texturePaintState();
        tp::Payload pay = payloadFor(s, false);
        filterChannels(pay, *set);
        const double scale = std::max(1e-12, glm::length(glm::dvec3(mPath.world[0])));
        const double radius = s.stroke.radius * mLastView.unitsPerPixel(glm::dvec3(mPath.world * glm::dvec4(mPath.pts.front(), 1.0))) / scale;
        tp::Stroke viewOnly;
        viewOnly.view = mPath.view;
        json call = {{"args", {set->name()}},
                     {"layer", mPath.layer},
                     {"mesh", mPath.meshPath},
                     {"points", pathPointsJson()},
                     {"radius", radius},
                     {"brush", s.stroke.toJson()},
                     {"payload", pay.toJson()},
                     {"view", viewOnly.toJson(false)["view"]}};
        if (std::optional<tp::StrokeStencil> st = strokeStencil())
            call["stencil"] = st->toJson();
        try
        {
            json r = ops::execute("texPaintPath", call, CallSource::UI);
            mPath.stroke = r.value("stroke", uint64_t(0));
            if (mPath.stroke)
                mPath.ids.push_back(mPath.stroke);
        }
        catch (const std::exception& ex)
        {
            log::warn("Texture paint: {}", ex.what());
        }
        viewportTools::requestRedraw();
    }

    /**
     * Follows undo and redo: the painted path takes the points of whichever of its strokes the layer holds now, and goes
     * away with the last of them.
     */
    void syncPathToLayer()
    {
        if (mPath.stroke == 0 || mPathDrag >= 0)
            return;
        const scene::Node* set = scene::Graph::current().node(mPath.set);
        const tex::paint::Buffer* buf = set ? tex::paint::readPaint(*set)->buffer(mPath.layer) : nullptr;
        auto find = [buf](uint64_t id) -> const tp::Stroke* {
            if (buf)
                for (const auto& st : buf->strokes)
                    if (st->id == id)
                        return st.get();
            return nullptr;
        };
        if (find(mPath.stroke))
            return;
        for (auto it = mPath.ids.rbegin(); it != mPath.ids.rend(); ++it)
            if (const tp::Stroke* st = find(*it); st && st->path.is_object())
            {
                mPath.stroke = *it;
                mPath.pts.clear();
                mPath.in.clear();
                mPath.out.clear();
                mPath.manual.clear();
                for (const json& o : st->path.value("points", json::array()))
                {
                    auto v = [&o](const char* k) {
                        const json& a = o.at(k);
                        return glm::dvec3(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
                    };
                    mPath.pts.push_back(v("position"));
                    mPath.in.push_back(o.contains("in") ? v("in") : mPath.pts.back());
                    mPath.out.push_back(o.contains("out") ? v("out") : mPath.pts.back());
                    mPath.manual.push_back(o.contains("in") && o.contains("out"));
                }
                return;
            }
        mPath = {};
    }

    void drawPath(OverlayPainter& p, const ViewInfo& view)
    {
        syncPathToLayer();
        if (mPath.pts.empty() || mPath.mesh == kNoNode)
            return;
        const std::vector<QPointF> line = pathOnScreen(view);
        const glm::vec4 ink = p.ink(mPath.stroke ? 0.6f : 0.9f);
        for (size_t i = 1; i < line.size(); ++i)
            p.line(line[i - 1], line[i], 1.5f, ink);
        for (size_t i = 0; i < mPath.pts.size(); ++i)
        {
            const auto at = pathPointOnScreen(view, mPath.pts[i]);
            if (!at)
                continue;
            if (mPath.manual[i] && mPath.stroke == 0)
                if (auto a = pathPointOnScreen(view, mPath.in[i]), b = pathPointOnScreen(view, mPath.out[i]); a && b)
                {
                    p.line(*a, *b, 1.0f, p.ink(0.7f));
                    p.disc(*a, 2.5f, p.ink(0.7f));
                    p.disc(*b, 2.5f, p.ink(0.7f));
                }
            p.disc(*at, int(i) == mPathDrag ? 5.0f : 4.0f, ink);
        }
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        if (texturePaintState().tool == "select" && texturePaintState().selectMode == "polygon" && !mSelect.empty())
        {
            if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            {
                finishSelection(mLastView);
                return true;
            }
            if (e->key() == Qt::Key_Escape)
            {
                mSelect.clear();
                viewportTools::requestRedraw();
                return true;
            }
        }
        if (texturePaintState().tool == "path")
        {
            if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && mPath.stroke == 0 && mPath.pts.size() >= 2)
            {
                commitPath();
                return true;
            }
            if (e->key() == Qt::Key_Backspace && mPath.stroke == 0 && !mPath.pts.empty())
            {
                mPath.pts.pop_back();
                mPath.in.pop_back();
                mPath.out.pop_back();
                mPath.manual.pop_back();
                viewportTools::requestRedraw();
                return true;
            }
            if (e->key() == Qt::Key_Escape && !mPath.pts.empty())
            {
                mPath = {};
                viewportTools::requestRedraw();
                return true;
            }
        }
        if (e->key() == Qt::Key_S && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && texturePaintState().stencil.active())
            return holdStencilKey(*e);
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && texturePaintState().tool == "project")
        {
            commitBuffer();
            return true;
        }
        if (e->key() == Qt::Key_Escape)
        {
            if (mStroking)
                cancelStroke();
            else if (texturePaintState().tool == "project" && !mBuffer.empty())
            {
                mBuffer.clear();
                viewportTools::requestRedraw();
            }
            else if (sculpt::active())
                QTimer::singleShot(0, [] { startSculptTool(); });
            else
                QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void draw(render::FrameState& fs, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        const TexturePaintSettings& st = texturePaintState();
        mLastView = view;
        gLastPaintView = view;
        const Target* t = mStroking ? &mTarget : (mOver ? &*mOver : nullptr);
        ToolStatus status{.tool = "Texture Paint"};
        if (st.tool == "project")
        {
            syncProjectionUndo();
            ensureBuffer(view);
            status.mode = "projection";
            status.flags = {{"transform", st.transform}, {"warp", st.warp}};
            status.prompt = st.warp ? "paint the buffer, drag the grid" : "paint the buffer";
            status.keys = {{"Enter", "project"}};
            if (st.transform)
                status.keys.insert(status.keys.end(), {{"drag", "move"}, {"Shift", "rotate"}, {"Ctrl", "scale"}});
            drawBuffer(fs, p, view);
        }
        else if (!t)
            status.prompt = reachLabel() == "all" ? "hover a mesh with a texture set" : "hover a selected mesh with a texture set";
        else if (t->set == kNoNode)
        {
            status.subject = QString::fromStdString(t->meshPath);
            status.prompt = "material has no texture set";
        }
        else
        {
            auto it = gLayers.find(t->set);
            status.subject = QString::fromStdString(t->setName);
            status.mode = QString::fromStdString(st.tool);
            status.fields = {{"layer", it != gLayers.end() ? QString::fromStdString(it->second) : QString("new layer")}, {"meshes", reachLabel()}};
            if ((st.tool == "clone" || st.tool == "heal") && !mCloneUv)
                status.keys = {{"Ctrl+Alt+click", "set source"}};
        }
        if (st.tool != "project")
            drawStencil(fs, view);
        if (st.tool == "reveal")
            drawReveal(fs, view);
        if (st.tool == "gradient")
        {
            drawSelection(p);
            status.keys = {{"drag", "start to fade"}, {"Ctrl", "reverse"}};
        }
        if (st.tool == "select")
        {
            drawSelection(p);
            status.keys = {{"drag", "replace"}, {"Shift", "add"}, {"Ctrl", "subtract"}};
            if (st.selectMode == "polygon")
                status.keys.insert(status.keys.end(), {{"click", "corner"}, {"Enter", "close"}});
        }
        if (st.quickMask)
            status.flags.push_back({"quick mask", true});
        else if (!st.quickLimit.empty())
            status.flags.push_back({"limited", true});
        if (st.tool == "path")
        {
            drawPath(p, view);
            if (mPath.stroke)
                status.keys = {{"drag point", "repaint"}, {"click", "new path"}};
            else
                status.keys = {{"click", "point"}, {"drag", "curve"}, {"Enter", "paint"}, {"Backspace", "undo point"}};
        }
        if (mStencilDrag == StencilDrag::Rotate)
            status.keys = {{"Ctrl", "snap 15 degrees"}};
        else if (st.stencil.active())
            status.keys.insert(status.keys.end(), {{"S+drag", "move stencil"}, {"S+Shift+drag", "rotate"}, {"S+Ctrl+drag", "scale"}});
        if (st.tool == "fill" && mStencilDrag == StencilDrag::None)
            status.keys.insert(status.keys.begin(), {{"click", "fill"}, {"drag", "box"}, {"Tab+drag", "fill each"}});
        drawToolStatus(p, view, status);
        if (st.tool == "fill")
            drawFill(p, view);

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        std::optional<brush::ui::CursorSurface> surface;
        if (t && st.tool != "project")
        {
            const glm::dvec3 c(t->world * glm::dvec4(t->hit.point, 1.0));
            const glm::dvec3 n = view.eye - c;
            surface = brush::ui::CursorSurface{c, glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0, 1, 0)};
        }
        if (st.tool != "fill" && !(st.tool == "project" && (st.transform || st.warp)))
            brush::ui::drawCursor(p, view, st.stroke, at, surface,
                                  {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .stroke = &mStroke});
    }

private:
    // ------------------------------------------------------------------------------------------- surface painting

    std::optional<Target> pick(QPointF px, const ViewInfo& view)
    {
        LILA_ZONE();
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const std::vector<NodeId> candidates = texturePaintTargets();
        glm::dvec3 o, d;
        view.ray(px, o, d);
        std::optional<Target> best;
        double nearest = 1e300;
        std::optional<Target> unpaintable;
        double nearestUnpaintable = 1e300;
        const std::string shownSet = PrefStore::get().value<bool>("general", kShownSetOnlyPref, false) ? shownTextureSet() : std::string();
        for (NodeId id : candidates)
        {
            scene::Node* n = g.node(id);
            if (!scene::effectivelyVisible(g, id))
                continue;
            std::shared_ptr<const tex::paint::PaintMesh> pm = rp::paintMesh(g, n->path(), "");
            if (!pm)
                continue;
            const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), inv = glm::inverse(w);
            geo::BvhHit hit;
            if (!pm->bvh->raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit,
                                  std::numeric_limits<double>::max(), false))
                continue;
            const double dist = glm::length(glm::dvec3(w * glm::dvec4(hit.point, 1.0)) - o);
            if (dist >= nearest)
                continue;
            Target t;
            t.mesh = id;
            t.meshPath = n->path();
            t.world = w;
            t.hit = hit;
            scene::MeshPtr mesh = rp::paintSurface(*n);
            scene::Node* set = mesh ? setForFace(g, *mesh, hit.face) : nullptr;
            // Sets: Shown leaves every other set's surfaces to the brush as if they had none.
            if (set && !shownSet.empty() && set->name() != shownSet)
                set = nullptr;
            if (set)
            {
                t.set = set->id();
                t.setName = set->name();
                t.paint = rp::paintMesh(g, t.meshPath, rp::setMaterial(*set));
            }
            // A surface without a texture set (glass, a canopy) doesn't stop the brush reaching the painted one behind it.
            if (t.set == kNoNode)
            {
                if (dist < nearestUnpaintable)
                {
                    nearestUnpaintable = dist;
                    unpaintable = t;
                }
                continue;
            }
            nearest = dist;
            best = t;
        }
        // With nothing paintable under the cursor, the nearest unpainted surface still says why.
        return best ? best : unpaintable;
    }

    /** The shared stroke's view of the target under a pixel, in its object space. */
    /** One mesh a stroke reaches: its target, layer, record and symmetry. A stroke over several meshes keeps one each. */
    struct Segment
    {
        Target target;
        std::string layer;
        tp::Stroke record;
        tp::PaintSymmetry sym;
    };

    /** Moves the stroke onto another mesh: what it painted so far stays with the last one, and the new one gets its own record. */
    bool switchSegment(const Target& t, const ViewInfo& view)
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(t.set);
        if (!set || !mStrokeTemplate)
            return false;
        flush();
        auto keep = [&] {
            for (Segment& sgm : mSegments)
                if (sgm.target.mesh == mTarget.mesh && sgm.target.set == mTarget.set)
                {
                    sgm = {mTarget, mLayer, mRecord, mSym};
                    return;
                }
            mSegments.push_back({mTarget, mLayer, mRecord, mSym});
        };
        keep();
        for (Segment& sgm : mSegments)
            if (sgm.target.mesh == t.mesh && sgm.target.set == t.set)
            {
                mTarget = sgm.target;
                mTarget.hit = t.hit;
                mLayer = sgm.layer;
                mRecord = sgm.record;
                mSym = sgm.sym;
                return true;
            }
        const std::string layer = texturePaintState().quickMask ? std::string(kQuickMaskBuffer) : layerFor(*set);
        if (layer.empty())
            return false;
        // A buffer paints one mesh's live stroke at a time: what the stroke laid on another mesh of it commits first.
        for (auto it = mSegments.begin(); it != mSegments.end();)
            if (it->target.set == t.set && it->layer == layer)
            {
                commitSegment(*it);
                it = mSegments.erase(it);
            }
            else
                ++it;
        auto header = std::make_shared<tp::Stroke>(*mStrokeTemplate);
        uint64_t top = 0;
        if (const tex::paint::Buffer* b = tex::paint::readPaint(*set)->buffer(layer))
            for (const auto& st : b->strokes)
                top = std::max(top, st->id);
        header->id = top + 1;
        header->mesh = t.meshPath;
        filterChannels(header->payload, *set);
        header->payload.locked = lockedTriangles(*set, t.meshPath, *t.paint);
        const scene::Node* meshNode = g.node(t.mesh);
        scene::MeshPtr outMesh = rp::paintSurface(*meshNode);
        tp::PaintSymmetry sym;
        texturePaintSymmetryFor(*meshNode, outMesh.get(), sym);
        addUvMirror(sym);
        header->symmetry = sym.toJson();
        header->view = strokeView(view, t.world);
        try
        {
            rp::beginLive(g, *set, layer, header);
        }
        catch (const std::exception& ex)
        {
            log::warn("Texture paint: {}", ex.what());
            return false;
        }
        mTarget = t;
        mLayer = layer;
        mSym = sym;
        mTileable = tex::readSettings(*set).tileable;
        mRecord = *header;
        mRecord.dabs.clear();
        return true;
    }

    bool surfaceQuery(glm::dvec2 px, const ViewInfo& view, brush::SurfaceHit& out)
    {
        glm::dvec3 o, d;
        view.ray(QPointF(px.x, px.y), o, d);
        geo::BvhHit hit;
        auto onTarget = [&] {
            const glm::dmat4 inv = glm::inverse(mTarget.world);
            const tex::paint::PaintMesh& tm = *mTarget.paint;
            return tm.bvh->raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit, std::numeric_limits<double>::max(), false) &&
                   hit.triangle < tm.triangleCount();
        };
        if (!onTarget())
        {
            // Off this mesh: the stroke carries on over any other textured mesh under the brush.
            std::optional<Target> other = mStroking ? pick(QPointF(px.x, px.y), view) : std::nullopt;
            if (!other || other->set == kNoNode || !switchSegment(*other, view) || !onTarget())
                return false;
        }
        const glm::dmat4 inv = glm::inverse(mTarget.world);
        const tex::paint::PaintMesh& m = *mTarget.paint;
        mTarget.hit = hit;
        const glm::vec3* p = &m.positions[size_t(hit.triangle) * 3];
        const glm::dvec3 n = glm::cross(glm::dvec3(p[1] - p[0]), glm::dvec3(p[2] - p[0]));
        const double scale = std::max(1e-12, glm::length(glm::dvec3(mTarget.world[0])));
        out.center = hit.point;
        out.normal = glm::length(n) > 1e-18 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
        out.viewDir = glm::normalize(glm::dvec3(inv * glm::dvec4(d, 0.0)));
        out.eye = glm::dvec3(inv * glm::dvec4(view.eye, 1.0));
        out.right = brush::ui::screenRight(view, mTarget.world);
        out.pixelSize = view.unitsPerPixel(glm::dvec3(mTarget.world * glm::dvec4(hit.point, 1.0))) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    /** The layer strokes go to, adding a paint layer (pointed at its paint) when the set has none. */
    /**
     * What the first stroke would build as it starts, built ahead in the background: every textured mesh's ray tree, paint
     * meshes and atlases, and each set's next graph compiled (the paint layer the stroke adds or wires).
     */
    void warmTargets()
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        for (const std::string& name : tex::setNames(g))
        {
            scene::Node* set = tex::findSet(g, name);
            if (!set)
                continue;
            for (NodeId id : g.nodes())
            {
                scene::Node* n = g.node(id);
                if (!n || !n->type().isA("mesh"))
                    continue;
                scene::MeshPtr m = rp::paintSurface(*n);
                if (!m || !m->numFaces())
                    continue;
                const auto& names = m->materialNames();
                if (rp::usesSet(names, *set))
                    rp::prewarmTarget(g, *set, n->path());
            }
            warmGraph(*set);
        }
    }

    /** Compiles in the background the graph the next stroke on set gives it: a new paint layer, or the target one wired for the stroke's channels. */
    void warmGraph(scene::Node& set)
    {
        std::string layer = textureSets::activeLayer(set.name());
        if (layer.empty())
            if (auto known = gLayers.find(set.id()); known != gLayers.end())
                layer = known->second;
        if (layer.empty())
            layer = rp::targetLayer(set, "", false);
        std::vector<std::string> channels;
        if (layer.empty())
        {
            // createLayer wires every channel and the mask.
            for (const tex::Channel& c : tex::readSettings(set).channels)
                channels.push_back(c.name);
            channels.push_back(tex::paint::kMaskChannel);
        }
        else
        {
            tp::Payload p = payloadFor(texturePaintState(), false);
            filterChannels(p, set);
            channels = p.written();
        }
        if (std::optional<tex::Graph> next = rp::wiredGraph(set, layer, channels))
            texLive::warm(set.name(), *next);
        // A layer never painted has 1x1 stand-ins for its buffers; sizing them now replans and loads the set here,
        // in the background, instead of on the first stroke's press.
        if (!layer.empty() && rp::bufferInfo(set.id(), layer).value("sizes", json::object()).empty())
            rp::rasterize(scene::Graph::current(), set, layer);
    }

    std::string layerFor(scene::Node& set)
    {
        // The Texture Set panel's active paint layer wins.
        if (std::string active = textureSets::activeLayer(set.name()); !active.empty())
        {
            gLayers[set.id()] = active;
            return active;
        }
        auto known = gLayers.find(set.id());
        if (known != gLayers.end() && !known->second.empty())
            return known->second;
        std::string layer = rp::targetLayer(set, "", false);
        if (layer.empty())
            layer = createLayer(set);
        gLayers[set.id()] = layer;
        return layer;
    }

    std::string createLayer(scene::Node& set)
    {
        std::string layer;
        try
        {
            UndoChunk chunk("New Paint Layer");
            json r = ops::execute("texAddLayer", {{"args", {set.name(), "paint"}}}, CallSource::UI);
            layer = r.value("id", std::string());
            if (!layer.empty())
            {
                std::vector<std::string> channels;
                for (const tex::Channel& c : tex::readSettings(set).channels)
                    channels.push_back(c.name);
                channels.push_back(tex::paint::kMaskChannel);
                rp::wireLayer(set, layer, channels);
                textureSets::setActiveLayer(set.name(), layer);
            }
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        return layer.empty() ? rp::targetLayer(set, "", true) : layer;
    }

    void newLayer()
    {
        scene::Graph& g = scene::Graph::current();
        const NodeId setId = mOver && mOver->set != kNoNode ? mOver->set : (gLayers.empty() ? kNoNode : gLayers.begin()->first);
        scene::Node* set = g.node(setId);
        if (!set)
        {
            const std::vector<std::string> names = tex::setNames(g);
            set = names.size() == 1 ? tex::findSet(g, names.front()) : nullptr;
        }
        if (!set)
        {
            log::warn("Texture paint: hover a mesh with a texture set first");
            return;
        }
        gLayers[set->id()] = createLayer(*set);
        updatePanel();
        viewportTools::requestRedraw();
    }

    /** Filters need channels to filter: the set's. */
    /** The More Channels ticked for another set that this one doesn't have: the page hides them here, so they don't paint. */
    static void dropMissingChannels(tp::Payload& p, const scene::Node& set)
    {
        std::set<std::string> names;
        for (const tex::Channel& c : tex::readSettings(set).channels)
            names.insert(c.name);
        const auto& all = texturePaintState().channels;
        for (size_t i = kMainChannels; i < all.size(); ++i)
            if (!names.contains(all[i].name))
                p.channels.erase(all[i].name);
    }

    void filterChannels(tp::Payload& p, const scene::Node& set)
    {
        if (p.source.is_object() && p.source.value("library", std::string()) == "scene/" + set.name())
        {
            log::warn("Texture paint: {} can't paint through itself; painting the channel values instead", set.name());
            p.source = json();
        }
        dropMissingChannels(p, set);
        if (!p.filters())
            return;
        p.channels.clear();
        for (const tex::Channel& c : tex::readSettings(set).channels)
            p.channels[c.name] = glm::vec3(0.0f);
    }

    /** The paintable triangles a fill seeded at tri covers in the current Fill mode. Ascending. */
    static std::vector<uint32_t> fillRegion(const tp::PaintMesh& pm, uint32_t tri, NodeId mesh)
    {
        const std::string& by = texturePaintState().fillBy;
        if (tri >= pm.triangleCount())
            return {};
        if (by == "mesh")
            return pm.allTriangles();
        if (by == "island")
            return pm.islandOf(tri);
        if (by == "material")
            return pm.materialOf(tri);
        if (by == "selection")
        {
            const std::vector<uint32_t> faces = scene::components::selected(mesh, scene::ComponentMode::Face);
            return faces.empty() ? std::vector<uint32_t>{} : pm.ofFaces(faces);
        }
        if (by == "polygon")
        {
            const uint32_t face = pm.faces[tri];
            return pm.ofFaces(std::span<const uint32_t>(&face, 1));
        }
        return pm.paintable[tri] ? std::vector<uint32_t>{tri} : std::vector<uint32_t>{};
    }

    /** Keeps the highlighted region on what a click would fill; recomputed only when the hover leaves it. */
    void updateFillHover()
    {
        const TexturePaintSettings& st = texturePaintState();
        if (st.tool != "fill" || !mOver || mOver->set == kNoNode || !mOver->paint)
        {
            mFillTris.clear();
            mFillKey.clear();
            return;
        }
        const uint32_t tri = uint32_t(mOver->hit.triangle);
        const std::string key = std::to_string(mOver->mesh) + "|" + st.fillBy;
        if (key == mFillKey && st.fillBy != "triangle" && std::binary_search(mFillTris.begin(), mFillTris.end(), tri))
            return;
        mFillKey = key;
        mFillPaint = mOver->paint;
        mFillWorld = mOver->world;
        mFillTris = fillRegion(*mOver->paint, tri, mOver->mesh);
    }

    /** The region under the cursor tinted (front faces only), and the box while dragging one. */
    void drawFill(OverlayPainter& p, const ViewInfo& view)
    {
        constexpr size_t kMaxShown = 200000;
        const glm::vec4 tint(1.0f, 1.0f, 1.0f, 0.22f);
        if (!mMarquee && mFillPaint && !mFillTris.empty() && mFillTris.size() <= kMaxShown)
        {
            const tp::PaintMesh& pm = *mFillPaint;
            for (uint32_t t : mFillTris)
            {
                const glm::dvec3 a(mFillWorld * glm::dvec4(glm::dvec3(pm.positions[3 * t]), 1.0));
                const glm::dvec3 b(mFillWorld * glm::dvec4(glm::dvec3(pm.positions[3 * t + 1]), 1.0));
                const glm::dvec3 c(mFillWorld * glm::dvec4(glm::dvec3(pm.positions[3 * t + 2]), 1.0));
                if (glm::dot(glm::cross(b - a, c - a), view.eye - a) <= 0.0)
                    continue;
                const auto pa = view.project(a), pb = view.project(b), pc = view.project(c);
                if (pa && pb && pc)
                    p.triangle(*pa, *pb, *pc, tint);
            }
        }
        if (mMarquee)
        {
            const QRectF r = QRectF(mMarqueeFrom, mMarqueeTo).normalized();
            p.rect(r, glm::vec4(1.0f, 1.0f, 1.0f, 0.08f));
            p.polyline({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.8f), true);
        }
    }

    /** Fills every region with a front-facing triangle centred in the box (visible ones only with occlusion on). */
    void fillBox(const Target& t, const QRectF& box, const ViewInfo& view)
    {
        if (!t.paint)
            return;
        const tp::PaintMesh& pm = *t.paint;
        const bool occlusion = texturePaintState().occlusion && pm.bvh;
        const glm::dmat4 inv = glm::inverse(t.world);
        const glm::dvec3 eyeObj(inv * glm::dvec4(view.eye, 1.0));
        std::vector<uint32_t> tris;
        for (uint32_t tri = 0; tri < pm.triangleCount(); ++tri)
        {
            if (!pm.paintable[tri] || std::binary_search(tris.begin(), tris.end(), tri))
                continue;
            const glm::dvec3 a(pm.positions[3 * tri]), b(pm.positions[3 * tri + 1]), c(pm.positions[3 * tri + 2]);
            const glm::dvec3 centre = (a + b + c) / 3.0;
            if (glm::dot(glm::cross(b - a, c - a), eyeObj - centre) <= 0.0)
                continue;
            const auto at = view.project(glm::dvec3(t.world * glm::dvec4(centre, 1.0)));
            if (!at || !box.contains(*at))
                continue;
            if (occlusion)
            {
                const glm::dvec3 dir = centre - eyeObj;
                geo::BvhHit hit;
                if (pm.bvh->raycast(eyeObj, glm::normalize(dir), hit) && hit.t < glm::length(dir) * 0.999 &&
                    hit.triangle < pm.triangleCount() && pm.faces[uint32_t(hit.triangle)] != pm.faces[tri])
                    continue;
            }
            std::vector<uint32_t> add = fillRegion(pm, tri, t.mesh);
            std::vector<uint32_t> merged;
            std::set_union(tris.begin(), tris.end(), add.begin(), add.end(), std::back_inserter(merged));
            tris = std::move(merged);
            if (texturePaintState().fillBy == "mesh" || texturePaintState().fillBy == "selection")
                break;
        }
        if (tris.empty())
            return;
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(t.set);
        if (!set)
            return;
        const std::string layer = layerFor(*set);
        tp::Payload boxed = payloadFor(texturePaintState(), false);
        dropMissingChannels(boxed, *set);
        json payload = boxed.toJson();
        payload["triangles"] = tris;
        try
        {
            ops::execute("texPaintStroke",
                         {{"args", {set->name(), layer}}, {"stroke", {{"mesh", t.meshPath}, {"brush", texturePaintState().stroke.toJson()}, {"payload", payload}}}},
                         CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
    }

    /** Tab+drag: fills the region under the cursor (by the Fill mode) unless this sweep already filled it. */
    void sweepFill(const Target& t)
    {
        if (!t.paint || t.hit.triangle < 0)
            return;
        const std::vector<uint32_t> region = fillRegion(*t.paint, uint32_t(t.hit.triangle), t.mesh);
        if (region.empty())
            return;
        const std::string key = std::to_string(t.mesh) + "|" + std::to_string(*std::min_element(region.begin(), region.end()));
        if (!mSwept.insert(key).second)
            return;
        fill(t);
    }

    void endSweep()
    {
        if (!mSweeping)
            return;
        mSweeping = false;
        mSwept.clear();
        UndoStack::get().closeChunk();
    }

    /** Tab held over the viewport, for sweep fills (watched app-wide: Tab is focus navigation otherwise). */
    void watchTab()
    {
        struct Watch final : QObject
        {
            bool* down = nullptr;
            bool eventFilter(QObject* obj, QEvent* ev) override
            {
                if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::KeyRelease && ev->type() != QEvent::ShortcutOverride)
                    return false;
                auto* k = static_cast<QKeyEvent*>(ev);
                QWidget* focus = QApplication::focusWidget();
                if ((k->key() != Qt::Key_Tab && k->key() != Qt::Key_Backtab) || !focus || obj != focus || !focus->inherits("lila::ui::ViewportWidget"))
                    return false;
                if (ev->type() == QEvent::ShortcutOverride)
                    ev->accept();
                else if (!k->isAutoRepeat())
                    *down = ev->type() == QEvent::KeyPress;
                return true;
            }
        };
        auto watch = std::make_unique<Watch>();
        watch->down = &mTabDown;
        qApp->installEventFilter(watch.get());
        mTabWatch = std::move(watch);
    }

    void fill(const Target& t)
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(t.set);
        if (!set)
            return;
        const std::string layer = layerFor(*set);
        tp::Payload p = payloadFor(texturePaintState(), false);
        dropMissingChannels(p, *set);
        json payload = p.toJson();
        payload.erase("triangles");
        payload["triangle"] = t.hit.triangle;
        if (std::vector<uint32_t> locked = lockedTriangles(*set, t.meshPath, *t.paint); !locked.empty())
            payload["locked"] = locked;
        try
        {
            ops::execute("texPaintStroke",
                         {{"args", {set->name(), layer}}, {"stroke", {{"mesh", t.meshPath}, {"brush", texturePaintState().stroke.toJson()}, {"payload", payload}}}},
                         CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    void beginStroke(const Target& t, QMouseEvent* e, const ViewInfo& view)
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(t.set);
        if (!set || !t.paint)
            return;
        const std::string tool = texturePaintState().tool;
        if ((tool == "clone" || tool == "heal") && (!mCloneUv || mCloneMesh != t.mesh))
        {
            log::warn("Texture paint: Ctrl+Alt+click the surface to set the {} source first", tool);
            return;
        }
        mTarget = t;
        mLayer = texturePaintState().quickMask ? std::string(kQuickMaskBuffer) : layerFor(*set);
        if (mLayer.empty())
            return;
        updatePanel();

        auto header = std::make_shared<tex::paint::Stroke>();
        uint64_t top = 0;
        if (const tex::paint::Buffer* b = tex::paint::readPaint(*set)->buffer(mLayer))
            for (const auto& s : b->strokes)
                top = std::max(top, s->id);
        header->id = top + 1;
        header->mesh = t.meshPath;
        texturePaintState().stroke.ensureAlpha();
        header->brush = texturePaintState().stroke.toJson();
        header->payload = payloadFor(texturePaintState(), e->modifiers() & Qt::ControlModifier);
        filterChannels(header->payload, *set);
        header->payload.locked = lockedTriangles(*set, t.meshPath, *t.paint);
        {
            const scene::Node* meshNode = g.node(t.mesh);
            scene::MeshPtr outMesh = rp::paintSurface(*meshNode);
            texturePaintSymmetryFor(*meshNode, outMesh.get(), mSym);
            addUvMirror(mSym);
            header->symmetry = mSym.toJson();
        }
        if (tool == "clone" || tool == "heal")
        {
            const tp::Dab hit{uint32_t(t.hit.triangle), float(t.hit.bary.y), float(t.hit.bary.z)};
            header->payload.sourceOffset = *mCloneUv - t.paint->uv(hit.triangle, hit.u, hit.v);
            // Each symmetry copy clones from the copy of the source, measured from where its copy of the stroke starts.
            for (uint32_t c = 0; c < mSym.count(); ++c)
            {
                tp::Dab src, dst;
                glm::vec2 off = header->payload.sourceOffset;
                if (c > 0 && mSym.copyPoint(*t.paint, mCloneSourceDab.triangle, mCloneSourceDab.u, mCloneSourceDab.v, c, src) &&
                    mSym.copyPoint(*t.paint, hit.triangle, hit.u, hit.v, c, dst))
                    off = t.paint->uv(src.triangle, src.u, src.v) - t.paint->uv(dst.triangle, dst.u, dst.v);
                header->payload.sourceOffsets.push_back(off);
            }
        }
        header->occlusion = texturePaintState().occlusion;
        header->seed = texturePaintState().stroke.seed;
        header->view = strokeView(view, t.world);
        header->stencil = texturePaintState().tool == "reveal" ? revealStencil() : strokeStencil();
        if (header->stencil && header->stencil->mode == "color" && !header->payload.channels.contains(header->stencil->colorChannel))
            header->payload.channels[header->stencil->colorChannel] = glm::vec3(1.0f);
        mParticles.reset();
        if (!texturePaintState().particles.empty() && tool == "paint")
            try
            {
                json spec = texturePaintState().particleSpec;
                spec["kind"] = texturePaintState().particles;
                spec["seed"] = header->seed;
                // Down in the mesh's object space.
                const glm::dvec3 down = glm::normalize(glm::dvec3(glm::inverse(t.world) * glm::dvec4(0.0, -1.0, 0.0, 0.0)));
                spec["down"] = {down.x, down.y, down.z};
                mParticles = tp::ParticleSpec::fromJson(spec);
                header->particles = mParticles->toJson();
                header->particles["expanded"] = true;
            }
            catch (const std::exception& ex)
            {
                log::warn("Texture paint: {}", ex.what());
            }
        mParticleIndex = 0;
        mRecord = *header;
        mRecord.dabs.clear();
        mSegments.clear();
        // Every mesh the stroke crosses commits into one undo step, some of them while it is still being drawn.
        mStrokeChunk.emplace("Paint Stroke");
        mTileable = tex::readSettings(*set).tileable;
        mStrokeTemplate = std::make_shared<tp::Stroke>(mRecord);
        if (!materialsShown())
            viewportToast::showActive(texturesOffText(), 4000, viewportToast::Kind::Warning);
        try
        {
            rp::beginLive(g, *set, mLayer, header);
        }
        catch (const std::exception& ex)
        {
            log::warn("Texture paint: {}", ex.what());
            return;
        }
        mStroking = true;
        // Airbrush: dabs keep coming while held, moving or not.
        if (texturePaintState().stroke.mode == brush::StrokeMode::Airbrush)
        {
            mAirTimer = std::make_unique<QTimer>();
            mAirClock.start();
            QObject::connect(mAirTimer.get(), &QTimer::timeout, [this, alive = std::weak_ptr<bool>(mAlive)] {
                if (!alive.lock() || !mStroking)
                    return;
                mStroke.tick(double(mAirClock.restart()));
                flush();
            });
            mAirTimer->start(16);
        }
        const ViewInfo vi = view;
        if (!mStroke.begin(texturePaintState().stroke, mControls.input(e),
                           [this, vi](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(px, vi, out); }, *this, true))
        {
            cancelStroke();
            return;
        }
        flush();
    }

    void dab(const brush::Dab& d) override
    {
        tex::paint::Dab r;
        if (!tex::paint::recordDab(*mTarget.paint, d, r))
            return;
        // The dab and its particles, then every symmetry copy of each, so particles mirror too.
        std::vector<tp::Dab> base{r};
        if (mParticles)
            base = rp::withParticles(mTarget.paint, base, mParticleIndex, *mParticles);
        ++mParticleIndex;
        for (const tp::Dab& b : base)
        {
            std::vector<tp::Dab> copies = mSym.copies(*mTarget.paint, b);
            mPending.insert(mPending.end(), copies.begin(), copies.end());
            // A tileable set: what reaches past the uv square's edge comes in on the other side too.
            if (mTileable)
                for (const tp::Dab& c : copies)
                {
                    std::vector<tp::Dab> w = tp::tileWrapped(*mTarget.paint, c);
                    mPending.insert(mPending.end(), w.begin(), w.end());
                }
        }
    }

    /** Paints the dabs the stroke made since the last flush. */
    void flush()
    {
        if (mPending.empty())
            return;
        try
        {
            rp::liveDabs(mTarget.set, mLayer, mPending);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        mRecord.dabs.insert(mRecord.dabs.end(), mPending.begin(), mPending.end());
        mPending.clear();
        viewportTools::requestRedraw();
    }

    /** How many meshes strokes reach, for the status line: the selected ones, else all. */
    QString reachLabel()
    {
        scene::Graph& g = scene::Graph::current();
        if (mReachLabel.isEmpty() || g.selection() != mReachSelection)
        {
            mReachSelection = g.selection();
            bool selected = false;
            for (NodeId id : mReachSelection)
            {
                std::vector<NodeId> under = g.descendants(id);
                under.push_back(id);
                selected = selected || std::ranges::any_of(under, [&](NodeId u) {
                    scene::Node* n = g.node(u);
                    return n && n->type().isA("mesh");
                });
            }
            const size_t count = selected ? texturePaintTargets().size() : 0;
            mReachLabel = selected ? QString("%1 selected").arg(count) : QString("all");
        }
        return mReachLabel;
    }

    /** Commits one mesh's part of the stroke; an empty one only drops its live pages. */
    void commitSegment(const Segment& sg)
    {
        scene::Node* set = scene::Graph::current().node(sg.target.set);
        if (!set || sg.record.dabs.empty())
        {
            rp::cancelLive(sg.target.set, sg.layer);
            return;
        }
        try
        {
            // The record goes in unechoed: it can hold thousands of dabs.
            ops::execute("texPaintStroke", {{"args", {set->name(), sg.layer}}, {"stroke", sg.record.toJson(false)}}, CallSource::Script);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
            rp::cancelLive(sg.target.set, sg.layer);
        }
    }

    void endStroke()
    {
        mAirTimer.reset();
        mStroke.end();
        flush();
        mStroking = false;
        std::vector<Segment> segments = std::exchange(mSegments, {});
        std::erase_if(segments, [&](const Segment& sg) { return sg.target.mesh == mTarget.mesh && sg.target.set == mTarget.set; });
        segments.push_back({mTarget, mLayer, mRecord, mSym});
        mStrokeTemplate.reset();
        for (const Segment& sg : segments)
            commitSegment(sg);
        mStrokeChunk.reset();
        // The stroke may have wired new channels; the next hover compiles what the stroke after it would add.
        mWarmedSet = kNoNode;
        viewportTools::requestRedraw();
    }

    void cancelStroke()
    {
        mAirTimer.reset();
        mStroke.cancel();
        mStroking = false;
        mPending.clear();
        for (const Segment& sg : std::exchange(mSegments, {}))
            rp::cancelLive(sg.target.set, sg.layer);
        mStrokeTemplate.reset();
        rp::cancelLive(mTarget.set, mLayer);
        mStrokeChunk.reset();
        viewportTools::requestRedraw();
    }

    void updatePanel()
    {
        LILA_ZONE();
        if (!mPanel)
            return;
        scene::Graph& g = scene::Graph::current();
        QString text = reachLabel() == "all" ? "No texture set under the brush" : "No texture set under the brush; only the selected meshes take paint";
        if (scene::Node* set = g.node(mStroking ? mTarget.set : (mOver ? mOver->set : kNoNode)))
        {
            auto it = gLayers.find(set->id());
            text = QString("%1 / %2").arg(QString::fromStdString(set->name()),
                                               it != gLayers.end() ? QString::fromStdString(it->second) : QString("new paint layer"));
        }
        mPanel->setTarget(text);
        syncSetChannels();
    }

    /**
     * The brush page's More Channels follow the set last under the brush (kept while the cursor is over the panel, so the
     * rows stay put), else the Texture Set panel's set, and that set's settings as they change.
     */
    void syncSetChannels()
    {
        LILA_ZONE();
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(mStroking ? mTarget.set : (mOver ? mOver->set : kNoNode));
        if (!set)
            set = g.node(mChannelsSet);
        if (!set)
            set = tex::findSet(g, currentTextureSet());
        const NodeId id = set ? set->id() : kNoNode;
        if (id == mChannelsSet && !mChannelsDirty)
            return;
        mChannelsSet = id;
        mChannelsDirty = false;
        std::set<std::string> names;
        if (set)
            try
            {
                for (const tex::Channel& c : tex::readSettings(*set).channels)
                    names.insert(c.name);
            }
            catch (const std::exception&)
            {
            }
        mPanel->setSetChannels(names);
    }

    // ------------------------------------------------------------------------------------------- projection buffer

    enum class BufferDrag
    {
        None,
        Paint,
        Move,
        Rotate,
        Scale,
        Warp,
    };

    /** Buffer pixels from widget pixels. */
    glm::vec2 toBuffer(QPointF px, const ViewInfo& view) const
    {
        return {float(px.x() / view.width * mBuffer.width()), float(px.y() / view.height * mBuffer.height())};
    }

    /** Sizes the buffer to the view (keeping it while the view's size and the scale hold) and remembers its view. */
    void ensureBuffer(const ViewInfo& view)
    {
        const int w = std::max(1, int(view.width * texturePaintState().bufferScale)), h = std::max(1, int(view.height * texturePaintState().bufferScale));
        if (mBuffer.width() != w || mBuffer.height() != h)
        {
            mBuffer.resize(w, h);
            mBufferView = view;
        }
        if (!samePaintView(view, mBufferView))
        {
            // Moving the camera leaves the buffer on the screen: project it first when asked to, else it stays to project later.
            // A buffer an undo put back stays until painted again, or projecting would undo the undo.
            if (texturePaintState().autoProject && !mBuffer.empty() && mBufferDrag == BufferDrag::None && !bufferRestored())
                commitBuffer();
            if (mBuffer.empty())
                mBufferView = view;
        }
    }

    bool bufferPress(QMouseEvent* e, const ViewInfo& view)
    {
        ensureBuffer(view);
        const TexturePaintSettings& st = texturePaintState();
        const glm::vec2 at = toBuffer(e->position(), view);
        mDragStart = at;
        mDragLast = at;
        mPlacementStart = mBuffer.placement;
        const bool source = (e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::AltModifier);
        if (source && st.bufferBrush == "clone")
        {
            captureCloneSource(view, at);
            return true;
        }
        if (st.transform)
        {
            mBufferDrag = (e->modifiers() & Qt::ShiftModifier) ? BufferDrag::Rotate : (e->modifiers() & Qt::ControlModifier) ? BufferDrag::Scale : BufferDrag::Move;
            return true;
        }
        if (st.warp)
        {
            float best = 1e30f;
            for (int j = 0; j <= mBuffer.warpCells; ++j)
                for (int i = 0; i <= mBuffer.warpCells; ++i)
                {
                    const glm::vec2 p = mBuffer.warpRest(i, j) + mBuffer.warpPoint(i, j);
                    if (const float d = glm::length(p - at); d < best)
                    {
                        best = d;
                        mWarpI = i;
                        mWarpJ = j;
                    }
                }
            mBufferDrag = best < 20.0f * st.bufferScale ? BufferDrag::Warp : BufferDrag::None;
            return true;
        }
        // Painting goes onto the content as it shows.
        mBuffer.bake();
        mBufferDrag = BufferDrag::Paint;
        mBufferErase = (e->modifiers() & Qt::ControlModifier) != 0;
        mCloneOffsetSet = false;
        mSampler.begin(st.stroke, mControls.input(e), st.stroke.radius);
        mPrevious = at;
        bufferSamples();
        return true;
    }

    void bufferMove(QMouseEvent* e, const ViewInfo& view)
    {
        const glm::vec2 at = toBuffer(e->position(), view);
        const glm::vec2 center(float(mBuffer.width()) * 0.5f, float(mBuffer.height()) * 0.5f);
        switch (mBufferDrag)
        {
        case BufferDrag::Paint:
            mSampler.move(mControls.input(e));
            bufferSamples();
            break;
        case BufferDrag::Move: mBuffer.placement.offset = mPlacementStart.offset + (at - mDragStart); break;
        case BufferDrag::Rotate:
        {
            const glm::vec2 a = mDragStart - center - mPlacementStart.offset, b = at - center - mPlacementStart.offset;
            mBuffer.placement.rotation = mPlacementStart.rotation + std::atan2(b.y, b.x) - std::atan2(a.y, a.x);
            break;
        }
        case BufferDrag::Scale: mBuffer.placement.scale = std::max(0.02f, mPlacementStart.scale * std::exp((at.x - mDragStart.x) / 200.0f)); break;
        case BufferDrag::Warp: mBuffer.warpPoint(mWarpI, mWarpJ) += at - mDragLast; break;
        default: break;
        }
        mDragLast = at;
        viewportTools::requestRedraw();
    }

    /** The sampler's dabs onto the buffer. */
    void bufferSamples()
    {
        const TexturePaintSettings& st = texturePaintState();
        const brush::FalloffTable falloff(st.stroke.resolveFalloff());
        const float scale = st.bufferScale;
        glm::vec3 color(1.0f);
        for (const ChannelValue& c : st.channels)
            if (std::string(c.name) == "baseColor")
                color = c.value;
        for (const brush::StrokeSample& s : mSampler.take())
        {
            const double pressure = st.stroke.pressureSize.map(s.pressure);
            tp::BufferDab d;
            d.center = glm::vec2(s.position) * scale;
            d.radius = float(st.stroke.radius * pressure * s.size) * scale;
            d.opacity = float(st.stroke.strength * st.stroke.flow * st.stroke.pressureOpacity.map(s.pressure) * s.opacity);
            d.previous = mPrevious;
            const tp::Stencil* stencil = texturePaintState().stencil.active() ? &texturePaintState().stencil : nullptr;
            const std::string& b = mBufferErase ? std::string("erase") : st.bufferBrush;
            if (b == "erase")
                mBuffer.erase(d, falloff, stencil);
            else if (b == "blur")
                mBuffer.blur(d, falloff, std::max(1.0f, d.radius * 0.25f), stencil);
            else if (b == "smudge")
                mBuffer.smudge(d, falloff, stencil);
            else if (b == "clone")
            {
                if (mCloneSource.width > 0)
                {
                    if (!mCloneOffsetSet)
                    {
                        mCloneOffset = mClonePoint - d.center;
                        mCloneOffsetSet = true;
                    }
                    mBuffer.clone(d, falloff, mCloneSource, mCloneOffset, stencil);
                }
            }
            else
                mBuffer.paint(d, falloff, color, stencil);
            mPrevious = d.center;
        }
        viewportTools::requestRedraw();
    }

    /** Where the buffer's mesh and set come from: the hovered target, else the last painted, else the only set's mesh. */
    std::optional<Target> bufferTarget(const ViewInfo& view)
    {
        std::optional<Target> t = pick(QPointF(view.width * 0.5, view.height * 0.5), view);
        if ((!t || t->set == kNoNode) && mOver && mOver->set != kNoNode)
            t = mOver;
        if (!t || t->set == kNoNode)
            return std::nullopt;
        return t;
    }

    /** The view's channel as the buffer's clone source: what the surface shows in the set's live base colour. */
    void captureCloneSource(const ViewInfo& view, const glm::vec2& at)
    {
        std::optional<Target> t = bufferTarget(view);
        if (!t)
        {
            log::warn("Texture paint: no textured mesh in view to clone from");
            return;
        }
        try
        {
            const int w = mBuffer.width(), h = mBuffer.height();
            const image::Image uvs = rp::screenUvs(scene::Graph::current(), t->meshPath, strokeView(view, t->world), w, h);
            render::GpuImageRef img = texLive::channelImage(t->setName, "baseColor");
            image::Image src{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 0.0f)};
            if (img)
            {
                const std::vector<uint8_t> bytes = render::readGpuImage(img);
                const bool half = img->format() == render::TextureFormat::RGBA16F;
                const int iw = img->width(), ih = img->height();
                for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
                {
                    const float* u = &uvs.rgba[i * 4];
                    if (u[3] == 0.0f || u[0] < 0.0f || u[0] >= 1.0f || u[1] < 0.0f || u[1] >= 1.0f)
                        continue;
                    const size_t x = size_t(std::min(iw - 1, int(u[0] * iw))), y = size_t(std::min(ih - 1, int((1.0f - u[1]) * ih)));
                    float* o = &src.rgba[i * 4];
                    if (half)
                    {
                        const auto* t16 = reinterpret_cast<const uint16_t*>(bytes.data()) + (y * size_t(iw) + x) * 4;
                        for (int c = 0; c < 3; ++c)
                            o[c] = tp::halfToFloat(t16[c]);
                    }
                    else if (img->format() == render::TextureFormat::RGBA8)
                        for (int c = 0; c < 3; ++c)
                            o[c] = srgbToLinear(bytes[(y * size_t(iw) + x) * 4 + size_t(c)] / 255.0f);
                    o[3] = 1.0f;
                }
            }
            mCloneSource = std::move(src);
            mClonePoint = at;
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
    }

    /** A projected buffer kept so undoing its stroke puts the paint back on the screen. */
    struct Projected
    {
        NodeId set = kNoNode;
        std::string layer;
        uint64_t stroke = 0;
        tp::ProjectionBuffer buffer;
        ViewInfo view;
    };

    bool projectionStrokeExists(const Projected& p) const
    {
        const scene::Node* set = scene::Graph::current().node(p.set);
        const tex::paint::Buffer* b = set ? tex::paint::readPaint(*set)->buffer(p.layer) : nullptr;
        return b && std::any_of(b->strokes.begin(), b->strokes.end(), [&](const auto& st) { return st->id == p.stroke; });
    }

    void rememberProjection(NodeId set, const std::string& layer, uint64_t stroke)
    {
        // A new projection ends redo: projections undone before it can't come back.
        while (!mProjected.empty() && !projectionStrokeExists(mProjected.back()))
            mProjected.pop_back();
        mRestored = -1;
        if (stroke == 0)
            return;
        mProjected.push_back({set, layer, stroke, mBuffer, mBufferView});
        // Each keeps a screen-sized buffer; the oldest go first.
        while (mProjected.size() > 8)
            mProjected.erase(mProjected.begin());
    }

    /** The buffer still holds exactly what an undo put back. */
    bool bufferRestored() const { return mRestored >= 0 && mBuffer.version() == mRestoredVersion; }

    /**
     * Follows undo and redo of projections: while the newest projections' strokes are undone, the buffer shows what the
     * oldest of them projected; when they come back it clears. Paint added to the buffer since is left alone.
     */
    void syncProjectionUndo()
    {
        if (mProjected.empty() || !(mBuffer.empty() || bufferRestored()))
            return;
        int want = -1;
        for (int i = int(mProjected.size()) - 1; i >= 0 && !projectionStrokeExists(mProjected[size_t(i)]); --i)
            want = i;
        if (want == mRestored)
            return;
        if (want < 0)
        {
            mBuffer.clear();
            mRestored = -1;
        }
        else
        {
            mBuffer = mProjected[size_t(want)].buffer;
            mBufferView = mProjected[size_t(want)].view;
            mRestored = want;
            mRestoredVersion = mBuffer.version();
        }
        viewportTools::requestRedraw();
    }

    void commitBuffer()
    {
        if (mBuffer.empty())
            return;
        std::optional<Target> t = bufferTarget(mBufferView);
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = t ? g.node(t->set) : nullptr;
        if (!set)
        {
            log::warn("Texture paint: the buffer projects onto a textured mesh in the middle of the view");
            return;
        }
        const std::string layer = layerFor(*set);
        const image::Image img = mBuffer.resolved();
        tp::Payload p = payloadFor(texturePaintState(), false);
        p.kind = "project";
        json payload = p.toJson();
        payload.erase("triangles");
        try
        {
            payload["image"] = tp::PaintImage::fromRgba(img.width, img.height, img.rgba)->toJson();
            json stroke = {{"mesh", t->meshPath}, {"brush", texturePaintState().stroke.toJson()}, {"payload", payload}, {"occlusion", texturePaintState().occlusion}};
            const json r = ops::execute("texPaintStroke", {{"args", {set->name(), layer}}, {"stroke", stroke}, {"view", projectionViewJson(mBufferView)}},
                                        CallSource::Script);
            rememberProjection(set->id(), r.value("layer", layer), r.value("stroke", uint64_t(0)));
            mBuffer.clear();
            mBufferView = mLastView;
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    /** A picture the user picks, its colour decoded as a file painted into channel decodes. */
    static std::optional<image::Image> openImage(const char* title, const std::string& channel)
    {
        const QString f = ui::fileDialog::getOpenFileName(mainWindow(), title, {}, "Images (*.png *.jpg *.jpeg *.exr *.tga *.tif *.tiff *.psd)");
        if (f.isEmpty())
            return std::nullopt;
        std::string err;
        image::Image img = image::read(f.toStdString(), &err);
        if (img.width <= 0)
        {
            log::warn("Texture paint: {}: {}", f.toStdString(), err);
            return std::nullopt;
        }
        decodeForChannel(img, f.toStdString(), channel);
        return img;
    }

    /** Decodes a picture file's colour to linear when it lands in a colour channel; data channels and float files read as stored. */
    static void decodeForChannel(image::Image& img, const std::string& file, const std::string& channel)
    {
        if (std::string_view(tex::maps::imageColorSpace(paths::fromUtf8(file), channel)) != "srgb")
            return;
        for (size_t i = 0; i < img.rgba.size(); ++i)
            if (i % 4 != 3)
                img.rgba[i] = srgbToLinear(std::clamp(img.rgba[i], 0.0f, 1.0f));
    }

    void loadBufferImage()
    {
        std::optional<image::Image> img = openImage("Project Image", payloadFor(texturePaintState(), false).colorChannel);
        if (!img)
            return;
        mBuffer.bake();
        mBuffer.place(*img);
        texturePaintState().transform = true;
        if (mPanel)
            mPanel->sync();
        viewportTools::requestRedraw();
    }

    /** The view projections and captures use: a recalled saved view, else the viewport's. */
    json projectionViewJson(const ViewInfo& view) const { return mRecalledView.is_null() ? viewJson(view) : mRecalledView; }

    void saveView(const std::string& name)
    {
        const std::string set = currentTextureSet();
        if (set.empty())
            return;
        try
        {
            ops::execute("texProjectionView", {{"args", {set, name}}, {"view", viewJson(mLastView)}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        if (mPanel)
            mPanel->sync();
    }

    /** Projects from a saved view again: the viewport moves to it and projections use it exactly. */
    void recallView(const std::string& name)
    {
        const std::string set = currentTextureSet();
        try
        {
            mRecalledView = ops::execute("texProjectionView", {{"args", {set, name}}}, CallSource::UI);
            if (mRecalledView.contains("eye") && mRecalledView.contains("lookAt"))
                ops::execute("viewCamera", {{"eye", mRecalledView["eye"]}, {"lookAt", mRecalledView["lookAt"]}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
            mRecalledView = json();
        }
        viewportTools::requestRedraw();
    }

    /**
     * Writes what the view shows of the channel the buffer projects into, opens it in the image editor (the preference,
     * else the system's), and projects the file back from the same view each time it is saved.
     */
    void editInImageEditor()
    {
        const std::string set = currentTextureSet();
        if (set.empty())
            return;
        json view = projectionViewJson(mLastView);
        // The view's eye and target travel with it, so the editor's projection lands from exactly here.
        const std::string channel = "baseColor";
        const std::string file = paths::toUtf8(std::filesystem::path(QDir::temp().filePath("lila_image_editor").toStdString()) /
                                               (set + "_" + std::to_string(QDateTime::currentMSecsSinceEpoch()) + ".png"));
        try
        {
            ops::execute("texCaptureView", {{"args", {set}}, {"view", view}, {"file", file}, {"channel", channel}, {"scale", texturePaintState().bufferScale}},
                         CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
            return;
        }
        mEditorFile = file;
        mEditorSet = set;
        mEditorView = view;
        if (!mWatcher)
        {
            mWatcher = std::make_unique<QFileSystemWatcher>();
            QObject::connect(mWatcher.get(), &QFileSystemWatcher::fileChanged, [this, alive = std::weak_ptr<bool>(mAlive)](const QString& f) {
                if (!alive.lock())
                    return;
                // Editors save by replacing the file: watch it again, and wait for the write to finish.
                QTimer::singleShot(400, [this, f, alive] {
                    if (!alive.lock())
                        return;
                    if (!mWatcher->files().contains(f))
                        mWatcher->addPath(f);
                    projectEdited();
                });
            });
        }
        mWatcher->addPath(QString::fromStdString(file));
        const std::string editor = PrefStore::get().value<std::string>("general", "/texturePaint/imageEditor", std::string());
        if (!editor.empty())
            QProcess::startDetached(QString::fromStdString(editor), {QString::fromStdString(file)});
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(file)));
        log::info("Texture paint: editing {} in the image editor; each save projects it back", file);
    }

    /** Patch Fill in screen space: what is painted in the buffer is the hole, rebuilt from the view's own picture around it. */
    void patchFillBuffer()
    {
        const std::string set = currentTextureSet();
        if (set.empty() || mBuffer.empty())
        {
            log::warn("Patch Fill: paint over what to remove first");
            return;
        }
        const image::Image painted = mBuffer.resolved();
        const std::string file = paths::toUtf8(std::filesystem::path(QDir::temp().filePath("lila_patch_fill").toStdString()) /
                                               (set + "_" + std::to_string(QDateTime::currentMSecsSinceEpoch()) + ".png"));
        image::Image view;
        try
        {
            ops::execute("texCaptureView", {{"args", {set}}, {"view", projectionViewJson(mBufferView)}, {"file", file}, {"scale", texturePaintState().bufferScale}},
                         CallSource::UI);
            std::string err;
            view = image::read(paths::fromUtf8(file), &err);
            if (view.width <= 0)
                throw std::runtime_error(err);
        }
        catch (const std::exception& e)
        {
            log::warn("Patch Fill: {}", e.what());
            return;
        }
        std::error_code ec;
        std::filesystem::remove(paths::fromUtf8(file), ec);
        // The capture is the view's size times the buffer scale, as the buffer is; nearest texel covers any rounding.
        const int w = painted.width, h = painted.height;
        image::Image base;
        base.width = w;
        base.height = h;
        base.rgba.resize(size_t(w) * h * 4);
        std::vector<uint8_t> hole(size_t(w) * h), source(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t i = size_t(y) * w + x;
                const float* v = &view.rgba[(size_t(std::min(view.height - 1, y * view.height / h)) * view.width + std::min(view.width - 1, x * view.width / w)) * 4];
                for (int c = 0; c < 3; ++c)
                    base.rgba[i * 4 + c] = srgbToLinear(std::clamp(v[c], 0.0f, 1.0f));
                base.rgba[i * 4 + 3] = 1.0f;
                hole[i] = painted.rgba[i * 4 + 3] > 0.01f ? 1 : 0;
                source[i] = !hole[i] && v[3] > 0.5f ? 1 : 0;
            }
        try
        {
            const image::Image filled = tex::patchFill({base}, hole, source).front();
            image::Image out = filled;
            for (size_t i = 0; i < hole.size(); ++i)
                out.rgba[i * 4 + 3] = hole[i] ? 1.0f : 0.0f;
            mBuffer.clear();
            mBuffer.place(out, glm::vec4(0.0f, 0.0f, float(w), float(h)));
            mBuffer.bake();
        }
        catch (const std::exception& e)
        {
            log::warn("Patch Fill: {}", e.what());
            return;
        }
        if (mPanel)
            mPanel->sync();
        viewportTools::requestRedraw();
        log::info("Patch Fill: filled; Project (Enter) lays it on the layer");
    }

    void pickStroke(const Target& t)
    {
        scene::Node* set = scene::Graph::current().node(t.set);
        if (!set)
            return;
        json r;
        try
        {
            r = ops::execute("texPickStroke", {{"args", {set->name(), layerFor(*set)}}, {"mesh", t.meshPath},
                                               {"position", {t.hit.point.x, t.hit.point.y, t.hit.point.z}}},
                             CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Strokes: {}", e.what());
        }
        if (r.is_null())
            log::info("Strokes: no stroke of the active layer there");
        gPickingStroke = false;
        if (auto fn = gStrokePicked)
            fn(r);
    }

    void projectEdited()
    {
        scene::Node* set = tex::findSet(scene::Graph::current(), mEditorSet);
        if (!set || mEditorFile.empty())
            return;
        tp::Payload p = payloadFor(texturePaintState(), false);
        p.kind = "project";
        p.channels.clear();
        p.colorChannel = "baseColor";
        json payload = p.toJson();
        payload["image"] = mEditorFile;
        try
        {
            ops::execute("texPaintStroke", {{"args", {set->name(), layerFor(*set)}}, {"stroke", {{"payload", payload}, {"brush", texturePaintState().stroke.toJson()}}},
                                            {"view", mEditorView}},
                         CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
    }

    void loadStencil()
    {
        const QString f = ui::fileDialog::getOpenFileName(mainWindow(), "Stencil", {}, "Images (*.png *.jpg *.jpeg *.exr *.tga *.tif *.tiff *.psd)");
        if (f.isEmpty())
            return;
        try
        {
            setStencilFile(f.toStdString());
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        if (mPanel)
            mPanel->sync();
        viewportTools::requestRedraw();
    }

    /** Reveal on the screen: the picture as a colour stencil filling the view's height, centred. */
    std::optional<tp::StrokeStencil> revealStencil()
    {
        const TexturePaintSettings& s = texturePaintState();
        if (s.revealSpace != "screen" || s.revealImage.empty())
            return std::nullopt;
        if (!mRevealPicture || mRevealPath != s.revealImage)
        {
            std::string err;
            image::Image img = image::read(paths::fromUtf8(s.revealImage), &err);
            if (img.width <= 0)
            {
                log::warn("Texture paint: reveal picture {}: {}", s.revealImage, err);
                return std::nullopt;
            }
            decodeForChannel(img, s.revealImage, tp::StrokeStencil().colorChannel);
            mRevealPicture = tp::PaintImage::fromRgba(img.width, img.height, img.rgba);
            mRevealPath = s.revealImage;
        }
        tp::holdImage(mRevealPicture);
        tp::StrokeStencil st;
        st.image = mRevealPicture->key();
        st.mode = "color";
        return st;
    }

    /** The stencil as a stroke records it, its picture held so the live stroke can read it before the command stores it. */
    std::optional<tp::StrokeStencil> strokeStencil()
    {
        const TexturePaintSettings& s = texturePaintState();
        if (!s.stencil.active())
            return std::nullopt;
        if (!mStencilPicture || mStencilPictureVersion != s.stencilVersion)
        {
            image::Image pic = s.stencil.image;
            // A mask reads the file's values as they are.
            if (s.stencilMode == "color")
                decodeForChannel(pic, s.stencilPath, tp::StrokeStencil().colorChannel);
            mStencilPicture = tp::PaintImage::fromRgba(pic.width, pic.height, pic.rgba);
            mStencilPictureVersion = s.stencilVersion;
        }
        tp::holdImage(mStencilPicture);
        tp::StrokeStencil out;
        out.image = mStencilPicture->key();
        out.offset = s.stencil.place.offset / s.bufferScale;
        out.scale = s.stencil.place.scale;
        out.rotation = s.stencil.place.rotation;
        out.invert = s.stencil.invert;
        out.tile = s.stencil.tile;
        out.mode = s.stencilMode;
        return out;
    }

    /** World corners of an NDC rectangle just behind the near plane: top left, top right, bottom right, bottom left. */
    static void screenQuad(const ViewInfo& view, const glm::vec2 (&ndc)[4], glm::dvec3 (&out)[4])
    {
        const glm::dmat4 inv = glm::inverse(view.viewProj());
        for (int k = 0; k < 4; ++k)
        {
            const glm::dvec4 p = inv * glm::dvec4(ndc[k].x, ndc[k].y, 0.99, 1.0);
            out[k] = glm::dvec3(p) / p.w;
        }
    }

    static render::GpuImageRef toGpu(const std::vector<float>& rgba, int w, int h, render::GpuImageRef img, float alphaScale, bool gray)
    {
        if (!img || img->width() != w || img->height() != h)
            img = render::GpuImage::create({w, h, render::TextureFormat::RGBA8, true, false, "paint.buffer"});
        std::vector<uint8_t> bytes(size_t(w) * size_t(h) * 4);
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
        {
            const float* t = &rgba[i * 4];
            const float l = t[0] * 0.2126f + t[1] * 0.7152f + t[2] * 0.0722f;
            for (int c = 0; c < 3; ++c)
                bytes[i * 4 + size_t(c)] = uint8_t(std::lround(linearToSrgb(gray ? l : t[c]) * 255.0f));
            bytes[i * 4 + 3] = uint8_t(std::lround(std::clamp((gray ? 1.0f : t[3]) * alphaScale, 0.0f, 1.0f) * 255.0f));
        }
        img->update(0, 0, w, h, bytes);
        return img;
    }

    void drawStencil(render::FrameState& fs, const ViewInfo& view)
    {
        const TexturePaintSettings& s = texturePaintState();
        if (!s.stencil.active())
            return;
        if (!mStencilImage || mStencilImageVersion != s.stencilVersion)
        {
            mStencilImage = toGpu(s.stencil.image.rgba, s.stencil.image.width, s.stencil.image.height, nullptr, 1.0f, s.stencilMode != "color");
            mStencilImageVersion = s.stencilVersion;
        }
        // The stencil image spans the view's height at scale 1, centred, placed like the buffer's content.
        const float aspect = float(s.stencil.image.width) / float(s.stencil.image.height);
        const glm::vec2 center(float(view.width) * 0.5f, float(view.height) * 0.5f);
        const float hw = float(view.height) * aspect * 0.5f, hh = float(view.height) * 0.5f;
        tp::Placement place = s.stencil.place;
        place.offset /= s.bufferScale;
        glm::vec2 ndc[4];
        render::ImagePlaneDraw sd;
        // A tiled stencil shows three repeats each way around the picture.
        const int reach = s.stencil.tile ? 1 : 0;
        for (int ty = -reach; ty <= reach; ++ty)
            for (int tx = -reach; tx <= reach; ++tx)
            {
                const glm::vec2 shift(float(tx) * hw * 2.0f, float(ty) * hh * 2.0f);
                const glm::vec2 corners[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
                for (int k = 0; k < 4; ++k)
                {
                    const glm::vec2 p = place.apply(center + shift + corners[k], center);
                    ndc[k] = {p.x / float(view.width) * 2.0f - 1.0f, p.y / float(view.height) * 2.0f - 1.0f};
                }
                screenQuad(view, ndc, sd.corners);
                sd.image = mStencilImage->asTexture();
                sd.opacity = s.stencil.opacity;
                sd.depth = render::ImagePlaneDraw::Depth::Front;
                fs.imagePlanes.push_back(sd);
            }
    }

    /** Reveal on the screen: the picture shown faintly where it sits (the view's height, centred), so strokes can be aimed. */
    void drawReveal(render::FrameState& fs, const ViewInfo& view)
    {
        const TexturePaintSettings& s = texturePaintState();
        if (s.revealSpace != "screen" || s.revealImage.empty())
            return;
        if (mRevealShownPath != s.revealImage)
        {
            mRevealShownPath = s.revealImage;
            std::string err;
            const image::Image img = image::read(paths::fromUtf8(s.revealImage), &err);
            mRevealShown = img.width > 0 ? toGpu(img.rgba, img.width, img.height, nullptr, 1.0f, false) : nullptr;
            mRevealAspect = img.width > 0 ? float(img.width) / float(img.height) : 1.0f;
        }
        if (!mRevealShown)
            return;
        const float hw = mRevealAspect * float(view.height) / float(view.width);
        const glm::vec2 ndc[4] = {{-hw, -1.0f}, {hw, -1.0f}, {hw, 1.0f}, {-hw, 1.0f}};
        render::ImagePlaneDraw d;
        screenQuad(view, ndc, d.corners);
        d.image = mRevealShown->asTexture();
        d.opacity = 0.35f;
        d.depth = render::ImagePlaneDraw::Depth::Front;
        fs.imagePlanes.push_back(d);
    }

    void drawBuffer(render::FrameState& fs, OverlayPainter& p, const ViewInfo& view)
    {
        if (mBuffer.width() <= 0)
            return;
        // The buffer stays where it was painted: drawn over the view it belongs to.
        const glm::vec2 full[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        if (mShownVersion != mBuffer.version() || mShownTransform != mBuffer.transformed() || mBuffer.transformed())
        {
            const image::Image shown = mBuffer.resolved();
            mBufferImage = toGpu(shown.rgba, shown.width, shown.height, mBufferImage, 1.0f, false);
            mShownVersion = mBuffer.version();
            mShownTransform = mBuffer.transformed();
        }
        render::ImagePlaneDraw d;
        screenQuad(view, full, d.corners);
        d.image = mBufferImage->asTexture();
        d.depth = render::ImagePlaneDraw::Depth::Front;
        fs.imagePlanes.push_back(std::move(d));
        drawStencil(fs, view);
        if (texturePaintState().warp)
        {
            const float sx = float(view.width) / float(mBuffer.width()), sy = float(view.height) / float(mBuffer.height());
            auto at = [&](int i, int j) {
                const glm::vec2 q = mBuffer.warpRest(i, j) + mBuffer.warpPoint(i, j);
                return QPointF(q.x * sx, q.y * sy);
            };
            const glm::vec4 ink = p.ink(0.85f);
            for (int j = 0; j <= mBuffer.warpCells; ++j)
                for (int i = 0; i <= mBuffer.warpCells; ++i)
                {
                    if (i < mBuffer.warpCells)
                        p.line(at(i, j), at(i + 1, j), 1.0f, ink);
                    if (j < mBuffer.warpCells)
                        p.line(at(i, j), at(i, j + 1), 1.0f, ink);
                    p.disc(at(i, j), 4.0f, ink);
                }
        }
        if (mCloneSource.width > 0 && texturePaintState().bufferBrush == "clone")
        {
            const QPointF c(mClonePoint.x / texturePaintState().bufferScale, mClonePoint.y / texturePaintState().bufferScale);
            p.line(c - QPointF(6, 0), c + QPointF(6, 0), 1.5f, p.ink(0.85f));
            p.line(c - QPointF(0, 6), c + QPointF(0, 6), 1.5f, p.ink(0.85f));
        }
    }

    std::shared_ptr<bool> mAlive;
    int mActiveListener = 0;
    QPointer<TexturePaintPanel> mPanel;
    /** The set the hover last compiled the next graph for. */
    NodeId mWarmedSet = kNoNode;
    /** Fill: the region a click fills, on the hovered mesh, and the press waiting to become a click or a box. */
    std::vector<uint32_t> mFillTris;
    std::shared_ptr<const tp::PaintMesh> mFillPaint;
    glm::dmat4 mFillWorld{1.0};
    std::string mFillKey;
    std::optional<Target> mFillPress;
    QPointF mMarqueeFrom, mMarqueeTo;
    bool mMarquee = false;
    QPointF mHover;
    std::optional<Target> mOver;
    Target mTarget;
    std::string mLayer;
    bool mStroking = false;
    brush::Stroke mStroke;
    tp::PaintSymmetry mSym;
    /** The meshes a stroke left for another, and the header every new one starts from. */
    std::vector<Segment> mSegments;
    /** reachLabel's answer, kept until the selection changes. */
    std::vector<NodeId> mReachSelection;
    QString mReachLabel;
    std::optional<UndoChunk> mStrokeChunk;
    std::unique_ptr<QTimer> mAirTimer;
    QElapsedTimer mAirClock;
    bool mTileable = false;
    std::shared_ptr<tp::Stroke> mStrokeTemplate;
    tp::Dab mCloneSourceDab;
    std::vector<tex::paint::Dab> mPending;
    tex::paint::Stroke mRecord;
    std::optional<tp::ParticleSpec> mParticles;
    uint32_t mParticleIndex = 0;
    std::optional<glm::vec2> mCloneUv;
    NodeId mCloneMesh = kNoNode;
    ViewInfo mLastView;

    tp::ProjectionBuffer mBuffer;
    std::vector<Projected> mProjected;
    int mRestored = -1;
    uint64_t mRestoredVersion = 0;
    ViewInfo mBufferView;
    render::GpuImageRef mBufferImage, mStencilImage, mRevealShown;
    std::string mRevealShownPath;
    float mRevealAspect = 1.0f;
    uint64_t mStencilImageVersion = ~0ull, mStencilPictureVersion = ~0ull;
    std::shared_ptr<const tp::PaintImage> mStencilPicture, mRevealPicture;
    std::string mRevealPath;
    /** S held: drags place the stencil instead of painting. */
    bool mStencilKey = false;
    std::unique_ptr<QObject> mKeyWatch, mTabWatch;
    bool mTabDown = false, mSweeping = false;
    int mSettingsListener = 0;
    NodeId mChannelsSet = kNoNode;
    bool mChannelsDirty = true;
    std::set<std::string> mSwept;
    enum class StencilDrag
    {
        None,
        Move,
        Rotate,
        Scale,
    } mStencilDrag = StencilDrag::None;
    QPointF mStencilFrom;
    /** A recalled saved projection view, null for the viewport's. */
    json mRecalledView;
    /** Edit in Image Editor: the file being edited, its set and the view it was captured from. */
    std::string mEditorFile, mEditorSet;
    json mEditorView;
    std::unique_ptr<QFileSystemWatcher> mWatcher;
    tp::Placement mStencilStart;
    /** The path being placed, or the last one painted (stroke set), whose points can still be dragged. Object space. */
    struct PathEdit
    {
        NodeId set = kNoNode, mesh = kNoNode;
        std::string meshPath, layer;
        glm::dmat4 world{1.0};
        tp::StrokeView view;
        std::vector<glm::dvec3> pts, in, out;
        std::vector<bool> manual;
        uint64_t stroke = 0;
        /** Every id the painted path has had (each edit gives it a new one), so undo and redo can find it again. */
        std::vector<uint64_t> ids;
    } mPath;
    int mPathDrag = -1;
    std::vector<QPointF> mSelect;
    QPointF mGradientFrom, mGradientTo;
    bool mGradientDrag = false;
    bool mSelecting = false;
    Qt::KeyboardModifiers mSelectMods;
    bool mPathHandle = false, mPathMoved = false;
    uint64_t mShownVersion = 0;
    bool mShownTransform = false;
    BufferDrag mBufferDrag = BufferDrag::None;
    bool mBufferErase = false;
    brush::StrokeSampler mSampler;
    glm::vec2 mDragStart{0.0f}, mDragLast{0.0f}, mPrevious{0.0f};
    tp::Placement mPlacementStart;
    int mWarpI = 0, mWarpJ = 0;
    image::Image mCloneSource;
    glm::vec2 mClonePoint{0.0f}, mCloneOffset{0.0f};
    bool mCloneOffsetSet = false;

    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return texturePaintState().stroke; },
                                       [this] {
                                           if (mPanel)
                                               mPanel->sync();
                                           viewportTools::requestRedraw();
                                       },
                                       [] { saveTexturePaintSettings(); }};
};

/** The set and layer the layer actions act on: the selected set (or the only one) and its last painted or active layer. */
std::optional<std::pair<std::string, std::string>> actionTarget()
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* set = nullptr;
    for (NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().name == tex::kTextureSetType)
            set = n;
    if (!set && gLayers.size() == 1)
        set = g.node(gLayers.begin()->first);
    if (!set)
    {
        const std::vector<std::string> names = tex::setNames(g);
        set = names.size() == 1 ? tex::findSet(g, names.front()) : nullptr;
    }
    if (!set)
    {
        log::warn("Select a texture set");
        return std::nullopt;
    }
    auto it = gLayers.find(set->id());
    std::string layer = it != gLayers.end() ? it->second : tex::paint::readPaint(*set)->active;
    if (layer.empty())
    {
        log::warn("{} has no paint layer", set->name());
        return std::nullopt;
    }
    return std::make_pair(set->name(), layer);
}
} // namespace

std::vector<uint64_t> texturePaintTargets()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<NodeId> out;
    auto add = [&](NodeId id) {
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && std::ranges::find(out, id) == out.end())
            out.push_back(id);
    };
    for (NodeId id : g.selection())
    {
        add(id);
        for (NodeId d : g.descendants(id))
            add(d);
    }
    if (out.empty())
        for (NodeId id : g.nodes())
            add(id);
    return out;
}

void startTexturePaint() { viewportTools::set(std::make_unique<TexturePaintTool>()); }

bool texturePaintActive() { return dynamic_cast<TexturePaintTool*>(viewportTools::active()) != nullptr; }

bool texturePaintDimWire() { return PrefStore::get().value<bool>("general", kDimWirePref, false); }

bool texturePaintShowsWire()
{
    const TexturePaintSettings& s = texturePaintState();
    return s.tool == "fill" && (s.fillBy == "triangle" || s.fillBy == "polygon" || s.fillBy == "island");
}

QWidget* texturePaintPage(QWidget* parent)
{
    TexturePaintPanel* page = brushPage();
    page->setParent(parent);
    return page;
}

std::string texturePaintPreset() { return texturePaintState().presetId; }

std::string quickMaskSet()
{
    const TexturePaintSettings& s = texturePaintState();
    return s.quickMask || !s.quickLimit.empty() ? currentTextureSet() : std::string();
}

nlohmann::json defaultTexturePaintBrush() { return TexturePaintSettings{}.stroke.toJson(); }

void resetTexturePaintBrush()
{
    // An empty preset: what any preset sets goes back to the defaults; channel values and the stencil stay.
    tex::brushes::Preset none;
    none.payload = {{"kind", "paint"}};
    TexturePaintSettings& s = texturePaintState();
    applyPreset(s, none);
    s.stroke = TexturePaintSettings{}.stroke;
    brushChanged();
    ensureTexturePaint();
}

void applyTexturePaintPreset(const std::string& id)
{
    std::optional<tex::brushes::Preset> p = tex::brushes::find(id);
    if (!p)
    {
        log::warn("Texture paint: no brush preset {}", id);
        return;
    }
    applyPreset(texturePaintState(), *p);
    brushChanged();
    ensureTexturePaint();
}

std::string texturePaintAlpha() { return texturePaintState().stroke.alphaPath; }

void setTexturePaintAlpha(const std::string& alphaPath)
{
    texturePaintState().stroke.alphaPath = alphaPath;
    brushChanged();
    ensureTexturePaint();
}

std::string texturePaintTexture() { return texturePaintState().source == "texture" ? texturePaintState().sourceImage : std::string(); }

void pickTextureChannelsAt(const std::string& set, double u, double v, bool add) { pickChannels(set, {{"uv", {u, v}}}, add); }

nlohmann::json texturePaintGradient(double u0, double v0, double u1, double v1, bool reverse)
{
    return gradientPayload(texturePaintState(), {u0, v0}, {u1, v1}, "uv", reverse);
}

void setTexturePaintTexture(const std::string& file)
{
    TexturePaintSettings& s = texturePaintState();
    s.sourceImage = file;
    s.source = "texture";
    if (s.tool != "paint" && s.tool != "fill" && s.tool != "project")
        s.tool = "paint";
    brushChanged();
    ensureTexturePaint();
}

nlohmann::json texturePaintSettings()
{
    const TexturePaintSettings& s = texturePaintState();
    json payload = payloadFor(s, false).toJson();
    payload.erase("triangles");
    json particles;
    if (!s.particles.empty() && s.tool == "paint")
    {
        particles = s.particleSpec;
        particles["kind"] = s.particles;
    }
    return {{"brush", s.stroke.toJson()}, {"payload", payload}, {"particles", particles}, {"tool", s.tool}};
}

void registerTexturePaintUi()
{
    quickMaskOverlay::registerCommands();
    // A drawer, not the tool's draw, so offscreen captures show the tint too.
    viewportHooks::addDrawer("lila.quickMaskOverlay", [](render::FrameState& fs, OverlayPainter&, const ViewInfo&) {
        const ViewportTool* tool = viewportTools::active();
        if (tool && std::string_view(tool->name()) == "texturePaint")
            quickMaskOverlay::addDraws(fs, quickMaskSet());
    });
    actions::add({.id = "tex.resetBrush",
                  .label = "Reset Brush",
                  .category = "Texture",
                  .run = [] { resetTexturePaintBrush(); },
                  .doc = "Put the texture brush back to the default round brush: size, strength, falloff and source, as "
                         "before any preset. Channel values stay.",
                  .keywords = {"default brush", "brush preset"}});
    actions::add({.id = "tex.paintWire",
                  .label = "Wireframe While Painting",
                  .category = "Texture",
                  .run =
                      [] {
                          PrefStore::get().set("general", kDimWirePref, !texturePaintDimWire());
                          viewportTools::requestRedraw();
                      },
                  .doc = "Keeps the mesh wire visible while Texture Paint hides it, dimmed: in the viewport and over the 2D View. Tap Shift+F while painting.",
                  .checked = [] { return texturePaintDimWire(); },
                  .icon = "grid-pattern",
                  .keywords = {"wireframe", "uvs", "edges"}});
    actions::add({.id = "tex.paint",
                  .label = "Texture Paint",
                  .category = "Texture",
                  .run = [] { startTexturePaint(); },
                  .doc = "Paint material channels into the texture set under the brush, in texture space across UV seams: paint, direction, "
                         "normal bumps and stamps, smudge, blur, sharpen, clone, heal, fills, particle brushes, library materials, and a "
                         "screen-space projection buffer. Each stroke is one undo step and is kept as a record the layer can re-rasterise.",
                  .python = "lila.ui.run_action(\"tex.paint\")"});
    auto layerAction = [](const char* id, const char* label, const char* command, const char* doc) {
        actions::add({.id = id,
                      .label = label,
                      .category = "Texture",
                      .run =
                          [command] {
                              if (auto t = actionTarget())
                                  actions::run({{"command", command}, {"args", {t->first, t->second}}});
                          },
                      .doc = doc,
                      .python = std::string(command) + "(\"<set>\", \"<layer>\")"});
    };
    layerAction("tex.rasterizeLayer", "Rasterize Paint Layer", "texRasterize", "Rebuilds the paint layer's pixels from its stroke records.");
    layerAction("tex.flattenLayer", "Flatten Paint Layer", "texFlattenLayer", "Keeps the paint layer's pixels and drops its stroke records.");
    layerAction("tex.clearLayer", "Clear Paint Layer", "texClearLayer", "Drops the paint layer's strokes and pixels.");

    actions::add({.id = "tex.stencil",
                  .label = "Load Stencil...",
                  .category = "Texture",
                  .run =
                      [] {
                          const QString f = ui::fileDialog::getOpenFileName(mainWindow(), "Stencil", {}, "Images (*.png *.jpg *.jpeg *.exr *.tga *.tif *.tiff *.psd)");
                          if (!f.isEmpty())
                              actions::run({{"command", "texPaintStencil"}, {"args", {{"args", {f.toStdString()}}}}});
                      },
                  .doc = "Pin a picture to the screen for texture painting: every stroke paints through it, as a mask or in its colours. "
                         "Hold S and drag to move it, with Shift to rotate (add Ctrl to snap to 15 degrees), with Ctrl to scale.",
                  .python = "texPaintStencil(\"<image file>\")",
                  .keywords = {"stencil", "mask", "overlay", "paint through"}});
    actions::add({.id = "tex.quickMaskOverlay",
                  .label = "Quick Mask Overlay",
                  .category = "Texture",
                  .run =
                      [] {
                          quickMaskOverlay::setShown(!quickMaskOverlay::style().shown);
                          brushChanged();
                      },
                  .doc = "Show or hide the tint over what the quick mask leaves out, on the meshes and in the 2D View. Its colour and "
                         "opacity are in Preferences > General > Textures.",
                  .checked = [] { return quickMaskOverlay::style().shown; },
                  .icon = "eye",
                  .keywords = {"quick mask", "mask overlay", "selection tint", "rubylith"}});
    actions::add({.id = "tex.materialFromMaps",
                  .label = "Material from Maps...",
                  .category = "Texture",
                  .run =
                      [] {
                          const QStringList files = ui::fileDialog::getOpenFileNames(mainWindow(), "Material from Maps", {},
                                                                                     "Images (*.png *.jpg *.jpeg *.tga *.tif *.tiff *.exr *.psd *.bmp)");
                          if (files.isEmpty())
                              return;
                          json list = json::array();
                          for (const QString& f : files)
                              list.push_back(f.toStdString());
                          actions::run({{"command", "texMaterialFromMaps"}, {"args", list}});
                      },
                  .doc = "Make library materials from texture maps: each file's channel is read from its name (base colour, roughness, "
                         "normal in either convention, packed ORM and more), and files that differ only by it make one material.",
                  .python = "texMaterialFromMaps(\"C:/maps/Rock_BaseColor.png\", \"C:/maps/Rock_Normal.png\")",
                  .keywords = {"import maps", "texture maps", "pbr maps", "create material"}});
    actions::add({.id = "tex.materialsFromFolder",
                  .label = "Materials from Maps Folder...",
                  .category = "Texture",
                  .run =
                      [] {
                          const QString dir = ui::fileDialog::getExistingDirectory(mainWindow(), "Materials from Maps Folder");
                          if (!dir.isEmpty())
                              actions::run({{"command", "texMaterialFromMaps"}, {"args", {{"folder", dir.toStdString()}}}});
                      },
                  .doc = "Make a library material for every set of texture maps in a folder and its subfolders.",
                  .python = "texMaterialFromMaps(folder=\"C:/maps\")",
                  .keywords = {"batch import", "texture maps", "pbr folder"}});
    actions::add({.id = "tex.patchFill",
                  .label = "Patch Fill Selection",
                  .category = "Texture",
                  .run =
                      [] {
                          const std::string set = currentTextureSet();
                          scene::Graph& g = scene::Graph::current();
                          scene::Node* node = set.empty() ? nullptr : tex::findSet(g, set);
                          const std::string layer = set.empty() ? std::string() : textureSets::activeLayer(set);
                          image::Image sel;
                          try
                          {
                              if (node && !layer.empty())
                                  sel = rp::readChannel(node->id(), layer, "mask");
                          }
                          catch (const std::exception&)
                          {
                              sel = {};
                          }
                          bool any = false;
                          for (size_t i = 3; i < sel.rgba.size() && !any; i += 4)
                              any = sel.rgba[i] > 0.5f;
                          if (!any)
                          {
                              log::warn("Patch Fill: select what to fill on a paint layer first (the Select tool)");
                              return;
                          }
                          for (size_t i = 0; i < sel.rgba.size(); i += 4)
                          {
                              const float v = sel.rgba[i + 3] > 0.5f ? 1.0f : 0.0f;
                              sel.rgba[i] = sel.rgba[i + 1] = sel.rgba[i + 2] = v;
                              sel.rgba[i + 3] = 1.0f;
                          }
                          const std::filesystem::path file = std::filesystem::path(QDir::temp().filePath("lila_patch_fill").toStdString()) /
                                                             (set + "_hole_" + std::to_string(QDateTime::currentMSecsSinceEpoch()) + ".png");
                          std::error_code ec;
                          std::filesystem::create_directories(file.parent_path(), ec);
                          image::writePng(file, sel.width, sel.height, sel.rgba, 8);
                          json call = {{"args", {set}}, {"hole", paths::toUtf8(file)}};
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                                  call["mesh"] = n->path();
                          if (!call.contains("mesh"))
                              log::info("Patch Fill: no mesh selected, so patches come from all of uv space, not just the hole's own islands");
                          actions::run({{"command", "texPatchFill"}, {"args", call}});
                      },
                  .doc = "Fill the selection on the active paint layer from patches of what is around it, in every channel: it lands as a new "
                         "fill layer masked to the selection. With the mesh selected, patches come only from the uv islands the selection is on.",
                  .python = "texPatchFill(\"Crate\", holeUv=[[0.4, 0.4], [0.6, 0.4], [0.6, 0.6], [0.4, 0.6]], mesh=\"|crate\")",
                  .keywords = {"content aware", "heal", "remove", "clone", "fill hole", "patch"}});
    // Ptex works on the open set and the selected mesh, one channel at a time.
    auto ptexTarget = [](const char* what, std::string& set, std::string& mesh, std::string& channel) {
        set = currentTextureSet();
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                mesh = n->path();
        if (set.empty() || mesh.empty())
        {
            log::warn("{}: open a texture set and select its mesh", what);
            return false;
        }
        QStringList names;
        if (scene::Node* n = tex::findSet(g, set))
            for (const tex::Channel& c : tex::readSettings(*n).channels)
                names << QString::fromStdString(c.name);
        bool ok = false;
        const QString picked = QInputDialog::getItem(mainWindow(), what, "Channel", names, std::max<qsizetype>(0, names.indexOf("baseColor")), false, &ok);
        channel = picked.toStdString();
        return ok && !picked.isEmpty();
    };
    actions::add({.id = "tex.importPtex",
                  .label = "Import Ptex...",
                  .category = "Texture",
                  .run =
                      [ptexTarget] {
                          std::string set, mesh, channel;
                          if (!ptexTarget("Import Ptex", set, mesh, channel))
                              return;
                          const QString f = ui::fileDialog::getOpenFileName(mainWindow(), "Import Ptex", {}, "Ptex (*.ptx)");
                          if (!f.isEmpty())
                              actions::run({{"command", "texImportPtex"},
                                            {"args", {{"args", {set}}, {"mesh", mesh}, {"file", f.toStdString()}, {"channel", channel}}}});
                      },
                  .doc = "Bring a Ptex file onto the open texture set as a fill layer, baked through the selected quad mesh's uvs.",
                  .python = "texImportPtex(\"Crate\", mesh=\"|crate\", file=\"C:/maps/crate.ptx\")",
                  .keywords = {"ptex", "ptx", "per-face texture", "import"}});
    actions::add({.id = "tex.exportPtex",
                  .label = "Export Ptex...",
                  .category = "Texture",
                  .run =
                      [ptexTarget] {
                          std::string set, mesh, channel;
                          if (!ptexTarget("Export Ptex", set, mesh, channel))
                              return;
                          const QString f = ui::fileDialog::getSaveFileName(mainWindow(), "Export Ptex", QString::fromStdString(set + "_" + channel + ".ptx"),
                                                                            "Ptex (*.ptx)");
                          if (!f.isEmpty())
                              actions::run({{"command", "texExportPtex"},
                                            {"args", {{"args", {set}}, {"mesh", mesh}, {"file", f.toStdString()}, {"channel", channel}}}});
                      },
                  .doc = "Write one channel of the open texture set to a Ptex file, per face of the selected quad mesh.",
                  .python = "texExportPtex(\"Crate\", mesh=\"|crate\", file=\"C:/maps/crate.ptx\", faceResolution=7)",
                  .keywords = {"ptex", "ptx", "per-face texture", "export"}});
    actions::add({.id = "tex.transferSet",
                  .label = "Transfer Texture Set",
                  .category = "Texture",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          std::vector<scene::Node*> meshes;
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                                  meshes.push_back(n);
                          const std::string set = currentTextureSet();
                          if (meshes.size() != 2 || set.empty())
                          {
                              log::warn("Transfer Texture Set: select the mesh the set paints, then the mesh to move it onto");
                              return;
                          }
                          actions::run({{"command", "texTransferSet"}, {"args", {{"args", {set}}, {"source", meshes[0]->path()}, {"onto", meshes[1]->path()}}}});
                      },
                  .doc = "Move the open texture set's look from the first selected mesh onto the second (an old version onto a new one, a high "
                         "mesh onto its low one), as a new paint layer.",
                  .python = "texTransferSet(\"<set>\", source=\"<old mesh>\", onto=\"<new mesh>\")",
                  .keywords = {"transfer", "reproject", "project texture", "bake to"}});
    actions::add({.id = "tex.paintAlongCurve",
                  .label = "Paint Along Curve",
                  .category = "Texture",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          scene::Node* curve = nullptr;
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                                  curve = n;
                          const std::string set = currentTextureSet();
                          if (!curve || set.empty())
                          {
                              log::warn("Paint Along Curve: select a curve, with a texture set open");
                              return;
                          }
                          const TexturePaintSettings& s = texturePaintState();
                          tp::Payload pay = payloadFor(s, false);
                          pay.kind = "paint";
                          json call = {{"args", {set}}, {"curve", curve->path()}, {"brush", s.stroke.toJson()}, {"payload", pay.toJson()}};
                          if (gLastPaintView)
                              call["radius"] = s.stroke.radius * gLastPaintView->unitsPerPixel(glm::dvec3(curve->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]));
                          else
                              log::info("Paint Along Curve: no paint view yet, so the brush's world radius ({}) sizes the dabs", s.stroke.worldRadius);
                          actions::run({{"command", "texPaintPath"}, {"args", call}});
                      },
                  .doc = "Paint along the selected curve, projected onto the mesh the open texture set paints, with the Texture Paint brush "
                         "and channels. The path stays on the stroke: the Path tool can move its points afterwards.",
                  .python = "texPaintPath(\"<set>\", curve=\"<curve>\", payload={\"channels\": {\"baseColor\": [1, 0, 0]}})",
                  .keywords = {"stroke curve", "paint along", "spline paint"}});
    ops::registerCommand({
        .name = "texPaintStencil",
        .doc = "The texture paint tool's stencil: a picture pinned to the screen that strokes and the projection buffer paint through. "
               "A file sets it (an empty one clears it); the flags change it. mode mask scales each stroke by the picture's brightness, "
               "color paints its colours into Base Color. Returns {file, mode, invert, tile, offset, scale, rotation}.",
        .positionalDoc = "[file]",
        .flags = {{"mode", "", ArgType::String, nullptr, "mask or color.", {"mask", "color"}},
                  {"invert", "", ArgType::Bool, nullptr, "Paint where the picture is dark."},
                  {"tile", "", ArgType::Bool, nullptr, "Repeat the picture past its edges."},
                  {"offset", "", ArgType::List, nullptr, "[x, y] screen pixels from the view's centre."},
                  {"scale", "", ArgType::Float, nullptr, "1 spans the view's height."},
                  {"rotation", "", ArgType::Float, nullptr, "Degrees."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            TexturePaintSettings& s = texturePaintState();
            if (!a.positional.empty())
                try
                {
                    setStencilFile(a.positional[0].get<std::string>());
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("texPaintStencil: ") + e.what());
                }
            if (a.has("mode"))
            {
                s.stencilMode = a.get<std::string>("mode") == "color" ? "color" : "mask";
                ++s.stencilVersion;
            }
            if (a.has("invert"))
                s.stencil.invert = a.get<bool>("invert");
            if (a.has("tile"))
                s.stencil.tile = a.get<bool>("tile");
            if (a.has("offset"))
            {
                const json o = a.flags["offset"];
                if (!o.is_array() || o.size() != 2)
                    throw CommandError("texPaintStencil: offset is [x, y]");
                s.stencil.place.offset = glm::vec2(o[0].get<float>(), o[1].get<float>()) * s.bufferScale;
            }
            if (a.has("scale"))
                s.stencil.place.scale = std::max(0.01f, float(a.get<double>("scale")));
            if (a.has("rotation"))
                s.stencil.place.rotation = float(glm::radians(a.get<double>("rotation")));
            brushChanged();
            viewportTools::requestRedraw();
            const glm::vec2 off = s.stencil.place.offset / s.bufferScale;
            return {{"file", s.stencilPath},
                    {"active", s.stencil.active()},
                    {"mode", s.stencilMode},
                    {"invert", s.stencil.invert},
                    {"tile", s.stencil.tile},
                    {"offset", {off.x, off.y}},
                    {"scale", s.stencil.place.scale},
                    {"rotation", glm::degrees(s.stencil.place.rotation)}};
        },
    });
}
} // namespace lila::ui
