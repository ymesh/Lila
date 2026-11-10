// The Sculpt Terrain viewport tool: brush strokes on a terrain through Lila's brush core (spacing, pen pressure, falloff,
// mirroring), each kept as one terrainStroke call. A stroke in the final stage (after erosion) re-runs only what follows
// it, since the modifier keeps the steps an edit leaves alone. A base-stage stroke previews in the final stage while it
// is drawn and moves to the base stage, eroded with the rest, when released. Brushes and settings sit in a palette panel;
// Region picks a patch to sculpt with the mesh sculpt brushes (terrainSculptRegion), baked back when sculpting ends.

#include "Analysis.h"
#include "Brushes.h"
#include "Terrain.h"

#include "PanelKit.h"

#include <lila/Actions.h>
#include <lila/Brush.h>
#include <lila/BrushUi.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Node.h>
#include <lila/Panels.h>
#include <lila/Prefs.h>
#include <lila/Sculpt.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <cmath>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
using json = nlohmann::json;

namespace sculpttool_
{
constexpr const char* kPrefix = "/terrainSculpt";
constexpr const char* kPaletteId = "terrain.sculptPalette";

struct BrushButton
{
    SculptOp op;
    const char* icon;
    const char* label;
    const char* tip;
};

constexpr std::array<BrushButton, 12> kBrushes = {{
    {SculptOp::Raise, "arrow-big-up", "Raise", "Raise the ground. Ctrl lowers."},
    {SculptOp::Lower, "arrow-big-down", "Lower", "Lower the ground. Ctrl raises."},
    {SculptOp::Smooth, "blur", "Smooth", "Smooth bumps away. Shift smooths with any brush."},
    {SculptOp::Flatten, "minus", "Flatten", "Level to the height under the press, or to the plane under the stroke."},
    {SculptOp::Noise, "wave-sine", "Noise", "Add rough noise."},
    {SculptOp::Ramp, "trending-up", "Ramp", "Drag from one height to another: a graded ramp between press and release."},
    {SculptOp::Terrace, "stairs", "Terrace", "Step the ground into treads."},
    {SculptOp::Stamp, "circle-dot", "Stamp", "Lay a library shape: peak, crater, dune, rocks, mesa, ridge, volcano."},
    {SculptOp::Erode, "droplet", "Erode", "Erode under the brush."},
    {SculptOp::Uplift, "mountain", "Uplift", "Paint where the ground rises; erosion carves it as it lifts (before erosion)."},
    {SculptOp::Relax, "feather", "Relax", "Even out small bumps, keeping the big shapes."},
    {SculptOp::Mask, "brush", "Mask", "Paint a mask. Ctrl erases."},
}};

struct Settings
{
    SculptOp op = SculptOp::Raise;
    brush::BrushSettings stroke;
    /** Raise, lower, noise and stamp: cm at full strength. */
    double height = 600.0;
    bool afterErosion = true;
    std::string mirror = "off";
    std::string stamp = "peak";
    double angle = 0.0;
    double step = 1000.0;
    double sharpness = 0.8;
    bool plane = false;
    std::string mask = "paint";
    int iterations = 10;
};

Settings& settings()
{
    static Settings s = [] {
        Settings c;
        c.stroke.radiusUnit = brush::RadiusUnit::World;
        c.stroke.worldRadius = 3000.0;
        c.stroke.strength = 0.5;
        c.stroke.loadPrefs(kPrefix);
        const PrefStore& p = PrefStore::get();
        auto key = [](const char* k) { return std::string(kPrefix) + "/" + k; };
        const std::string op = p.value<std::string>("general", key("brush"), "raise");
        for (size_t i = 0; i < sculptOpNames().size(); ++i)
            if (sculptOpNames()[i] == op)
                c.op = SculptOp(i);
        c.height = std::max(1.0, p.value<double>("general", key("height"), c.height));
        c.afterErosion = p.value<bool>("general", key("afterErosion"), c.afterErosion);
        c.mirror = p.value<std::string>("general", key("mirror"), c.mirror);
        c.stamp = p.value<std::string>("general", key("stamp"), c.stamp);
        c.angle = p.value<double>("general", key("angle"), c.angle);
        c.step = std::max(1.0, p.value<double>("general", key("step"), c.step));
        c.sharpness = std::clamp(p.value<double>("general", key("sharpness"), c.sharpness), 0.0, 1.0);
        c.plane = p.value<bool>("general", key("plane"), c.plane);
        c.mask = p.value<std::string>("general", key("mask"), c.mask);
        c.iterations = std::clamp(p.value<int>("general", key("iterations"), c.iterations), 1, 200);
        return c;
    }();
    return s;
}

void syncPalette();

void save()
{
    const Settings& c = settings();
    PrefStore& p = PrefStore::get();
    auto key = [](const char* k) { return std::string(kPrefix) + "/" + k; };
    c.stroke.savePrefs(kPrefix);
    p.set("general", key("brush"), sculptOpNames()[size_t(c.op)]);
    p.set("general", key("height"), c.height);
    p.set("general", key("afterErosion"), c.afterErosion);
    p.set("general", key("mirror"), c.mirror);
    p.set("general", key("stamp"), c.stamp);
    p.set("general", key("angle"), c.angle);
    p.set("general", key("step"), c.step);
    p.set("general", key("sharpness"), c.sharpness);
    p.set("general", key("plane"), c.plane);
    p.set("general", key("mask"), c.mask);
    p.set("general", key("iterations"), c.iterations);
    syncPalette();
    viewportTools::requestRedraw();
}

bool inCm(SculptOp op) { return op == SculptOp::Raise || op == SculptOp::Lower || op == SculptOp::Noise || op == SculptOp::Stamp; }

const BrushButton& buttonOf(SculptOp op)
{
    for (const BrushButton& b : kBrushes)
        if (b.op == op)
            return b;
    return kBrushes[0];
}

std::vector<KeyHint> keyHints()
{
    return {{"Drag", buttonOf(settings().op).label}, {"Shift+drag", "Smooth"}, {"Ctrl+drag", "Invert"}, {"B+drag", "Radius"},
            {"M+drag", "Strength"}, {"[  ]", "Smaller, larger"}, {"Esc", "Exit"}};
}

struct Target
{
    std::string mesh;
    glm::dmat4 world{1.0};
    std::shared_ptr<const Heightfield> h;
};

/** The first selected mesh whose modifiers make a heightfield. */
std::optional<Target> selectedTerrain()
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (gn::isGnModifier(*n))
            if (scene::NodeId m = scene::modifiers::meshOf(g, id); m != scene::kNoNode)
                n = g.node(m);
        if (!n || !n->type().isA("mesh"))
            continue;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (scene::Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()); h && !h->empty())
                    return Target{n->path(), n->as<glm::dmat4>(scene::Transform::kWorldMatrix), std::move(h)};
    }
    return std::nullopt;
}

/** Arms the next click to pick a Sculpt Region patch. */
bool& regionArmed()
{
    static bool armed = false;
    return armed;
}

/** "peak" -> "Peak": a stored name as the palette shows it. */
QString terrainDisplayName(std::string_view name)
{
    QString s = QString::fromUtf8(name.data(), qsizetype(name.size()));
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

/** Mirror values in segment order; a segment's id is its index. */
constexpr std::array<const char*, 4> kMirrors = {"off", "x", "z", "xz"};

class Palette final : public QWidget
{
public:
    explicit Palette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "terrainSculptPalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        std::vector<QWidget*> tiles;
        for (size_t i = 0; i < kBrushes.size(); ++i)
        {
            const BrushButton& def = kBrushes[i];
            auto* b = new QToolButton;
            b->setIcon(icon(def.icon));
            b->setIconSize(QSize(24, 24));
            b->setText(def.label);
            b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            panelkit::setRole(b, "tile");
            b->setToolTip(QString("%1: %2").arg(def.label, def.tip));
            connect(b, &QToolButton::clicked, this, [op = def.op] {
                settings().op = op;
                save();
            });
            tiles.push_back(b);
            mButtons[i] = b;
        }
        v->addWidget(panelkit::section("Brushes", panelkit::grid(tiles, 4, this), this, kSections));

        auto makeForm = [](QWidget* w) {
            auto* f = new QFormLayout(w);
            f->setContentsMargins(0, 0, 0, 0);
            f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
            f->setVerticalSpacing(4);
            return f;
        };
        auto connectSpin = [this](QDoubleSpinBox* s, const char* label, const char* tip, auto&& apply) {
            s->setToolTip(QString("%1: %2").arg(label, tip));
            connect(s, &QDoubleSpinBox::valueChanged, this, [apply](double value) {
                apply(value);
                save();
            });
        };

        auto* brushBox = new QWidget(this);
        auto* brushForm = makeForm(brushBox);
        mRadius = panelkit::valueField(10.0, 1e7, 0, "cm", brushBox);
        connectSpin(mRadius, "Radius", "brush radius  (B+drag, [ ])", [](double v) { settings().stroke.worldRadius = v; });
        brushForm->addRow("Radius", mRadius);
        mStrength = panelkit::valueField(0, 1, 0, {}, brushBox);
        mStrength->setRange(0.0, 1.0);
        mStrength->setDecimals(2);
        mStrength->setSingleStep(0.05);
        connectSpin(mStrength, "Strength", "how much one stroke does  (M+drag)", [](double v) { settings().stroke.strength = v; });
        brushForm->addRow("Strength", mStrength);
        mFalloff = new QComboBox(brushBox);
        for (int i = 0; i < int(brush::Shape::Count); ++i)
        {
            const std::string_view name = brush::shapeName(brush::Shape(i));
            mFalloff->addItem(terrainDisplayName(name), QString::fromUtf8(name.data(), qsizetype(name.size())));
        }
        mFalloff->setToolTip("Falloff: how the brush fades toward its edge");
        connect(mFalloff, &QComboBox::currentIndexChanged, this, [this](int) {
            settings().stroke.falloff = mFalloff->currentData().toString().toStdString();
            save();
        });
        brushForm->addRow("Falloff", mFalloff);
        mMirror = panelkit::segmented(brushBox);
        mMirror.add("Off", "No mirroring", 0);
        mMirror.add("X", "Mirror strokes across the terrain's centre in X", 1);
        mMirror.add("Z", "Mirror strokes across the terrain's centre in Z", 2);
        mMirror.add("XZ", "Mirror strokes into all four quarters", 3);
        connect(mMirror.group, &QButtonGroup::idClicked, this, [](int id) {
            settings().mirror = kMirrors[size_t(std::clamp(id, 0, int(kMirrors.size()) - 1))];
            save();
        });
        brushForm->addRow("Mirror", mMirror.widget);
        v->addWidget(panelkit::section("Brush", brushBox, this, kSections));

        // Only the current brush's own settings show.
        mSettingsBox = new QWidget(this);
        mSettingsForm = makeForm(mSettingsBox);
        mHeight = panelkit::valueField(1.0, 1e6, 0, "cm", mSettingsBox);
        connectSpin(mHeight, "Height", "raise, lower, noise and stamp: change at full strength", [](double v) { settings().height = v; });
        mSettingsForm->addRow("Height", mHeight);
        mStep = panelkit::valueField(1.0, 1e6, 0, "cm", mSettingsBox);
        connectSpin(mStep, "Step", "terrace step height", [](double v) { settings().step = v; });
        mSettingsForm->addRow("Step", mStep);
        mSharpness = panelkit::valueField(0, 1, 0, {}, mSettingsBox);
        mSharpness->setRange(0.0, 1.0);
        mSharpness->setDecimals(2);
        mSharpness->setSingleStep(0.05);
        connectSpin(mSharpness, "Sharpness", "terrace: soft to sheer risers", [](double v) { settings().sharpness = v; });
        mSettingsForm->addRow("Sharpness", mSharpness);
        mStamp = new QComboBox(mSettingsBox);
        for (const std::string& s : stampNames())
            mStamp->addItem(terrainDisplayName(s), QString::fromStdString(s));
        mStamp->setToolTip("Stamp: the library shape to lay");
        connect(mStamp, &QComboBox::currentIndexChanged, this, [this](int) {
            settings().stamp = mStamp->currentData().toString().toStdString();
            save();
        });
        mSettingsForm->addRow("Stamp", mStamp);
        mAngle = panelkit::valueField(-360.0, 360.0, 0, "\xc2\xb0", mSettingsBox);
        connectSpin(mAngle, "Angle", "stamp turn", [](double v) { settings().angle = v; });
        mSettingsForm->addRow("Angle", mAngle);
        mMask = new QComboBox(mSettingsBox);
        mMask->setEditable(true);
        mMask->setToolTip("Mask: the mask to paint; type a new name to make one");
        connect(mMask, &QComboBox::currentTextChanged, this, [](const QString& t) {
            if (!t.isEmpty())
            {
                settings().mask = t.toStdString();
                save();
            }
        });
        mSettingsForm->addRow("Mask", mMask);
        mIterations = panelkit::valueField(0, 1, 0, {}, mSettingsBox);
        mIterations->setRange(1, 200);
        mIterations->setDecimals(0);
        connectSpin(mIterations, "Steps", "erode and relax steps", [](double v) { settings().iterations = int(std::lround(v)); });
        mSettingsForm->addRow("Steps", mIterations);
        mPlane = new QCheckBox("Plane", mSettingsBox);
        mPlane->setToolTip("Plane: flatten to the plane under the stroke instead of one height");
        connect(mPlane, &QCheckBox::clicked, this, [](bool on) {
            settings().plane = on;
            save();
        });
        mSettingsForm->addRow("", mPlane);
        mSettingsSection = panelkit::section("Settings", mSettingsBox, this, kSections);
        v->addWidget(mSettingsSection);

        auto* optionsBox = new QWidget(this);
        auto* options = new QVBoxLayout(optionsBox);
        options->setContentsMargins(0, 0, 0, 0);
        options->setSpacing(4);
        auto check = [&](const char* label, const char* tip, auto&& apply) {
            auto* c = new QCheckBox(label, optionsBox);
            c->setToolTip(QString("%1: %2").arg(label, tip));
            connect(c, &QCheckBox::clicked, this, [apply](bool on) {
                apply(on);
                save();
            });
            options->addWidget(c);
            return c;
        };
        mAfter = check("After Erosion", "strokes stay as drawn; off, erosion shapes them", [](bool on) { settings().afterErosion = on; });
        mKeys = check("Show Keys", "key hints on screen, in every tool", [](bool on) { PrefStore::get().set("general", kShowKeysPref, on); });
        mRegion = new QToolButton(optionsBox);
        mRegion->setText("Sculpt Region");
        mRegion->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        mRegion->setCheckable(true);
        mRegion->setToolTip("Sculpt Region: click the terrain to sculpt a patch twice the brush wide with the mesh sculpt brushes; Exit Sculpt bakes it back");
        panelkit::setRole(mRegion, "push");
        connect(mRegion, &QToolButton::clicked, this, [](bool on) {
            regionArmed() = on;
            viewportTools::requestRedraw();
        });
        options->addWidget(mRegion);
        v->addWidget(panelkit::section("Options", optionsBox, this, kSections));
        v->addStretch();
        sync();
    }

    void sync()
    {
        const Settings& s = settings();
        for (size_t i = 0; i < kBrushes.size(); ++i)
            mButtons[i]->setChecked(kBrushes[i].op == s.op);
        for (auto [box, value] : {std::pair<QDoubleSpinBox*, double>{mRadius, s.stroke.worldRadius}, std::pair<QDoubleSpinBox*, double>{mStrength, s.stroke.strength},
                                  std::pair<QDoubleSpinBox*, double>{mHeight, s.height}, std::pair<QDoubleSpinBox*, double>{mStep, s.step},
                                  std::pair<QDoubleSpinBox*, double>{mSharpness, s.sharpness}, std::pair<QDoubleSpinBox*, double>{mAngle, s.angle},
                                  std::pair<QDoubleSpinBox*, double>{mIterations, double(s.iterations)}})
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        const bool terrace = s.op == SculptOp::Terrace, stamp = s.op == SculptOp::Stamp;
        const std::pair<QWidget*, bool> rows[] = {{mHeight, inCm(s.op)}, {mStep, terrace}, {mSharpness, terrace}, {mStamp, stamp}, {mAngle, stamp},
                                                  {mMask, s.op == SculptOp::Mask}, {mIterations, s.op == SculptOp::Erode || s.op == SculptOp::Relax},
                                                  {mPlane, s.op == SculptOp::Flatten}};
        bool any = false;
        for (auto [field, shown] : rows)
        {
            mSettingsForm->setRowVisible(field, shown);
            any = any || shown;
        }
        mSettingsSection->setVisible(any);
        mAfter->setEnabled(s.op != SculptOp::Uplift);
        {
            const QSignalBlocker b1(mStamp), b2(mMask), b3(mFalloff), b4(mMirror.group);
            mStamp->setCurrentIndex(std::max(0, mStamp->findData(QString::fromStdString(s.stamp))));
            mMask->clear();
            if (auto t = selectedTerrain())
                for (const auto& [name, values] : t->h->masks)
                    mMask->addItem(QString::fromStdString(name));
            if (mMask->findText(QString::fromStdString(s.mask)) < 0)
                mMask->addItem(QString::fromStdString(s.mask));
            mMask->setCurrentText(QString::fromStdString(s.mask));
            mFalloff->setCurrentIndex(std::max(0, mFalloff->findData(QString::fromStdString(s.stroke.falloff))));
            auto mirror = std::ranges::find_if(kMirrors, [&](const char* m) { return s.mirror == m; });
            mMirror.setCurrent(mirror == kMirrors.end() ? 0 : int(mirror - kMirrors.begin()));
        }
        mPlane->setChecked(s.plane);
        mAfter->setChecked(s.afterErosion || s.op == SculptOp::Uplift);
        mKeys->setChecked(keysShown());
        mRegion->setChecked(regionArmed());
    }

private:
    std::array<QToolButton*, kBrushes.size()> mButtons{};
    QDoubleSpinBox *mRadius = nullptr, *mHeight = nullptr, *mStep = nullptr, *mAngle = nullptr;
    QDoubleSpinBox *mStrength = nullptr, *mSharpness = nullptr, *mIterations = nullptr;
    QComboBox *mStamp = nullptr, *mMask = nullptr, *mFalloff = nullptr;
    panelkit::Segmented mMirror;
    QWidget *mSettingsBox = nullptr, *mSettingsSection = nullptr;
    QFormLayout* mSettingsForm = nullptr;
    QCheckBox *mPlane = nullptr, *mAfter = nullptr, *mKeys = nullptr;
    QToolButton* mRegion = nullptr;
};

QPointer<Palette> gPalette;

void syncPalette()
{
    if (gPalette)
        gPalette->sync();
}

QDockWidget* paletteDock()
{
    for (QWidget* w = gPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

class SculptTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "Sculpt Terrain"; }

    void activate() override
    {
        refresh();
        mControls.activate({}, {});
        panels::show(kPaletteId);
        panels::placeToolPalette(paletteDock());
        syncPalette();
    }

    void deactivate() override
    {
        if (mStroke.active())
            finishStroke();
        mControls.deactivate();
        regionArmed() = false;
        if (QDockWidget* d = paletteDock())
            d->hide();
        viewportTools::requestRedraw();
    }

    bool ownsBrushRadius() const override { return true; }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        refresh();
        if (!mTarget)
        {
            log::warn("Sculpt Terrain: select a terrain");
            return true;
        }
        if (regionArmed())
        {
            pickRegion(e->position(), view);
            return true;
        }
        Settings& st = settings();
        mOp = st.op;
        mInvert = e->modifiers() & Qt::ControlModifier;
        if (e->modifiers() & Qt::ShiftModifier)
            mOp = SculptOp::Smooth;
        if (mInvert && (mOp == SculptOp::Raise || mOp == SculptOp::Lower))
            mOp = mOp == SculptOp::Raise ? SculptOp::Lower : SculptOp::Raise;
        mPoints = json::array();
        mPressure = json::array();
        mRadiusUsed = 0.0;
        mPreviewed = false;
        mFlatHeight.reset();
        brush::BrushSettings stroke = st.stroke;
        if (mOp == SculptOp::Ramp)
            stroke.mode = brush::StrokeMode::Line;
        if (mOp == SculptOp::Stamp)
            stroke.spacing = std::max(stroke.spacing, 1.0);
        const ViewInfo v = view;
        UndoStack::get().openChunk("Sculpt Terrain");
        if (!mStroke.begin(stroke, mControls.input(e), [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return query(QPointF(px.x, px.y), v, out); }, *this,
                           mOp != SculptOp::Ramp))
        {
            UndoStack::get().closeChunk();
            return true;
        }
        mTimer.start();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        mHoverView = view.camera;
        if (mControls.mouseMove(e))
            return true;
        if (!mStroke.active())
        {
            viewportTools::requestRedraw();
            return false;
        }
        mStroke.move(mControls.input(e));
        // Re-evaluating on every mouse event would queue up behind itself; a few times a second keeps up.
        if (mTimer.elapsed() > 60 && mOp != SculptOp::Ramp)
        {
            send(false);
            mTimer.restart();
        }
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (e->button() != Qt::LeftButton || !mStroke.active())
            return false;
        finishStroke();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return key(e); }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        return key(*e);
    }

    void dab(const brush::Dab& d) override
    {
        if (!mTarget)
            return;
        if (mPoints.empty() && mOp == SculptOp::Flatten && !settings().plane)
            mFlatHeight = d.center;
        mPoints.push_back({d.center.x, d.center.y, d.center.z});
        mPressure.push_back(std::clamp(d.opacity * d.flow, 0.0, 1.0));
        mRadiusUsed = std::max(mRadiusUsed, d.radius);
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        (void)s;
        const Settings& st = settings();
        if (!mTarget)
        {
            drawToolStatus(p, view, {.tool = name(), .prompt = "select a terrain"});
            return;
        }
        const bool cm = inCm(st.op);
        drawToolStatus(p, view,
                       {.tool = name(),
                        .subject = QString::fromStdString(mTarget->mesh).section('|', -1),
                        .mode = buttonOf(st.op).label,
                        .fields = {{"radius", QString("%1 cm").arg(st.stroke.worldRadius, 0, 'f', 0)},
                                   {"strength", cm ? QString("%1 cm").arg(st.stroke.strength * st.height, 0, 'f', 0) : QString::number(st.stroke.strength, 'f', 2)}},
                        .flags = {{"after erosion", st.afterErosion && st.op != SculptOp::Uplift}},
                        .prompt = regionArmed() ? QString("click the patch to sculpt") : QString(),
                        .symmetry = st.mirror != "off" ? std::optional<ToolSymmetry>(ToolSymmetry{true, QString::fromStdString(st.mirror).toUpper()}) : std::nullopt});
        drawKeyList(p, QPointF(16.0, view.height - 84.0), keyHints());
        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        std::optional<brush::ui::CursorSurface> cs;
        brush::SurfaceHit hit;
        if (mHoverView == view.camera && query(at, view, hit))
            cs = brush::ui::CursorSurface{hit.center, hit.normal};
        brush::BrushSettings shown = st.stroke;
        if (regionArmed())
            shown.worldRadius *= 2.0;
        brush::ui::drawCursor(p, view, shown, at, cs, {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .stroke = &mStroke});
    }

private:
    void refresh() { mTarget = selectedTerrain(); }

    bool query(QPointF px, const ViewInfo& view, brush::SurfaceHit& out) const
    {
        if (!mTarget)
            return false;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const glm::dmat4 inv = glm::inverse(mTarget->world);
        glm::dvec3 at;
        if (!raycast(*mTarget->h, glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), &at))
            return false;
        out.center = glm::dvec3(mTarget->world * glm::dvec4(at, 1.0));
        out.normal = glm::normalize(glm::transpose(glm::inverse(glm::dmat3(mTarget->world))) * mTarget->h->normalAt(at.x, at.z));
        out.viewDir = glm::normalize(d);
        out.eye = view.eye;
        out.right = brush::ui::screenRight(view, glm::dmat4(1.0));
        out.pixelSize = view.unitsPerPixel(out.center);
        out.worldToLocal = 1.0;
        return true;
    }

    bool key(const QKeyEvent& e)
    {
        if (e.key() == Qt::Key_Escape)
        {
            if (regionArmed())
            {
                regionArmed() = false;
                syncPalette();
                viewportTools::requestRedraw();
                return true;
            }
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void pickRegion(QPointF px, const ViewInfo& view)
    {
        brush::SurfaceHit hit;
        if (!query(px, view, hit))
            return;
        regionArmed() = false;
        syncPalette();
        const std::string mesh = mTarget->mesh;
        const double size = settings().stroke.worldRadius * 4.0;
        const json call = {{"args", {mesh}}, {"center", {hit.center.x, hit.center.y, hit.center.z}}, {"size", size},
                           {"stage", settings().afterErosion ? "final" : "base"}};
        QTimer::singleShot(0, [call] {
            try
            {
                ops::execute("terrainSculptRegion", call, CallSource::UI);
                actions::run(json("sculpt.tool"));
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
    }

    void finishStroke()
    {
        mStroke.end();
        try
        {
            send(true);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        UndoStack::get().closeChunk();
        mPoints = json::array();
        mPressure = json::array();
        refresh();
        viewportTools::requestRedraw();
    }

    /** The stroke so far as terrainStroke: replacing its own preview, and on release moving it where it belongs. */
    void send(bool done)
    {
        if (!mTarget || mPoints.empty())
            return;
        const Settings& st = settings();
        json call = {{"args", {mTarget->mesh}},
                     {"brush", sculptOpNames()[size_t(mOp)]},
                     {"radius", mRadiusUsed > 0.0 ? mRadiusUsed : st.stroke.worldRadius},
                     {"falloff", st.stroke.falloff},
                     {"mirror", st.mirror},
                     {"stage", "final"}};
        const double strength = std::clamp(st.stroke.strength, 0.0, 1.0);
        call["strength"] = inCm(mOp) ? strength * st.height * (mInvert && mOp == SculptOp::Stamp ? -1.0 : 1.0) : strength;
        if (mOp == SculptOp::Ramp)
        {
            call["points"] = json::array({mPoints.front(), mPoints.back()});
        }
        else
        {
            call["points"] = mPoints;
            call["pressure"] = mPressure;
        }
        switch (mOp)
        {
        case SculptOp::Flatten:
            if (st.plane)
                call["plane"] = true;
            else if (mFlatHeight)
                call["height"] = math::upAxis() == math::UpAxis::Z ? mFlatHeight->z : mFlatHeight->y;
            break;
        case SculptOp::Terrace:
            call["step"] = st.step;
            call["sharpness"] = st.sharpness;
            break;
        case SculptOp::Stamp:
            call["stamp"] = st.stamp;
            call["angle"] = st.angle;
            break;
        case SculptOp::Erode:
        case SculptOp::Relax:
            call["iterations"] = st.iterations;
            break;
        case SculptOp::Mask:
            call["mask"] = st.mask;
            call["value"] = mInvert ? 0.0 : 1.0;
            break;
        case SculptOp::Uplift:
            call["value"] = mInvert ? 0.0 : 1.0;
            break;
        default:
            break;
        }
        const bool base = !st.afterErosion || mOp == SculptOp::Uplift;
        // Final strokes and the preview of base ones live in the final stage; the preview replaces itself as it grows.
        if (mPreviewed)
            call["replaceLast"] = true;
        if (done && base)
        {
            if (mPreviewed)
                ops::execute("terrainSculpt", {{"args", {mTarget->mesh}}, {"stage", "final"}, {"removeLast", true}}, CallSource::UI);
            call["stage"] = "base";
            call.erase("replaceLast");
            ops::execute("terrainStroke", call, CallSource::UI);
            return;
        }
        // Uplift only means something before erosion, so it doesn't preview in the final stage.
        if (mOp == SculptOp::Uplift && !done)
            return;
        ops::execute("terrainStroke", call, CallSource::UI);
        mPreviewed = true;
    }

    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return settings().stroke; }, [] { save(); }, {}};
    brush::Stroke mStroke;
    std::optional<Target> mTarget;
    QPointF mHover;
    std::string mHoverView;
    json mPoints = json::array(), mPressure = json::array();
    double mRadiusUsed = 0.0;
    std::optional<glm::dvec3> mFlatHeight;
    QElapsedTimer mTimer;
    SculptOp mOp = SculptOp::Raise;
    bool mInvert = false;
    bool mPreviewed = false;
};

/** A Sculpt Region proxy whose sculpt session is open: baked back into its terrain when the session closes. */
scene::NodeId gRegionSession = scene::kNoNode;

void watchRegionSessions(ext::Context& ctx)
{
    const int id = sculpt::addListener([] {
        auto s = sculpt::active();
        scene::Graph& g = scene::Graph::current();
        if (s)
        {
            scene::Node* n = g.node(s->node);
            gRegionSession = n && n->type().attrIndex("terrainRegion") >= 0 ? s->node : scene::kNoNode;
            return;
        }
        if (gRegionSession == scene::kNoNode)
            return;
        const scene::NodeId id = gRegionSession;
        gRegionSession = scene::kNoNode;
        // After the exit command finishes, so the bake is its own undo step with the sculpt already in the mesh.
        QTimer::singleShot(0, [id] {
            scene::Node* n = scene::Graph::current().node(id);
            if (!n || n->type().attrIndex("terrainRegion") < 0)
                return;
            const json info = json::parse(ops::execute("getAttr", {{"args", {n->path() + ".terrainRegion"}}}).get<std::string>(), nullptr, false);
            if (!info.is_object() || info.value("done", false))
                return;
            try
            {
                ops::execute("terrainSculptRegion", {{"args", {n->path()}}, {"finish", true}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
    });
    ctx.track("sculpt listener", [id] { sculpt::removeListener(id); });
}
} // namespace sculpttool_

void registerSculptTool(ext::Context& ctx)
{
    namespace st = sculpttool_;
    panels::add(ctx, {.id = st::kPaletteId,
                      .title = "Sculpt Terrain",
                      .create = [](QWidget* parent) -> QWidget* { return st::gPalette = new st::Palette(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "brush",
                      .open = false,
                      .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                      .floatFirst = true,
                      .toolPalette = true});
    viewportTools::add(ctx, {.id = "terrain.sculptTool", .label = "Sculpt Terrain", .icon = "brush", .create = [] { return std::make_unique<st::SculptTool>(); }});
    st::watchRegionSessions(ctx);
}
} // namespace terrain
