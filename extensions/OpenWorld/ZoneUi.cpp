// Zones in the UI: the viewport overlay (coloured regions draped on the ground, the current layer's outlines and names,
// check markers), the Zones tool with its palette (paint, erase, draw an outline, pick), the Zones panel (layers,
// zones, attributes, counts), Level > Open World entries, the toolkit's Zones group and the option boxes.

#include "OpenWorld.h"
#include "ZoneScene.h"

#include "PanelKit.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/ExtensionScope.h>
#include <lila/Graph.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Panels.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QGuiApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <array>
#include <format>
#include <cmath>
#include <map>
#include <optional>

using namespace lila;
using namespace lila::ui;

namespace openworld::zones
{
namespace
{
using scene::Graph;
using scene::Node;

constexpr const char* kPaletteId = "openworld.zonePalette";
constexpr const char* kPanelId = "openworld.zones";
constexpr const char* kToolId = "openworld.zoneTool";
constexpr const char* kPrefix = "/openworldZones";

// ----------------------------------------------------------------------------------------------------------------------
// Shared state: which set, layer and zone the panel and the tool work on.

struct UiState
{
    uint64_t set = 0;
    std::string layer;
    /** Empty: the next stroke or outline makes a new zone. */
    std::string zone;
};
UiState& ui()
{
    static UiState s;
    return s;
}

/** The selected zone set, else the one last worked on, else the only one. */
Node* currentSet()
{
    Graph& g = Graph::current();
    for (scene::NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA(kSetType))
        {
            ui().set = id;
            return n;
        }
    if (Node* n = ui().set ? g.node(scene::NodeId(ui().set)) : nullptr; n && n->type().isA(kSetType))
        return n;
    const std::vector<scene::NodeId> all = zoneSets();
    if (all.size() == 1)
    {
        ui().set = all[0];
        return g.node(all[0]);
    }
    return nullptr;
}

/** The current layer of a spec: the remembered one, else the first. */
const Layer* currentLayer(const ZoneSpec& s)
{
    if (const Layer* l = s.layer(ui().layer))
        return l;
    if (s.layers.empty())
        return nullptr;
    ui().layer = s.layers.front().id;
    ui().zone.clear();
    return &s.layers.front();
}

/** A stroke being painted: a copy of the spec with its dabs, drawn instead of the stored one. */
struct Preview
{
    uint64_t set = 0;
    std::shared_ptr<ZoneSpec> spec;
    uint64_t revision = 0;
};
Preview gPreview;

/** The last check per set, with the spec text it was run on. */
struct CheckState
{
    std::shared_ptr<const ZoneSpec> spec;
    nlohmann::json result;
};
std::map<uint64_t, CheckState> gChecks;

void warnOnThrow(const std::function<void()>& f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}

std::string leaf(const std::string& path) { return path.substr(path.find_last_of('|') == std::string::npos ? 0 : path.find_last_of('|') + 1); }

QString display(const std::string& camel)
{
    std::string out;
    for (size_t i = 0; i < camel.size(); ++i)
    {
        if (i > 0 && std::isupper(uint8_t(camel[i])) && !std::isupper(uint8_t(camel[i - 1])))
            out += ' ';
        out += i == 0 ? char(std::toupper(uint8_t(camel[i]))) : camel[i];
    }
    return QString::fromStdString(out);
}

// ----------------------------------------------------------------------------------------------------------------------
// Overlay

glm::dvec3 draped(const Ground& g, Vec2 p) { return worldOf(p, g.heightAt(p) + 30.0); }

struct LayerDraw
{
    /** A run of cells along x, draped at every cell corner so neighbouring runs share their edges' points (no seams). */
    struct Quad
    {
        std::vector<glm::dvec3> top, bottom;
        glm::vec4 color;
    };
    struct Line
    {
        std::vector<glm::dvec3> pts;
        int zone = -1;
    };
    struct Label
    {
        glm::dvec3 at;
        QString text;
        int zone = -1;
    };
    std::vector<Quad> quads;
    std::vector<Line> lines;
    std::vector<Label> labels;
};

LayerDraw buildLayer(const Layer& l, const Ground& g, int resolution)
{
    LILA_ZONE();
    LayerDraw d;
    Vec2 lo(1e300), hi(-1e300);
    for (const Zone& z : l.zones)
    {
        Vec2 a, b;
        if (bounds(z, a, b))
        {
            lo = glm::min(lo, a);
            hi = glm::max(hi, b);
        }
    }
    if (lo.x > hi.x)
        return d;
    const double cell = std::max({(hi.x - lo.x) / resolution, (hi.y - lo.y) / resolution, 1.0});
    const Raster r = rasterize(l, lo, hi, cell);
    std::vector<Vec2> sums(l.zones.size(), Vec2(0.0));
    std::vector<int> counts(l.zones.size(), 0);
    for (int z = 0; z < r.nz; ++z)
        for (int x = 0; x < r.nx;)
        {
            const int w = r.winner[size_t(z) * size_t(r.nx) + size_t(x)];
            if (w < 0)
            {
                ++x;
                continue;
            }
            // Runs of one zone (and, in density layers, one weight step) become one quad, at most eight cells long so it drapes.
            auto level = [&](int xx) { return l.density ? int(weight(l.zones[size_t(w)], r.at(xx, z)) * 8.0) : 0; };
            const int lv = level(x);
            int x1 = x;
            while (x1 + 1 < r.nx && x1 - x < 7 && r.winner[size_t(z) * size_t(r.nx) + size_t(x1 + 1)] == w && level(x1 + 1) == lv)
                ++x1;
            for (int k = x; k <= x1; ++k)
            {
                sums[size_t(w)] += r.at(k, z);
                ++counts[size_t(w)];
            }
            const glm::vec3 c = zoneColor(l, l.zones[size_t(w)]);
            const float a = l.density ? std::clamp(float(lv + 1) / 8.0f, 0.1f, 1.0f) : 1.0f;
            const Vec2 p0 = r.origin + Vec2(x, z) * r.cell, p1 = r.origin + Vec2(x1 + 1, z + 1) * r.cell;
            LayerDraw::Quad q{{}, {}, glm::vec4(c, a)};
            for (int k = x; k <= x1 + 1; ++k)
            {
                const double px = r.origin.x + double(k) * r.cell;
                q.top.push_back(draped(g, {px, p0.y}));
                q.bottom.push_back(draped(g, {px, p1.y}));
            }
            d.quads.push_back(std::move(q));
            x = x1 + 1;
        }
    auto addRing = [&](const Polygon& ring, int zone) {
        LayerDraw::Line line{{}, zone};
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const Vec2 a = ring[i], b = ring[(i + 1) % ring.size()];
            const int n = std::clamp(int(glm::length(b - a) / cell), 1, 256);
            for (int k = 0; k < n; ++k)
                line.pts.push_back(draped(g, a + (b - a) * (double(k) / n)));
        }
        if (!line.pts.empty())
            line.pts.push_back(line.pts.front());
        d.lines.push_back(std::move(line));
    };
    for (size_t i = 0; i < l.zones.size(); ++i)
    {
        const Zone& z = l.zones[i];
        for (const Polygon& ring : z.polygons)
            addRing(ring, int(i));
        for (const Polygon& ring : z.holes)
            addRing(ring, int(i));
        if (z.hasMask())
        {
            std::vector<Polygon> rings, holes;
            maskOutline(z.mask, l.density ? 1 : 128, z.mask.cell, rings, holes);
            for (const Polygon& ring : rings)
                addRing(ring, int(i));
            for (const Polygon& ring : holes)
                addRing(ring, int(i));
        }
        // The name sits on the zone's own sample nearest the middle of its samples, so it lands on the zone whatever its shape.
        if (counts[i] > 0)
        {
            const Vec2 mean = sums[i] / double(counts[i]);
            Vec2 best = mean;
            double bestD = 1e300;
            for (int zz = 0; zz < r.nz; ++zz)
                for (int xx = 0; xx < r.nx; ++xx)
                    if (r.winner[size_t(zz) * size_t(r.nx) + size_t(xx)] == int(i))
                        if (const double dd = glm::length(r.at(xx, zz) - mean); dd < bestD)
                        {
                            bestD = dd;
                            best = r.at(xx, zz);
                        }
            d.labels.push_back({draped(g, best), QString::fromStdString(z.name.empty() ? z.id : z.name), int(i)});
        }
    }
    return d;
}

struct CachedLayer
{
    const void* spec = nullptr;
    uint64_t revision = 0;
    const void* ground = nullptr;
    LayerDraw draw;
};
std::map<std::pair<uint64_t, std::string>, CachedLayer> gDrawCache;

const LayerDraw& layerDraw(uint64_t set, const ZoneSpec& spec, uint64_t revision, const Layer& l, const Ground& g)
{
    CachedLayer& c = gDrawCache[{set, l.id}];
    if (c.spec != &spec || c.revision != revision || c.ground != g.h.get())
    {
        c.draw = buildLayer(l, g, revision ? 96 : 160);
        c.spec = &spec;
        c.revision = revision;
        c.ground = g.h.get();
    }
    return c.draw;
}

const glm::vec4 kWarn{1.0f, 0.62f, 0.2f, 1.0f};

/** Zones panels on screen; fills show while one is. */
int gPanelsShown = 0;

/** Someone is working on this set's zones: it is selected, a Zones panel is open or the Zones tool is on. */
bool editingZones(scene::NodeId set)
{
    if (gPanelsShown > 0 || viewportTools::activeId() == kToolId)
        return true;
    const auto& sel = Graph::current().selection();
    return std::find(sel.begin(), sel.end(), set) != sel.end();
}

/**
 * The one layer a set draws: the layer being worked on in the current set, else the visible layer on top (highest
 * priority). Drawing every layer stacks their fills into an opaque sheet over the map.
 */
const Layer* shownLayer(const ZoneSpec& spec, bool current)
{
    if (current)
        if (const Layer* l = spec.layer(ui().layer); l && l->visible)
            return l;
    const Layer* top = nullptr;
    for (const Layer& l : spec.layers)
        if (l.visible && (!top || l.priority > top->priority))
            top = &l;
    return top;
}

void drawZones(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    const Node* current = currentSet();
    for (scene::NodeId id : zoneSets())
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        if (const int oi = n->type().attrIndex(kOverlayAttr); oi >= 0)
            if (const bool* on = std::get_if<bool>(&n->value(oi)); on && !*on)
                continue;
        std::shared_ptr<const ZoneSpec> stored = specOf(*n);
        if (!stored)
            continue;
        const bool previewing = gPreview.spec && gPreview.set == id;
        const ZoneSpec& spec = previewing ? *gPreview.spec : *stored;
        const Ground ground = groundOf(groundPathOf(*n));
        double opacity = kDefaultFillOpacity;
        if (const int ai = n->type().attrIndex(kOpacityAttr); ai >= 0)
            if (const double* v = std::get_if<double>(&n->value(ai)))
                opacity = *v;
        if (const Layer* l = shownLayer(spec, n == current))
        {
            const uint64_t revision = previewing && l->id == ui().layer ? gPreview.revision : 0;
            const LayerDraw& d = layerDraw(id, spec, revision, *l, ground);
            // A built world shows its zones as outlines and names; the faint fill only while they are being worked on.
            const float a = editingZones(id) ? float(opacity) : 0.0f;
            for (size_t qi = 0; a > 0.0f && qi < d.quads.size(); ++qi)
            {
                const LayerDraw::Quad& q = d.quads[qi];
                const glm::vec4 color(glm::vec3(q.color), q.color.a * a);
                std::optional<QPointF> t0 = view.project(q.top[0]), b0 = view.project(q.bottom[0]);
                for (size_t k = 1; k < q.top.size(); ++k)
                {
                    const std::optional<QPointF> t1 = view.project(q.top[k]), b1 = view.project(q.bottom[k]);
                    if (t0 && b0 && t1 && b1)
                    {
                        p.triangle(*t0, *t1, *b1, color);
                        p.triangle(*t0, *b1, *b0, color);
                    }
                    t0 = t1;
                    b0 = b1;
                }
            }
            for (const LayerDraw::Line& line : d.lines)
            {
                std::vector<QPointF> pts;
                for (const glm::dvec3& c : line.pts)
                    if (auto s = view.project(c))
                        pts.push_back(*s);
                const bool sel = size_t(line.zone) < l->zones.size() && l->zones[size_t(line.zone)].id == ui().zone;
                const glm::vec3 c = zoneColor(*l, l->zones[size_t(line.zone)]);
                p.polyline(pts, sel ? 2.5f : 1.25f, glm::vec4(sel ? glm::vec3(1.0f) : c, sel ? 1.0f : 0.85f));
            }
            p.setShadow(true);
            int shown = 0;
            for (const LayerDraw::Label& lb : d.labels)
                if (auto s = view.project(lb.at); s && shown++ < 200)
                    p.text(*s, lb.text, glm::vec4(0.95f, 0.95f, 0.95f, 1.0f), OverlayPainter::Center);
            p.setShadow(false);
        }
        // Markers for the last check while the spec is the one it ran on.
        if (auto it = gChecks.find(id); it != gChecks.end() && it->second.spec == stored)
            for (const nlohmann::json& i : it->second.result.value("issues", nlohmann::json::array()))
                if (i.contains("at"))
                {
                    const Vec2 at(i["at"][0].get<double>(), i["at"][1].get<double>());
                    if (auto s = view.project(draped(ground, at)))
                    {
                        p.arc(*s, 7.0, 0.0, 360.0, 2.0f, kWarn);
                        p.setShadow(true);
                        p.text(*s + QPointF(11, 4), display(i.value("kind", std::string())), kWarn);
                        p.setShadow(false);
                    }
                }
    }
}

// ----------------------------------------------------------------------------------------------------------------------
// Ground picking

bool groundHit(const Ground& g, QPointF px, const ViewInfo& view, Vec2& out)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    if (std::abs(upOf(d)) < 1e-9)
        return false;
    // March along the ray until it drops under the ground, then bisect; a flat plane at 0 without a ground.
    if (!g.valid())
    {
        const double t = -upOf(o) / upOf(d);
        if (t <= 0.0)
            return false;
        out = planeOf(o + d * t);
        return true;
    }
    Vec2 lo, hi;
    g.extent(lo, hi);
    const double span = glm::length(hi - lo) * 2.0 + glm::length(planeOf(o) - (lo + hi) * 0.5);
    const double step = std::max(g.h->cell * 0.5, span / 4000.0);
    double t0 = 0.0;
    auto below = [&](double t) {
        const glm::dvec3 p = o + d * t;
        return upOf(p) < g.heightAt(planeOf(p));
    };
    if (below(0.0))
        return false;
    for (double t = step; t < span; t += step)
    {
        if (below(t))
        {
            double a = t0, b = t;
            for (int k = 0; k < 24; ++k)
            {
                const double m = (a + b) * 0.5;
                (below(m) ? b : a) = m;
            }
            out = planeOf(o + d * b);
            return true;
        }
        t0 = t;
    }
    // Past the ground's edge: fall onto the plane of its lowest point so painting off the map still works.
    const double base = g.h->range().x;
    const double t = (base - upOf(o)) / upOf(d);
    if (t <= 0.0)
        return false;
    out = planeOf(o + d * t);
    return true;
}

// ----------------------------------------------------------------------------------------------------------------------
// Tool settings and palette

enum class Mode
{
    Paint,
    Erase,
    Draw,
    Pick,
};

struct Settings
{
    Mode mode = Mode::Paint;
    double radius = 3000.0;
    double value = 1.0;
    double strength = 1.0;
    double softness = 0.3;
};

Settings& settings()
{
    static Settings s = [] {
        Settings c;
        const PrefStore& p = PrefStore::get();
        auto key = [](const char* k) { return std::string(kPrefix) + "/" + k; };
        c.mode = Mode(std::clamp(p.value<int>("general", key("mode"), 0), 0, 3));
        c.radius = std::max(10.0, p.value<double>("general", key("radius"), c.radius));
        c.value = std::clamp(p.value<double>("general", key("value"), c.value), 0.0, 1.0);
        c.strength = std::clamp(p.value<double>("general", key("strength"), c.strength), 0.0, 1.0);
        c.softness = std::clamp(p.value<double>("general", key("softness"), c.softness), 0.0, 1.0);
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
    p.set("general", key("mode"), int(c.mode));
    p.set("general", key("radius"), c.radius);
    p.set("general", key("value"), c.value);
    p.set("general", key("strength"), c.strength);
    p.set("general", key("softness"), c.softness);
    syncPalette();
    viewportTools::requestRedraw();
}

struct ModeButton
{
    Mode mode;
    const char* icon;
    const char* label;
    const char* tip;
};
constexpr std::array<ModeButton, 4> kModes = {{
    {Mode::Paint, "brush", "Paint", "Paint the zone's region; on density layers, its weight. Ctrl erases."},
    {Mode::Erase, "eraser", "Erase", "Erase the zone's painted region."},
    {Mode::Draw, "polygon", "Draw", "Click an outline; Enter or double-click closes it. Backspace removes the last point."},
    {Mode::Pick, "click", "Pick", "Click to read every layer here and make the zone under the cursor current."},
}};

const ModeButton& modeOf(Mode m) { return kModes[size_t(m)]; }

/** The menu of layer types; picking one adds a layer through zoneLayer. */
void fillTypeMenu(QMenu* menu)
{
    menu->clear();
    std::shared_ptr<const ZoneSpec> spec;
    if (Node* n = currentSet())
        spec = specOf(*n);
    auto add = [&](const LayerType& t) {
        QAction* a = menu->addAction(QString::fromStdString(t.label), [id = t.id] {
            warnOnThrow([&] {
                nlohmann::json call = {{"type", id}};
                if (Node* n = currentSet())
                    call["args"] = {n->path()};
                const nlohmann::json r = ops::execute("zoneLayer", call, CallSource::UI);
                ui().set = Graph::current().find(r["set"].get<std::string>())->id();
                ui().layer = r["layer"]["id"].get<std::string>();
                ui().zone.clear();
            });
        });
        a->setToolTip(QString::fromStdString(t.doc));
    };
    for (const LayerType& t : builtInTypes())
        add(t);
    if (spec && !spec->types.empty())
    {
        menu->addSeparator();
        for (const LayerType& t : spec->types)
            add(t);
    }
}

QToolButton* addLayerButton(QWidget* parent)
{
    auto* b = panelkit::button("Add Layer", "Add a zone layer of a type", parent);
    b->setIcon(icon("plus"));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(b);
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { fillTypeMenu(menu); });
    b->setMenu(menu);
    return b;
}

std::string gPickText;

class Palette final : public QWidget
{
public:
    explicit Palette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "openworldZonePalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        std::vector<QWidget*> tiles;
        for (size_t i = 0; i < kModes.size(); ++i)
        {
            const ModeButton& m = kModes[i];
            QToolButton* b = panelkit::tile(m.icon, m.label, QString("%1: %2").arg(m.label, m.tip), this);
            connect(b, &QToolButton::clicked, this, [mode = m.mode] {
                settings().mode = mode;
                save();
            });
            tiles.push_back(b);
            mModes[i] = b;
        }
        v->addWidget(panelkit::section("Mode", panelkit::grid(tiles, 4, this), this, kSections));

        auto* target = new QWidget(this);
        auto* tf = panelkit::form(target);
        mLayer = new QComboBox(target);
        mLayer->setToolTip("Layer: the zone layer painted and drawn into");
        connect(mLayer, &QComboBox::activated, this, [this](int) {
            ui().layer = mLayer->currentData().toString().toStdString();
            ui().zone.clear();
            save();
        });
        tf->addRow("Layer", mLayer);
        mZone = new QComboBox(target);
        mZone->setToolTip("Zone: the zone painted into; New Zone makes one with the next stroke or outline");
        connect(mZone, &QComboBox::activated, this, [this](int) {
            ui().zone = mZone->currentData().toString().toStdString();
            save();
        });
        tf->addRow("Zone", mZone);
        tf->addRow("", addLayerButton(target));
        mEmpty = panelkit::dim("No zone layers. Add one to paint.", target);
        tf->addRow(mEmpty);
        v->addWidget(panelkit::section("Target", target, this, kSections));

        mBrushBox = new QWidget(this);
        mBrushForm = panelkit::form(mBrushBox);
        auto spin = [this](double lo, double hi, int decimals, const QString& unit, const char* label, const char* tip, auto&& apply) {
            QDoubleSpinBox* s = panelkit::valueField(lo, hi, decimals, unit, mBrushBox);
            s->setToolTip(QString("%1: %2").arg(label, tip));
            if (decimals > 0)
                s->setSingleStep(0.05);
            connect(s, &QDoubleSpinBox::valueChanged, this, [apply](double x) {
                apply(x);
                save();
            });
            mBrushForm->addRow(label, s);
            return s;
        };
        mRadius = spin(10.0, 1e7, 0, "cm", "Radius", "brush radius  ([ ])", [](double x) { settings().radius = x; });
        mValue = spin(0.0, 1.0, 2, {}, "Weight", "density painted toward", [](double x) { settings().value = x; });
        mStrength = spin(0.0, 1.0, 2, {}, "Strength", "how far one dab goes toward the weight", [](double x) { settings().strength = x; });
        mSoftness = spin(0.0, 1.0, 2, {}, "Softness", "0 a hard edge, 1 fading from the centre", [](double x) { settings().softness = x; });
        mBrushSection = panelkit::section("Brush", mBrushBox, this, kSections);
        v->addWidget(mBrushSection);

        mPick = panelkit::dim({}, this);
        mPick->setTextInteractionFlags(Qt::TextSelectableByMouse);
        mPickSection = panelkit::section("Here", mPick, this, kSections);
        v->addWidget(mPickSection);

        mKeys = new QCheckBox("Show Keys", this);
        mKeys->setToolTip("Show Keys: key hints on screen, in every tool");
        connect(mKeys, &QCheckBox::clicked, this, [](bool on) { PrefStore::get().set("general", kShowKeysPref, on); });
        v->addWidget(mKeys);
        v->addStretch();
        sync();
        // The panel, undo and commands change layers and zones under the palette; follow them.
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            Node* n = currentSet();
            std::shared_ptr<const ZoneSpec> spec = n ? specOf(*n) : nullptr;
            const auto key = std::make_tuple(spec.get(), ui().layer, ui().zone);
            if (key != mKey && isVisible())
                sync();
        });
        timer->start(400);
    }

    void sync()
    {
        const Settings& s = settings();
        if (Node* n = currentSet())
            mKey = std::make_tuple(specOf(*n).get(), ui().layer, ui().zone);
        else
            mKey = {};
        for (size_t i = 0; i < kModes.size(); ++i)
            mModes[i]->setChecked(kModes[i].mode == s.mode);
        for (auto [box, value] : {std::pair{mRadius, s.radius}, std::pair{mValue, s.value}, std::pair{mStrength, s.strength}, std::pair{mSoftness, s.softness}})
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        const QSignalBlocker b1(mLayer), b2(mZone);
        mLayer->clear();
        mZone->clear();
        std::shared_ptr<const ZoneSpec> spec;
        if (Node* n = currentSet())
            spec = specOf(*n);
        const Layer* l = spec ? currentLayer(*spec) : nullptr;
        if (spec)
            for (const Layer& x : spec->layers)
                mLayer->addItem(QString::fromStdString(x.label), QString::fromStdString(x.id));
        if (l)
        {
            mLayer->setCurrentIndex(mLayer->findData(QString::fromStdString(l->id)));
            mZone->addItem("New Zone", QString());
            for (const Zone& z : l->zones)
                mZone->addItem(QString::fromStdString(z.name.empty() ? z.id : z.name), QString::fromStdString(z.id));
            mZone->setCurrentIndex(std::max(0, mZone->findData(QString::fromStdString(ui().zone))));
        }
        mLayer->setEnabled(l != nullptr);
        mZone->setEnabled(l != nullptr);
        mEmpty->setVisible(l == nullptr);
        const bool brush = s.mode == Mode::Paint || s.mode == Mode::Erase;
        mBrushSection->setVisible(brush);
        mBrushForm->setRowVisible(mValue, l && l->density && s.mode == Mode::Paint);
        mBrushForm->setRowVisible(mStrength, l && l->density);
        mPickSection->setVisible(s.mode == Mode::Pick);
        mPick->setText(gPickText.empty() ? QString("Click the map") : QString::fromStdString(gPickText));
        mKeys->setChecked(keysShown());
    }

private:
    std::array<QToolButton*, kModes.size()> mModes{};
    QComboBox *mLayer = nullptr, *mZone = nullptr;
    QLabel *mEmpty = nullptr, *mPick = nullptr;
    QWidget *mBrushBox = nullptr, *mBrushSection = nullptr, *mPickSection = nullptr;
    QFormLayout* mBrushForm = nullptr;
    QDoubleSpinBox *mRadius = nullptr, *mValue = nullptr, *mStrength = nullptr, *mSoftness = nullptr;
    QCheckBox* mKeys = nullptr;
    std::tuple<const void*, std::string, std::string> mKey;
};

QPointer<Palette> gPalette;

void syncPalette()
{
    if (gPalette)
        gPalette->sync();
}

QDockWidget* dockOf(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

// ----------------------------------------------------------------------------------------------------------------------
// The Zones tool

class ZoneTool final : public ViewportTool
{
public:
    const char* name() const override { return "Zones"; }

    void activate() override
    {
        panels::show(kPaletteId);
        panels::placeToolPalette(dockOf(gPalette));
        syncPalette();
    }

    void deactivate() override
    {
        if (mStroke)
            finishStroke();
        mOutline.clear();
        gPreview = {};
        if (QDockWidget* d = dockOf(gPalette))
            d->hide();
        viewportTools::requestRedraw();
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (e->button() != Qt::LeftButton)
            return false;
        Node* set = currentSet();
        std::shared_ptr<const ZoneSpec> spec = set ? specOf(*set) : nullptr;
        const Layer* layer = spec ? currentLayer(*spec) : nullptr;
        Vec2 at;
        if (!groundHit(set ? groundOf(groundPathOf(*set)) : Ground{}, e->position(), view, at))
            return true;
        const Mode mode = settings().mode;
        if (mode == Mode::Pick)
        {
            if (set)
                pick(*set, *spec, at);
            return true;
        }
        if (!layer)
        {
            log::warn("Zones: add a zone layer first (Add Layer in the palette)");
            return true;
        }
        if (mode == Mode::Draw)
        {
            if (e->type() == QEvent::MouseButtonDblClick)
                closeOutline();
            else
                mOutline.push_back(at);
            viewportTools::requestRedraw();
            return true;
        }
        mErase = mode == Mode::Erase || (e->modifiers() & Qt::ControlModifier);
        if (mErase && ui().zone.empty())
        {
            log::warn("Zones: pick a zone to erase from");
            return true;
        }
        mStroke = true;
        mSet = set->id();
        mPoints = {at};
        gPreview = {set->id(), std::make_shared<ZoneSpec>(*spec), 1};
        dabPreview(at, at);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        mView = view.camera;
        if (!mStroke)
        {
            viewportTools::requestRedraw();
            return false;
        }
        Node* set = Graph::current().node(scene::NodeId(mSet));
        Vec2 at;
        if (!set || !groundHit(groundOf(groundPathOf(*set)), e->position(), view, at))
            return true;
        if (glm::length(at - mPoints.back()) < settings().radius * 0.2)
            return true;
        dabPreview(mPoints.back(), at);
        mPoints.push_back(at);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mStroke)
            return false;
        finishStroke();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        if (action == "edit.undo" && !mOutline.empty())
        {
            mOutline.pop_back();
            viewportTools::requestRedraw();
            return true;
        }
        return key(e);
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override { return key(*e); }

    bool cancelStep() override
    {
        if (mOutline.empty())
            return false;
        mOutline.clear();
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        Node* set = currentSet();
        std::shared_ptr<const ZoneSpec> spec = set ? specOf(*set) : nullptr;
        const Layer* layer = spec ? currentLayer(*spec) : nullptr;
        const Settings& st = settings();
        QString zoneName = "New Zone";
        if (layer)
            if (const Zone* z = layer->zone(ui().zone))
                zoneName = QString::fromStdString(z->name.empty() ? z->id : z->name);
        ToolStatus status{.tool = name(), .subject = set ? QString::fromStdString(leaf(set->path())) : QString(), .mode = modeOf(st.mode).label};
        if (layer)
            status.fields = {{"layer", QString::fromStdString(layer->label)}, {"zone", zoneName}};
        if (st.mode == Mode::Paint || st.mode == Mode::Erase)
            status.fields.push_back({"radius", QString("%1 cm").arg(st.radius, 0, 'f', 0)});
        if (!layer && st.mode != Mode::Pick)
            status.prompt = "add a zone layer";
        else if (st.mode == Mode::Draw)
            status.prompt = mOutline.empty() ? "click the outline's first point" : "click more points; Enter closes";
        else if (st.mode == Mode::Pick)
            status.prompt = "click to read the zones here";
        status.keys = keys();
        drawToolStatus(p, view, status);
        drawKeyList(p, QPointF(16.0, view.height - 84.0), status.keys);

        const Ground ground = set ? groundOf(groundPathOf(*set)) : Ground{};
        if (!mOutline.empty())
        {
            std::vector<QPointF> pts;
            for (Vec2 v : mOutline)
                if (auto s = view.project(draped(ground, v)))
                    pts.push_back(*s);
            p.polyline(pts, 2.0f, glm::vec4(1.0f));
            for (const QPointF& s : pts)
                p.smoothDisc(s, 3.5, glm::vec4(1.0f));
            Vec2 hover;
            if (mView == view.camera && groundHit(ground, mHover, view, hover))
                if (auto a = view.project(draped(ground, mOutline.back())), b = view.project(draped(ground, hover)); a && b)
                    p.dashedLine(*a, *b, 1.5f, 5.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.7f));
            if (mOutline.size() >= 3)
                if (auto a = view.project(draped(ground, mOutline.front())), b = view.project(draped(ground, mOutline.back())); a && b)
                    p.dashedLine(*a, *b, 1.0f, 3.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.4f));
        }
        // Preselection: in Pick, the zone the click would read is outlined under the cursor.
        if (st.mode == Mode::Pick && layer && mView == view.camera && !layer->zones.empty())
        {
            Vec2 c;
            if (groundHit(ground, mHover, view, c))
                if (const std::vector<const Zone*> at = zonesAt(*layer, c); !at.empty())
                {
                    const int index = int(at.front() - layer->zones.data());
                    p.setShadow(true);
                    for (const LayerDraw::Line& line : layerDraw(set->id(), *spec, 0, *layer, ground).lines)
                        if (line.zone == index)
                        {
                            std::vector<QPointF> pts;
                            for (const glm::dvec3& w : line.pts)
                                if (auto s = view.project(w))
                                    pts.push_back(*s);
                            p.polyline(pts, 3.0f, glm::vec4(1.0f), true);
                        }
                    p.label(mHover + QPointF(14.0, -10.0), QString::fromStdString(at.front()->name.empty() ? at.front()->id : at.front()->name));
                    p.setShadow(false);
                }
        }
        if ((st.mode == Mode::Paint || st.mode == Mode::Erase) && mView == view.camera)
        {
            Vec2 c;
            if (groundHit(ground, mHover, view, c))
            {
                std::vector<QPointF> ring;
                for (int i = 0; i <= 48; ++i)
                {
                    const double t = i * 6.283185307179586 / 48.0;
                    if (auto s = view.project(draped(ground, c + Vec2(std::cos(t), std::sin(t)) * st.radius)))
                        ring.push_back(*s);
                }
                const bool erase = st.mode == Mode::Erase || (QGuiApplication::keyboardModifiers() & Qt::ControlModifier);
                p.setShadow(true);
                p.polyline(ring, 1.5f, erase ? kWarn : glm::vec4(1.0f));
                if (st.softness > 0.0)
                {
                    std::vector<QPointF> inner;
                    for (int i = 0; i <= 48; ++i)
                    {
                        const double t = i * 6.283185307179586 / 48.0;
                        if (auto s = view.project(draped(ground, c + Vec2(std::cos(t), std::sin(t)) * st.radius * (1.0 - st.softness))))
                            inner.push_back(*s);
                    }
                    p.dashedPolyline(inner, 1.0f, 4.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
                }
                p.setShadow(false);
            }
        }
    }

private:
    std::vector<KeyHint> keys() const
    {
        switch (settings().mode)
        {
        case Mode::Paint:
            return {{"Drag", "Paint"}, {"Ctrl+drag", "Erase"}, {"[  ]", "Smaller, larger"}, {"Esc", "Exit"}};
        case Mode::Erase:
            return {{"Drag", "Erase"}, {"[  ]", "Smaller, larger"}, {"Esc", "Exit"}};
        case Mode::Draw:
            return {{"Click", "Add point"}, {"Enter", "Close outline"}, {"Backspace", "Remove point"}, {"Esc", mOutline.empty() ? "Exit" : "Cancel"}};
        case Mode::Pick:
            return {{"Click", "Read zones"}, {"Esc", "Exit"}};
        }
        return {};
    }

    bool key(const QKeyEvent& e)
    {
        if (e.key() == Qt::Key_BracketLeft || e.key() == Qt::Key_BracketRight)
        {
            settings().radius = std::clamp(settings().radius * (e.key() == Qt::Key_BracketLeft ? 0.8 : 1.25), 10.0, 1e7);
            save();
            return true;
        }
        if ((e.key() == Qt::Key_Return || e.key() == Qt::Key_Enter) && !mOutline.empty())
        {
            closeOutline();
            return true;
        }
        if (e.key() == Qt::Key_Backspace && !mOutline.empty())
        {
            mOutline.pop_back();
            viewportTools::requestRedraw();
            return true;
        }
        if (e.key() == Qt::Key_Escape)
        {
            if (cancelStep())
                return true;
            QTimer::singleShot(0, lila::ui::scope("OpenWorld"), [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void dabPreview(Vec2 from, Vec2 to)
    {
        if (!gPreview.spec)
            return;
        Layer* l = gPreview.spec->layer(ui().layer);
        if (!l)
            return;
        Zone* z = l->zone(ui().zone);
        if (!z)
        {
            if (l->zones.empty() || l->zones.back().id != "__stroke")
            {
                Zone made;
                made.id = "__stroke";
                made.name = "New Zone";
                l->zones.push_back(made);
            }
            z = &l->zones.back();
        }
        z->mask.cell = gPreview.spec->maskCell;
        const Settings& st = settings();
        const double len = glm::length(to - from);
        const int n = std::max(1, int(std::ceil(len / std::max(st.radius * 0.25, z->mask.cell * 0.5))));
        for (int k = len > 0.0 ? 1 : 0; k <= n; ++k)
            paint(z->mask, {.center = from + (to - from) * (double(k) / n), .radius = st.radius, .value = l->density ? st.value : 1.0,
                            .strength = l->density ? st.strength : 1.0, .softness = st.softness, .erase = mErase});
        ++gPreview.revision;
        viewportTools::requestRedraw();
    }

    void finishStroke()
    {
        mStroke = false;
        Node* set = Graph::current().node(scene::NodeId(mSet));
        std::shared_ptr<const ZoneSpec> spec = set ? specOf(*set) : nullptr;
        const Layer* l = spec ? spec->layer(ui().layer) : nullptr;
        gPreview = {};
        if (!set || !l)
            return;
        nlohmann::json pts = nlohmann::json::array();
        for (Vec2 v : mPoints)
            pts.push_back({v.x, v.y});
        const Settings& st = settings();
        nlohmann::json call = {{"args", {set->path()}}, {"layer", l->id}, {"points", pts}, {"radius", st.radius}, {"softness", st.softness}, {"erase", mErase}};
        if (l->density)
        {
            call["value"] = st.value;
            call["strength"] = st.strength;
        }
        if (!ui().zone.empty())
            call["zone"] = ui().zone;
        warnOnThrow([&] {
            const nlohmann::json r = ops::execute("zonePaint", call, CallSource::UI);
            ui().zone = r["zone"]["id"].get<std::string>();
        });
        syncPalette();
        viewportTools::requestRedraw();
    }

    void closeOutline()
    {
        if (mOutline.size() < 3)
        {
            log::warn("Zones: an outline needs three points or more");
            return;
        }
        Node* set = currentSet();
        std::shared_ptr<const ZoneSpec> spec = set ? specOf(*set) : nullptr;
        const Layer* l = spec ? currentLayer(*spec) : nullptr;
        if (!l)
            return;
        nlohmann::json ring = nlohmann::json::array();
        for (Vec2 v : mOutline)
            ring.push_back({v.x, v.y});
        nlohmann::json call = {{"args", {set->path()}}, {"layer", l->id}};
        const Zone* z = l->zone(ui().zone);
        if (z)
        {
            // Drawing on an existing zone adds the outline to its region.
            nlohmann::json polys = nlohmann::json::array();
            for (const Polygon& r : z->polygons)
            {
                nlohmann::json pr = nlohmann::json::array();
                for (Vec2 v : r)
                    pr.push_back({v.x, v.y});
                polys.push_back(pr);
            }
            polys.push_back(ring);
            call["id"] = z->id;
            call["polygons"] = polys;
        }
        else
            call["polygon"] = ring;
        mOutline.clear();
        warnOnThrow([&] {
            const nlohmann::json r = ops::execute("zoneCreate", call, CallSource::UI);
            ui().zone = r["zone"]["id"].get<std::string>();
        });
        syncPalette();
        viewportTools::requestRedraw();
    }

    void pick(Node& set, const ZoneSpec& spec, Vec2 at)
    {
        std::string text;
        for (const Layer& l : spec.layers)
        {
            const nlohmann::json q = queryLayer(l, at);
            text += l.label + ": ";
            if (q["zone"].is_null())
                text += "none";
            else
            {
                text += q.value("name", std::string());
                if (l.density)
                    text += std::format(" ({:.2f})", q["density"].get<double>());
            }
            text += "\n";
            if (l.id == ui().layer && !q["zone"].is_null())
                ui().zone = q["zone"].get<std::string>();
        }
        gPickText = text.empty() ? "No layers" : text.substr(0, text.size() - 1);
        ui().set = set.id();
        log::info("zoneQuery at [{:.0f}, {:.0f}]: {}", at.x, at.y, gPickText);
        syncPalette();
        viewportTools::requestRedraw();
    }

    bool mStroke = false;
    bool mErase = false;
    uint64_t mSet = 0;
    std::vector<Vec2> mPoints;
    std::vector<Vec2> mOutline;
    QPointF mHover;
    std::string mView;
};

// ----------------------------------------------------------------------------------------------------------------------
// The Zones panel

class ZonesPanel final : public QWidget
{
protected:
    void showEvent(QShowEvent* e) override
    {
        ++gPanelsShown;
        viewportTools::requestRedraw();
        QWidget::showEvent(e);
    }
    void hideEvent(QHideEvent* e) override
    {
        gPanelsShown = std::max(0, gPanelsShown - 1);
        viewportTools::requestRedraw();
        QWidget::hideEvent(e);
    }

public:
    explicit ZonesPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 6, 8, 6);
        v->setSpacing(4);

        auto* top = new QHBoxLayout;
        mSet = new QComboBox(this);
        mSet->setToolTip("Zone set");
        connect(mSet, &QComboBox::activated, this, [this](int) {
            ui().set = mSet->currentData().toULongLong();
            ui().layer.clear();
            ui().zone.clear();
            refresh(true);
        });
        top->addWidget(mSet, 1);
        mOverlay = new QCheckBox("Overlay", this);
        mOverlay->setToolTip("Overlay: draw the zones over the viewport");
        connect(mOverlay, &QCheckBox::clicked, this, [](bool on) {
            if (Node* n = currentSet())
                warnOnThrow([&] { ops::execute("setAttr", {{"args", {n->path() + "." + kOverlayAttr, on}}}, CallSource::UI); });
        });
        top->addWidget(mOverlay);
        v->addLayout(top);

        mLayers = new QTreeWidget(this);
        mLayers->setHeaderLabels({"Layer", "Type", "Zones"});
        mLayers->setRootIsDecorated(false);
        mLayers->setUniformRowHeights(true);
        mLayers->header()->setStretchLastSection(false);
        mLayers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mLayers->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        mLayers->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        mLayers->setMaximumHeight(170);
        connect(mLayers, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it, QTreeWidgetItem*) {
            if (mFilling || !it)
                return;
            ui().layer = it->data(0, Qt::UserRole).toString().toStdString();
            ui().zone.clear();
            refresh(true);
            syncPalette();
            viewportTools::requestRedraw();
        });
        connect(mLayers, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
            if (mFilling || col != 0)
                return;
            if (Node* n = currentSet())
                warnOnThrow([&] {
                    ops::execute("zoneLayer", {{"args", {n->path()}}, {"layer", it->data(0, Qt::UserRole).toString().toStdString()}, {"visible", it->checkState(0) == Qt::Checked}},
                                 CallSource::UI);
                });
        });
        v->addWidget(mLayers);

        auto* layerRow = new QHBoxLayout;
        layerRow->addWidget(addLayerButton(this));
        auto* removeLayer = panelkit::button("Remove", "Remove the layer and its zones", this);
        connect(removeLayer, &QToolButton::clicked, this, [] {
            if (Node* n = currentSet(); n && !ui().layer.empty())
                warnOnThrow([&] { ops::execute("zoneDelete", {{"args", {n->path()}}, {"layer", ui().layer}}, CallSource::UI); });
        });
        layerRow->addWidget(removeLayer);
        v->addLayout(layerRow);

        mZones = new QTreeWidget(this);
        mZones->setHeaderLabels({"Zone", "Priority", "Area"});
        mZones->setRootIsDecorated(false);
        mZones->setUniformRowHeights(true);
        mZones->header()->setStretchLastSection(false);
        mZones->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mZones->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        mZones->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        connect(mZones, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it, QTreeWidgetItem*) {
            if (mFilling || !it)
                return;
            ui().zone = it->data(0, Qt::UserRole).toString().toStdString();
            fillZone();
            syncPalette();
            viewportTools::requestRedraw();
        });
        v->addWidget(mZones, 1);

        auto* zoneRow = new QHBoxLayout;
        auto* paintBtn = panelkit::button("Paint", "Paint, erase, draw and pick zones in the viewport", this);
        connect(paintBtn, &QToolButton::clicked, this, [] { viewportTools::activate(kToolId); });
        zoneRow->addWidget(paintBtn);
        auto* removeZone = panelkit::button("Delete", "Delete the zone", this);
        connect(removeZone, &QToolButton::clicked, this, [] {
            if (Node* n = currentSet(); n && !ui().zone.empty())
                warnOnThrow([&] { ops::execute("zoneDelete", {{"args", {n->path()}}, {"layer", ui().layer}, {"zone", ui().zone}}, CallSource::UI); });
        });
        zoneRow->addWidget(removeZone);
        v->addLayout(zoneRow);

        mZoneBox = new QWidget(this);
        auto* zf = panelkit::form(mZoneBox);
        mName = new QLineEdit(mZoneBox);
        mName->setToolTip("Name; clear it to generate one");
        connect(mName, &QLineEdit::editingFinished, this, [this] {
            if (mFilling)
                return;
            edit({{"name", mName->text().toStdString()}});
        });
        zf->addRow("Name", mName);
        mPriority = new QSpinBox(mZoneBox);
        mPriority->setRange(-1000, 1000);
        mPriority->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mPriority->setButtonSymbols(QAbstractSpinBox::NoButtons);
        mPriority->setKeyboardTracking(false);
        mPriority->setToolTip("Priority: higher wins where zones of the layer overlap");
        connect(mPriority, &QSpinBox::valueChanged, this, [this](int p) {
            if (!mFilling)
                edit({{"priority", p}});
        });
        zf->addRow("Priority", mPriority);
        mAttrs = new QTableWidget(0, 2, mZoneBox);
        mAttrs->setHorizontalHeaderLabels({"Attribute", "Value"});
        mAttrs->verticalHeader()->hide();
        mAttrs->horizontalHeader()->setStretchLastSection(true);
        mAttrs->setToolTip("Attributes: values as JSON (numbers, true/false, [lists]); anything else is text");
        connect(mAttrs, &QTableWidget::cellChanged, this, [this](int row, int col) {
            if (mFilling || col != 1)
                return;
            const std::string key = mAttrs->item(row, 0)->text().toStdString();
            const std::string text = mAttrs->item(row, 1)->text().toStdString();
            nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
            if (value.is_discarded())
                value = text;
            edit({{"attrs", {{key, value}}}});
        });
        zf->addRow(mAttrs);
        v->addWidget(panelkit::section("Zone", mZoneBox, this, "openworldZonesPanel"));

        auto* foot = new QHBoxLayout;
        auto* check = panelkit::button("Check", "Gaps, overlaps and unnamed zones", this);
        connect(check, &QToolButton::clicked, this, [] { actions::run(nlohmann::json("openworld.zoneCheck")); });
        foot->addWidget(check);
        foot->addWidget(panelkit::overflow({"openworld.zoneFromDistricts", "openworld.zoneFromTerrain", "openworld.zoneFromWater", "openworld.zoneFromCurve",
                                            "openworld.zonePlayArea", "openworld.zoneExport", "openworld.zoneImport", "openworld.zoneSendToUnreal", "openworld.zoneRemoveFromUnreal"},
                                           this));
        v->addLayout(foot);
        mStatus = panelkit::dim({}, this);
        v->addWidget(mStatus);

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(false); });
        timer->start(400);
        refresh(true);
    }

private:
    void edit(const nlohmann::json& flags)
    {
        Node* n = currentSet();
        if (!n || ui().zone.empty())
            return;
        nlohmann::json call = flags;
        call["args"] = {n->path()};
        call["layer"] = ui().layer;
        call["zone"] = ui().zone;
        warnOnThrow([&] { ops::execute("zoneEdit", call, CallSource::UI); });
    }

    void refresh(bool force)
    {
        Graph& g = Graph::current();
        Node* set = currentSet();
        std::shared_ptr<const ZoneSpec> spec = set ? specOf(*set) : nullptr;
        const std::vector<scene::NodeId> sets = zoneSets();
        const nlohmann::json check = set && gChecks.count(set->id()) && gChecks[set->id()].spec == spec ? gChecks[set->id()].result : nlohmann::json();
        const auto key = std::make_tuple(sets, set ? set->id() : 0, spec.get(), ui().layer, ui().zone, check.dump());
        if (!force && key == mKey)
            return;
        mKey = key;
        mFilling = true;
        mSet->clear();
        for (scene::NodeId id : sets)
            mSet->addItem(QString::fromStdString(leaf(g.node(id)->path())), qulonglong(id));
        mSet->setCurrentIndex(set ? mSet->findData(qulonglong(set->id())) : -1);
        mSet->setEnabled(sets.size() > 1);
        bool overlay = true;
        if (set)
            if (const bool* on = std::get_if<bool>(&set->value(set->type().attrIndex(kOverlayAttr))))
                overlay = *on;
        mOverlay->setChecked(overlay);
        mOverlay->setEnabled(set != nullptr);

        mLayers->clear();
        const Layer* current = spec ? currentLayer(*spec) : nullptr;
        size_t zones = 0;
        if (spec)
            for (const Layer& l : spec->layers)
            {
                auto* it = new QTreeWidgetItem(mLayers, {QString::fromStdString(l.label), display(l.type), QString::number(l.zones.size())});
                it->setData(0, Qt::UserRole, QString::fromStdString(l.id));
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(0, l.visible ? Qt::Checked : Qt::Unchecked);
                it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
                it->setToolTip(0, QString::fromStdString(l.id + (l.exclusive ? ", exclusive" : "") + (l.mustCover ? ", must cover" : "") + (l.density ? ", density" : "")));
                if (&l == current)
                    mLayers->setCurrentItem(it);
                zones += l.zones.size();
            }
        mZones->clear();
        if (current)
            for (const Zone& z : current->zones)
            {
                auto* it = new QTreeWidgetItem(mZones, {QString::fromStdString(z.name.empty() ? z.id : z.name), QString::number(z.priority),
                                                         QString("%1 m²").arg(areaM2(z, *current), 0, 'f', 0)});
                it->setData(0, Qt::UserRole, QString::fromStdString(z.id));
                it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
                it->setToolTip(0, QString::fromStdString(z.id + (z.source.is_object() ? ", from " + z.source.value("kind", std::string()) : std::string())));
                if (z.id == ui().zone)
                    mZones->setCurrentItem(it);
            }
        mFilling = false;
        fillZone();
        if (!set)
            mStatus->setText("No zone set. Add a layer to make one.");
        else if (check.is_object())
        {
            const nlohmann::json& c = check["counts"];
            QStringList parts;
            for (const auto& [kind, one, many] : {std::tuple{"gap", "gap", "gaps"}, std::tuple{"overlap", "overlap", "overlaps"},
                                                  std::tuple{"empty", "empty zone", "empty zones"}, std::tuple{"unnamed", "unnamed zone", "unnamed zones"}})
                if (const int n = c.value(kind, 0))
                    parts << QString("%1 %2").arg(n).arg(n == 1 ? one : many);
            mStatus->setText(parts.isEmpty() ? QString("%1 layers, %2 zones, no issues").arg(spec->layers.size()).arg(zones) : parts.join(", "));
        }
        else
            mStatus->setText(QString("%1 layers, %2 zones").arg(spec ? spec->layers.size() : 0).arg(zones));
    }

    void fillZone()
    {
        mFilling = true;
        Node* set = currentSet();
        std::shared_ptr<const ZoneSpec> spec = set ? specOf(*set) : nullptr;
        const Layer* l = spec ? spec->layer(ui().layer) : nullptr;
        const Zone* z = l ? l->zone(ui().zone) : nullptr;
        mZoneBox->setEnabled(z != nullptr);
        mName->setText(z ? QString::fromStdString(z->name) : QString());
        mPriority->setValue(z ? z->priority : 0);
        mAttrs->setRowCount(0);
        if (z)
        {
            nlohmann::json attrs = l->defaults;
            attrs.update(z->attrs);
            for (auto it = attrs.begin(); it != attrs.end(); ++it)
            {
                const int row = mAttrs->rowCount();
                mAttrs->insertRow(row);
                auto* k = new QTableWidgetItem(display(it.key()));
                k->setText(QString::fromStdString(it.key()));
                k->setFlags(k->flags() & ~Qt::ItemIsEditable);
                if (!z->attrs.contains(it.key()))
                    k->setToolTip("Layer default");
                mAttrs->setItem(row, 0, k);
                auto* val = new QTableWidgetItem(QString::fromStdString(it->is_string() ? it->get<std::string>() : it->dump()));
                if (it->is_number())
                    val->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                mAttrs->setItem(row, 1, val);
            }
        }
        mFilling = false;
    }

    QComboBox* mSet = nullptr;
    QCheckBox* mOverlay = nullptr;
    QTreeWidget *mLayers = nullptr, *mZones = nullptr;
    QWidget* mZoneBox = nullptr;
    QLineEdit* mName = nullptr;
    QSpinBox* mPriority = nullptr;
    QTableWidget* mAttrs = nullptr;
    QLabel* mStatus = nullptr;
    bool mFilling = false;
    std::tuple<std::vector<scene::NodeId>, uint64_t, const void*, std::string, std::string, std::string> mKey;
};

// ----------------------------------------------------------------------------------------------------------------------
// Actions

std::vector<std::string> selectedOfType(const char* type)
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA(type))
            out.push_back(n->path());
    return out;
}

/** The call's set: the current one when there is one, so menu actions act on what the panel shows. */
nlohmann::json withSet(nlohmann::json call = nlohmann::json::object())
{
    if (Node* n = currentSet())
        call["args"] = {n->path()};
    return call;
}

void runCheck()
{
    warnOnThrow([] {
        Node* n = currentSet();
        if (!n)
            throw std::runtime_error("no zone set (add a zone layer first)");
        const nlohmann::json c = ops::execute("zoneCheck", {{"args", {n->path()}}, {"report", true}}, CallSource::UI);
        gChecks[n->id()] = {specOf(*n), c};
        if (c.value("ok", false))
            log::info("zoneCheck: no issues");
        else
            for (const nlohmann::json& i : c["issues"])
                log::warn("zoneCheck: {}", i["what"].get<std::string>());
        viewportTools::requestRedraw();
    });
}

const std::vector<std::string> kKeywords = {"zone", "region", "territory", "district", "jurisdiction", "spawn", "density", "trigger volume",
                                            "gameplay volume", "data layer", "world partition", "mission", "wanted", "police", "heat map"};

json endsOfSelectedCurves()
{
    const std::vector<std::string> curves = selectedOfType("curve");
    return curves.empty() ? json::object() : json{{"curve", curves}};
}
} // namespace

void registerZoneUi(ext::Context& ctx)
{
    panels::add(ctx, {.id = kPaletteId,
                      .title = "Zones",
                      .create = [](QWidget* parent) -> QWidget* { return gPalette = new Palette(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "map-2",
                      .open = false,
                      .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                      .floatFirst = true,
                      .toolPalette = true,
                      .docs = "open-world?id=zones"});
    panels::add(ctx, {.id = kPanelId,
                      .title = "Zones",
                      .create = [](QWidget* parent) -> QWidget* { return new ZonesPanel(parent); },
                      .area = Qt::RightDockWidgetArea,
                      .icon = "map-2",
                      .open = false,
                      .toggleAction = "openworld.zonesPanel",
                      .keywords = kKeywords,
                      .docs = "open-world?id=zones"});
    viewportTools::add(ctx, {.id = kToolId, .label = "Zones", .icon = "map-2", .create = [] { return std::make_unique<ZoneTool>(); }});

    auto add = [&](Action a) {
        a.keywords.insert(a.keywords.end(), kKeywords.begin(), kKeywords.end());
        actions::add(ctx, std::move(a));
    };
    add({.id = "openworld.zoneTool",
         .label = "Paint Zones",
         .category = "Open World",
         .run = [] { viewportTools::activate(kToolId); },
         .doc = "Paint, erase and draw gameplay zones on the map, or pick to read every layer at a point.",
         .checked = [] { return viewportTools::activeId() == kToolId; },
         .icon = "brush"});
    add({.id = "openworld.zoneLayer",
         .label = "Add Zone Layer",
         .category = "Open World",
         .python = "zoneLayer(type='police')",
         .command = "zoneLayer",
         .call = nlohmann::json::object(),
         .icon = "stack-2"});
    add({.id = "openworld.zoneFromCurve",
         .label = "Zone from Curve",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zoneCreate", withSet(endsOfSelectedCurves())); }); },
         .doc = "Make a zone from the selected closed curves in the current layer.",
         .python = "zoneCreate(layer='district', curve='Outline')",
         .icon = "polygon"});
    add({.id = "openworld.zoneFromDistricts",
         .label = "Zones from Districts",
         .category = "Open World",
         .run =
             [] {
                 warnOnThrow([] {
                     nlohmann::json call = withSet();
                     if (const std::vector<std::string> m = selectedOfType("mesh"); !m.empty())
                         call["settlement"] = m.front();
                     actions::runWithOptions("zoneFromDistricts", call);
                 });
             },
         .doc = "One zone per district of the selected Settlement, named as the settlement names it.",
         .python = "zoneFromDistricts(settlement='Town')",
         .icon = "building-community"});
    add({.id = "openworld.zoneFromTerrain",
         .label = "Zones from Terrain Masks",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zoneFromTerrain", withSet()); }); },
         .doc = "One painted zone per terrain mask or biome.",
         .python = "zoneFromTerrain(biomes=True, layer='pedSpawn', type='pedSpawn')",
         .icon = "mountain"});
    add({.id = "openworld.zoneFromWater",
         .label = "Zones from Water Routes",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zoneFromWater", withSet()); }); },
         .doc = "Boat (or aircraft) spawn zones along the terrain's water and air routes.",
         .python = "zoneFromWater(terrain='Coast')",
         .icon = "sailboat"});
    add({.id = "openworld.zonePlayArea",
         .label = "Play Area from Curve",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zonePlayArea", withSet(endsOfSelectedCurves())); }); },
         .doc = "Set the area must-cover layers are checked against from the selected closed curves.",
         .python = "zonePlayArea(curve='Island')",
         .icon = "border-outer"});
    add({.id = "openworld.zoneCheck",
         .label = "Check Zones",
         .category = "Open World",
         .run = runCheck,
         .doc = "Find gaps in must-cover layers, overlaps in exclusive ones and zones without a region or name; marked on the map.",
         .python = "zoneCheck()",
         .icon = "list-check"});
    add({.id = "openworld.zoneOverlay",
         .label = "Zone Overlay",
         .category = "Open World",
         .run =
             [] {
                 warnOnThrow([] {
                     if (Node* n = currentSet())
                     {
                         const bool* on = std::get_if<bool>(&n->value(n->type().attrIndex(kOverlayAttr)));
                         ops::execute("setAttr", {{"args", {n->path() + "." + kOverlayAttr, !(on && *on)}}}, CallSource::UI);
                     }
                 });
             },
         .doc = "Draw the current zone set over the viewport.",
         .checked = [] {
             Node* n = currentSet();
             const bool* on = n ? std::get_if<bool>(&n->value(n->type().attrIndex(kOverlayAttr))) : nullptr;
             return on && *on;
         },
         .icon = "layers-intersect"});
    add({.id = "openworld.zoneExport",
         .label = "Export Zones",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zoneExport", withSet()); }); },
         .doc = "Write the zones as JSON with PNG masks for any engine.",
         .python = "zoneExport(folder='C:/game/zones')",
         .icon = "file-export"});
    add({.id = "openworld.zoneImport",
         .label = "Import Zones",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zoneImport", withSet()); }); },
         .doc = "Read zones from a lila.zones JSON and its PNG masks.",
         .python = "zoneImport(path='C:/game/zones/Zones_zones.json')",
         .icon = "file-import"});
    add({.id = "openworld.zoneSendToUnreal",
         .label = "Send Zones to Unreal",
         .category = "Open World",
         .run = [] { warnOnThrow([] { actions::runWithOptions("zoneSendToUnreal", withSet()); }); },
         .doc = "Zone volumes, a zone data asset and a data layer per layer in the open Unreal level.",
         .python = "zoneSendToUnreal()",
         .icon = "brand-unreal-engine"});
    add({.id = "openworld.zoneRemoveFromUnreal",
         .label = "Remove Zones from Unreal",
         .category = "Open World",
         .run = [] { warnOnThrow([] { ops::execute("zoneRemoveFromUnreal", withSet(), CallSource::UI); }); },
         .doc = "Take the zone set's volumes, zone data asset and unused data layers out of the open Unreal level.",
         .python = "zoneRemoveFromUnreal()",
         .icon = "brand-unreal-engine"});

    addTools(ctx,
             {"", "openworld.zoneTool", "openworld.zonesPanel", "openworld.zoneLayer", "openworld.zoneFromCurve", "openworld.zoneFromDistricts",
              "openworld.zoneFromTerrain", "openworld.zoneFromWater", "openworld.zonePlayArea", "openworld.zoneOverlay", "openworld.zoneCheck",
              "openworld.zoneExport", "openworld.zoneImport", "openworld.zoneSendToUnreal", "openworld.zoneRemoveFromUnreal"},
             {{.action = "openworld.zoneTool", .icon = "brush", .text = "Paint", .shelf = "Zones", .group = "Zones"},
              {.action = "openworld.zonesPanel", .icon = "map-2", .text = "Panel", .group = "Zones"},
              {.action = "openworld.zoneFromDistricts", .icon = "building-community", .text = "Districts", .group = "Zones"},
              {.action = "openworld.zoneCheck", .icon = "list-check", .text = "Check", .group = "Zones"},
              {.action = "openworld.zoneLayer", .text = "Add Zone Layer", .rare = true},
              {.action = "openworld.zoneFromCurve", .text = "Zone from Curve", .rare = true},
              {.action = "openworld.zoneFromTerrain", .text = "Zones from Terrain", .rare = true},
              {.action = "openworld.zoneFromWater", .text = "Zones from Water", .rare = true},
              {.action = "openworld.zonePlayArea", .text = "Play Area", .rare = true},
              {.action = "openworld.zoneOverlay", .text = "Zone Overlay", .rare = true},
              {.action = "openworld.zoneExport", .text = "Export Zones", .rare = true},
              {.action = "openworld.zoneImport", .text = "Import Zones", .rare = true},
              {.action = "openworld.zoneSendToUnreal", .text = "Send to Unreal", .rare = true},
              {.action = "openworld.zoneRemoveFromUnreal", .text = "Remove from Unreal", .rare = true}});

    viewportHooks::addDrawer(ctx, "openworld.zones", drawZones);
    viewOverlays::add(ctx, {.id = "openworld.zones",
                            .label = "Zones",
                            .doc = "Zone sets over the ground: the shown layer's outlines, names and a faint fill.",
                            .drawers = {"openworld.zones"},
                            .keywords = {"territory", "jurisdiction", "police", "districts"}});

    using ops::options::Override;
    Override layer;
    layer.order = {"type", "layer", "label", "priority", "exclusive", "mustCover", "density", "visible", "dataLayer"};
    layer.hidden = {"setName", "ground", "set", "remove", "color", "defaults"};
    layer.labels = {{"layer", "Id"}, {"mustCover", "Must Cover"}, {"dataLayer", "Data Layer"}};
    layer.docs = "open-world?id=zones";
    ops::options::setOverride("zoneLayer", layer);

    Override create;
    create.order = {"curve", "layer", "type", "name", "priority", "id"};
    create.hidden = {"setName", "ground", "set", "polygon", "polygons", "holes", "circle", "attrs"};
    create.shown = {"curve"};
    create.fills = {{"From Selection", "The selected closed curves.", endsOfSelectedCurves}};
    create.docs = "open-world?id=zones";
    ops::options::setOverride("zoneCreate", create);

    Override districts;
    districts.order = {"settlement", "layer", "type"};
    districts.hidden = {"setName", "ground", "set", "attrs"};
    districts.fills = {{"From Selection", "The selected settlement.", [] {
                            const std::vector<std::string> m = selectedOfType("mesh");
                            return m.empty() ? json::object() : json{{"settlement", m.front()}};
                        }}};
    districts.docs = "open-world?id=zones";
    ops::options::setOverride("zoneFromDistricts", districts);

    Override terrain;
    terrain.order = {"terrain", "biomes", "masks", "layer", "type", "threshold"};
    terrain.shown = {"masks"};
    terrain.hidden = {"setName", "ground", "set", "attrs"};
    terrain.enabledWhen = {{"masks", {"biomes", {"false"}}}};
    terrain.docs = "open-world?id=zones";
    ops::options::setOverride("zoneFromTerrain", terrain);

    Override water;
    water.order = {"terrain", "air", "crafts", "kinds", "widthScale", "airWidth", "layer"};
    water.shown = {"crafts", "kinds"};
    water.hidden = {"setName", "ground", "set"};
    water.labels = {{"widthScale", "Width Scale"}, {"airWidth", "Air Corridor"}};
    water.docs = "open-world?id=zones";
    ops::options::setOverride("zoneFromWater", water);

    Override play;
    play.shown = {"curve"};
    play.hidden = {"set", "polygon"};
    play.fills = {{"From Selection", "The selected closed curves.", endsOfSelectedCurves}};
    ops::options::setOverride("zonePlayArea", play);

    Override check;
    check.labels = {{"minCells", "Min Samples"}};
    check.hidden = {"set"};
    ops::options::setOverride("zoneCheck", check);

    Override exp;
    exp.hidden = {"set"};
    ops::options::setOverride("zoneExport", exp);

    Override imp;
    imp.hidden = {"set", "setName", "ground"};
    ops::options::setOverride("zoneImport", imp);

    Override send;
    send.hidden = {"set"};
    send.labels = {{"dataLayers", "Data Layers"}, {"asset", "Content Folder"}};
    ops::options::setOverride("zoneSendToUnreal", send);
}
} // namespace openworld::zones
