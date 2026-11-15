#include "ui/TexturePaintTool.h"

// Before Qt: its slots macro breaks tex/Plan.h.
#include "render/PaintGpu.h"
#include "tex/BrushPresets.h"
#include "tex/Library.h"
#include "tex/PaintBuffer.h"
#include "tex/PaintParticles.h"
#include "tex/PaintSymmetry.h"
#include "tex/TextureSet.h"

#include "brush/BrushUi.h"
#include "brush/Symmetry.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Symmetry.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/GpuImage.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/ColorPopup.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/TexLive.h"
#include "ui/TextureSetPanel.h"
#include "ui/UiCommon.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"
#include "ui/FileDialog.h"

#include <QApplication>
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
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
using scene::kNoNode;
using scene::NodeId;
namespace tp = tex::paint;
namespace rp = render::paint;

constexpr const char* kTexturePaintPrefix = "/texturePaint";

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
    {"fill", "Fill", "Click to fill what the Fill mode picks (the highlight shows it); drag a box to fill everything in it.", "bucket-droplet"},
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
    std::vector<ChannelValue> channels = {{"baseColor", "Base Color", true, true, {0.8f, 0.1f, 0.08f}},
                                          {"roughness", "Roughness", false, false, glm::vec3(0.5f)},
                                          {"metallic", "Metallic", false, false, glm::vec3(0.0f)},
                                          {"height", "Height", false, false, glm::vec3(1.0f)},
                                          {"emissive", "Emissive", true, false, glm::vec3(1.0f)}};
    tp::Blend blend = tp::Blend::Normal;
    float tilt = 0.5f;
    std::string flowChannel = "normal";
    bool occlusion = true;
    float filterRadius = 4.0f;
    float amount = 1.0f;
    std::string normalMode = "bump";
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
};

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

bool filterTool(const std::string& t) { return t == "smudge" || t == "blur" || t == "sharpen" || t == "clone" || t == "heal"; }

tp::Payload payloadFor(const TexturePaintSettings& s, bool erase)
{
    tp::Payload p;
    p.kind = s.tool == "project" ? "project" : s.tool;
    for (const ChannelValue& c : s.channels)
        if (c.on)
            p.channels[c.name] = c.color ? c.value : glm::vec3(c.value.x);
    if (s.tool == "direction")
    {
        p.channels.clear();
        p.directionChannel = s.flowChannel;
    }
    if (s.tool == "normal")
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
    const bool sourced = s.tool == "paint" || s.tool == "fill" || s.tool == "project";
    if (sourced && s.source == "material" && !s.material.empty())
        p.source = {{"library", s.material}, {"resolution", 512}, {"scale", s.materialScale}};
    else if (sourced && s.source == "texture" && !s.sourceImage.empty())
        p.source = {{"node", "layerImage"}, {"values", {{"file", s.sourceImage}}}, {"resolution", 1024}, {"scale", s.materialScale}};
    if (!s.tip.is_null())
        p.tip = s.tip;
    return p;
}

/** Applies a preset over the settings: only what it names changes. */
void applyPreset(TexturePaintSettings& s, const tex::brushes::Preset& p)
{
    s.stroke = brush::BrushSettings::fromJson(p.brush, s.stroke);
    const json& pay = p.payload;
    const std::string kind = pay.value("kind", std::string("paint"));
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
    s.normalMode = pay.value("normalMode", std::string("bump"));
    s.intensity = pay.value("intensity", 1.0f);
    s.fillBy = pay.value("fillBy", std::string("island"));
    s.material = pay.contains("source") ? pay["source"].value("library", std::string()) : std::string();
    s.materialScale = pay.contains("source") ? pay["source"].value("scale", 1.0f) : 1.0f;
    s.source = s.material.empty() ? "values" : "material";
    s.presetId = p.id;
    s.tip = pay.contains("tip") ? pay["tip"] : json();
    s.particles = p.particles.is_object() ? p.particles.value("kind", std::string()) : std::string();
    s.particleSpec = p.particles.is_object() ? p.particles : json::object();
}

float toDisplay(float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f); }
float fromDisplay(float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 2.2f); }
/** The texture set whose material a face of the mesh uses; the only set when the mesh names none of theirs. */
scene::Node* setForFace(scene::Graph& g, const geo::Mesh& mesh, geo::Index face)
{
    std::string slot;
    std::span<const int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    if (!ids.empty() && face < ids.size())
        slot = scene::materials::slotName(mesh, ids[face]);
    const std::vector<std::string> names = tex::setNames(g);
    for (const std::string& name : names)
        if (scene::Node* s = tex::findSet(g, name); s && !slot.empty() && rp::setMaterial(*s) == slot)
            return s;
    return names.size() == 1 ? tex::findSet(g, names.front()) : nullptr;
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

class TexturePaintPanel final : public QWidget
{
public:
    std::function<void()> onChanged, onNewLayer, onCommit, onClearBuffer, onLoadImage, onLoadStencil, onClearStencil;

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
        moreMenu->addAction("Save Brush Preset...", this, [this] { savePresetAs(); });
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
                texturePaintState().tool = id;
                syncTools();
                changed();
            });
            mTools.push_back(b);
            tiles.push_back(b);
        }
        // One icon strip: the brush row above names the current tool, the tooltips name the rest.
        col->addWidget(controls::grid(tiles, int(std::size(kTools)), this));
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
        for (size_t i = 0; i < texturePaintState().channels.size(); ++i)
        {
            ChannelValue& c = texturePaintState().channels[i];
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
                form->addRow(c.label, field);
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
                auto* v = settingSpin(this, 0.0, 1.0, 0.05, 3);
                fieldRow->addWidget(v, 1);
                form->addRow(c.label, field);
                mValue.push_back(v);
                mSwatch.push_back(nullptr);
                mBright.push_back(nullptr);
                connect(v, &QDoubleSpinBox::valueChanged, this, [this, i](double x) {
                    texturePaintState().channels[i].value = glm::vec3(float(x));
                    changed();
                });
            }
        }
        mMaterial = new QComboBox(this);
        mMaterial->setToolTip("The library material the stroke paints");
        for (const tex::library::Entry& e : tex::library::all())
            if (e.tier != "scene")
                mMaterial->addItem(QString::fromStdString(e.meta.name), QString::fromStdString(e.id));
        mMaterialRow = addRow(form, "Material", mMaterial);
        mTexture = new QToolButton(this);
        mTexture->setObjectName("brushTexture");
        controls::setRole(mTexture, "row");
        mTexture->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mTexture->setIcon(icon("photo"));
        mTexture->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mTexture->setToolTip("The image the stroke paints: pick another in the library's Textures");
        connect(mTexture, &QToolButton::clicked, this, [] { textureSets::showLibrary("textures"); });
        mTextureRow = addRow(form, "Texture", mTexture);
        mMaterialScale = settingSpin(this, 0.05, 64.0, 0.25, 2, "Tiles per uv unit");
        mScaleRow = addRow(form, "Scale", mMaterialScale);
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
        mNormalMode->addItem("Bump", "bump");
        mNormalMode->addItem("Stamp", "stamp");
        mIntensity = settingSpin(this, -4.0, 4.0, 0.1, 2, "Normal: strength; negative digs");
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
        mParticles->setToolTip("Particles each dab emits, painting as they move over the surface");
        mParticleRow = addRow(form, "Particles", mParticles);

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
        auto* stencil = new QPushButton("Stencil...", this);
        stencil->setToolTip("Pin a greyscale mask to the screen: buffer painting goes through it");
        auto* noStencil = new QPushButton("No Stencil", this);
        for (QPushButton* b : {commit, clearBuf, image, stencil, noStencil})
            bufButtons->addWidget(b);
        mBufferButtons = addRow(form, "", bufButtons);
        connect(commit, &QPushButton::clicked, this, [this] { call(onCommit); });
        connect(clearBuf, &QPushButton::clicked, this, [this] { call(onClearBuffer); });
        connect(image, &QPushButton::clicked, this, [this] { call(onLoadImage); });
        connect(stencil, &QPushButton::clicked, this, [this] { call(onLoadStencil); });
        connect(noStencil, &QPushButton::clicked, this, [this] { call(onClearStencil); });

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
        mMirror = new QCheckBox("Mirror", this);
        mMirror->setToolTip("Mirror strokes across the mesh's symmetry axis (X when it has none)");
        form->addRow(mMirror);
        col->addLayout(form);
        mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return texturePaintState().stroke; }, [this] { changed(); });
        col->addWidget(mStrokeOptions);

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
        connect(mMirror, &QCheckBox::toggled, this, [this](bool v) {
            texturePaintState().stroke.mirror = v;
            changed();
        });
        sync();
    }

    void setTarget(const QString& text) { mTarget->setText(text); }

    void syncTools()
    {
        for (size_t i = 0; i < mTools.size(); ++i)
            mTools[i]->setChecked(texturePaintState().tool == kTools[i].id);
    }

    void sync()
    {
        const TexturePaintSettings& s = texturePaintState();
        syncTools();
        const QSignalBlocker b1(mBlend), b2(mTilt), b3(mRadius), b4(mStrength), b5(mFalloff), b6(mOcclusion), b7(mMirror);
        const QSignalBlocker b8(mMaterial), b9(mMaterialScale), b10(mFlow), b11(mNormalMode), b12(mIntensity), b13(mStamp), b14(mFilterRadius);
        const QSignalBlocker b15(mAmount), b17(mParticles), b18(mBufferBrush), b19(mBufferScale), b20(mTransform), b21(mWarp);
        const QSignalBlocker b22(mAuto), b23(mFacing);
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
        mMirror->setChecked(s.stroke.mirror);
        mSource->setCurrent(s.source == "material" ? 1 : s.source == "texture" ? 2 : 0);
        mTexture->setText(s.sourceImage.empty() ? QString("None") : QFileInfo(QString::fromStdString(s.sourceImage)).completeBaseName());
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
                mValue[i]->setValue(s.channels[i].value.x);
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
        const bool values = t == "paint" || t == "fill" || t == "project";
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
        showRow(mDirRow, t == "direction");
        showRow(mNormalRow, t == "normal");
        showRow(mStampRow, t == "normal" && texturePaintState().normalMode == "stamp");
        showRow(mFilterRow, filterTool(t));
        showRow(mFillRow, t == "fill");
        showRow(mParticleRow, t == "paint");
        for (QWidget* w : {mBufferRow, mBufferFlags, mFacingRow, mBufferButtons})
            showRow(w, t == "project");
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
    std::vector<QToolButton*> mTools;
    std::vector<QCheckBox*> mOn;
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
    std::map<QWidget*, QWidget*> mLabels;
    QSpinBox* mRadius = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QComboBox* mFalloff = nullptr;
    QCheckBox* mOcclusion = nullptr;
    QCheckBox* mMirror = nullptr;
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

/** The last layer painted per set, so the panel and the actions know it. */
std::map<NodeId, std::string> gLayers;

/** A view as the JSON texPaintStroke takes (world space). */
json viewJson(const ViewInfo& v)
{
    const glm::dmat4 vp = v.viewProj();
    json m = json::array();
    for (int i = 0; i < 16; ++i)
        m.push_back((&vp[0][0])[i]);
    return {{"viewProj", m}, {"eye", {v.eye.x, v.eye.y, v.eye.z}}, {"width", v.width}, {"height", v.height}};
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

bool sameView(const ViewInfo& a, const ViewInfo& b) { return a.view == b.view && a.proj == b.proj && a.width == b.width && a.height == b.height; }

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
        mPanel->onLoadStencil = [this] { loadStencil(); };
        mPanel->onClearStencil = [this] {
            mStencil = {};
            mStencilImage.reset();
            viewportTools::requestRedraw();
        };
        mPanel->sync();
        updatePanel();
    }

    void deactivate() override
    {
        if (mStroking)
            cancelStroke();
        mControls.deactivate();
        textureSets::removeActiveLayerListener(mActiveListener);
        mActiveListener = 0;
        mAlive.reset();
        if (mPanel)
        {
            for (std::function<void()>* fn : {&mPanel->onNewLayer, &mPanel->onCommit, &mPanel->onClearBuffer, &mPanel->onLoadImage,
                                              &mPanel->onLoadStencil, &mPanel->onClearStencil})
                *fn = nullptr;
            mPanel->setTarget({});
        }
        textureSets::showBrushPage(false);
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        const std::string& tool = texturePaintState().tool;
        if (tool == "project")
            return bufferPress(e, view);
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

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
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
        const Target* t = mStroking ? &mTarget : (mOver ? &*mOver : nullptr);
        ToolStatus status{.tool = "Texture Paint"};
        if (st.tool == "project")
        {
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
            status.prompt = "hover a mesh with a texture set";
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
            status.fields = {{"layer", it != gLayers.end() ? QString::fromStdString(it->second) : QString("new layer")}};
            if ((st.tool == "clone" || st.tool == "heal") && !mCloneUv)
                status.keys = {{"Ctrl+Alt+click", "set source"}};
        }
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
        std::vector<NodeId> candidates;
        for (NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                candidates.push_back(id);
        if (candidates.empty())
            for (NodeId id : g.nodes())
                if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                    candidates.push_back(id);
        glm::dvec3 o, d;
        view.ray(px, o, d);
        std::optional<Target> best;
        double nearest = 1e300;
        for (NodeId id : candidates)
        {
            scene::Node* n = g.node(id);
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
            nearest = dist;
            Target t;
            t.mesh = id;
            t.meshPath = n->path();
            t.world = w;
            t.hit = hit;
            scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            if (scene::Node* set = mesh ? setForFace(g, *mesh, hit.face) : nullptr)
            {
                t.set = set->id();
                t.setName = set->name();
                t.paint = rp::paintMesh(g, t.meshPath, rp::setMaterial(*set));
            }
            best = t;
        }
        return best;
    }

    /** The shared stroke's view of the target under a pixel, in its object space. */
    bool surfaceQuery(glm::dvec2 px, const ViewInfo& view, brush::SurfaceHit& out)
    {
        glm::dvec3 o, d;
        view.ray(QPointF(px.x, px.y), o, d);
        const glm::dmat4 inv = glm::inverse(mTarget.world);
        geo::BvhHit hit;
        const tex::paint::PaintMesh& m = *mTarget.paint;
        if (!m.bvh->raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit, std::numeric_limits<double>::max(), false) ||
            hit.triangle >= m.triangleCount())
            return false;
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
            const std::string material = rp::setMaterial(*set);
            for (NodeId id : g.nodes())
            {
                scene::Node* n = g.node(id);
                if (!n || !n->type().isA("mesh"))
                    continue;
                scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
                if (!m || !m->numFaces())
                    continue;
                const auto& names = m->materialNames();
                if (std::find(names.begin(), names.end(), material) != names.end())
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
    void filterChannels(tp::Payload& p, const scene::Node& set)
    {
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
        json payload = payloadFor(texturePaintState(), false).toJson();
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

    void fill(const Target& t)
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(t.set);
        if (!set)
            return;
        const std::string layer = layerFor(*set);
        tp::Payload p = payloadFor(texturePaintState(), false);
        json payload = p.toJson();
        payload.erase("triangles");
        payload["triangle"] = t.hit.triangle;
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
        mLayer = layerFor(*set);
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
        {
            const scene::Node* meshNode = g.node(t.mesh);
            scene::MeshPtr outMesh = meshNode->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            mSym = tp::brushSymmetry(texturePaintState().stroke, outMesh.get(), scene::paintSymmetrySpec(*meshNode));
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
        {
            std::vector<tp::Dab> extra = tp::emitParticles(*mTarget.paint, r, mParticleIndex, *mParticles);
            base.insert(base.end(), extra.begin(), extra.end());
        }
        ++mParticleIndex;
        for (const tp::Dab& b : base)
        {
            std::vector<tp::Dab> copies = mSym.copies(*mTarget.paint, b);
            mPending.insert(mPending.end(), copies.begin(), copies.end());
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

    void endStroke()
    {
        mStroke.end();
        flush();
        mStroking = false;
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = g.node(mTarget.set);
        if (!set || mRecord.dabs.empty())
        {
            rp::cancelLive(mTarget.set, mLayer);
            return;
        }
        try
        {
            // The record goes in unechoed: it can hold thousands of dabs.
            ops::execute("texPaintStroke", {{"args", {set->name(), mLayer}}, {"stroke", mRecord.toJson(false)}}, CallSource::Script);
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
            rp::cancelLive(mTarget.set, mLayer);
        }
        // The stroke may have wired new channels; the next hover compiles what the stroke after it would add.
        mWarmedSet = kNoNode;
        viewportTools::requestRedraw();
    }

    void cancelStroke()
    {
        mStroke.cancel();
        mStroking = false;
        mPending.clear();
        rp::cancelLive(mTarget.set, mLayer);
        viewportTools::requestRedraw();
    }

    void updatePanel()
    {
        if (!mPanel)
            return;
        scene::Graph& g = scene::Graph::current();
        QString text = "No texture set under the brush";
        if (scene::Node* set = g.node(mStroking ? mTarget.set : (mOver ? mOver->set : kNoNode)))
        {
            auto it = gLayers.find(set->id());
            text = QString("%1 / %2").arg(QString::fromStdString(set->name()),
                                               it != gLayers.end() ? QString::fromStdString(it->second) : QString("new paint layer"));
        }
        mPanel->setTarget(text);
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
        if (!sameView(view, mBufferView))
        {
            // Moving the camera leaves the buffer on the screen: project it first when asked to, else it stays to project later.
            if (texturePaintState().autoProject && !mBuffer.empty() && mBufferDrag == BufferDrag::None)
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
            const tp::Stencil* stencil = mStencil.active() ? &mStencil : nullptr;
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
            ops::execute("texPaintStroke", {{"args", {set->name(), layer}}, {"stroke", stroke}, {"view", viewJson(mBufferView)}}, CallSource::Script);
            mBuffer.clear();
            mBufferView = mLastView;
        }
        catch (const std::exception& e)
        {
            log::warn("Texture paint: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    static std::optional<image::Image> openImage(const char* title)
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
        return img;
    }

    void loadBufferImage()
    {
        std::optional<image::Image> img = openImage("Project Image");
        if (!img)
            return;
        for (size_t i = 0; i < img->rgba.size(); ++i)
            if (i % 4 != 3)
                img->rgba[i] = srgbToLinear(std::clamp(img->rgba[i], 0.0f, 1.0f));
        mBuffer.bake();
        mBuffer.place(*img);
        texturePaintState().transform = true;
        if (mPanel)
            mPanel->sync();
        viewportTools::requestRedraw();
    }

    void loadStencil()
    {
        std::optional<image::Image> img = openImage("Stencil");
        if (!img)
            return;
        mStencil = {};
        mStencil.image = std::move(*img);
        mStencilImage.reset();
        viewportTools::requestRedraw();
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
        if (mStencil.active())
        {
            if (!mStencilImage)
                mStencilImage = toGpu(mStencil.image.rgba, mStencil.image.width, mStencil.image.height, nullptr, 1.0f, true);
            // The stencil image spans the view's height at scale 1, centred, placed like the buffer's content.
            const float aspect = float(mStencil.image.width) / float(mStencil.image.height);
            const glm::vec2 center(float(view.width) * 0.5f, float(view.height) * 0.5f);
            const float hw = float(view.height) * aspect * 0.5f, hh = float(view.height) * 0.5f;
            const glm::vec2 corners[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            glm::vec2 ndc[4];
            tp::Placement place = mStencil.place;
            place.offset /= texturePaintState().bufferScale;
            for (int k = 0; k < 4; ++k)
            {
                const glm::vec2 s = place.apply(center + corners[k], center);
                ndc[k] = {s.x / float(view.width) * 2.0f - 1.0f, s.y / float(view.height) * 2.0f - 1.0f};
            }
            render::ImagePlaneDraw sd;
            screenQuad(view, ndc, sd.corners);
            sd.image = mStencilImage->asTexture();
            sd.opacity = mStencil.opacity;
            sd.depth = render::ImagePlaneDraw::Depth::Front;
            fs.imagePlanes.push_back(std::move(sd));
        }
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
    tp::Dab mCloneSourceDab;
    std::vector<tex::paint::Dab> mPending;
    tex::paint::Stroke mRecord;
    std::optional<tp::ParticleSpec> mParticles;
    uint32_t mParticleIndex = 0;
    std::optional<glm::vec2> mCloneUv;
    NodeId mCloneMesh = kNoNode;
    ViewInfo mLastView;

    tp::ProjectionBuffer mBuffer;
    ViewInfo mBufferView;
    tp::Stencil mStencil;
    render::GpuImageRef mBufferImage, mStencilImage;
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

void startTexturePaint() { viewportTools::set(std::make_unique<TexturePaintTool>()); }

QWidget* texturePaintPage(QWidget* parent)
{
    TexturePaintPanel* page = brushPage();
    page->setParent(parent);
    return page;
}

std::string texturePaintPreset() { return texturePaintState().presetId; }

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
    return {{"brush", s.stroke.toJson()}, {"payload", payload}, {"particles", particles}};
}

void registerTexturePaintUi()
{
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
}
} // namespace lila::ui
