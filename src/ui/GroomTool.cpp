#include "ui/CurveUi.h"

#include "brush/BrushUi.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "groom/Groom.h"
#include "curve/CurveNodes.h"
#include "groom/Attrs.h"
#include "groom/GroomGraph.h"
#include "groom/GroomLook.h"
#include "groom/GroomNodes.h"
#include "groom/LockOps.h"
#include "groom/GroomPresets.h"
#include "groom/Style.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Controls.h"
#include "ui/FileDialog.h"
#include "ui/GroomGpu.h"
#include "ui/Icons.h"
#include "ui/NodeHandles.h"
#include "ui/PaintSelect.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/Tools.h"
#include "ui/UiCommands.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <memory>

namespace lila::ui
{
namespace
{
using Point3 = glm::dvec3;
using scene::Graph;
using scene::Node;
using scene::NodeId;

enum class HairMode : uint8_t
{
    Lock,
    Braid,
    Rope,
    Ringlet,
    Part,
    Ponytail,
    Bun,
    Bangs,
    Count
};

constexpr std::array<const char*, size_t(HairMode::Count)> kModeNames = {"lock", "braid", "rope", "ringlet", "part", "ponytail", "bun", "bangs"};
constexpr std::array<const char*, size_t(HairMode::Count)> kModeLabels = {"Lock", "Braid", "Rope", "Ringlet", "Part", "Ponytail", "Bun", "Bangs"};
constexpr const char* kPrefix = "/drawHair";

/** What the Draw Hair tool remembers between sessions, for one mode: each kind of hair keeps its own. */
struct DrawHairSettings
{
    /** The mode these settings belong to. */
    HairMode mode = HairMode::Lock;
    /** New lock width and thickness relative to the groom's. */
    double width = 1.0;
    double thickness = 1.0;
    /** Height above the scalp the stroke draws at, cm. */
    double lift = 0.4;
    double size = 1.0;
    double period = 4.0;
    double partRadius = 3.0;
    int presetCount = 16;
    double presetLength = 20.0;
    double presetRadius = 6.0;
};

DrawHairSettings defaultsFor(HairMode m)
{
    DrawHairSettings d;
    d.mode = m;
    switch (m)
    {
    case HairMode::Braid: d.size = 1.0, d.period = 4.0; break;
    case HairMode::Rope: d.size = 0.8, d.period = 3.0; break;
    case HairMode::Ringlet: d.width = 0.7, d.size = 1.2, d.period = 3.0; break;
    case HairMode::Bun: d.presetCount = 12, d.presetRadius = 4.0; break;
    case HairMode::Bangs: d.presetCount = 7, d.presetLength = 10.0; break;
    default: break;
    }
    return d;
}

std::string key(HairMode m, const char* k) { return std::string(kPrefix) + "/" + kModeNames[size_t(m)] + "/" + k; }

struct DrawHairState
{
    HairMode current = HairMode::Lock;
    std::array<DrawHairSettings, size_t(HairMode::Count)> modes;
};

DrawHairState& drawHairState()
{
    static DrawHairState st = [] {
        DrawHairState out;
        const PrefStore& p = PrefStore::get();
        const std::string mode = p.value<std::string>("general", std::string(kPrefix) + "/mode", "lock");
        for (size_t k = 0; k < kModeNames.size(); ++k)
        {
            if (mode == kModeNames[k])
                out.current = HairMode(k);
            const HairMode m = HairMode(k);
            DrawHairSettings c = defaultsFor(m);
            c.width = std::clamp(p.value<double>("general", key(m, "width"), c.width), 0.05, 20.0);
            c.thickness = std::clamp(p.value<double>("general", key(m, "thickness"), c.thickness), 0.05, 10.0);
            c.lift = std::max(0.0, p.value<double>("general", key(m, "lift"), c.lift));
            c.size = std::max(0.01, p.value<double>("general", key(m, "size"), c.size));
            c.period = std::max(0.05, p.value<double>("general", key(m, "period"), c.period));
            c.partRadius = std::max(0.01, p.value<double>("general", key(m, "partRadius"), c.partRadius));
            c.presetCount = std::clamp(p.value<int>("general", key(m, "presetCount"), c.presetCount), 1, 4096);
            c.presetLength = std::max(0.01, p.value<double>("general", key(m, "presetLength"), c.presetLength));
            c.presetRadius = std::max(0.01, p.value<double>("general", key(m, "presetRadius"), c.presetRadius));
            out.modes[k] = c;
        }
        return out;
    }();
    return st;
}

/** The current mode's settings; its mode field says which mode that is. */
DrawHairSettings& drawHairSettings() { return drawHairState().modes[size_t(drawHairState().current)]; }

void saveDrawHair()
{
    syncGroomPalette();
    PrefStore& p = PrefStore::get();
    const DrawHairSettings& c = drawHairSettings();
    const HairMode m = c.mode;
    p.set("general", std::string(kPrefix) + "/mode", std::string(kModeNames[size_t(m)]));
    p.set("general", key(m, "width"), c.width);
    p.set("general", key(m, "thickness"), c.thickness);
    p.set("general", key(m, "lift"), c.lift);
    p.set("general", key(m, "size"), c.size);
    p.set("general", key(m, "period"), c.period);
    p.set("general", key(m, "partRadius"), c.partRadius);
    p.set("general", key(m, "presetCount"), c.presetCount);
    p.set("general", key(m, "presetLength"), c.presetLength);
    p.set("general", key(m, "presetRadius"), c.presetRadius);
    viewportTools::requestRedraw();
}

void setDrawHairMode(HairMode m)
{
    drawHairState().current = m;
    saveDrawHair();
}

json vec(const Point3& v) { return json::array({v.x, v.y, v.z}); }

/** The groom of the selection, or of the only groom in the scene. */
Node* selectedGroom(Graph& g)
{
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = groom::findGroom(g, *n))
                return gr;
    Node* only = nullptr;
    int count = 0;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("groom"))
        {
            only = n;
            ++count;
        }
    return count == 1 ? only : nullptr;
}

constexpr const char* kGroomPaletteId = "groomPalette";
constexpr const char* kGroomKeysPref = "/groom/keys";

struct GroomPaletteButton
{
    const char* icon;
    const char* label;
};
constexpr std::array<GroomPaletteButton, std::size_t(HairMode::Count)> kModeButtons = {{{"line", "Lock"},
                                                                              {"arrows-shuffle", "Braid"},
                                                                              {"link", "Rope"},
                                                                              {"spiral", "Ringlet"},
                                                                              {"separator-vertical", "Part"},
                                                                              {"pennant", "Ponytail"},
                                                                              {"lifebuoy", "Bun"},
                                                                              {"grip-horizontal", "Bangs"}}};
constexpr std::array<GroomPaletteButton, std::size_t(groom::Brush::Count)> kBrushButtons = {{{"hand-move", "Comb"},
                                                                                   {"blur", "Smooth"},
                                                                                   {"ruler", "Length"},
                                                                                   {"arrows-maximize", "Puff"},
                                                                                   {"circle-plus", "Add"},
                                                                                   {"eraser", "Delete"},
                                                                                   {"grid-dots", "Density"},
                                                                                   {"scissors", "Cut"},
                                                                                   {"arrows-minimize", "Pinch"},
                                                                                   {"arrows-move-horizontal", "Slide"},
                                                                                   {"hand-grab", "Grab"},
                                                                                   {"rotate-clockwise", "Twist"},
                                                                                   {"arrow-autofit-width", "Widen"},
                                                                                   {"fold-down", "Flatten"},
                                                                                   {"curly-loop", "Curl"},
                                                                                   {"sparkles", "Frizz"},
                                                                                   {"magnet", "Clump"},
                                                                                   {"arrows-split", "Split"},
                                                                                   {"arrows-join", "Merge"},
                                                                                   {"arrow-bar-to-down", "Settle"}}};

/** The guides the palette's Mirror X works on: the selected groom's, else a selected curve. */
Node* groomPaletteGuides()
{
    Graph& g = Graph::current();
    if (Node* gr = selectedGroom(g))
        return groom::guidesOf(g, *gr);
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("curve"))
            return n;
    return nullptr;
}

/**
 * The Groom palette, shown while Draw Hair or Sculpt Curves is active: what the next stroke draws or which brush paints,
 * their settings, and the guides' mirror. Number keys stay with the app.
 */
class GroomPalette final : public QWidget
{
public:
    explicit GroomPalette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "groomPalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);
        auto tileButton = [this](const GroomPaletteButton& def, std::function<void()> fn) {
            auto* b = new QToolButton;
            b->setIcon(icon(def.icon));
            b->setIconSize(QSize(24, 24));
            b->setText(def.label);
            b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            controls::setRole(b, "tile");
            b->setToolTip(def.label);
            connect(b, &QToolButton::clicked, this, std::move(fn));
            return b;
        };

        mPages = new controls::Segmented(this);
        mPages->add("Draw", {}, "Draw: stylized hair as locks, braids, ropes, ringlets, ponytails, buns and bangs, then shaped with the lock brushes",
                    int(GroomPalettePage::Draw));
        mPages->add("Strands", {}, "Strands: guides placed and combed one by one, for realistic hair and fur", int(GroomPalettePage::Groom));
        connect(mPages, &controls::Segmented::picked, this, [](int page) {
            viewportTools::activate(page == int(GroomPalettePage::Draw) ? "groom.drawHair" : "groom.brushes");
        });
        v->addWidget(mPages);
        v->addSpacing(4);

        mDraw = new QWidget;
        auto* dv = new QVBoxLayout(mDraw);
        dv->setContentsMargins(0, 0, 0, 0);
        dv->setSpacing(4);
        std::vector<QWidget*> modeTiles;
        for (size_t i = 0; i < kModeButtons.size(); ++i)
        {
            mModes[i] = tileButton(kModeButtons[i], [i] {
                setDrawHairMode(HairMode(i));
                if (!viewportTools::active() || std::string_view(viewportTools::active()->name()) != "Draw Hair")
                    viewportTools::activate("groom.drawHair");
            });
            modeTiles.push_back(mModes[i]);
        }
        dv->addWidget(controls::section("Mode", controls::grid(modeTiles, 4, mDraw), mDraw, kSections));
        auto* fields = new QWidget(mDraw);
        auto* form = new QFormLayout(fields);
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        auto spin = [&](const char* label, double lo, double hi, const char* unit, const char* tip, double DrawHairSettings::*field) {
            QDoubleSpinBox* s = controls::valueField(lo, hi, 2, unit, fields);
            s->setSingleStep(0.1);
            s->setToolTip(QString("%1: %2").arg(label, tip));
            mFields.push_back({s, field});
            connect(s, &QDoubleSpinBox::valueChanged, this, [field](double value) {
                drawHairSettings().*field = value;
                saveDrawHair();
            });
            form->addRow(label, s);
            return s;
        };
        mWidth = spin("Width", 0.05, 20.0, "x", "new locks' width, times the groom's  (B+drag, [ ], Ctrl+scroll)", &DrawHairSettings::width);
        mThickness = spin("Thickness", 0.05, 10.0, "x", "new locks' thickness, times the groom's  (B+drag up and down)", &DrawHairSettings::thickness);
        mLift = spin("Lift", 0.0, 100.0, "cm", "how far above the scalp the stroke draws.", &DrawHairSettings::lift);
        mSize = spin("Size", 0.01, 100.0, "cm", "braid and rope half width, ringlet radius.", &DrawHairSettings::size);
        mPeriod = spin("Turn", 0.05, 500.0, "cm", "length of one turn of a braid, rope or ringlet.", &DrawHairSettings::period);
        mPartRadius = spin("Reach", 0.01, 100.0, "cm", "how far from a parting line locks turn.", &DrawHairSettings::partRadius);
        mLength = spin("Length", 0.01, 1000.0, "cm", "length of a ponytail, bun or bangs.", &DrawHairSettings::presetLength);
        mCount = controls::valueField(1, 4096, 0, {}, fields);
        mCount->setToolTip("Count: locks in a ponytail, bun or bangs.");
        connect(mCount, &QDoubleSpinBox::valueChanged, this, [](double value) {
            drawHairSettings().presetCount = int(std::lround(value));
            saveDrawHair();
        });
        form->addRow("Count", mCount);
        dv->addWidget(controls::section("Settings", fields, mDraw, kSections));
        v->addWidget(mDraw);

        mSculpt = new QWidget;
        auto* sv = new QVBoxLayout(mSculpt);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(4);
        for (int set = 0; set < 2; ++set)
        {
            std::vector<QWidget*> brushTiles;
            const int first = set ? int(groom::kFirstLockBrush) : 0, last = set ? int(groom::Brush::Count) : int(groom::kFirstLockBrush);
            for (int i = first; i < last; ++i)
            {
                mBrushes[size_t(i)] = tileButton(kBrushButtons[size_t(i)], [i] { setCurveSculptBrush(i); });
                brushTiles.push_back(mBrushes[size_t(i)]);
            }
            sv->addWidget(controls::section(set ? "Locks" : "Strands", controls::grid(brushTiles, 4, mSculpt), mSculpt, kSections));
        }
        mSettleGrab = new QCheckBox("Settle While Grabbing");
        mSettleGrab->setFocusPolicy(Qt::NoFocus);
        mSettleGrab->setToolTip("Settle While Grabbing: Grab settles the locks around it, so they make room.");
        setCheckDefault(mSettleGrab, true);
        // Toggled rather than clicked so a middle-click reset applies too; sync() setting the current state is skipped.
        connect(mSettleGrab, &QCheckBox::toggled, this, [](bool on) {
            if (on != curveSculptSettleGrab())
                setCurveSculptSettleGrab(on);
        });
        v->addWidget(mSculpt);
        mGroom = makeGroomBrushPage(this);
        v->addWidget(mGroom);

        auto* optionsBox = new QWidget(this);
        auto* options = new QVBoxLayout(optionsBox);
        options->setContentsMargins(0, 0, 0, 0);
        options->setSpacing(4);
        mSettleGrab->setParent(optionsBox);
        options->addWidget(mSettleGrab);
        mMirror = new QCheckBox("Mirror X", optionsBox);
        mMirror->setFocusPolicy(Qt::NoFocus);
        setCheckDefault(mMirror, false);
        connect(mMirror, &QCheckBox::toggled, this, [](bool on) {
            Graph& g = Graph::current();
            if (Node* guides = groomPaletteGuides(); guides && on != (curve::symmetryAxis(*guides) == 0))
            {
                // A groom's guides mirror as the groom does.
                Node* gr = groom::groomOf(g, *guides);
                runUi("setAttr", {{"args", {(gr ? gr->path() : guides->path()) + ".symmetry", on ? "x" : "off"}}});
            }
        });
        options->addWidget(mMirror);
        mKeys = new QCheckBox("Show Keys", optionsBox);
        mKeys->setFocusPolicy(Qt::NoFocus);
        mKeys->setToolTip("Show Keys: list the tool's keys in the corner of the view.");
        setCheckDefault(mKeys, true);
        connect(mKeys, &QCheckBox::toggled, this, [](bool on) {
            if (on == groomKeysShown())
                return;
            PrefStore::get().set("general", kGroomKeysPref, on);
            viewportTools::requestRedraw();
        });
        options->addWidget(mKeys);
        auto* reset = new QToolButton(optionsBox);
        reset->setText("Reset Settings");
        reset->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        reset->setFocusPolicy(Qt::NoFocus);
        reset->setToolTip("Reset Settings: put the current mode's or brush's settings back to their defaults.");
        controls::setRole(reset, "push");
        connect(reset, &QToolButton::clicked, this, [this] {
            if (mPage == GroomPalettePage::Groom && groomBrushToolActive())
            {
                resetGroomBrushSettings();
                return;
            }
            if (mPage != GroomPalettePage::Draw)
            {
                resetCurveSculptSettings();
                return;
            }
            DrawHairSettings& s = drawHairSettings();
            s = defaultsFor(s.mode);
            saveDrawHair();
        });
        options->addWidget(reset);
        v->addWidget(controls::section("Options", optionsBox, this, kSections));
        v->addStretch(1);

        mRefresh = new QTimer(this);
        mRefresh->setSingleShot(true);
        mRefresh->setInterval(60);
        connect(mRefresh, &QTimer::timeout, this, [this] { sync(); });
        mGraphListener = Graph::current().addListener([this](scene::Change, NodeId) { mRefresh->start(); });
        sync();
    }

    ~GroomPalette() override { Graph::current().removeListener(mGraphListener); }

    void setPage(GroomPalettePage page)
    {
        mPage = page;
        sync();
    }

    void sync()
    {
        // Plain curves have one page; a groom switches between drawing locks and placing strands.
        mPages->setVisible(mPage != GroomPalettePage::Sculpt);
        if (mPage != GroomPalettePage::Sculpt)
            mPages->setCurrent(int(mPage));
        mDraw->setVisible(mPage == GroomPalettePage::Draw);
        mSculpt->setVisible(mPage == GroomPalettePage::Sculpt);
        mGroom->setVisible(mPage == GroomPalettePage::Groom);
        syncGroomBrushPage(mGroom);
        const DrawHairSettings& s = drawHairSettings();
        for (size_t i = 0; i < mModes.size(); ++i)
            mModes[i]->setChecked(HairMode(i) == s.mode);
        for (auto [box, value] : {std::pair{mWidth, s.width}, std::pair{mThickness, s.thickness}, std::pair{mLift, s.lift}, std::pair{mSize, s.size}, std::pair{mPeriod, s.period},
                                  std::pair{mPartRadius, s.partRadius}, std::pair{mLength, s.presetLength}})
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        {
            const QSignalBlocker block(mCount);
            mCount->setValue(s.presetCount);
        }
        // Middle click resets to this mode's own defaults.
        const DrawHairSettings d = defaultsFor(s.mode);
        for (auto [box, field] : mFields)
            setSpinDefault(box, d.*field);
        setSpinDefault(mCount, d.presetCount);
        const bool plait = s.mode == HairMode::Braid || s.mode == HairMode::Rope || s.mode == HairMode::Ringlet;
        const bool preset = s.mode == HairMode::Ponytail || s.mode == HairMode::Bun || s.mode == HairMode::Bangs;
        mThickness->setEnabled(!preset && s.mode != HairMode::Part);
        mSize->setEnabled(plait);
        mPeriod->setEnabled(plait);
        mPartRadius->setEnabled(s.mode == HairMode::Part);
        mLength->setEnabled(preset);
        mCount->setEnabled(preset);
        const int brush = curveSculptBrush();
        for (size_t i = 0; i < mBrushes.size(); ++i)
            mBrushes[i]->setChecked(int(i) == brush);
        mSettleGrab->setVisible(mPage != GroomPalettePage::Draw && !groomBrushToolActive());
        mSettleGrab->setChecked(curveSculptSettleGrab());
        mSettleGrab->setEnabled(brush == int(groom::Brush::Grab));
        const Node* guides = groomPaletteGuides();
        mMirror->setEnabled(guides);
        mMirror->setChecked(guides && curve::symmetryAxis(*guides) == 0);
        mMirror->setToolTip(guides ? QString("Mirror X: mirror %1 across X.").arg(QString::fromStdString(guides->name())) : "Mirror X: select a groom to mirror its guides.");
        mKeys->setChecked(groomKeysShown());
    }

private:
    GroomPalettePage mPage = GroomPalettePage::Draw;
    controls::Segmented* mPages = nullptr;
    QWidget *mDraw = nullptr, *mSculpt = nullptr, *mGroom = nullptr;
    std::array<QToolButton*, kModeButtons.size()> mModes{};
    std::array<QToolButton*, kBrushButtons.size()> mBrushes{};
    QDoubleSpinBox *mWidth = nullptr, *mThickness = nullptr, *mLift = nullptr, *mSize = nullptr, *mPeriod = nullptr, *mPartRadius = nullptr, *mLength = nullptr;
    QDoubleSpinBox* mCount = nullptr;
    std::vector<std::pair<QDoubleSpinBox*, double DrawHairSettings::*>> mFields;
    QCheckBox *mSettleGrab = nullptr, *mMirror = nullptr, *mKeys = nullptr;
    QTimer* mRefresh = nullptr;
    int mGraphListener = 0;
};

QPointer<GroomPalette> gGroomPalette;

QDockWidget* groomPaletteDock()
{
    for (QWidget* w = gGroomPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

/**
 * Draw Hair: strokes on the scalp become locks, braids, ropes or ringlets; a stroke can also part the hair, or place a
 * ponytail, bun or bangs. The stroke hugs the scalp at the lift height and carries on in the view plane past its edge,
 * so a lock can be drawn falling off the head. Pen pressure is the width along it.
 */
class DrawHairTool final : public ViewportTool
{
public:
    const char* name() const override { return "Draw Hair"; }
    bool ownsBrushRadius() const override { return true; }

    void activate() override
    {
        Node* gr = groomForSelection();
        mGroom = gr ? gr->id() : scene::kNoNode;
        mControls.activate();
        showGroomPalette(GroomPalettePage::Draw);
    }

    void deactivate() override
    {
        mControls.deactivate();
        hideGroomPalette();
        viewportTools::setHoverHighlight(scene::kNoNode);
        mStroke.clear();
        mPressure.clear();
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        // B is the brush controls' while the viewport has focus, the app's otherwise; either arms the drag.
        if (tools::brushRadiusDrag(e, mControls.armed() == brush::ui::BrushControls::Adjust::Radius))
        {
            mSizing = true;
            mSizePress = e->position();
            mSizeStart = {e->button() == Qt::MiddleButton ? 0.05 : drawHairSettings().width, sizingSecond()};
            Point3 at;
            mSizeUnit = point(e->position(), view, true, at) ? view.unitsPerPixel(at) : 0.0;
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        if (!groomNode())
        {
            // No groom yet: the click picks the mesh it grows from.
            if (Node* gr = startGroomOn(groomScalpUnder(view, e->position())))
                mGroom = gr->id();
            viewportTools::requestRedraw();
            return true;
        }
        mStroke.clear();
        mPressure.clear();
        Point3 p;
        if (!point(e->position(), view, true, p))
            return true;
        mStroke.push_back(p);
        mPressure.push_back(mControls.input(e).pressure);
        mLastPx = e->position();
        mStroking = true;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mSizing)
        {
            // Right is wider; up is thicker, longer or reaching further, as the mode has it.
            DrawHairSettings& s = drawHairSettings();
            const double dx = e->position().x() - mSizePress.x(), dy = mSizePress.y() - e->position().y();
            s.width = std::clamp(mSizeStart.first * std::pow(2.0, dx / 120.0), 0.05, 20.0);
            setSizingSecond(mSizeStart.second * std::pow(2.0, dy / 120.0));
            saveDrawHair();
            return true;
        }
        if (!groomNode())
        {
            groomScalpUnder(view, e->position());
            viewportTools::requestRedraw();
        }
        if (!mStroking)
            return false;
        if (QLineF(mLastPx, e->position()).length() < 4.0)
            return true;
        Point3 p;
        if (point(e->position(), view, false, p))
        {
            mStroke.push_back(p);
            mPressure.push_back(mControls.input(e).pressure);
            mLastPx = e->position();
            viewportTools::requestRedraw();
        }
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mSizing)
        {
            mSizing = false;
            viewportTools::requestRedraw();
            return true;
        }
        if (e->button() != Qt::LeftButton || !mStroking)
            return false;
        mStroking = false;
        finish();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ControlModifier))
            return false;
        DrawHairSettings& s = drawHairSettings();
        s.width = std::clamp(s.width * std::pow(1.1, e->angleDelta().y() / 120.0), 0.05, 20.0);
        saveDrawHair();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override { return key(e->key()); }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            return false;
        // [ ] are app hotkeys too; the tool takes them for the lock width while active.
        const int k = e.key();
        return (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight) && key(k);
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();
        const DrawHairSettings& s = drawHairSettings();
        const glm::vec4 col(0.95f, 0.75f, 0.35f, 1.0f);
        for (size_t i = 1; i < mStroke.size(); ++i)
        {
            f.lines.push_back({glm::dvec3(mStroke[i - 1]), col});
            f.lines.push_back({glm::dvec3(mStroke[i]), col});
        }
        Node* gr = groomNode();
        Node* guides = gr ? groom::guidesOf(Graph::current(), *gr) : nullptr;
        const bool mirrored = guides && groom::mirrorPlane(*guides);
        drawToolStatus(p, view,
                       {.tool = "Draw Hair",
                        .subject = gr ? QString::fromStdString(gr->name()) : QString(),
                        .mode = kModeLabels[size_t(s.mode)],
                        .fields = {{"width", QString::number(s.width, 'f', 2)}},
                        .prompt = gr ? QString() : QString("click the mesh hair grows from"),
                        .symmetry = mirrored ? std::optional<ToolSymmetry>(ToolSymmetry{true, QString("XYZ").mid(curve::symmetryAxis(*guides), 1)}) : std::nullopt});
        if (mSizing)
            drawSizing(p, gr);
        if (groomKeysShown())
            drawKeyList(p, QPointF(16.0, view.height - 84.0), keys(mirrored));
    }

private:
    Node* groomNode() const
    {
        Node* n = Graph::current().node(mGroom);
        return n && n->type().isA("groom") ? n : nullptr;
    }

    static bool isPreset(HairMode m) { return m == HairMode::Ponytail || m == HairMode::Bun || m == HairMode::Bangs; }

    static const char* secondName(HairMode m) { return m == HairMode::Part ? "reach" : isPreset(m) ? "length" : "thickness"; }

    /** What B+drag's up and down changes: thickness for drawn locks, length for presets, reach for a parting. */
    static double sizingSecond()
    {
        const DrawHairSettings& s = drawHairSettings();
        return s.mode == HairMode::Part ? s.partRadius : isPreset(s.mode) ? s.presetLength : s.thickness;
    }

    static void setSizingSecond(double v)
    {
        DrawHairSettings& s = drawHairSettings();
        if (s.mode == HairMode::Part)
            s.partRadius = std::clamp(v, 0.01, 100.0);
        else if (isPreset(s.mode))
            s.presetLength = std::clamp(v, 0.01, 1000.0);
        else
            s.thickness = std::clamp(v, 0.05, 10.0);
    }

    /** The new lock's cross-section at the press while B+dragging, at its size on screen, with the values. */
    void drawSizing(OverlayPainter& p, Node* gr) const
    {
        const DrawHairSettings& s = drawHairSettings();
        const bool cm = s.mode == HairMode::Part || isPreset(s.mode);
        QString name = secondName(s.mode);
        name[0] = name[0].toUpper();
        const QString text = QString("Width %1x   %2 %3").arg(s.width, 0, 'f', 2).arg(name, cm ? QString("%1 cm").arg(sizingSecond(), 0, 'f', 1) : QString("%1x").arg(sizingSecond(), 0, 'f', 2));
        if (gr && mSizeUnit > 0.0 && !cm)
        {
            const groom::LockOptions lo = groom::lockOptionsOf(*gr);
            const double w = lo.width * s.width / mSizeUnit, h = std::max(2.0, w * std::clamp(lo.thickness * s.thickness, 0.02, 4.0));
            std::vector<QPointF> ring;
            for (int i = 0; i < 40; ++i)
            {
                const double a = glm::two_pi<double>() * i / 40.0;
                ring.emplace_back(mSizePress.x() + 0.5 * w * std::cos(a), mSizePress.y() + 0.5 * h * std::sin(a));
            }
            p.polyline(ring, 1.5f, glm::vec4(0.95f, 0.75f, 0.35f, 1.0f), true);
        }
        p.label(mSizePress + QPointF(12.0, -18.0), text);
    }

    static std::vector<KeyHint> keys(bool mirrored)
    {
        const HairMode m = drawHairSettings().mode;
        const char* what = m == HairMode::Part ? "Part" : m == HairMode::Ponytail || m == HairMode::Bun ? "Place" : "Draw";
        std::vector<KeyHint> out = {{"Drag", QString("%1 %2").arg(what, QString(kModeLabels[size_t(m)]).toLower())}};
        if (mirrored)
            out.push_back({"X+drag", "Snap to mirror"});
        out.insert(out.end(), {{"B+drag", QString("Width; up and down, %1").arg(secondName(m))}, {"[  ]", "Narrower, wider"}, {"Esc", "Exit"}});
        return out;
    }

    bool key(int k)
    {
        DrawHairSettings& s = drawHairSettings();
        if (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight)
        {
            s.width = std::clamp(s.width * (k == Qt::Key_BracketRight ? 1.1 : 1.0 / 1.1), 0.05, 20.0);
            saveDrawHair();
            return true;
        }
        if (k == Qt::Key_Escape)
        {
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    /**
     * With the guides mirrored, a stroke whose root is within a lock's width of the mirror plane roots on it, and one
     * that runs along the plane lies on it: one centred lock rather than a pair hugging the middle. X widens the reach.
     */
    static void snapToMirror(Node& gr, std::vector<Point3>& pts)
    {
        Graph& g = Graph::current();
        Node* guides = groom::guidesOf(g, gr);
        const auto plane = guides ? groom::mirrorPlane(*guides) : std::nullopt;
        if (!plane || pts.empty())
            return;
        const auto [at, n] = *plane;
        const double half = 0.5 * groom::lockOptionsOf(gr).width * drawHairSettings().width;
        const double reach = (tools::snaps() & SnapGrid) ? 4.0 * half : half;
        auto offset = [&](const Point3& p) { return glm::dot(p - at, n); };
        if (std::ranges::all_of(pts, [&](const Point3& p) { return std::abs(offset(p)) < reach; }))
            for (Point3& p : pts)
                p -= n * offset(p);
        else if (std::abs(offset(pts.front())) < reach)
            pts.front() -= n * offset(pts.front());
    }

    /** The scalp in world space, rebuilt when its mesh or matrix changes. */
    const groom::Surface* scalp()
    {
        Node* gr = groomNode();
        if (!gr)
            return nullptr;
        Graph& g = Graph::current();
        Node* guides = groom::guidesOf(g, *gr);
        Node* mesh = guides ? g.find(groom::groomSurfaceOf(*guides)) : nullptr;
        if (!mesh || !mesh->type().isA("mesh"))
            return nullptr;
        g.evaluate();
        scene::MeshPtr m = mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        const glm::dmat4 w = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        if (m.get() != mScalpMesh || w != mScalpMatrix || !mScalp)
        {
            mScalpMesh = m.get();
            mScalpMatrix = w;
            mScalp.reset();
            if (m && m->numFaces() > 0)
            {
                geo::Mesh c = *m;
                for (geo::Index pi = 0; pi < c.pointCapacity(); ++pi)
                    if (c.pointValid(pi))
                        c.setPosition(pi, Point3(w * glm::dvec4(c.position(pi), 1.0)));
                mScalp = std::make_unique<groom::Surface>(std::move(c));
            }
        }
        return mScalp.get();
    }

    /** Where the stroke is at px: on the scalp lifted off it, else on the plane through the last point facing the view. */
    bool point(QPointF px, const ViewInfo& view, bool first, Point3& out)
    {
        Point3 o, d;
        view.ray(px, o, d);
        d = glm::normalize(d);
        const DrawHairSettings& s = drawHairSettings();
        const bool parting = s.mode == HairMode::Part;
        if (const groom::Surface* sc = scalp())
            if (auto h = sc->raycast(o, d))
            {
                // The first point sits on the scalp: it is the root.
                out = h->point + h->normal * (first || parting ? 0.0 : s.lift);
                return true;
            }
        if (first || mStroke.empty())
            return false;
        // Off the edge of the head: keep drawing at the depth of the last point.
        const Point3 anchor = mStroke.back();
        const Point3 n = -view.viewDir(anchor);
        const double den = glm::dot(d, n);
        if (std::abs(den) < 1e-9)
            return false;
        const double t = glm::dot(anchor - o, n) / den;
        if (t <= 0.0)
            return false;
        out = o + d * t;
        return true;
    }

    void finish()
    {
        Node* gr = groomNode();
        const DrawHairSettings& s = drawHairSettings();
        std::vector<Point3> pts = std::move(mStroke);
        std::vector<double> pressure = std::move(mPressure);
        mStroke.clear();
        mPressure.clear();
        viewportTools::requestRedraw();
        if (!gr || pts.empty())
            return;
        snapToMirror(*gr, pts);
        json points = json::array();
        for (const Point3& p : pts)
            points.push_back(vec(p));
        const json args = {gr->path()};
        switch (s.mode)
        {
        case HairMode::Lock:
        case HairMode::Braid:
        case HairMode::Rope:
        case HairMode::Ringlet:
            if (pts.size() < 2)
                return;
            runUi("groomDraw", {{"args", args},
                                {"points", points},
                                {"pressure", pressure},
                                {"shape", kModeNames[size_t(s.mode)]},
                                {"size", s.size},
                                {"period", s.period},
                                {"width", s.width},
                                {"thickness", s.thickness}});
            break;
        case HairMode::Part:
            if (pts.size() >= 2)
                runUi("groomPart", {{"args", args}, {"points", points}, {"radius", s.partRadius}});
            break;
        case HairMode::Ponytail:
        case HairMode::Bun:
            runUi("groomPreset", {{"args", args},
                                  {"preset", kModeNames[size_t(s.mode)]},
                                  {"at", vec(pts.front())},
                                  {"count", s.presetCount},
                                  {"length", s.presetLength},
                                  {"radius", s.presetRadius}});
            break;
        case HairMode::Bangs:
            if (pts.size() >= 2)
                runUi("groomPreset",
                      {{"args", args}, {"preset", "bangs"}, {"at", vec(pts.front())}, {"to", vec(pts.back())}, {"count", s.presetCount}, {"length", s.presetLength}});
            break;
        default: break;
        }
    }

    brush::BrushSettings mBrush;
    brush::ui::BrushControls mControls{[this]() -> brush::BrushSettings& { return mBrush; }, [] {}, {}};
    NodeId mGroom = scene::kNoNode;
    std::vector<Point3> mStroke;
    std::vector<double> mPressure;
    QPointF mLastPx;
    bool mStroking = false;
    bool mSizing = false;
    QPointF mSizePress;
    std::pair<double, double> mSizeStart{1.0, 1.0};
    double mSizeUnit = 0.0;
    const geo::Mesh* mScalpMesh = nullptr;
    glm::dmat4 mScalpMatrix{0.0};
    std::unique_ptr<groom::Surface> mScalp;
};

/** Where a guide's lock handles sit, world space: the width handle on its edge 40% along, the twist handle 85% along. */
struct LockHandles
{
    Point3 widthAt{0.0}, widthCentre{0.0}, twistAt{0.0}, twistCentre{0.0};
    double width = 1.0;
    double twist = 0.0; // degrees at the tip, from the guide's tilt
};

/** Each own guide's handles, or nothing for a curve that isn't a groom's guides. */
std::vector<std::optional<LockHandles>> lockHandles(const Node& curveNode)
{
    Graph& g = Graph::current();
    Node* gr = groom::groomOf(g, curveNode);
    if (!gr)
        return {};
    const curve::Splines& own = curve::shapeSplines(g, curveNode.id());
    const glm::dmat4 w = curveNode.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const groom::LockOptions lo = groom::lockOptionsOf(*gr);
    std::vector<std::optional<LockHandles>> out(own.size());
    for (size_t k = 0; k < own.size(); ++k)
    {
        curve::Spline s = own[k];
        for (Point3& p : s.points)
            p = Point3(w * glm::dvec4(p, 1.0));
        const curve::Splines r = groom::resolveLocks({s}, nullptr, lo);
        if (r.empty() || r[0].size() < 2)
            continue;
        const curve::Spline& lock = r[0];
        auto at = [&](double t, Point3& centre, Point3& edge) {
            const size_t i = std::min(lock.size() - 1, size_t(std::lround(t * double(lock.size() - 1))));
            const Point3 side(groom::attr::point(lock, groom::attr::kSideX, i, 1.0), groom::attr::point(lock, groom::attr::kSideY, i, 0.0),
                              groom::attr::point(lock, groom::attr::kSideZ, i, 0.0));
            centre = lock.points[i];
            edge = centre + side * groom::attr::point(lock, groom::attr::kHalfWidth, i, 0.5);
        };
        LockHandles h;
        at(0.4, h.widthCentre, h.widthAt);
        at(0.85, h.twistCentre, h.twistAt);
        h.width = groom::attr::get(own[k], groom::attr::kWidth, 1.0);
        h.twist = own[k].tilt.empty() ? 0.0 : glm::degrees(own[k].tilt.back());
        out[k] = h;
    }
    return out;
}

/** Drags a guide's width or twist: live on the guides with undo off, landing as one groomEditLock. */
class LockHandleDrag final : public nodeHandles::Drag
{
public:
    LockHandleDrag(NodeId guides, size_t index, bool twist, const LockHandles& h, QPointF press, const ViewInfo& view)
        : mGuides(guides), mIndex(index), mTwist(twist), mStart(h), mPress(press)
    {
        Graph& g = Graph::current();
        mStartValue = g.value({guides, curve::CurveShape::kLocal, -1});
        if (Node* n = g.node(guides))
            if (Node* gr = groom::groomOf(g, *n))
            {
                mHierarchyAt = {gr->id(), gr->type().attrIndex("hierarchy"), -1};
                mHierarchyStart = g.value(mHierarchyAt);
            }
        if (auto c = view.project(twist ? h.twistCentre : h.widthCentre))
            mCentre = *c;
        mValue = twist ? h.twist : h.width;
    }

    void move(QPointF px, const ViewInfo&, Qt::KeyboardModifiers mods) override
    {
        const double fine = (mods & Qt::ControlModifier) ? 0.2 : 1.0;
        if (mTwist)
            mValue = mStart.twist + (px.x() - mPress.x()) * fine;
        else
        {
            const double d0 = std::max(1.0, QLineF(mCentre, mPress).length());
            mValue = std::max(0.01, mStart.width * (1.0 + (QLineF(mCentre, px).length() / d0 - 1.0) * fine));
        }
        Graph& g = Graph::current();
        curve::Splines s = curve::splinesOf(mStartValue);
        if (mIndex >= s.size())
            return;
        apply(s[mIndex]);
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        if (Node* n = g.node(mGuides))
            groom::storeGuides(g, *n, std::move(s));
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void release(bool moved) override
    {
        cancel();
        if (!moved)
            return;
        Node* n = Graph::current().node(mGuides);
        if (!n)
            return;
        json call = {{"args", {n->path()}}, {"index", mIndex}};
        call[mTwist ? "twist" : "width"] = mValue;
        runUi("groomEditLock", call);
    }

    void cancel() override
    {
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        Graph& g = Graph::current();
        if (mHierarchyAt.node)
            g.setValue(mHierarchyAt, mHierarchyStart);
        else
            g.setValue({mGuides, curve::CurveShape::kCreate, -1}, mStartValue);
        u.setEnabled(was);
    }

    QString readout() const override { return mTwist ? QString("Twist %1\xc2\xb0").arg(mValue, 0, 'f', 0) : QString("Width %1x").arg(mValue, 0, 'f', 2); }

private:
    void apply(curve::Spline& sp) const
    {
        if (!mTwist)
        {
            groom::attr::set(sp, groom::attr::kWidth, mValue);
            return;
        }
        const double len = std::max(1e-12, groom::attr::length(sp));
        sp.tilt.assign(sp.size(), 0.0);
        double walked = 0.0;
        for (size_t k = 1; k < sp.size(); ++k)
        {
            walked += glm::distance(sp.points[k - 1], sp.points[k]);
            sp.tilt[k] = glm::radians(mValue) * walked / len;
        }
    }

    NodeId mGuides;
    size_t mIndex;
    bool mTwist;
    LockHandles mStart;
    QPointF mPress, mCentre;
    scene::PlugValue mStartValue;
    /** The groom whose hierarchy the drag shows its work in, and its value before. */
    scene::PlugRef mHierarchyAt{scene::kNoNode, -1, -1};
    scene::PlugValue mHierarchyStart;
    double mValue = 1.0;
};

void registerLockHandles()
{
    if (nodeHandles::registry().find("lila.groomLocks"))
        return;
    nodeHandles::add({
        .id = "lila.groomLocks",
        .nodeType = "curve",
        .draw =
            [](OverlayPainter& p, const nodeHandles::Context& c, int hovered, int active) {
                const auto hs = lockHandles(c.node);
                for (size_t k = 0; k < hs.size(); ++k)
                {
                    if (!hs[k])
                        continue;
                    const auto wc = c.view.project(hs[k]->widthCentre), we = c.view.project(hs[k]->widthAt);
                    const auto tc = c.view.project(hs[k]->twistCentre), te = c.view.project(hs[k]->twistAt);
                    const int wid = int(k) * 2, tid = wid + 1;
                    auto colour = [&](int id) {
                        return id == active ? glm::vec4(1.0f, 0.85f, 0.3f, 1.0f) : id == hovered ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) : glm::vec4(0.6f, 0.8f, 1.0f, 0.9f);
                    };
                    if (wc && we)
                    {
                        p.line(*wc, *we, 1.0f, glm::vec4(0.6f, 0.8f, 1.0f, 0.5f));
                        p.rect(QRectF(we->x() - 4.0, we->y() - 4.0, 8.0, 8.0), colour(wid));
                    }
                    if (tc && te)
                    {
                        p.line(*tc, *te, 1.0f, glm::vec4(0.6f, 0.8f, 1.0f, 0.5f));
                        p.disc(*te, 4.5f, colour(tid));
                    }
                }
            },
        .pick =
            [](QPointF px, const nodeHandles::Context& c) {
                const auto hs = lockHandles(c.node);
                int best = -1;
                double bestD = kHandleGrabPx;
                for (size_t k = 0; k < hs.size(); ++k)
                {
                    if (!hs[k])
                        continue;
                    for (int which = 0; which < 2; ++which)
                        if (auto q = c.view.project(which ? hs[k]->twistAt : hs[k]->widthAt))
                            if (double d = QLineF(*q, px).length(); d < bestD)
                            {
                                bestD = d;
                                best = int(k) * 2 + which;
                            }
                }
                return best;
            },
        .begin = [](int handle, QPointF px, const nodeHandles::Context& c) -> std::unique_ptr<nodeHandles::Drag> {
            const auto hs = lockHandles(c.node);
            const size_t k = size_t(handle / 2);
            if (handle < 0 || k >= hs.size() || !hs[k])
                return nullptr;
            return std::make_unique<LockHandleDrag>(c.node.id(), k, handle % 2 == 1, *hs[k], px, c.view);
        },
    });
}

/** "widthScale" as "Width Scale". */
QString groomLabel(const std::string& id)
{
    QString out;
    for (size_t i = 0; i < id.size(); ++i)
    {
        const QChar c(id[i]);
        if (i == 0)
            out += c.toUpper();
        else if (c.isUpper())
            out += QString(" ") + c;
        else
            out += c;
    }
    return out;
}

/** A lock style's params as fields; an edit sets that param in the groom's graph. */
QWidget* styleFields(const std::string& groomPath, const std::string& node, groom::Style kind, const std::string& paramsJson, bool enabled)
{
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 0, 0, 6);
    form->setSpacing(2);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const json values = json::parse(paramsJson, nullptr, false);
    auto set = [groomPath, node](const std::string& name, const json& value) {
        runUi("groomStack", {{"args", {groomPath}}, {"edit", "set"}, {"node", node}, {"params", json{{name, value}}.dump()}});
    };
    for (const groom::LockStyleParam& p : groom::lockStyleParams(kind))
    {
        QWidget* field = nullptr;
        if (p.kind == 's')
        {
            auto* edit = new QLineEdit(w);
            edit->setText(QString::fromStdString(values.is_object() ? values.value(p.name, std::string()) : std::string()));
            edit->setPlaceholderText("Default");
            QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, set, name = p.name] { set(name, edit->text().toStdString()); });
            field = edit;
        }
        else
        {
            const bool integer = p.kind == 'i';
            QDoubleSpinBox* box = controls::valueField(p.min.value_or(-1e6), p.max.value_or(1e6), integer ? 0 : 3, {}, w);
            const double value = values.is_object() && values.contains(p.name) && values[p.name].is_number() ? values[p.name].get<double>() : p.defaultValue;
            box->setValue(value);
            QObject::connect(box, &QDoubleSpinBox::editingFinished, box, [box, set, name = p.name, integer, value] {
                if (box->value() == value)
                    return;
                set(name, integer ? json(int64_t(std::llround(box->value()))) : json(box->value()));
            });
            field = box;
        }
        field->setToolTip(QString::fromStdString(p.doc));
        field->setEnabled(enabled);
        form->addRow(groomLabel(p.name), field);
    }
    return w;
}

/**
 * A look node's settings. Dragging a number previews it (previewLook: on the GPU when it can) without an undo step; the
 * release sets it once, undoable. A typed value or a checkbox sets it straight away.
 */
QWidget* lookFields(NodeId groom, const std::string& groomPath, const std::string& node, const json& params, bool enabled)
{
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 0, 0, 6);
    form->setSpacing(2);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto commit = [groomPath, node](const std::string& name, const json& value) {
        runUi("groomLook", {{"args", {groomPath}}, {"edit", "set"}, {"node", node}, {"params", json{{name, value}}.dump()}});
    };
    for (const json& p : params)
    {
        const std::string name = p.value("name", std::string());
        const std::string kind = p.value("kind", std::string("f"));
        QWidget* field = nullptr;
        if (kind == "b")
        {
            auto* box = new QCheckBox(w);
            box->setChecked(p.value("value", false));
            QObject::connect(box, &QCheckBox::toggled, box, [commit, name](bool on) { commit(name, on); });
            field = box;
        }
        else
        {
            const bool integer = kind == "i";
            QDoubleSpinBox* box = controls::valueField(p.value("min", -1e6), p.value("max", 1e6), integer ? 0 : 3, {}, w);
            // Typed values set on Enter; a drag previews on every step.
            box->setKeyboardTracking(false);
            box->setValue(p["value"].get<double>());
            setSpinDefault(box, p["default"].get<double>());
            auto previewing = std::make_shared<bool>(false);
            QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [groom, node, name, previewing](double x) {
                if (QApplication::mouseButtons() == Qt::NoButton)
                    return;
                *previewing = true;
                groom::previewLook(Graph::current(), groom, node, {{name, x}});
                viewportTools::requestRedraw();
            });
            QObject::connect(box, &QDoubleSpinBox::editingFinished, box, [box, groom, commit, name, integer, previewing, start = p["value"]] {
                if (*previewing)
                {
                    *previewing = false;
                    groom::endPreviewLook(Graph::current(), groom);
                }
                const json value = integer ? json(int64_t(std::llround(box->value()))) : json(box->value());
                if (value != start)
                    commit(name, value);
                viewportTools::requestRedraw();
            });
            field = box;
        }
        field->setToolTip(QString::fromStdString(p.value("doc", std::string())));
        field->setEnabled(enabled);
        form->addRow(groomLabel(name), field);
    }
    return w;
}

/** The groom's look nodes in the Attribute Editor: each with on/off, remove and its settings, and an add menu. */
QWidget* lookPanel(NodeId id)
{
    Graph& g = Graph::current();
    Node* part = g.node(id);
    Node* gr = part ? groom::findGroom(g, *part) : nullptr;
    if (!gr)
        return nullptr;
    const std::string path = gr->path();
    const NodeId groomId = gr->id();
    json look;
    try
    {
        look = ops::execute("groomLook", {{"args", {path}}});
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    if (look.empty())
    {
        auto* empty = new QLabel("No look nodes. Add Clump, Noise or Curl below.");
        controls::setRole(empty, "dim");
        v->addWidget(empty);
    }
    for (const json& n : look)
    {
        const std::string node = n["node"];
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        auto* on = new QCheckBox(QString::fromStdString(n["type"].get<std::string>()));
        on->setChecked(n.value("enabled", true));
        on->setToolTip(QString::fromStdString(node) + ": on or off.");
        QObject::connect(on, &QCheckBox::toggled, on, [path, node](bool) { runUi("groomLook", {{"args", {path}}, {"edit", "toggle"}, {"node", node}}); });
        row->addWidget(on, 1);
        auto* remove = new QToolButton;
        remove->setIcon(icon("trash"));
        remove->setToolTip("Remove");
        remove->setAutoRaise(true);
        QObject::connect(remove, &QToolButton::clicked, remove, [path, node] { runUi("groomLook", {{"args", {path}}, {"edit", "remove"}, {"node", node}}); });
        row->addWidget(remove);
        v->addLayout(row);
        v->addWidget(lookFields(groomId, path, node, n["params"], n.value("enabled", true)));
    }
    auto* add = new QComboBox;
    add->addItem("Add Look...");
    for (const std::string& type : groom::groomLookTypes())
        add->addItem(QString::fromStdString(type), QString::fromStdString(type));
    QObject::connect(add, &QComboBox::activated, add, [add, path](int i) {
        if (i <= 0)
            return;
        const std::string type = add->itemData(i).toString().toStdString();
        QTimer::singleShot(0, [path, type] { runUi("groomLook", {{"args", {path}}, {"edit", "add"}, {"type", type}}); });
    });
    v->addWidget(add);
    return w;
}

/** The groom's lock styles in the Attribute Editor: each with on/off, up, down, remove and its settings, and an add menu. */
QWidget* stackPanel(NodeId id)
{
    Graph& g = Graph::current();
    Node* part = g.node(id);
    Node* gr = part ? groom::findGroom(g, *part) : nullptr;
    if (!gr)
        return nullptr;
    g.evaluate();
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    const std::string path = gr->path();
    const int locks = int(gr->as<int64_t>(gr->type().attrIndex("lockCount")));
    v->addWidget(new QLabel(QString("%1 locks").arg(locks)));
    const std::string& graphText = gr->as<std::string>(gr->type().attrIndex("graph"));
    const std::string graph = graphText.empty() ? groom::groomGraph::defaultGraph() : graphText;
    std::vector<groom::groomGraph::StyleEntry> styles;
    try
    {
        styles = groom::groomGraph::styles(graph);
    }
    catch (const std::exception& e)
    {
        v->addWidget(new QLabel(QString::fromStdString(e.what())));
    }
    for (size_t at = 0; at < styles.size(); ++at)
    {
        const groom::groomGraph::StyleEntry& style = styles[at];
        const std::string node = style.node;
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        auto* on = new QCheckBox(groomLabel(groom::styleName(style.kind)));
        on->setChecked(style.enabled);
        on->setToolTip(QString::fromStdString(node) + ": on or off.");
        QObject::connect(on, &QCheckBox::toggled, on, [path, node](bool) { runUi("groomStack", {{"args", {path}}, {"edit", "toggle"}, {"node", node}}); });
        row->addWidget(on, 1);
        auto tool = [&](const char* iconName, const char* tip, bool enabled, std::function<void()> fn) {
            auto* b = new QToolButton;
            b->setIcon(icon(iconName));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            b->setEnabled(enabled);
            QObject::connect(b, &QToolButton::clicked, b, std::move(fn));
            row->addWidget(b);
        };
        const int i = int(at);
        tool("chevron-up", "Move Up", at > 0, [path, node, i] { runUi("groomStack", {{"args", {path}}, {"edit", "move"}, {"node", node}, {"to", i - 1}}); });
        tool("chevron-down", "Move Down", at + 1 < styles.size(),
             [path, node, i] { runUi("groomStack", {{"args", {path}}, {"edit", "move"}, {"node", node}, {"to", i + 1}}); });
        tool("trash", "Remove", true, [path, node] { runUi("groomStack", {{"args", {path}}, {"edit", "remove"}, {"node", node}}); });
        v->addLayout(row);
        v->addWidget(styleFields(path, node, style.kind, groom::groomGraph::params(graph, node), style.enabled));
    }
    auto* add = new QComboBox;
    add->addItem("Add Style...");
    for (int k = 0; k < int(groom::Style::Count); ++k)
    {
        QString n = groom::styleName(groom::Style(k));
        add->addItem(n.left(1).toUpper() + n.mid(1), QString(groom::styleName(groom::Style(k))));
    }
    QObject::connect(add, &QComboBox::activated, add, [add, path](int i) {
        if (i <= 0)
            return;
        const std::string style = add->itemData(i).toString().toStdString();
        QTimer::singleShot(0, [path, style] { runUi("groomAddStyle", {{"args", {path}}, {"style", style}}); });
    });
    v->addWidget(add);
    auto* preset = new QComboBox;
    preset->addItem("Apply Preset...");
    for (const groom::presets::Preset& pr : groom::presets::list())
    {
        preset->addItem(QString::fromStdString(pr.name + (pr.guides ? "  (with guides)" : "")), QString::fromStdString(pr.id));
        preset->setItemData(preset->count() - 1, QString::fromStdString(pr.doc), Qt::ToolTipRole);
    }
    QObject::connect(preset, &QComboBox::activated, preset, [preset, path](int i) {
        if (i <= 0)
            return;
        const std::string id = preset->itemData(i).toString().toStdString();
        QTimer::singleShot(0, [path, id] { runUi("groomLoadPreset", {{"args", {path}}, {"preset", id}}); });
    });
    v->addWidget(preset);
    auto* buttons = new QHBoxLayout;
    auto button = [&](const char* text, const char* tip, std::function<void()> fn) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
        buttons->addWidget(b);
    };
    button("Draw", "Draw Hair tool: draw locks, braids, partings and presets on the scalp.", [] { viewportTools::activate("groom.drawHair"); });
    button("Settle", "Settle the guides under gravity onto the scalp and each other.", [path] { runUiWithOptions("groomSettle", argsCall({path})); });
    button("Shell", "One mesh over all the locks, ready for retopology.", [path] { runUiWithOptions("groomShell", argsCall({path})); });
    button("Save Preset", "Save the lock settings and styles, and optionally the guides, as a hairstyle preset.", [path] {
        bool ok = false;
        const QString name = QInputDialog::getText(QApplication::activeWindow(), "Save Hair Preset", "Name", QLineEdit::Normal, {}, &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        const QStringList kinds = {"Style only", "Style and guides"};
        const QString kind = QInputDialog::getItem(QApplication::activeWindow(), "Save Hair Preset", "Keep", kinds, 0, false, &ok);
        if (ok)
            runUi("groomSavePreset", {{"args", {path}}, {"name", name.trimmed().toStdString()}, {"guides", kind == kinds[1]}});
    });
    v->addLayout(buttons);
    return w;
}
} // namespace

void showGroomPalette(GroomPalettePage page)
{
    // A layout that closed the docked toolkit gets the floating palette back when a groom tool starts.
    if (QDockWidget* d = groomPaletteDock(); d && d->isHidden() && !d->isFloating())
        d->setFloating(true);
    panels::show(kGroomPaletteId);
    if (gGroomPalette)
        gGroomPalette->setPage(page);
    panels::placeToolPalette(groomPaletteDock());
}

void hideGroomPalette()
{
    // Docked, it is a toolkit the layout keeps; only the floating palette follows the tool.
    if (QDockWidget* d = groomPaletteDock(); d && d->isFloating())
        d->hide();
}

namespace
{
/** A clay node, or the mesh a clay surface feeds: live geometry hair cannot bind to. */
bool isClay(Graph& g, const Node& n)
{
    if (n.type().name.starts_with("clay"))
        return true;
    if (!n.type().isA("mesh"))
        return false;
    const std::optional<scene::PlugRef> src = g.source({n.id(), scene::MeshNode::kInMesh, -1});
    const Node* from = src ? g.node(src->node) : nullptr;
    return from && from->type().isA("claySurface");
}

bool canGrowHair(Graph& g, const Node& n) { return n.type().isA("mesh") || isClay(g, n); }
} // namespace

NodeId groomScalpUnder(const ViewInfo& view, QPointF at)
{
    Graph& g = Graph::current();
    NodeId hit = scene::kNoNode;
    for (NodeId id : paintSelect::objectsUnder(view, at, 0.0))
        if (Node* n = g.node(id); n && canGrowHair(g, *n))
            hit = id;
    viewportTools::setHoverHighlight(hit);
    return hit;
}

Node* startGroomOn(NodeId scalp)
{
    Graph& g = Graph::current();
    Node* n = g.node(scalp);
    if (!n || !canGrowHair(g, *n))
        return nullptr;
    viewportTools::setHoverHighlight(scene::kNoNode);
    std::string mesh = n->path();
    if (isClay(g, *n))
    {
        // Roots bind to faces, which a live clay surface rebuilds on every edit: hair grows from a baked copy.
        const json baked = runUi("convertClayToMesh", argsCall({mesh}));
        if (!baked.is_string())
            return nullptr;
        log::info("Hair grows on a mesh: baked {} to {}; the clay is kept, hidden", n->name(), baked.get<std::string>());
        mesh = baked.get<std::string>();
    }
    const json made = runUi("createGroom", argsCall({mesh}));
    return made.is_object() ? g.find(made.value("groom", std::string())) : nullptr;
}

Node* groomForSelection()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = groom::findGroom(g, *n))
                return gr;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && canGrowHair(g, *n))
            return startGroomOn(id);
    return selectedGroom(g);
}

void syncGroomPalette()
{
    if (gGroomPalette)
        gGroomPalette->sync();
    viewportTools::requestRedraw();
}

bool groomKeysShown() { return PrefStore::get().value<bool>("general", kGroomKeysPref, true); }

void registerGroomUi()
{
    groom::setLookDeviceFactory([] { return makeGpuLookDevice(); });
    panels::add({.id = kGroomPaletteId,
                 .title = "Groom",
                 .create = [](QWidget* parent) -> QWidget* { return gGroomPalette = new GroomPalette(parent); },
                 .area = Qt::LeftDockWidgetArea,
                 .icon = "feather",
                 .open = false,
                 .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                 .floatFirst = true,
                 .toolPalette = true});
    if (!viewportTools::registry().find("groom.drawHair"))
        viewportTools::add({.id = "groom.drawHair", .label = "Draw Hair", .icon = "brush", .create = [] { return std::make_unique<DrawHairTool>(); }});
    actions::add({.id = "groom.drawHair",
                  .label = "Draw Hair",
                  .category = "Groom",
                  .run = [] { viewportTools::activate("groom.drawHair"); },
                  .doc = "Draws locks, braids, ropes and ringlets on the scalp with pen pressure as width, parts the hair, and places "
                         "ponytails, buns and bangs. Select a groom, or a mesh to start one on it.",
                  .python = "lila.ui.run_action(\"groom.drawHair\")"});
    actions::add({.id = "groom.create",
                  .label = "New Groom",
                  .category = "Groom",
                  .doc = "Starts a groom on the selected mesh: empty guides with X symmetry, the groom node and its locks mesh.",
                  .python = ops::toPython("createGroom", json::object()),
                  .command = "createGroom",
                  .call = json::object()});
    actions::add({.id = "groom.settle",
                  .label = "Settle Hair",
                  .category = "Groom",
                  .doc = "Settles the selected groom's guides under gravity: roots stay, locks rest on the scalp and on each other.",
                  .python = ops::toPython("groomSettle", json::object()),
                  .command = "groomSettle",
                  .call = json::object()});
    actions::add({.id = "groom.shell",
                  .label = "Hair Shell",
                  .category = "Groom",
                  .doc = "One watertight mesh over the selected groom's locks, joined smoothly, ready for retopology.",
                  .python = ops::toPython("groomShell", json::object()),
                  .command = "groomShell",
                  .call = json::object()});
    for (int k = 0; k < int(groom::Style::Count); ++k)
    {
        const std::string n = groom::styleName(groom::Style(k));
        std::string label = n;
        label[0] = char(std::toupper(label[0]));
        actions::add({.id = "groom.style." + n,
                      .label = label,
                      .category = "Groom",
                      .doc = "Adds " + n + " to the selected groom's lock styles.",
                      .python = ops::toPython("groomAddStyle", {{"style", n}}),
                      .command = "groomAddStyle",
                      .call = {{"style", n}}});
    }
    for (const char* t : {"groom", "curve", "mesh"})
    {
        attributeEditor::addSection(t, "Groom", [](NodeId id) { return stackPanel(id); });
        attributeEditor::addSection(t, "Hair Look", [](NodeId id) { return lookPanel(id); });
    }
    // The Groom section shows the graph's lock styles: rebuilt when the graph changes (an edit, undo, a script).
    attributeEditor::filters().put("lila.groomGraph", {.typeName = "groom", .triggers = {"graph", "lockCount"}});
    registerLockHandles();
    registerGroomBrushTool();
    actions::add({.id = "groom.export",
                  .label = "Export Groom...",
                  .category = "Groom",
                  .run =
                      [] {
                          const QString file = fileDialog::getSaveFileName(QApplication::activeWindow(), "Export Groom", {},
                                                                           "Unreal groom (*.abc);;USD curves (*.usd *.usda *.usdc);;GroomLab groom (*.groom)");
                          if (!file.isEmpty())
                              actions::runWithOptions("exportGroom", {{"path", file.toStdString()}});
                      },
                  .doc = "Writes the selected groom's strands for Unreal or Unity (.abc with groom_* attributes and guides), as USD curves, "
                         "or as a GroomLab .groom file.",
                  .python = "exportGroom(\"HairGroom\", path=\"hair.abc\", start=1, end=48)",
                  .keywords = {"alembic", "abc", "unreal", "groom", "usd", "hair", "export"}});
    actions::add({.id = "groom.import",
                  .label = "Import Groom...",
                  .category = "Groom",
                  .run =
                      [] {
                          const QString file = fileDialog::getOpenFileName(QApplication::activeWindow(), "Import Groom", {}, "GroomLab groom (*.groom)");
                          if (!file.isEmpty())
                              actions::runWithOptions("importGroom", {{"args", {file.toStdString()}}});
                      },
                  .doc = "Reads a GroomLab .groom file into a new groom, on the selected mesh or the file's own scalp.",
                  .python = "importGroom(\"hair.groom\", mesh=\"Head\")",
                  .keywords = {"groomlab", "groom", "hair", "import"}});
    actions::add({.id = "groom.importYeti",
                  .label = "Import Yeti...",
                  .category = "Groom",
                  .run =
                      [] {
                          const QString file = fileDialog::getOpenFileName(QApplication::activeWindow(), "Import Yeti", {}, "Maya ASCII (*.ma)");
                          if (!file.isEmpty())
                              actions::runWithOptions("importYeti", {{"args", {file.toStdString()}}});
                      },
                  .doc = "Reads the Yeti grooms of a Maya ASCII file into grooms, without Maya or Yeti. Import the .ma first so its scalp "
                         "meshes are in the scene.",
                  .python = "importYeti(\"scene.ma\")",
                  .keywords = {"yeti", "peregrine", "maya", "groom", "hair", "fur", "import"}});
    actions::add({.id = "groom.sendToUnreal",
                  .label = "Send Groom to Unreal",
                  .category = "Groom",
                  .doc = "Sends the selected groom to the connected Unreal editor as a groom asset with its guides and M_LilaHair, built on "
                         "MF_LilaHair. Sending again reimports the same asset.",
                  .python = ops::toPython("unrealSendGroom", json::object()),
                  .command = "unrealSendGroom",
                  .call = json::object(),
                  .keywords = {"unreal", "groom", "hair", "send", "lilalink", "alembic"}});
    actions::add({.id = "groom.cards",
                  .label = "Hair Cards Preset",
                  .category = "Groom",
                  .doc = "Gives the selected groom a Cards node set from a hero, mid or low preset; outCards holds the cards.",
                  .python = ops::toPython("groomCards", {{"lod", "mid"}}),
                  .command = "groomCards",
                  .call = json::object(),
                  .keywords = {"cards", "lod", "game", "hair cards", "groom"}});
    actions::add({.id = "groom.bakeCards",
                  .label = "Bake Hair Cards...",
                  .category = "Groom",
                  .run =
                      [] {
                          const QString folder = fileDialog::getExistingDirectory(QApplication::activeWindow(), "Bake Hair Cards");
                          if (!folder.isEmpty())
                              actions::runWithOptions("groomBakeCards", {{"folder", folder.toStdString()}});
                      },
                  .doc = "Bakes the selected groom's cards into texture maps and an atlas in a folder; the cards keep their cells from it.",
                  .python = ops::toPython("groomBakeCards", {{"folder", "cards"}}),
                  .keywords = {"cards", "bake", "atlas", "textures", "groom"}});
}
} // namespace lila::ui
