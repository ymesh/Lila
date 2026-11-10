#include "ui/VertexPaintTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "brush/BrushUi.h"
#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/RenderMesh.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/SoftSelect.h"
#include "ui/Actions.h"
#include "ui/ColorPopup.h"
#include "ui/FlowLayout.h"
#include "ui/PaintColor.h"
#include "ui/Icons.h"
#include "ui/Tools.h"
#include "ui/MeshCache.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"
#include "vcolor/VertexColors.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
using scene::kNoNode;
using scene::MeshPtr;
using scene::NodeId;

constexpr size_t kRecentMax = 12;

json colorJson(const glm::vec4& c) { return json::array({c.r, c.g, c.b, c.a}); }
glm::vec4 colorFrom(const json& j, const glm::vec4& fallback)
{
    if (!j.is_array() || j.size() < 4)
        return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}
/** Display color of a linear RGBA, for swatches. */
QColor vertexColorSwatch(const glm::vec4& linear)
{
    glm::vec4 s = vcolor::linearToSrgb(glm::clamp(linear, 0.0f, 1.0f));
    return QColor::fromRgbF(s.r, s.g, s.b);
}

/** The shared brush's defaults for vertex paint. */
brush::BrushSettings defaultVertexPaintStroke()
{
    brush::BrushSettings b;
    b.radius = 50.0;
    b.strength = 1.0;
    b.spacing = 0.15;
    b.falloff = "Smooth";
    b.mirror = true;
    b.pressureSize.enabled = true;
    b.pressureOpacity.enabled = true;
    return b;
}

struct VertexPaintSettings
{
    glm::vec4 color{1.0f};
    glm::vec4 secondary{0.0f, 0.0f, 0.0f, 1.0f};
    std::string blend = "replace";
    std::string channels = "rgba";
    bool frontFacesOnly = true;
    bool paintThrough = false;
    bool selectionOnly = true;
    /** Radius, strength, spacing, falloff, mirror, pressure and the rest of the shared brush. */
    brush::BrushSettings stroke = defaultVertexPaintStroke();
    std::vector<glm::vec4> swatches;
    std::vector<glm::vec4> recent;
};

VertexPaintSettings load()
{
    VertexPaintSettings s;
    const PrefStore& p = PrefStore::get();
    s.color = colorFrom(p.value<json>("general", "/vertexPaint/color", json()), s.color);
    s.secondary = colorFrom(p.value<json>("general", "/vertexPaint/secondary", json()), s.secondary);
    s.blend = p.value<std::string>("general", "/vertexPaint/blend", s.blend);
    s.stroke.loadPrefs("/vertexPaint");
    s.stroke.radius = std::clamp(s.stroke.radius, 2.0, 2000.0);
    s.stroke.spacing = std::clamp(s.stroke.spacing, 0.02, 2.0);
    s.channels = p.value<std::string>("general", "/vertexPaint/channels", s.channels);
    s.frontFacesOnly = p.value<bool>("general", "/vertexPaint/frontFacesOnly", s.frontFacesOnly);
    s.paintThrough = p.value<bool>("general", "/vertexPaint/paintThrough", s.paintThrough);
    s.selectionOnly = p.value<bool>("general", "/vertexPaint/selectionOnly", s.selectionOnly);
    for (const json& j : p.value<json>("general", "/vertexPaint/swatches", json::array()))
        s.swatches.push_back(colorFrom(j, glm::vec4(1.0f)));
    for (const json& j : p.value<json>("general", "/vertexPaint/recent", json::array()))
        s.recent.push_back(colorFrom(j, glm::vec4(1.0f)));
    if (vcolor::channelsFromString(s.channels) == 0)
        s.channels = "rgba";
    return s;
}

VertexPaintSettings& vertexPaintSettings()
{
    static VertexPaintSettings s = load();
    return s;
}

void saveVertexPaintSettings()
{
    const VertexPaintSettings& s = vertexPaintSettings();
    PrefStore& p = PrefStore::get();
    p.set("general", "/vertexPaint/color", colorJson(s.color));
    p.set("general", "/vertexPaint/secondary", colorJson(s.secondary));
    p.set("general", "/vertexPaint/blend", s.blend);
    s.stroke.savePrefs("/vertexPaint");
    p.set("general", "/vertexPaint/channels", s.channels);
    p.set("general", "/vertexPaint/frontFacesOnly", s.frontFacesOnly);
    p.set("general", "/vertexPaint/paintThrough", s.paintThrough);
    p.set("general", "/vertexPaint/selectionOnly", s.selectionOnly);
    json sw = json::array(), rc = json::array();
    for (const glm::vec4& c : s.swatches)
        sw.push_back(colorJson(c));
    for (const glm::vec4& c : s.recent)
        rc.push_back(colorJson(c));
    p.set("general", "/vertexPaint/swatches", sw);
    p.set("general", "/vertexPaint/recent", rc);
}

void pushRecent(const glm::vec4& c)
{
    auto& r = vertexPaintSettings().recent;
    std::erase(r, c);
    r.insert(r.begin(), c);
    if (r.size() > kRecentMax)
        r.resize(kRecentMax);
}

/** The lead selected mesh, or kNoNode. */
NodeId leadPaintMesh()
{
    scene::Graph& g = scene::Graph::current();
    const auto& sel = g.selection();
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().isA("mesh") && !n->type().isA("lattice"))
            return n->id();
    return kNoNode;
}

bool sameGeometry(const geo::Mesh& a, const geo::Mesh& b)
{
    if (a.pointCapacity() != b.pointCapacity() || a.cornerCapacity() != b.cornerCapacity() || a.faceCapacity() != b.faceCapacity())
        return false;
    auto pa = a.positions(), pb = b.positions();
    return std::memcmp(pa.data(), pb.data(), pa.size_bytes()) == 0;
}

/** A small square button showing a color. */
class Swatch final : public QToolButton
{
public:
    Swatch(QWidget* parent, int size) : QToolButton(parent)
    {
        setFixedSize(size, size);
        setAutoRaise(false);
    }
    void setColor(const glm::vec4& linear)
    {
        mColor = linear;
        QColor c = vertexColorSwatch(linear);
        setProperty("lilaRawColors", true);
        setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #1c1c1c; border-radius: 2px; }"
                              "QToolButton:hover { border: 1px solid #d0d0d0; }")
                          .arg(c.name()));
        setToolTip(QString("Linear %1, %2, %3, alpha %4").arg(linear.r, 0, 'f', 3).arg(linear.g, 0, 'f', 3).arg(linear.b, 0, 'f', 3).arg(linear.a, 0, 'f', 3));
    }
    const glm::vec4& color() const { return mColor; }

private:
    glm::vec4 mColor{1.0f};
};

class VertexPaintTool;
VertexPaintTool* gTool = nullptr;

/** Floating tool settings. Every change lands in settings() and is saved. */
class VertexPaintPanel final : public QWidget
{
public:
    std::function<void()> onClosed, onEyedropper, onChanged;
    std::function<NodeId()> target;

    explicit VertexPaintPanel(QWidget* parent) : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Paint Vertex Colors");
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(scroll);
        auto* body = new QWidget(scroll);
        scroll->setWidget(body);
        auto* col = new QVBoxLayout(body);
        col->setSpacing(6);

        // Color sets.
        mTarget = new QLabel(body);
        col->addWidget(mTarget);
        auto* setRow = new QHBoxLayout;
        mSets = new QComboBox(body);
        mSets->setToolTip("Current color set: the one shown and painted. The first set is the vertex color Unreal imports.");
        setRow->addWidget(mSets, 1);
        auto* setMenuButton = new QToolButton(body);
        setMenuButton->setIcon(icon("dots-vertical"));
        setMenuButton->setToolTip("New, rename, copy, delete, reorder and precision of color sets");
        setMenuButton->setPopupMode(QToolButton::InstantPopup);
        auto* setMenu = new QMenu(setMenuButton);
        setMenu->addAction(icon("plus"), "New Color Set", this, [this] { setOp("create"); });
        setMenu->addAction(icon("pencil"), "Rename...", this, [this] { setOp("rename"); });
        setMenu->addAction(icon("copy"), "Duplicate", this, [this] { setOp("copy"); });
        setMenu->addAction(icon("arrow-bar-to-up"), "Make Primary (exported first)", this, [this] { setOp("primary"); });
        setMenu->addAction(icon("trash"), "Delete", this, [this] { setOp("delete"); });
        setMenu->addSeparator();
        mHalf = setMenu->addAction("Half Precision");
        mHalf->setCheckable(true);
        connect(mHalf, &QAction::toggled, this, [this](bool on) { setOp(on ? "half" : "float"); });
        setMenuButton->setMenu(setMenu);
        setRow->addWidget(setMenuButton);
        col->addLayout(setRow);
        connect(mSets, &QComboBox::activated, this, [this](int) { setOp("current"); });

        // Colors: primary, secondary, swap, pick, eyedropper.
        auto* colorRow = new QHBoxLayout;
        mPrimary = new Swatch(body, 34);
        mSecondary = new Swatch(body, 22);
        mPrimary->setToolTip("Primary color (click to edit)");
        colorRow->addWidget(mPrimary);
        colorRow->addWidget(mSecondary, 0, Qt::AlignBottom);
        auto* swap = new QToolButton(body);
        swap->setIcon(icon("arrows-exchange"));
        swap->setToolTip("Swap primary and secondary (X)");
        colorRow->addWidget(swap);
        auto* eyedrop = new QToolButton(body);
        eyedrop->setIcon(icon("color-picker"));
        eyedrop->setToolTip("Pick a color from the mesh (or hold I and click)");
        colorRow->addWidget(eyedrop);
        colorRow->addStretch(1);
        col->addLayout(colorRow);
        connect(mPrimary, &QToolButton::clicked, this, [this] { editColor(true); });
        connect(mSecondary, &QToolButton::clicked, this, [this] { editColor(false); });
        connect(swap, &QToolButton::clicked, this, [this] {
            std::swap(vertexPaintSettings().color, vertexPaintSettings().secondary);
            changed();
        });
        connect(eyedrop, &QToolButton::clicked, this, [this] {
            if (onEyedropper)
                onEyedropper();
        });

        // Linear RGBA of the primary.
        auto* rgba = new QHBoxLayout;
        for (int k = 0; k < 4; ++k)
        {
            mValue[k] = new QDoubleSpinBox(body);
            mValue[k]->setRange(0.0, 16.0);
            mValue[k]->setDecimals(3);
            mValue[k]->setSingleStep(0.05);
            mValue[k]->setPrefix(QString("RGBA"[k]) + " ");
            mValue[k]->setToolTip("Primary color, linear (what the mesh stores and Unreal reads)");
            rgba->addWidget(mValue[k]);
            connect(mValue[k], &QDoubleSpinBox::valueChanged, this, [this, k](double v) {
                vertexPaintSettings().color[k] = float(v);
                changed();
            });
        }
        col->addLayout(rgba);

        // Swatches and recent colors.
        auto* swGroup = new QGroupBox("Swatches", body);
        auto* swCol = new QVBoxLayout(swGroup);
        mSwatchHost = new QWidget(swGroup);
        mSwatchLayout = new FlowLayout(3);
        mSwatchHost->setLayout(mSwatchLayout);
        swCol->addWidget(mSwatchHost);
        auto* recentLabel = new QLabel("Recent", swGroup);
        swCol->addWidget(recentLabel);
        mRecentHost = new QWidget(swGroup);
        mRecentLayout = new FlowLayout(3);
        mRecentHost->setLayout(mRecentLayout);
        swCol->addWidget(mRecentHost);
        col->addWidget(swGroup);

        // Brush.
        auto* brush = new QGroupBox("Brush", body);
        auto* form = new QFormLayout(brush);
        mBlend = new QComboBox(brush);
        for (int i = 0; i < int(vcolor::Blend::Count); ++i)
        {
            auto name = vcolor::blendName(vcolor::Blend(i));
            QString label = QString::fromUtf8(name.data(), qsizetype(name.size()));
            label[0] = label[0].toUpper();
            mBlend->addItem(label, QString::fromUtf8(name.data(), qsizetype(name.size())));
        }
        form->addRow("Blend", mBlend);
        mStrength = new QDoubleSpinBox(brush);
        mStrength->setRange(0.0, 1.0);
        mStrength->setSingleStep(0.05);
        mStrength->setDecimals(3);
        mStrength->setToolTip("Most one stroke changes a point; going over a spot again in the same stroke doesn't build up (M + drag)");
        form->addRow("Strength", mStrength);
        mRadius = new QSpinBox(brush);
        mRadius->setRange(2, 2000);
        mRadius->setSuffix(" px");
        mRadius->setToolTip("Brush radius on screen (B + drag, [ and ])");
        form->addRow("Radius", mRadius);
        mSpacing = new QDoubleSpinBox(brush);
        mSpacing->setRange(0.02, 2.0);
        mSpacing->setSingleStep(0.05);
        mSpacing->setToolTip("Distance between dabs along a stroke, as a fraction of the radius");
        form->addRow("Spacing", mSpacing);
        mFalloff = new QComboBox(brush);
        mFalloff->setToolTip("Falloff curve, shared with soft selection's presets");
        form->addRow("Falloff", mFalloff);
        auto* chRow = new QHBoxLayout;
        for (int k = 0; k < 4; ++k)
        {
            mChannel[k] = new QToolButton(brush);
            mChannel[k]->setText(QString("RGBA"[k]));
            mChannel[k]->setCheckable(true);
            mChannel[k]->setToolTip("Paint this channel; the others are left alone");
            chRow->addWidget(mChannel[k]);
            connect(mChannel[k], &QToolButton::toggled, this, [this] { channelsChanged(); });
        }
        chRow->addStretch(1);
        form->addRow("Channels", chRow);
        col->addWidget(brush);

        auto* opts = new QGroupBox("Options", body);
        auto* optCol = new QVBoxLayout(opts);
        auto check = [&](const char* label, const char* tip, std::function<bool&()> ref) {
            auto* cb = new QCheckBox(label, opts);
            cb->setToolTip(tip);
            optCol->addWidget(cb);
            connect(cb, &QCheckBox::toggled, this, [this, ref](bool on) {
                ref() = on;
                changed();
            });
            mChecks.push_back({cb, ref});
        };
        check("Front faces only", "Leave points facing away from the view", []() -> bool& { return vertexPaintSettings().frontFacesOnly; });
        check("Paint through", "Reach points hidden behind other parts of the mesh", []() -> bool& { return vertexPaintSettings().paintThrough; });
        check("Mirror (mesh symmetry)", "Mirror strokes across the mesh's symmetry plane when it is on",
              []() -> bool& { return vertexPaintSettings().stroke.mirror; });
        check("Selected components only", "With components selected, only they change", []() -> bool& { return vertexPaintSettings().selectionOnly; });
        col->addWidget(opts);
        mStrokeOptions = new brush::ui::StrokeOptions(body, []() -> brush::BrushSettings& { return vertexPaintSettings().stroke; }, [this] { changed(); });
        col->addWidget(mStrokeOptions);

        // Display.
        auto* disp = new QGroupBox("Display", body);
        auto* dispRow = new QHBoxLayout(disp);
        mDisplay = new QComboBox(disp);
        mDisplay->addItem("Shaded", "shaded");
        mDisplay->addItem("Unlit", "unlit");
        mDisplay->addItem("Hidden", "off");
        mChannelView = new QComboBox(disp);
        for (const char* c : {"rgb", "r", "g", "b", "a"})
            mChannelView->addItem(std::string(c) == "rgb" ? "RGB" : QString(c).toUpper() + " only", c);
        mChannelView->setToolTip("Show one channel as grayscale, the way an Unreal material reads a mask");
        dispRow->addWidget(mDisplay);
        dispRow->addWidget(mChannelView);
        col->addWidget(disp);
        connect(mDisplay, &QComboBox::activated, this, [this](int) {
            actions::run(json("viewport.vertexColors." + mDisplay->currentData().toString().toStdString()));
        });
        connect(mChannelView, &QComboBox::activated, this, [this](int) {
            actions::run(json("viewport.colorChannel." + mChannelView->currentData().toString().toStdString()));
        });

        // Fill and adjust.
        auto* fills = new QGroupBox("Fill and Adjust", body);
        auto* fillGrid = new QVBoxLayout(fills);
        auto buttonRow = [&](std::initializer_list<std::tuple<const char*, const char*, const char*, std::function<void()>>> items) {
            auto* row = new QHBoxLayout;
            for (const auto& [label, iconName, tip, fn] : items)
            {
                auto* b = new QPushButton(icon(iconName), label, fills);
                b->setToolTip(tip);
                connect(b, &QPushButton::clicked, this, fn);
                row->addWidget(b);
            }
            fillGrid->addLayout(row);
        };
        buttonRow({{"Flood", "bucket", "Blend the primary color over the whole mesh", [this] { fill("all"); }},
                   {"Selection", "bucket-droplet", "Blend the primary color over the selected components", [this] { fill("selection"); }},
                   {"Element", "box", "Blend the primary color over whole shells of the selection", [this] { fill("element"); }}});
        auto* gradRow = new QHBoxLayout;
        mGradType = new QComboBox(fills);
        mGradType->addItem("Linear", "linear");
        mGradType->addItem("Radial", "radial");
        mGradAxis = new QComboBox(fills);
        for (const char* a : {"y", "x", "z"})
            mGradAxis->addItem(QString(a).toUpper(), a);
        auto* grad = new QPushButton(icon("gradienter"), "Gradient", fills);
        grad->setToolTip("Secondary to primary across the selection (or mesh) along the axis. Or hold G and drag in the viewport.");
        gradRow->addWidget(mGradType);
        gradRow->addWidget(mGradAxis);
        gradRow->addWidget(grad, 1);
        fillGrid->addLayout(gradRow);
        connect(grad, &QPushButton::clicked, this, [this] {
            json call = targetCall("vertexColorGradient");
            call["type"] = mGradType->currentData().toString().toStdString();
            call["axis"] = mGradAxis->currentData().toString().toStdString();
            call["colorA"] = colorJson(vertexPaintSettings().secondary);
            call["colorB"] = colorJson(vertexPaintSettings().color);
            runUi("vertexColorGradient", call);
        });
        buttonRow({{"Invert", "contrast", "Invert the painted channels", [this] { runUi("vertexColorAdjust", targetCall("vertexColorAdjust")); }},
                   {"Blur", "blur", "Smooth the painted channels across edges", [this] {
                        json c = targetCall("vertexColorBlur");
                        c["iterations"] = 2;
                        runUi("vertexColorBlur", c);
                    }},
                   {"Sharpen", "focus-2", "Sharpen the painted channels", [this] {
                        json c = targetCall("vertexColorBlur");
                        c["strength"] = -0.5;
                        runUi("vertexColorBlur", c);
                    }}});
        auto* levelsRow = new QHBoxLayout;
        for (int k = 0; k < 3; ++k)
        {
            mLevels[k] = new QDoubleSpinBox(fills);
            mLevels[k]->setRange(k == 2 ? 0.05 : 0.0, k == 2 ? 10.0 : 1.0);
            mLevels[k]->setSingleStep(0.05);
            mLevels[k]->setValue(k == 1 ? 1.0 : k == 2 ? 1.0 : 0.0);
            mLevels[k]->setPrefix(k == 0 ? "in " : k == 1 ? "to " : "gamma ");
            levelsRow->addWidget(mLevels[k]);
        }
        auto* lv = new QPushButton("Levels", fills);
        lv->setToolTip("Remap the painted channels: input black and white points, then gamma");
        levelsRow->addWidget(lv);
        fillGrid->addLayout(levelsRow);
        connect(lv, &QPushButton::clicked, this, [this] {
            json c = targetCall("vertexColorAdjust");
            c["operation"] = "levels";
            c["inBlack"] = mLevels[0]->value();
            c["inWhite"] = mLevels[1]->value();
            c["gamma"] = mLevels[2]->value();
            runUi("vertexColorAdjust", c);
        });
        col->addWidget(fills);

        // Bakes.
        auto* bakes = new QGroupBox("Bake to Channels", body);
        auto* bakeRow = new QHBoxLayout(bakes);
        mBakeType = new QComboBox(bakes);
        mBakeType->addItem("Ambient Occlusion", "ao");
        mBakeType->addItem("Cavity", "cavity");
        mBakeType->addItem("Curvature", "curvature");
        mBakeType->addItem("Height Gradient", "height");
        mBakeType->setToolTip("Written into the checked channels; AO comes from the mesh itself");
        mBakeRays = new QSpinBox(bakes);
        mBakeRays->setRange(4, 1024);
        mBakeRays->setValue(64);
        mBakeRays->setSuffix(" rays");
        auto* bake = new QPushButton(icon("sparkles"), "Bake", bakes);
        bakeRow->addWidget(mBakeType, 1);
        bakeRow->addWidget(mBakeRays);
        bakeRow->addWidget(bake);
        connect(bake, &QPushButton::clicked, this, [this] {
            json c = targetCall("vertexColorBake");
            c["type"] = mBakeType->currentData().toString().toStdString();
            c["rays"] = mBakeRays->value();
            runUi("vertexColorBake", c);
        });
        col->addWidget(bakes);
        col->addStretch(1);

        connect(mBlend, &QComboBox::activated, this, [this](int) {
            vertexPaintSettings().blend = mBlend->currentData().toString().toStdString();
            changed();
        });
        connect(mStrength, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            vertexPaintSettings().stroke.strength = v;
            changed();
        });
        connect(mRadius, &QSpinBox::valueChanged, this, [this](int v) {
            vertexPaintSettings().stroke.radius = v;
            changed();
        });
        connect(mSpacing, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            vertexPaintSettings().stroke.spacing = v;
            changed();
        });
        connect(mFalloff, &QComboBox::activated, this, [this](int) {
            vertexPaintSettings().stroke.falloff = mFalloff->currentText().toStdString();
            changed();
        });

        // Wide enough for the body, so only the vertical scroll bar ever shows.
        resize(std::max(330, body->minimumSizeHint().width() + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 4), 760);
        if (parent)
            move(parent->mapToGlobal(QPoint(60, 90)));
        syncFromSettings();
    }

    void syncFromSettings()
    {
        const VertexPaintSettings& s = vertexPaintSettings();
        mPrimary->setColor(s.color);
        mSecondary->setColor(s.secondary);
        for (int k = 0; k < 4; ++k)
        {
            QSignalBlocker b(mValue[k]);
            mValue[k]->setValue(s.color[k]);
        }
        {
            QSignalBlocker b1(mBlend), b2(mStrength), b3(mRadius), b4(mSpacing), b5(mFalloff);
            mBlend->setCurrentIndex(std::max(0, mBlend->findData(QString::fromStdString(s.blend))));
            mStrength->setValue(s.stroke.strength);
            mRadius->setValue(int(std::lround(s.stroke.radius)));
            mSpacing->setValue(s.stroke.spacing);
            mFalloff->clear();
            for (const auto& p : scene::softSelect::builtinPresets())
                mFalloff->addItem(QString::fromStdString(p.name));
            for (const auto& p : scene::softSelect::userPresets())
                mFalloff->addItem(QString::fromStdString(p.name));
            mFalloff->setCurrentText(QString::fromStdString(s.stroke.falloff));
        }
        uint8_t ch = vcolor::channelsFromString(s.channels);
        for (int k = 0; k < 4; ++k)
        {
            QSignalBlocker b(mChannel[k]);
            mChannel[k]->setChecked(ch & (1 << k));
        }
        for (auto& [cb, ref] : mChecks)
        {
            QSignalBlocker b(cb);
            cb->setChecked(ref());
        }
        if (mStrokeOptions)
            mStrokeOptions->sync();
        {
            QSignalBlocker b1(mDisplay), b2(mChannelView);
            std::string mode = PrefStore::get().value<std::string>("general", viewportDisplay::kVertexColors, "off");
            mDisplay->setCurrentIndex(std::max(0, mDisplay->findData(QString::fromStdString(mode))));
            std::string channel = PrefStore::get().value<std::string>("general", viewportDisplay::kColorChannel, "rgb");
            mChannelView->setCurrentIndex(std::max(0, mChannelView->findData(QString::fromStdString(channel))));
        }
        rebuildSwatches();
    }

    void syncTarget()
    {
        NodeId id = target ? target() : kNoNode;
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = g.node(id);
        QSignalBlocker block(mSets);
        mSets->clear();
        if (!n)
        {
            mTarget->setText("Select a mesh");
            return;
        }
        mTarget->setText(QString::fromStdString(n->name()));
        mTarget->setToolTip(QString::fromStdString(n->fullPath()));
        MeshPtr m = scene::modifiers::cage(g, id);
        if (!m)
            return;
        auto sets = geo::colors::sets(*m);
        for (const std::string& s : sets)
            mSets->addItem(QString::fromStdString(s));
        if (sets.empty())
            mSets->addItem("(none: painting adds colorSet)");
        std::string cur = geo::colors::current(*m);
        mSets->setCurrentText(QString::fromStdString(cur));
        QSignalBlocker b(mHalf);
        mHalf->setChecked(!cur.empty() && geo::colors::isHalf(*m, cur));
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        if (onClosed)
            onClosed();
        QWidget::closeEvent(e);
    }

private:
    void changed()
    {
        saveVertexPaintSettings();
        syncFromSettings();
        if (onChanged)
            onChanged();
    }

    void channelsChanged()
    {
        uint8_t ch = 0;
        for (int k = 0; k < 4; ++k)
            if (mChannel[k]->isChecked())
                ch |= uint8_t(1 << k);
        vertexPaintSettings().channels = ch ? vcolor::channelsToString(ch) : "rgba";
        changed();
    }

    std::string meshName() const
    {
        scene::Node* n = scene::Graph::current().node(target ? target() : kNoNode);
        return n ? n->path() : std::string();
    }

    json targetCall(const char*) const
    {
        return {{"args", {meshName()}}, {"channels", vertexPaintSettings().channels}};
    }

    void fill(const char* which)
    {
        json c = targetCall("vertexColorFill");
        c["target"] = which;
        c["color"] = colorJson(vertexPaintSettings().color);
        std::string blend = vertexPaintSettings().blend;
        c["blend"] = blend;
        c["opacity"] = vertexPaintSettings().stroke.strength;
        runUi("vertexColorFill", c);
        pushRecent(vertexPaintSettings().color);
        changed();
    }

    void setOp(const std::string& op)
    {
        std::string mesh = meshName();
        if (mesh.empty())
            return;
        std::string cur = mSets->currentText().toStdString();
        json c = {{"args", {mesh}}};
        if (op == "create")
            c["create"] = true;
        else if (op == "current")
            c.update({{"colorSet", cur}, {"current", true}});
        else if (op == "delete")
            c.update({{"colorSet", cur}, {"delete", true}});
        else if (op == "copy")
            c.update({{"colorSet", cur}, {"copy", true}});
        else if (op == "primary")
            c.update({{"colorSet", cur}, {"moveTo", 0}});
        else if (op == "half" || op == "float")
            c.update({{"colorSet", cur}, {"precision", op}});
        else if (op == "rename")
        {
            bool ok = false;
            QString to = QInputDialog::getText(this, "Rename Color Set", "Name", QLineEdit::Normal, QString::fromStdString(cur), &ok);
            if (!ok || to.isEmpty())
                return;
            c.update({{"colorSet", cur}, {"rename", true}, {"newColorSet", to.toStdString()}});
        }
        runUi("polyColorSet", c);
        syncTarget();
    }

    void editColor(bool primary)
    {
        auto apply = [this, primary](const QColor& picked) {
            glm::vec4& c = primary ? vertexPaintSettings().color : vertexPaintSettings().secondary;
            c = vcolor::srgbToLinear(glm::vec4(float(picked.redF()), float(picked.greenF()), float(picked.blueF()), float(picked.alphaF())));
            changed();
        };
        const glm::vec4 c = primary ? vertexPaintSettings().color : vertexPaintSettings().secondary;
        if (QColor picked = colorPopup::getColor(vertexColorSwatch(c), this, primary ? "Primary Color" : "Secondary Color", true, apply); picked.isValid())
            apply(picked);
    }

    void rebuildSwatches()
    {
        auto clear = [](QLayout* l) {
            while (QLayoutItem* it = l->takeAt(0))
            {
                delete it->widget();
                delete it;
            }
        };
        clear(mSwatchLayout);
        clear(mRecentLayout);
        VertexPaintSettings& s = vertexPaintSettings();
        for (size_t i = 0; i < s.swatches.size(); ++i)
        {
            auto* sw = new Swatch(mSwatchHost, 20);
            sw->setColor(s.swatches[i]);
            sw->setToolTip(sw->toolTip() + "\nClick: use. Right click: remove.");
            sw->setContextMenuPolicy(Qt::CustomContextMenu);
            glm::vec4 c = s.swatches[i];
            connect(sw, &QToolButton::clicked, this, [this, c] {
                QTimer::singleShot(0, this, [this, c] {
                    vertexPaintSettings().color = c;
                    changed();
                });
            });
            connect(sw, &QWidget::customContextMenuRequested, this, [this, c] {
                QTimer::singleShot(0, this, [this, c] {
                    std::erase(vertexPaintSettings().swatches, c);
                    changed();
                });
            });
            mSwatchLayout->addWidget(sw);
        }
        auto* add = new QToolButton(mSwatchHost);
        add->setIcon(icon("plus"));
        add->setFixedSize(20, 20);
        add->setToolTip("Save the primary color as a swatch");
        connect(add, &QToolButton::clicked, this, [this] {
            QTimer::singleShot(0, this, [this] {
                auto& sw = vertexPaintSettings().swatches;
                if (std::find(sw.begin(), sw.end(), vertexPaintSettings().color) == sw.end())
                    sw.push_back(vertexPaintSettings().color);
                changed();
            });
        });
        mSwatchLayout->addWidget(add);
        for (const glm::vec4& c : s.recent)
        {
            auto* sw = new Swatch(mRecentHost, 16);
            sw->setColor(c);
            connect(sw, &QToolButton::clicked, this, [this, c] {
                QTimer::singleShot(0, this, [this, c] {
                    vertexPaintSettings().color = c;
                    changed();
                });
            });
            mRecentLayout->addWidget(sw);
        }
    }

    QLabel* mTarget = nullptr;
    QComboBox* mSets = nullptr;
    QAction* mHalf = nullptr;
    Swatch* mPrimary = nullptr;
    Swatch* mSecondary = nullptr;
    QDoubleSpinBox* mValue[4]{};
    QWidget* mSwatchHost = nullptr;
    QLayout* mSwatchLayout = nullptr;
    QWidget* mRecentHost = nullptr;
    QLayout* mRecentLayout = nullptr;
    QComboBox* mBlend = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QSpinBox* mRadius = nullptr;
    QDoubleSpinBox* mSpacing = nullptr;
    QComboBox* mFalloff = nullptr;
    QToolButton* mChannel[4]{};
    std::vector<std::pair<QCheckBox*, std::function<bool&()>>> mChecks;
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
    QComboBox* mDisplay = nullptr;
    QComboBox* mChannelView = nullptr;
    QComboBox* mGradType = nullptr;
    QComboBox* mGradAxis = nullptr;
    QDoubleSpinBox* mLevels[3]{};
    QComboBox* mBakeType = nullptr;
    QSpinBox* mBakeRays = nullptr;
};

class VertexPaintTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "vertexPaint"; }

    void activate() override
    {
        gTool = this;
        mAlive = std::make_shared<bool>(true);
        mControls.activate({Qt::Key_I, Qt::Key_G, Qt::Key_X}, [this](int k, Qt::KeyboardModifiers, bool down) { return key(k, down); });
        mPanel = new VertexPaintPanel(mainWindow());
        mPanel->target = [this] { return mMesh; };
        mPanel->onChanged = [] { viewportTools::requestRedraw(); };
        mPanel->onEyedropper = [this] { mEyedropArmed = true; };
        mPanel->onClosed = [] {
            QTimer::singleShot(0, [] {
                if (viewportTools::active() && std::string(viewportTools::active()->name()) == "vertexPaint")
                    viewportTools::set(nullptr);
            });
        };
        mPanel->show();
        std::weak_ptr<bool> alive = mAlive;
        mListener = scene::Graph::current().addListener([this, alive](scene::Change, NodeId) {
            if (mStroking || mRefreshQueued)
                return;
            mRefreshQueued = true;
            QTimer::singleShot(0, [this, alive] {
                if (alive.expired())
                    return;
                mRefreshQueued = false;
                retarget();
            });
        });
        retarget();
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        scene::Graph::current().removeListener(mListener);
        mControls.deactivate();
        mAlive.reset();
        if (mPanel)
        {
            mPanel->onClosed = nullptr;
            mPanel->deleteLater();
        }
        gTool = nullptr;
        viewportTools::requestRedraw();
    }

    /** The tool's own held keys; true when the key is the tool's. */
    bool key(int k, bool down)
    {
        if (k == Qt::Key_X)
        {
            if (down)
            {
                std::swap(vertexPaintSettings().color, vertexPaintSettings().secondary);
                saveVertexPaintSettings();
                if (mPanel)
                    mPanel->syncFromSettings();
                viewportTools::requestRedraw();
            }
            return true;
        }
        if (k == Qt::Key_I)
        {
            mEyedropHeld = down;
            return true;
        }
        if (k == Qt::Key_G)
        {
            mGradientHeld = down;
            return true;
        }
        return false;
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mMesh == kNoNode)
            return true;
        Hit hit;
        if (mEyedropHeld || mEyedropArmed)
        {
            mEyedropArmed = false;
            if (mPaint && surfaceHit(e->position(), view, hit))
            {
                vertexPaintSettings().color = mPaint->sample(hit.bvh);
                pushRecent(vertexPaintSettings().color);
                saveVertexPaintSettings();
                if (mPanel)
                    mPanel->syncFromSettings();
            }
            return true;
        }
        if (mGradientHeld)
        {
            if (surfaceHit(e->position(), view, hit))
            {
                mGradientFrom = hit.world;
                mGradientTo = hit.world;
                mGradientDrag = true;
            }
            return true;
        }
        if (!ensurePaint())
            return true;
        const bool line = vertexPaintSettings().stroke.mode == brush::StrokeMode::Line || vertexPaintSettings().stroke.mode == brush::StrokeMode::Curve;
        if (!line && !surfaceHit(e->position(), view, hit))
            return false;
        beginStroke(e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mGradientDrag)
        {
            Hit hit;
            if (surfaceHit(e->position(), view, hit))
                mGradientTo = hit.world;
            viewportTools::requestRedraw();
            return true;
        }
        if (mStroking)
        {
            mStroke.move(mControls.input(e));
            viewportTools::requestRedraw();
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mGradientDrag)
        {
            mGradientDrag = false;
            scene::Node* n = scene::Graph::current().node(mMesh);
            if (n && glm::length(mGradientTo - mGradientFrom) > 1e-9)
                runUi("vertexColorGradient", {{"args", {n->path()}},
                                                   {"type", (e->modifiers() & Qt::ShiftModifier) ? "radial" : "linear"},
                                                   {"start", vecJson(mGradientFrom)},
                                                   {"end", vecJson(mGradientTo)},
                                                   {"colorA", colorJson(vertexPaintSettings().secondary)},
                                                   {"colorB", colorJson(vertexPaintSettings().color)},
                                                   {"channels", vertexPaintSettings().channels}});
            viewportTools::requestRedraw();
            return true;
        }
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        switch (e->key())
        {
        case Qt::Key_Escape:
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        default: return false;
        }
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        scene::Node* n = g.node(mMesh);
        if (!n)
        {
            drawToolStatus(p, view, {.tool = "Vertex Paint", .prompt = "select a mesh"});
            return;
        }
        substituteLiveMesh(s, *n);
        const VertexPaintSettings& st = vertexPaintSettings();
        const bool blur = ((mStroking ? mStrokeMods : QApplication::keyboardModifiers()) & Qt::ShiftModifier) != 0;
        const std::string blend = blur ? std::string(vcolor::blendName(vcolor::Blend::Blur)) : st.blend;
        drawToolStatus(p, view,
                       {.tool = "Vertex Paint",
                        .subject = QString::fromStdString(n->path()),
                        .mode = QString::fromStdString(blend),
                        .fields = {{"set", QString::fromStdString(mSet.empty() ? "(new set)" : mSet)},
                                   {"channels", QString::fromStdString(st.channels).toUpper()},
                                   {"radius", QString("%1 px").arg(int(std::lround(st.stroke.radius)))},
                                   {"strength", QString::number(st.stroke.strength, 'f', 2)}},
                        .keys = {{"Shift", "blur"}},
                        .symmetry = ToolSymmetry{mSymmetryOn}});
        if (mGradientDrag)
        {
            auto a = view.project(mGradientFrom), b = view.project(mGradientTo);
            if (a && b)
            {
                p.line(*a, *b, 2.0f, {0.95f, 0.95f, 0.95f, 1.0f});
                p.disc(*a, 5.0f, swatchVec(st.secondary));
                p.disc(*b, 5.0f, swatchVec(st.color));
            }
            return;
        }

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        const glm::vec4 rim(0.95f, 0.95f, 0.95f, 1.0f);
        Hit hit;
        std::optional<brush::ui::CursorSurface> surface;
        if (mPaint && surfaceHit(at, view, hit))
            surface = brush::ui::CursorSurface{hit.world, hit.worldNormal};
        brush::ui::drawCursor(p, view, st.stroke, at, surface,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .stroke = &mStroke});
        // The brush color beside the cursor; the eyedropper shows a ring instead.
        QPointF chip = at + QPointF(14, 14);
        if (mEyedropHeld || mEyedropArmed)
            p.polyline({chip + QPointF(-6, -6), chip + QPointF(6, -6), chip + QPointF(6, 6), chip + QPointF(-6, 6)}, 1.5f, rim, true);
        else
        {
            p.disc(chip, 7.0f, {0.1f, 0.1f, 0.1f, 1.0f});
            p.disc(chip, 6.0f, swatchVec(st.color));
        }
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }

private:
    struct Hit
    {
        geo::BvhHit bvh;
        glm::dvec3 local{0.0}, world{0.0}, worldNormal{0.0};
        glm::dvec3 viewDir{0.0}, eye{0.0}; // local
    };

    static glm::vec4 swatchVec(const glm::vec4& linear)
    {
        glm::vec4 c = vcolor::linearToSrgb(glm::clamp(linear, 0.0f, 1.0f));
        return {c.r, c.g, c.b, 1.0f};
    }

    glm::dmat4 world() const { return scene::Graph::current().node(mMesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix); }

    bool surfaceHit(QPointF px, const ViewInfo& view, Hit& out) const
    {
        if (!mPaint || !scene::Graph::current().node(mMesh))
            return false;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dmat4 w = world(), inv = glm::inverse(w);
        glm::dvec3 lo(inv * glm::dvec4(o, 1.0)), ld(inv * glm::dvec4(d, 0.0));
        if (!mPaint->raycast(lo, ld, out.bvh))
            return false;
        out.local = out.bvh.point;
        out.world = glm::dvec3(w * glm::dvec4(out.local, 1.0));
        const auto& tris = mPaint->bvh().triangles();
        const geo::Mesh& m = mPaint->mesh();
        glm::dvec3 a = m.position(m.cornerPoint(tris.corners[size_t(out.bvh.triangle) * 3]));
        glm::dvec3 b = m.position(m.cornerPoint(tris.corners[size_t(out.bvh.triangle) * 3 + 1]));
        glm::dvec3 c = m.position(m.cornerPoint(tris.corners[size_t(out.bvh.triangle) * 3 + 2]));
        glm::dvec3 n = glm::transpose(glm::inverse(glm::dmat3(w))) * glm::cross(b - a, c - a);
        out.worldNormal = glm::length(n) > 1e-18 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
        out.viewDir = glm::normalize(ld);
        // Orthographic views have no eye point: occlusion is tested back along the view direction instead.
        out.eye = view.ortho ? out.local - out.viewDir * 1e7 : glm::dvec3(inv * glm::dvec4(view.eye, 1.0));
        return true;
    }

    /** The mesh to paint: its editable base (the cage under modifiers). */
    MeshPtr cageOf(NodeId id) const { return scene::modifiers::cage(scene::Graph::current(), id); }

    void retarget()
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        NodeId id = leadPaintMesh();
        if (id == kNoNode && mMesh != kNoNode && g.node(mMesh))
            id = mMesh;
        if (id != mMesh)
        {
            mPaint.reset();
            mLive.reset();
            mMesh = id;
        }
        if (mPanel)
            mPanel->syncTarget();
        viewportTools::requestRedraw();
        if (id == kNoNode)
            return;
        MeshPtr cage = cageOf(id);
        if (!cage)
        {
            mPaint.reset();
            return;
        }
        std::string set = geo::colors::current(*cage);
        mSet = set;
        if (set.empty())
        {
            mPaint.reset();
            mLive.reset();
            return;
        }
        if (mPaint && mPaint->meshPtr() == cage && mPaint->set() == set)
            return;
        if (mPaint && mPaint->set() == set && sameGeometry(mPaint->mesh(), *cage))
        {
            // Same surface, other colors (undo, a fill, a command): take them over without rebuilding the brush data.
            mPaint->rebase(cage);
            refreshLiveColors(true);
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        mPaint = std::make_unique<vcolor::PaintMesh>(cage, set);
        mSymmetryFor = "-";
        mLive.reset();
        QApplication::restoreOverrideCursor();
    }

    /** Makes sure the mesh has a color set to paint (adding one is its own undo step). */
    bool ensurePaint()
    {
        if (mPaint)
            return true;
        scene::Node* n = scene::Graph::current().node(mMesh);
        if (!n)
            return false;
        MeshPtr cage = cageOf(mMesh);
        if (cage && geo::colors::current(*cage).empty())
            runUi("polyColorSet", {{"args", {n->path()}}, {"create", true}, {"colorSet", "colorSet"}});
        retarget();
        return mPaint != nullptr;
    }

    /** The fast preview needs the drawn mesh to be the painted base itself: no modifiers or smooth preview between them. */
    bool directDraw() const
    {
        scene::Node* n = scene::Graph::current().node(mMesh);
        return n && mPaint && n->as<MeshPtr>(scene::MeshNode::kOutMesh) == mPaint->meshPtr() &&
               n->as<int64_t>(scene::MeshNode::kSmoothPreview) == 0;
    }

    void ensureLive()
    {
        if (!directDraw())
        {
            mLive.reset();
            return;
        }
        if (mLive && mLiveFor == mPaint->meshPtr())
            return;
        auto built = renderMeshFor(mPaint->meshPtr());
        auto pick = pickDataFor(mPaint->meshPtr());
        if (!built || !pick || pick->cornerVertex.size() != mPaint->mesh().cornerCapacity())
            return;
        auto live = std::make_shared<render::RenderMesh>();
        live->dynamic = std::make_shared<render::DynamicVertices>();
        live->dynamic->vertices.assign(built->vertices.begin(), built->vertices.end());
        live->indices = built->indices;
        live->sections = built->sections;
        live->edgePoints = built->edgePoints;
        live->edgeIndices = built->edgeIndices;
        live->edgeMask = built->edgeMask;
        live->boundsMin = built->boundsMin;
        live->boundsMax = built->boundsMax;
        mLive = std::move(live);
        mLiveBuilt = built;
        mLiveFor = mPaint->meshPtr();
        mCornerVertex.assign(pick->cornerVertex.begin(), pick->cornerVertex.end());
        refreshLiveColors(true);
    }

    /** Copies changed (or all) corner colors into the live vertices. */
    void refreshLiveColors(bool all)
    {
        if (!mPaint)
            return;
        std::vector<geo::Index> changed = mPaint->takeChanged();
        if (!mLive || mCornerVertex.empty())
            return;
        render::DynamicVertices& dv = *mLive->dynamic;
        std::lock_guard lock(dv.mutex);
        auto colors = mPaint->colors();
        if (all)
        {
            tbb::parallel_for(size_t(0), mCornerVertex.size(), [&](size_t c) {
                if (geo::Index v = mCornerVertex[c]; v != geo::kInvalid && v < dv.vertices.size())
                    dv.vertices[v].color = colors[c];
            });
            dv.markAllDirty();
        }
        else
            for (geo::Index c : changed)
                if (geo::Index v = mCornerVertex[c]; v != geo::kInvalid && v < dv.vertices.size())
                {
                    render::MeshVertex vert = dv.vertices[v];
                    vert.color = colors[c];
                    dv.write(v, vert);
                }
        viewportTools::requestRedraw();
    }

    /** Draws the live copy in place of the mesh's own render copy, its colors shown even when the display pref hides them. */
    void substituteLiveMesh(render::FrameState& s, scene::Node& n)
    {
        ensureLive();
        auto shown = renderMeshFor(smoothedFor(n.as<MeshPtr>(scene::MeshNode::kOutMesh), int(n.as<int64_t>(scene::MeshNode::kSmoothPreview))));
        for (render::MeshDraw& d : s.meshes.edit())
        {
            if (!d.mesh || (d.mesh != shown && d.mesh != mLiveBuilt))
                continue;
            if (mLive && (d.flags & render::DrawShaded))
                d.mesh = mLive;
            if (d.vertexColors == render::VertexColorMode::Off && mPaint)
            {
                d.vertexColors = render::VertexColorMode::Shaded;
                d.colorChannel = viewportDisplay::colorChannel();
            }
        }
    }

    void beginStroke(const QMouseEvent* e, const ViewInfo& view)
    {
        const Qt::KeyboardModifiers mods = e->modifiers();
        mStrokeMods = mods;
        const VertexPaintSettings& st = vertexPaintSettings();
        scene::Node* n = scene::Graph::current().node(mMesh);
        vcolor::BrushSettings b;
        b.blend = (mods & Qt::ShiftModifier) ? vcolor::Blend::Blur : vcolor::blendFromName(st.blend).value_or(vcolor::Blend::Replace);
        b.color = (mods & Qt::ControlModifier) ? st.secondary : st.color;
        b.strength = float(st.stroke.strength);
        b.channels = vcolor::channelsFromString(st.channels);
        b.falloff = st.stroke.resolveFalloff();
        b.frontFacesOnly = st.frontFacesOnly;
        b.paintThrough = st.paintThrough;
        b.radial = st.stroke.radial;
        const std::optional<geo::SymmetrySpec> spec = n ? scene::symmetrySpec(*n) : std::nullopt;
        const std::string key = !spec ? std::string() : spec->radial ? geo::radialLayerName(*spec->radial) : geo::symmetryLayerName(spec->axis);
        if (st.stroke.mirror && key != mSymmetryFor)
        {
            mPaint->setSymmetry(n ? vcolor::symmetryOf(*n, mPaint->mesh()) : geo::SymmetryMap());
            mSymmetryFor = key;
        }
        b.mirror = st.stroke.mirror && !mPaint->symmetry().empty();
        // A mesh with radial symmetry repeats the stroke through its orbits, so the brush's own radial copies would double it.
        if (b.mirror && mPaint->symmetry().isRadial())
            b.radial = {};
        mSymmetryOn = b.mirror;
        std::vector<uint8_t> mask;
        if (st.selectionOnly && scene::components::mode() != scene::ComponentMode::Object)
            if (auto sel = scene::components::pointsOf(mMesh); !sel.empty())
            {
                mask.assign(mPaint->mesh().pointCapacity(), 0);
                for (uint32_t p : sel)
                    if (p < mask.size())
                        mask[p] = 1;
            }
        mPaint->setMask(std::move(mask));
        if (!vcolor::isFilter(b.blend))
            pushRecent(b.color);
        mPaint->beginStroke(b);
        mStroking = true;
        mPreviewClock.start();
        brush::BrushSettings stroke = st.stroke;
        stroke.seed = uint32_t(mPreviewClock.msecsSinceReference());
        const ViewInfo v = view;
        mStroke.begin(stroke, mControls.input(e), [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(QPointF(px.x, px.y), v, out); },
                      *this);
    }

    /** The shared stroke's view of the surface under a pixel, in the mesh's object space. */
    bool surfaceQuery(QPointF px, const ViewInfo& view, brush::SurfaceHit& out) const
    {
        Hit hit;
        if (!surfaceHit(px, view, hit))
            return false;
        const glm::dmat4 w = world();
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        out.center = hit.local;
        out.normal = glm::normalize(glm::transpose(glm::dmat3(w)) * hit.worldNormal);
        out.viewDir = hit.viewDir;
        out.eye = hit.eye;
        out.right = brush::ui::screenRight(view, w);
        out.pixelSize = view.unitsPerPixel(hit.world) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    void dab(const brush::Dab& d) override
    {
        mPaint->dab(d);
        if (mLive)
            refreshLiveColors(false);
        else if (mPreviewClock.elapsed() > 120)
        {
            // Under modifiers the stack has to run to show the stroke, so preview it a few times a second.
            mPreviewClock.restart();
            mPaint->takeChanged();
            mKnown = vcolor::preview(mMesh, mPaint->set(), mPaint->colors());
            if (mKnown)
                mPaint->rebase(mKnown);
            viewportTools::requestRedraw();
        }
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        saveVertexPaintSettings();
        if (mPanel)
            mPanel->syncFromSettings();
        scene::Node* n = scene::Graph::current().node(mMesh);
        if (!mPaint || !n)
            return;
        vcolor::ColorDelta delta = mPaint->endStroke();
        mPaint->takeChanged();
        if (delta.empty())
            return;
        MeshPtr before = mPaint->meshPtr();
        uint64_t token = vcolor::stashLiveStroke(mMesh, mPaint->set(), std::move(delta));
        try
        {
            // Not echoed: a stroke is hundreds of dabs, and repeating it from the script editor is rarely wanted.
            ops::execute("vertexColorStroke", {{"args", {n->path()}}, {"live", int64_t(token)}}, CallSource::Script);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            return;
        }
        MeshPtr after = cageOf(mMesh);
        if (!after)
            return;
        if (mLive)
        {
            // The new snapshot only differs in colors: hand it the live vertices instead of rebuilding its render copy.
            auto live = mLive;
            reuseRenderMesh(before, after, [&](UninitVector<render::MeshVertex>& verts) {
                std::lock_guard lock(live->dynamic->mutex);
                verts.assign(live->dynamic->vertices.begin(), live->dynamic->vertices.end());
            });
            mLiveFor = after;
            mLiveBuilt = renderMeshFor(after);
        }
        mPaint->rebase(after);
    }

    std::shared_ptr<bool> mAlive;
    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return vertexPaintSettings().stroke; },
                                       [this] {
                                           if (mPanel)
                                               mPanel->syncFromSettings();
                                           viewportTools::requestRedraw();
                                       },
                                       [] { saveVertexPaintSettings(); }};
    brush::Stroke mStroke;
    QPointer<VertexPaintPanel> mPanel;
    int mListener = 0;
    bool mRefreshQueued = false;

    NodeId mMesh = kNoNode;
    std::string mSet;
    std::unique_ptr<vcolor::PaintMesh> mPaint;
    std::string mSymmetryFor = "-";
    bool mSymmetryOn = false;
    MeshPtr mKnown;

    std::shared_ptr<render::RenderMesh> mLive;
    std::shared_ptr<const render::RenderMesh> mLiveBuilt;
    MeshPtr mLiveFor;
    std::vector<geo::Index> mCornerVertex;

    bool mStroking = false;
    Qt::KeyboardModifiers mStrokeMods;
    QElapsedTimer mPreviewClock;
    QPointF mHover;

    bool mEyedropHeld = false;
    bool mEyedropArmed = false;
    bool mGradientHeld = false;
    bool mGradientDrag = false;
    glm::dvec3 mGradientFrom{0.0}, mGradientTo{0.0};
};

} // namespace

void startVertexPaint()
{
    viewportTools::set(std::make_unique<VertexPaintTool>());
}

void registerVertexPaintUi()
{
    paintColor::addListener([](const glm::vec3& c) {
        vertexPaintSettings().color = glm::vec4(c, vertexPaintSettings().color.a);
        pushRecent(vertexPaintSettings().color);
        saveVertexPaintSettings();
    });
    actions::add({.id = "vcolor.paint",
                  .label = "Paint Vertex Colors",
                  .category = "Vertex Colors",
                  .run = [] { startVertexPaint(); },
                  .doc = "Paint the selected mesh's current color set: blend modes, channel masks, symmetry, pressure, fills and bakes.",
                  .python = "lila.ui.run_action(\"vcolor.paint\")"});
    auto commandAction = [](const char* id, const char* label, const char* command, json call, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Vertex Colors", .doc = doc, .command = command, .call = std::move(call)});
    };
    commandAction("vcolor.createSet", "Create Color Set", "polyColorSet", {{"create", true}},
                  "Adds a white vertex color set to the selected meshes.");
    commandAction("vcolor.fill", "Fill Vertex Colors", "vertexColorFill", json::object(),
                  "Blends a color over the selected meshes, or their selected components.");
    commandAction("vcolor.invert", "Invert Vertex Colors", "vertexColorAdjust", {{"operation", "invert"}},
                  "Inverts the chosen channels of the current color set.");
    commandAction("vcolor.levels", "Vertex Color Levels", "vertexColorAdjust", {{"operation", "levels"}},
                  "Remaps the chosen channels: input black and white points, gamma, output range.");
    commandAction("vcolor.blur", "Blur Vertex Colors", "vertexColorBlur", json::object(), "Smooths vertex colors across edges.");
    commandAction("vcolor.gradient", "Vertex Color Gradient", "vertexColorGradient", json::object(),
                  "Fills a linear or radial gradient across the mesh or its selected components.");
    commandAction("vcolor.bakeAO", "Bake Ambient Occlusion", "vertexColorBake", {{"type", "ao"}},
                  "Bakes the mesh's own ambient occlusion into vertex color channels.");
    commandAction("vcolor.bakeCavity", "Bake Cavity", "vertexColorBake", {{"type", "cavity"}},
                  "Bakes creases (dark) into vertex color channels, for dirt and edge-wear masks.");
    commandAction("vcolor.bakeCurvature", "Bake Curvature", "vertexColorBake", {{"type", "curvature"}},
                  "Bakes curvature (0.5 flat, convex brighter) into vertex color channels.");
    commandAction("vcolor.bakeHeight", "Bake Height Gradient", "vertexColorBake", {{"type", "height"}},
                  "Bakes a bottom-to-top gradient into vertex color channels.");
}
} // namespace lila::ui
