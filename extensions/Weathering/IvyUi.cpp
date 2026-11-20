// The Ivy brush: drag over a mesh and ivy grows along the stroke (ivyPaint); Ctrl-drag erases. Plants and settings sit in
// a palette panel. Also the Level > Weathering menu items for ivy.

#include "Ivy.h"
#include "IvyPanelKit.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/ExtensionScope.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Panels.h>
#include <lila/Prefs.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QDockWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>

#include <array>
#include <memory>

namespace weathering
{
namespace ivyui
{
using namespace lila;
using namespace lila::ui;
using json = nlohmann::json;
namespace kit = weathering::ivykit;

constexpr const char* kPrefix = "/weatheringIvy";
constexpr const char* kPaletteId = "weathering.ivyPalette";
constexpr const char* kToolId = "weathering.ivyBrush";

struct PlantTile
{
    const char* plant;
    const char* icon;
    const char* label;
    const char* tip;
};

constexpr std::array<PlantTile, 5> kPlantTiles = {{
    {"ivy", "plant-2", "Ivy", "Climbing ivy: dense leaves, hangs over ledges."},
    {"vine", "leaf", "Vine", "Long vines with broad leaves that drape."},
    {"roots", "git-fork", "Roots", "Bare roots spreading every way."},
    {"moss", "grain", "Moss", "Flat, dense, short tufted strands."},
    {"tendril", "spiral", "Tendril", "Smooth curling stylized arcs."},
}};

struct Settings
{
    std::string plant = "ivy";
    bool toon = false;
    bool erase = false;
    double radius = 20.0; // erase radius, cm
    double length = 0.0;  // 0: the plant's own
    double gravity = -1.0;
    double smoothness = -1.0;
    double leafDensity = -1.0;
    double leafSize = 0.0;
};

std::string key(const char* k) { return std::string(kPrefix) + "/" + k; }

Settings& settings()
{
    static Settings s = [] {
        Settings c;
        const PrefStore& p = PrefStore::get();
        c.plant = p.value<std::string>("general", key("plant"), c.plant);
        c.toon = p.value<bool>("general", key("toon"), c.toon);
        c.radius = std::max(1.0, p.value<double>("general", key("radius"), c.radius));
        c.length = p.value<double>("general", key("length"), c.length);
        c.gravity = p.value<double>("general", key("gravity"), c.gravity);
        c.smoothness = p.value<double>("general", key("smoothness"), c.smoothness);
        c.leafDensity = p.value<double>("general", key("leafDensity"), c.leafDensity);
        c.leafSize = p.value<double>("general", key("leafSize"), c.leafSize);
        return c;
    }();
    return s;
}

void syncPalette();

void save()
{
    const Settings& c = settings();
    PrefStore& p = PrefStore::get();
    p.set("general", key("plant"), c.plant);
    p.set("general", key("toon"), c.toon);
    p.set("general", key("radius"), c.radius);
    p.set("general", key("length"), c.length);
    p.set("general", key("gravity"), c.gravity);
    p.set("general", key("smoothness"), c.smoothness);
    p.set("general", key("leafDensity"), c.leafDensity);
    p.set("general", key("leafSize"), c.leafSize);
    syncPalette();
    viewportTools::requestRedraw();
}

/** The brush's settings as ivyPaint flags; values left at their "plant's own" mark are not sent. */
json paintFlags()
{
    const Settings& s = settings();
    json f = {{"plant", s.plant}, {"style", s.toon ? "toon" : "realistic"}};
    if (s.length > 0.0)
        f["length"] = s.length;
    if (s.gravity >= 0.0)
        f["gravity"] = s.gravity;
    if (s.smoothness >= 0.0)
        f["smoothness"] = s.smoothness;
    if (s.leafDensity >= 0.0)
        f["leafDensity"] = s.leafDensity;
    if (s.leafSize > 0.0)
        f["leafSize"] = s.leafSize;
    return f;
}

class Palette final : public QWidget
{
public:
    explicit Palette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "weatheringIvyPalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        std::vector<QWidget*> tiles;
        for (size_t i = 0; i < kPlantTiles.size(); ++i)
        {
            const PlantTile& def = kPlantTiles[i];
            QToolButton* b = kit::tile(def.icon, def.label, QString("%1: %2").arg(def.label, def.tip), this);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            connect(b, &QToolButton::clicked, this, [plant = std::string(def.plant)] {
                settings().plant = plant;
                save();
            });
            tiles.push_back(b);
            mTiles[i] = b;
        }
        v->addWidget(kit::section("Plants", kit::grid(tiles, 3, this), this, kSections));

        auto* brushBox = new QWidget(this);
        QFormLayout* brushForm = kit::form(brushBox);
        mMode = kit::segmented(brushBox);
        mMode.add("Grow", "Grow ivy along the stroke", 0);
        mMode.add("Erase", "Remove strokes and seeds under the brush (Ctrl+drag)", 1);
        connect(mMode.group, &QButtonGroup::idClicked, this, [](int id) {
            settings().erase = id == 1;
            save();
        });
        brushForm->addRow("Mode", mMode.widget);
        mStyle = kit::segmented(brushBox);
        mStyle.add("Realistic", "Realistic leaves and natural wander", 0);
        mStyle.add("Toon", "Chunky leaves and smooth arcs", 1);
        connect(mStyle.group, &QButtonGroup::idClicked, this, [](int id) {
            settings().toon = id == 1;
            save();
        });
        brushForm->addRow("Style", mStyle.widget);
        mRadius = kit::valueField(1.0, 1e5, 0, "cm", brushBox);
        mRadius->setToolTip("Radius: erase reach  ([ ])");
        connect(mRadius, &QDoubleSpinBox::valueChanged, this, [](double x) {
            settings().radius = x;
            save();
        });
        brushForm->addRow("Radius", mRadius);
        v->addWidget(kit::section("Brush", brushBox, this, kSections));

        // Growth: 0 or below means the plant's own value, shown as "Plant".
        auto* growBox = new QWidget(this);
        QFormLayout* growForm = kit::form(growBox);
        auto field = [&](const char* label, const char* tip, double lo, double hi, int decimals, const QString& unit, double Settings::* member) {
            QDoubleSpinBox* f = kit::valueField(lo, hi, decimals, unit, growBox);
            f->setSpecialValueText("Plant");
            f->setToolTip(QString("%1: %2. At the bottom of its range it uses the plant's own value.").arg(label, tip));
            connect(f, &QDoubleSpinBox::valueChanged, this, [member, lo](double x) {
                settings().*member = x <= lo ? (lo < 0.0 ? -1.0 : 0.0) : x;
                save();
            });
            growForm->addRow(label, f);
            return f;
        };
        mLength = field("Length", "longest growth from the stroke start", 0.0, 1e5, 0, "cm", &Settings::length);
        mGravity = field("Gravity", "how much hangs over ledges", -0.05, 1.0, 2, {}, &Settings::gravity);
        mSmooth = field("Smoothness", "natural wander to smooth arcs", -0.05, 1.0, 2, {}, &Settings::smoothness);
        mLeaves = field("Leaves", "leaves per metre", -1.0, 1000.0, 0, {}, &Settings::leafDensity);
        mLeafSize = field("Leaf Size", "leaf length", 0.0, 1000.0, 1, "cm", &Settings::leafSize);
        for (QDoubleSpinBox* f : {mGravity, mSmooth})
            f->setSingleStep(0.05);
        v->addWidget(kit::section("Growth", growBox, this, kSections));

        auto* optionsBox = new QWidget(this);
        auto* options = new QVBoxLayout(optionsBox);
        options->setContentsMargins(0, 0, 0, 0);
        mKeys = new QCheckBox("Show Keys", optionsBox);
        mKeys->setToolTip("Show Keys: key hints on screen, in every tool");
        connect(mKeys, &QCheckBox::clicked, this, [](bool on) {
            PrefStore::get().set("general", kShowKeysPref, on);
            viewportTools::requestRedraw();
        });
        options->addWidget(mKeys);
        v->addWidget(kit::section("Options", optionsBox, this, kSections));
        v->addStretch();
        sync();
    }

    void sync()
    {
        const Settings& s = settings();
        for (size_t i = 0; i < kPlantTiles.size(); ++i)
            mTiles[i]->setChecked(s.plant == kPlantTiles[i].plant);
        const QSignalBlocker b1(mMode.group), b2(mStyle.group);
        mMode.setCurrent(s.erase ? 1 : 0);
        mStyle.setCurrent(s.toon ? 1 : 0);
        const std::pair<QDoubleSpinBox*, double> values[] = {{mRadius, s.radius},
                                                             {mLength, s.length},
                                                             {mGravity, s.gravity < 0.0 ? -0.05 : s.gravity},
                                                             {mSmooth, s.smoothness < 0.0 ? -0.05 : s.smoothness},
                                                             {mLeaves, s.leafDensity < 0.0 ? -1.0 : s.leafDensity},
                                                             {mLeafSize, s.leafSize}};
        for (auto [box, value] : values)
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        mRadius->setEnabled(s.erase);
        mKeys->setChecked(keysShown());
    }

private:
    std::array<QToolButton*, kPlantTiles.size()> mTiles{};
    kit::Segmented mMode, mStyle;
    QDoubleSpinBox *mRadius = nullptr, *mLength = nullptr, *mGravity = nullptr, *mSmooth = nullptr, *mLeaves = nullptr, *mLeafSize = nullptr;
    QCheckBox* mKeys = nullptr;
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

const char* plantLabel(const std::string& plant)
{
    for (const PlantTile& t : kPlantTiles)
        if (plant == t.plant)
            return t.label;
    return "Ivy";
}

/** Drag over the selected meshes: ivy grows along the stroke; Ctrl erases. */
class IvyBrush final : public ViewportTool
{
public:
    const char* name() const override { return "Ivy Brush"; }

    void activate() override
    {
        refresh();
        panels::show(kPaletteId);
        panels::placeToolPalette(paletteDock());
        syncPalette();
        viewportTools::requestRedraw();
    }

    void deactivate() override
    {
        mStroke.clear();
        if (QDockWidget* d = paletteDock())
            d->hide();
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        refresh();
        if (mTargets.empty())
        {
            log::warn("Ivy Brush: select the meshes to paint ivy on");
            return true;
        }
        mStroke.clear();
        mHost = -1;
        mErase = settings().erase || e->modifiers().testFlag(Qt::ControlModifier);
        sample(e->position(), view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (!(e->buttons() & Qt::LeftButton) || mTargets.empty())
        {
            viewportTools::requestRedraw();
            return false;
        }
        sample(e->position(), view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mStroke.empty() || mHost < 0)
            return false;
        json pts = json::array();
        for (const glm::dvec3& q : mStroke)
            pts.push_back({q.x, q.y, q.z});
        if (!mErase && pts.size() == 1)
            pts.push_back(pts[0]);
        json call = paintFlags();
        call["args"] = {mTargets[size_t(mHost)].path};
        call["points"] = pts;
        if (mErase)
        {
            call["erase"] = true;
            call["radius"] = settings().radius;
        }
        mStroke.clear();
        // After this event, so the scene changes outside the viewport's event handling.
        QTimer::singleShot(0, lila::ui::scope("Weathering"), [call] {
            try
            {
                ops::execute("ivyPaint", call, CallSource::UI);
            }
            catch (const std::exception& ex)
            {
                log::write(LogLevel::Warning, ex.what());
            }
        });
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return key(e); }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override { return key(*e); }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        const Settings& s = settings();
        if (mTargets.empty())
        {
            drawToolStatus(p, view, {.tool = name(), .prompt = "select the meshes to paint ivy on", .keys = {{"Esc", "exit"}}});
            return;
        }
        drawToolStatus(p, view,
                       {.tool = name(),
                        .mode = s.erase ? QString("Erase") : QString(plantLabel(s.plant)),
                        .fields = {{"style", QString(s.toon ? "toon" : "realistic")}, {"radius", QString("%1 cm").arg(s.radius, 0, 'f', 0)}}});
        if (keysShown())
            drawKeyList(p, QPointF(16.0, view.height - 84.0), {{"Drag", "Grow"}, {"Ctrl+drag", "Erase"}, {"[  ]", "Radius"}, {"Esc", "Exit"}});
        std::vector<QPointF> line;
        for (const glm::dvec3& q : mStroke)
            if (auto px = view.project(q))
                line.push_back(*px);
        if (line.size() >= 2)
            p.polyline(line, 2.0f, mErase ? glm::vec4(0.95f, 0.45f, 0.35f, 0.9f) : glm::vec4(0.45f, 0.85f, 0.4f, 0.9f));
        if (mErase || s.erase)
            p.disc(mHover, 3.0f, glm::vec4(0.95f, 0.95f, 0.95f, 0.6f));
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    struct Target
    {
        std::string path;
        std::unique_ptr<geo::MeshBvh> bvh;
    };

    void refresh()
    {
        mTargets.clear();
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        for (scene::NodeId id : g.selection())
        {
            scene::Node* n = g.node(id);
            if (!n || !n->type().isA("mesh") || n->type().attrIndex("outMesh") < 0)
                continue;
            const auto mesh = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
            if (!mesh || mesh->numFaces() == 0)
                continue;
            const glm::dmat4 xf = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            geo::Mesh world = *mesh;
            for (glm::dvec3& q : world.positions())
                q = glm::dvec3(xf * glm::dvec4(q, 1.0));
            mTargets.push_back({n->path(), std::make_unique<geo::MeshBvh>(world)});
        }
    }

    void sample(QPointF px, const ViewInfo& view)
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        double best = std::numeric_limits<double>::max();
        int host = -1;
        glm::dvec3 at;
        for (size_t k = 0; k < mTargets.size(); ++k)
        {
            geo::BvhHit hit;
            if (mTargets[k].bvh->raycast(o, d, hit) && hit.t < best)
            {
                best = hit.t;
                host = int(k);
                at = hit.point;
            }
        }
        if (host < 0)
            return;
        // A stroke stays on the mesh it started on.
        if (mHost >= 0 && host != mHost)
            return;
        mHost = host;
        const double spacing = std::max(view.unitsPerPixel(at) * 6.0, 0.5);
        if (mStroke.empty() || glm::distance(at, mStroke.back()) > spacing)
            mStroke.push_back(at);
        viewportTools::requestRedraw();
    }

    bool key(const QKeyEvent& e)
    {
        if (e.key() == Qt::Key_BracketLeft || e.key() == Qt::Key_BracketRight)
        {
            settings().radius = std::max(1.0, settings().radius * (e.key() == Qt::Key_BracketRight ? 1.25 : 0.8));
            save();
            return true;
        }
        if (e.key() == Qt::Key_Escape)
        {
            QTimer::singleShot(0, lila::ui::scope("Weathering"), [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    std::vector<Target> mTargets;
    std::vector<glm::dvec3> mStroke;
    int mHost = -1;
    bool mErase = false;
    QPointF mHover;
};
} // namespace ivyui

void registerIvyUi(lila::ext::Context& ctx)
{
    using namespace lila;
    using namespace lila::ui;
    namespace iu = ivyui;
    panels::add(ctx, {.id = iu::kPaletteId,
                      .title = "Ivy Brush",
                      .create = [](QWidget* parent) -> QWidget* { return iu::gPalette = new iu::Palette(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "plant-2",
                      .open = false,
                      .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                      .floatFirst = true,
                      .toolPalette = true});
    viewportTools::add(ctx, {.id = iu::kToolId, .label = "Ivy Brush", .icon = "plant-2", .create = [] {
                                 return std::unique_ptr<ViewportTool>(std::make_unique<iu::IvyBrush>());
                             }});
    actions::add(ctx, {.id = "weathering.ivy",
                       .label = "Ivy",
                       .category = "Weathering",
                       .doc = "Grow ivy over the selected meshes from where they meet the ground; it regrows when they change.",
                       .python = "ivy('Wall', plant='ivy', length=300)",
                       .command = "ivy",
                       .call = json::object(),
                       .icon = "plant-2",
                       .keywords = {"vine", "creeper", "overgrowth", "foliage", "climbing plant", "roots", "moss", "tendril", "ivy generator",
                                    "ivygen", "growth", "baga ivy", "scatter ivy"}});
    actions::add(ctx, {.id = "weathering.ivyBrush",
                       .label = "Ivy Brush",
                       .category = "Weathering",
                       .run = [] { viewportTools::activate(iu::kToolId); },
                       .doc = "Paint strokes on the selected meshes and ivy grows along them; Ctrl erases. Agents call ivyPaint.",
                       .python = "ivyPaint('Wall', points=[[0, 10, 50], [40, 120, 50]])",
                       .icon = "brush",
                       .keywords = {"paint ivy", "vine brush", "growth brush", "draw vines", "ivy paint"}});
    actions::add(ctx, {.id = "weathering.ivySend",
                       .label = "Send Ivy to Unreal",
                       .category = "Weathering",
                       .doc = "Send the selected ivy to Unreal as its own mesh with the foliage wind material.",
                       .python = "ivySend('Wall_ivy', nanite=True)",
                       .command = "ivySend",
                       .call = json::object(),
                       .icon = "send",
                       .keywords = {"export ivy", "ivy unreal"}});
}
} // namespace weathering
