// Paint Bays: a viewport tool that repaints a building's bays (window, door, shopfront, wall, arcade, garage) or switches
// their parts on and off (balcony, awning, AC unit, grille, shutter) by clicking or dragging across them. The brush is
// picked in a floating palette; each stroke is one buildingEdit, kept by the building's id.

#include "BuildingsScene.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Controls.h>
#include <lila/Extension.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/Panels.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QButtonGroup>
#include <QDockWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace lila;
using namespace lila::ui;

namespace buildings
{
namespace
{
constexpr const char* kPaletteId = "buildingsPaintPalette";
constexpr const char* kToolId = "buildings.paintTool";

struct Brush
{
    const char* id;    // bay kind, part name, or "reset"
    const char* label;
    const char* icon;
    const char* tip;
    bool part;
};
const Brush kBrushes[] = {
    {"window", "Window", "window", "Paint bays as windows", false},
    {"door", "Door", "door", "Paint bays as doors", false},
    {"shopfront", "Shopfront", "building-store", "Paint bays as shopfronts", false},
    {"wall", "Wall", "wall", "Paint bays blank", false},
    {"arcade", "Arcade", "building-arch", "Paint ground-floor street bays as portales", false},
    {"garage", "Garage", "building-warehouse", "Paint bays as roll-up doors", false},
    {"balcony", "Balcony", "fence", "Add or remove a balcony", true},
    {"awning", "Awning", "tent", "Add or remove an awning over a shopfront", true},
    {"ac", "AC Unit", "air-conditioning", "Add or remove an AC unit by the window", true},
    {"grille", "Grille", "grid-4x4", "Add or remove window bars", true},
    {"shutter", "Shutter", "layout-rows", "Add or remove a roll-up shutter", true},
    {"reset", "Reset", "eraser", "Back to what the style makes", true},
};
int gBrush = 0;

class BayPalette;
QPointer<BayPalette> gPalette;

class BayPalette final : public QWidget
{
public:
    explicit BayPalette(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);
        mGroup = new QButtonGroup(this);
        mGroup->setExclusive(true);
        std::vector<QWidget*> bays, parts;
        for (int i = 0; i < int(std::size(kBrushes)); ++i)
        {
            const Brush& b = kBrushes[i];
            auto* t = new QToolButton;
            t->setIcon(icon(b.icon));
            t->setIconSize(QSize(24, 24));
            t->setText(b.label);
            t->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            t->setCheckable(true);
            t->setFocusPolicy(Qt::NoFocus);
            t->setToolTip(QString("%1: %2").arg(b.label, b.tip));
            controls::setRole(t, "tile");
            mGroup->addButton(t, i);
            (b.part ? parts : bays).push_back(t);
        }
        connect(mGroup, &QButtonGroup::idClicked, this, [](int id) {
            gBrush = id;
            viewportTools::requestRedraw();
        });
        v->addWidget(controls::section("Bay", controls::grid(bays, 3, this), this, "buildingsPaint"));
        v->addWidget(controls::section("Parts", controls::grid(parts, 3, this), this, "buildingsPaint"));
        v->addStretch(1);
        sync();
    }
    void sync()
    {
        if (QAbstractButton* b = mGroup->button(gBrush))
            b->setChecked(true);
    }

private:
    QButtonGroup* mGroup = nullptr;
};

QDockWidget* paletteDock()
{
    for (QWidget* w = gPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

const glm::vec4 kHover{0.95f, 0.95f, 0.95f, 0.95f};
const glm::vec4 kStroke{0.95f, 0.95f, 0.95f, 0.25f};

class PaintTool final : public ViewportTool
{
public:
    const char* name() const override { return "Paint Bays"; }

    void activate() override
    {
        panels::show(kPaletteId);
        panels::placeToolPalette(paletteDock());
        if (gPalette)
            gPalette->sync();
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
        if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier))
            return false;
        mStroke.clear();
        const std::optional<BayHit> h = hit(e->position(), view);
        if (!h)
            return true;
        mBuilding = h->building;
        currentBuilding() = h->building;
        // A part brush sets every bay of the stroke the opposite of how the first bay has it.
        const Brush& b = kBrushes[gBrush];
        mTurnOn = !has(*h, b.id);
        mStroke.push_back(*h);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = hit(e->position(), view);
        if (!mStroke.empty() && (e->buttons() & Qt::LeftButton) && mHover && mHover->building == mBuilding)
        {
            bool seen = false;
            for (const BayHit& s : mStroke)
                seen = seen || s.bay == mHover->bay;
            if (!seen)
                mStroke.push_back(*mHover);
        }
        viewportTools::requestRedraw();
        return !mStroke.empty();
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mStroke.empty())
            return false;
        apply();
        mStroke.clear();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        auto quad = [&](const BayHit& h, bool fill) {
            std::vector<QPointF> pts;
            for (const Vec3& c : h.corners)
                if (auto s = view.project(c))
                    pts.push_back(*s);
            if (pts.size() != 4)
                return;
            if (fill)
                p.fill(pts, kStroke);
            else
                p.polyline(pts, 2.0f, kHover, true);
        };
        for (const BayHit& s : mStroke)
            quad(s, true);
        if (mHover)
            quad(*mHover, false);
        ToolStatus st;
        st.tool = "Paint Bays";
        st.mode = kBrushes[gBrush].label;
        if (mHover)
        {
            st.subject = QString::fromStdString(mHover->name.empty() ? mHover->building : mHover->name);
            st.fields.push_back({"bay", QString::fromStdString(mHover->bay)});
            st.fields.push_back({"is", QString::fromStdString(mHover->kind)});
        }
        st.prompt = mHover ? "click or drag across bays" : "point at a building's facade";
        st.keys = {{"Drag", "paint bays of one building"}, {"Esc", "leave the tool"}};
        drawToolStatus(p, view, st);
    }

private:
    std::optional<BayHit> hit(QPointF px, const ViewInfo& view) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        return pickBay(o, glm::normalize(d));
    }

    static bool has(const BayHit& h, const std::string& part)
    {
        if (part == "balcony")
            return h.balcony;
        for (const std::string& p : h.parts)
            if (p == part)
                return true;
        return false;
    }

    void apply()
    {
        const Brush& b = kBrushes[gBrush];
        json call = {{"args", {mBuilding}}};
        const std::string id = b.id;
        if (id == "reset")
        {
            json bays = json::array();
            for (const BayHit& s : mStroke)
                bays.push_back(s.bay);
            call["clearBays"] = bays;
        }
        else if (b.part)
        {
            json t = json::object();
            for (const BayHit& s : mStroke)
                t[s.bay][id] = mTurnOn;
            call["toggles"] = t;
        }
        else
        {
            json bays = json::object();
            for (const BayHit& s : mStroke)
                bays[s.bay] = id;
            call["bays"] = bays;
        }
        try
        {
            ops::execute("buildingEdit", call, CallSource::UI);
            refreshSections();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
    }

    std::vector<BayHit> mStroke;
    std::optional<BayHit> mHover;
    std::string mBuilding;
    bool mTurnOn = true;
};
} // namespace

void registerPaintTool(ext::Context& ctx)
{
    panels::add(ctx, {.id = kPaletteId,
                      .title = "Paint Bays",
                      .create = [](QWidget* parent) -> QWidget* { return gPalette = new BayPalette(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "brush",
                      .open = false,
                      .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                      .floatFirst = true,
                      .toolPalette = true});
    viewportTools::add(ctx, {.id = kToolId, .label = "Paint Bays", .icon = "brush", .create = [] { return std::make_unique<PaintTool>(); }});
    actions::add(ctx, {.id = "buildings.paint",
                       .label = "Paint Bays",
                       .category = "Buildings",
                       .run = [] { viewportTools::activate(kToolId); },
                       .doc = "Repaint a building's bays (window, door, shopfront, wall, arcade, garage) and add or remove balconies, awnings, AC units, grilles and shutters.",
                       .python = "buildingEdit('building:1a2b', bays={'t0/f0/l0/b2': 'door'})",
                       .checked = [] { return viewportTools::activeId() == kToolId; },
                       .icon = "brush",
                       .keywords = {"facade", "bay", "window", "door", "balcony", "paint", "cityengine"}});
}
} // namespace buildings
