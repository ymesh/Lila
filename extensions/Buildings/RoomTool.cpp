// Edit Rooms: a viewport tool for an enterable building's room layout. A floating palette picks the edit (Kind, Split,
// Merge, Wall, Door), the floor and the kind to give; clicking or dragging across rooms on that floor runs buildingRoom or
// buildingDoor. While the tool is on, the building's shell and the floors above are hidden (cutaway) without touching undo.

#include "BuildingsScene.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Controls.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Panels.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

using namespace lila;
using namespace lila::ui;

namespace buildings
{
namespace
{
constexpr const char* kRoomPaletteId = "buildingsRoomPalette";
constexpr const char* kRoomToolId = "buildings.roomTool";

struct Mode
{
    const char* id;
    const char* label;
    const char* icon;
    const char* tip;
};
const Mode kModes[] = {
    {"kind", "Kind", "tag", "Give a room the kind picked below: click it"},
    {"split", "Split", "layout-columns", "Cut a room in two where you click"},
    {"merge", "Merge", "arrows-join", "Drag from a room into its neighbour to make them one"},
    {"wall", "Wall", "arrows-horizontal", "Drag from a room into its neighbour to move the wall between"},
    {"door", "Door", "door", "Drag between two rooms to add a door; click beside a door to remove it"},
};
int gMode = 0;
int gFloor = 0;
bool gCutaway = true;
std::string gKind = "kitchen";

QString display(const std::string& k)
{
    if (k == "openfloor")
        return "Open Floor";
    QString s = QString::fromStdString(k);
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

void syncCutaway();

class RoomPalette;
QPointer<RoomPalette> gRoomPalette;

class RoomPalette final : public QWidget
{
public:
    explicit RoomPalette(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);
        mGroup = new QButtonGroup(this);
        mGroup->setExclusive(true);
        std::vector<QWidget*> tiles;
        for (int i = 0; i < int(std::size(kModes)); ++i)
        {
            auto* t = new QToolButton;
            t->setIcon(icon(kModes[i].icon));
            t->setIconSize(QSize(24, 24));
            t->setText(kModes[i].label);
            t->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            t->setCheckable(true);
            t->setFocusPolicy(Qt::NoFocus);
            t->setToolTip(QString("%1: %2").arg(kModes[i].label, kModes[i].tip));
            controls::setRole(t, "tile");
            mGroup->addButton(t, i);
            tiles.push_back(t);
        }
        connect(mGroup, &QButtonGroup::idClicked, this, [this](int id) {
            gMode = id;
            sync();
            viewportTools::requestRedraw();
        });
        v->addWidget(controls::section("Edit", controls::grid(tiles, 3, this), this, "buildingsRooms"));

        auto* form = new QWidget(this);
        auto* f = new QFormLayout(form);
        f->setContentsMargins(0, 0, 0, 0);
        f->setSpacing(4);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* floorRow = new QWidget(form);
        auto* fh = new QHBoxLayout(floorRow);
        fh->setContentsMargins(0, 0, 0, 0);
        fh->setSpacing(4);
        mFloor = new QSpinBox(floorRow);
        mFloor->setRange(0, 120);
        mFloor->setAlignment(Qt::AlignRight);
        mFloor->setToolTip("The floor the tool edits: 0 is the ground floor (Page Up and Page Down in the view)");
        connect(mFloor, &QSpinBox::valueChanged, this, [](int v) {
            gFloor = v;
            syncCutaway();
            viewportTools::requestRedraw();
        });
        mCut = new QToolButton(floorRow);
        mCut->setText("Cutaway");
        mCut->setCheckable(true);
        mCut->setToolTip("Hide the building's shell and the floors above while editing");
        controls::setRole(mCut, "push");
        connect(mCut, &QToolButton::toggled, this, [](bool on) {
            gCutaway = on;
            syncCutaway();
        });
        fh->addWidget(mFloor, 1);
        fh->addWidget(mCut);
        f->addRow("Floor", floorRow);
        mKind = new QComboBox(form);
        for (const std::string& k : roomKinds())
            mKind->addItem(display(k), QString::fromStdString(k));
        mKind->setToolTip("The kind Kind gives a room; its furnishing follows");
        connect(mKind, &QComboBox::activated, this, [this](int i) { gKind = mKind->itemData(i).toString().toStdString(); });
        f->addRow("Kind", mKind);
        v->addWidget(form);
        v->addStretch(1);
        sync();
    }

    void sync()
    {
        if (QAbstractButton* b = mGroup->button(gMode))
            b->setChecked(true);
        QSignalBlocker b1(mFloor), b2(mCut);
        mFloor->setValue(gFloor);
        mCut->setChecked(gCutaway);
        mKind->setCurrentIndex(std::max(0, mKind->findData(QString::fromStdString(gKind))));
        mKind->setEnabled(std::string(kModes[gMode].id) == "kind");
    }

    void setFloors(int top) { mFloor->setMaximum(std::max(0, top)); }

private:
    QButtonGroup* mGroup = nullptr;
    QSpinBox* mFloor = nullptr;
    QToolButton* mCut = nullptr;
    QComboBox* mKind = nullptr;
};

QDockWidget* roomPaletteDock()
{
    for (QWidget* w = gRoomPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

// Cutaway: the building is left out of its mesh and interior parts are hidden with undo off, put back when the tool ends.
std::vector<scene::NodeId> gHidden;
scene::NodeId gCutModifier = scene::kNoNode;
std::string gToolBuilding;
bool gToolActive = false;

void setVisible(scene::NodeId id, bool on)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(id);
    if (!n || !n->type().isA("transform"))
        return;
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    g.setValue({id, scene::Transform::kVisibility, -1}, on);
    u.setEnabled(was);
}

void restoreCutaway()
{
    for (scene::NodeId id : gHidden)
        setVisible(id, true);
    gHidden.clear();
    if (setCutawayBuilding("") && gCutModifier != scene::kNoNode && scene::Graph::current().node(gCutModifier))
        lila::gn::rerunModifier(scene::Graph::current(), gCutModifier);
    gCutModifier = scene::kNoNode;
}

void syncCutaway()
{
    restoreCutaway();
    if (!gToolActive || !gCutaway || gToolBuilding.empty())
        return;
    scene::Graph& g = scene::Graph::current();
    auto hide = [&](scene::Node* n) {
        if (n && n->type().isA("transform") && n->as<bool>(scene::Transform::kVisibility))
        {
            gHidden.push_back(n->id());
            setVisible(n->id(), false);
        }
    };
    if (auto loc = locateBuilding(gToolBuilding))
        if (scene::Node* mod = g.find(loc->modifier))
        {
            setCutawayBuilding(gToolBuilding);
            gCutModifier = mod->id();
            lila::gn::rerunModifier(g, mod->id());
        }
    if (scene::Node* grp = g.find(interiorGroupOf(gToolBuilding)))
        for (scene::NodeId c : g.children(grp->id()))
            if (scene::Node* n = g.node(c); n && n->type().attrIndex(kPartMark) >= 0)
            {
                const std::string part = nodeAttrString(n->path() + "." + kPartMark);
                const size_t l = part.find('l');
                const int floor = l == std::string::npos || part.rfind("core", 0) == 0 ? -1 : std::atoi(part.c_str() + l + 1);
                if (floor > gFloor || (floor == gFloor && part.ends_with("/ceiling")))
                    hide(n);
            }
}

const glm::vec4 kFill{0.95f, 0.95f, 0.95f, 0.08f};
const glm::vec4 kHoverFill{0.95f, 0.95f, 0.95f, 0.28f};
const glm::vec4 kLine{0.95f, 0.95f, 0.95f, 0.9f};
const glm::vec4 kText{0.95f, 0.95f, 0.95f, 1.0f};
const glm::vec4 kDim{0.95f, 0.95f, 0.95f, 0.55f};

void runRoomUi(const char* cmd, json call)
{
    try
    {
        ops::execute(cmd, call, CallSource::UI);
        refreshSections();
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}

class RoomTool final : public ViewportTool
{
public:
    const char* name() const override { return "Edit Rooms"; }

    void activate() override
    {
        gToolActive = true;
        panels::show(kRoomPaletteId);
        panels::placeToolPalette(roomPaletteDock());
        gToolBuilding = currentBuilding();
        if (gToolBuilding.empty() || !interiorOf(gToolBuilding) || interiorGroupOf(gToolBuilding).empty())
        {
            const std::vector<std::string> ids = enterableBuildings();
            gToolBuilding.clear();
            for (const std::string& id : ids)
                if (!interiorGroupOf(id).empty())
                {
                    gToolBuilding = id;
                    break;
                }
        }
        syncFloors();
        syncCutaway();
        if (gRoomPalette)
            gRoomPalette->sync();
    }

    void deactivate() override
    {
        gToolActive = false;
        restoreCutaway();
        if (QDockWidget* d = roomPaletteDock())
            d->hide();
        viewportTools::requestRedraw();
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_PageUp && e->key() != Qt::Key_PageDown)
            return false;
        gFloor = std::max(0, gFloor + (e->key() == Qt::Key_PageUp ? 1 : -1));
        if (gRoomPalette)
            gRoomPalette->sync();
        syncCutaway();
        viewportTools::requestRedraw();
        return true;
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier))
            return false;
        mPress = hit(e->position(), view, true);
        if (mPress && mPress->building != gToolBuilding)
        {
            gToolBuilding = mPress->building;
            currentBuilding() = gToolBuilding;
            syncFloors();
            syncCutaway();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = hit(e->position(), view, false);
        viewportTools::requestRedraw();
        return mPress.has_value();
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mPress)
            return false;
        const std::optional<RoomHit> release = hit(e->position(), view, false);
        apply(*mPress, release);
        mPress.reset();
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        std::shared_ptr<const Interior> in = gToolBuilding.empty() ? nullptr : interiorOf(gToolBuilding);
        if (in)
        {
            for (const Room& r : in->rooms)
            {
                if (r.floor != gFloor)
                    continue;
                const bool hot = mHover && mHover->room == r.id && mHover->building == gToolBuilding;
                const bool pressed = mPress && mPress->room == r.id;
                for (const auto& q : r.rects)
                {
                    std::vector<QPointF> pts;
                    for (const Vec2& c : q)
                        if (auto s = view.project(Vec3(c.x, r.y + 2.0, c.y)))
                            pts.push_back(*s);
                    if (pts.size() == 4)
                        p.fill(pts, hot || pressed ? kHoverFill : kFill);
                }
                if (auto s = view.project(Vec3(r.label.x, r.y + 5.0, r.label.y)))
                    p.text(*s, display(r.kind), r.reachable ? kText : glm::vec4(1.0f, 0.45f, 0.35f, 1.0f), OverlayPainter::Center);
            }
            for (const InteriorDoor& d : in->doors)
            {
                if (d.floor != gFloor)
                    continue;
                const Vec3 along = glm::normalize(glm::cross(Vec3(0, 1, 0), d.normal)) * (d.width * 0.5);
                auto a = view.project(d.origin + along + Vec3(0, 3, 0)), b = view.project(d.origin - along + Vec3(0, 3, 0));
                if (a && b)
                    p.polyline({*a, *b}, 3.0f, kLine, false);
            }
        }
        if (mPress && mHover && mHover->room != mPress->room)
        {
            auto a = view.project(mPress->point), b = view.project(mHover->point);
            if (a && b)
                p.polyline({*a, *b}, 2.0f, kDim, false);
        }
        ToolStatus st;
        st.tool = "Edit Rooms";
        st.mode = kModes[gMode].label;
        st.fields.push_back({"floor", QString::number(gFloor)});
        if (mHover)
        {
            st.subject = QString::fromStdString(mHover->room);
            st.fields.push_back({"is", display(mHover->kind)});
        }
        if (!in)
            st.prompt = "make a building enterable first";
        else
            st.prompt = gMode == 0 || gMode == 1 ? "click a room" : gMode == 4 ? "drag between rooms, or click beside a door" : "drag into the neighbouring room";
        st.keys = {{"PgUp/PgDn", "floor"}, {"Esc", "leave the tool"}};
        drawToolStatus(p, view, st);
    }

private:
    std::optional<RoomHit> hit(QPointF px, const ViewInfo& view, bool any) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        return pickRoom(o, glm::normalize(d), gFloor, any ? std::string() : gToolBuilding);
    }

    void syncFloors()
    {
        int top = 0;
        if (std::shared_ptr<const Interior> in = gToolBuilding.empty() ? nullptr : interiorOf(gToolBuilding))
            for (const Room& r : in->rooms)
                top = std::max(top, r.floor);
        gFloor = std::min(gFloor, top);
        if (gRoomPalette)
        {
            gRoomPalette->setFloors(top);
            gRoomPalette->sync();
        }
    }

    void apply(const RoomHit& a, const std::optional<RoomHit>& b)
    {
        const std::string mode = kModes[gMode].id;
        const json args = json::array({a.building});
        const bool same = !b || (b->room == a.room && b->building == a.building);
        if (mode == "kind")
            runRoomUi("buildingRoom", {{"args", args}, {"room", a.room}, {"kind", gKind}});
        else if (mode == "split")
        {
            // Across the room's longer side, where it was clicked.
            if (a.rects.empty())
                return;
            const Vec2 ex = glm::normalize(a.rects[0][1] - a.rects[0][0]), ez = glm::normalize(a.rects[0][3] - a.rects[0][0]);
            double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
            for (const auto& q : a.rects)
                for (const Vec2& c : q)
                {
                    u0 = std::min(u0, glm::dot(c, ex)), u1 = std::max(u1, glm::dot(c, ex));
                    v0 = std::min(v0, glm::dot(c, ez)), v1 = std::max(v1, glm::dot(c, ez));
                }
            const bool alongU = u1 - u0 >= v1 - v0;
            const Vec2 at{a.point.x, a.point.z};
            const double f = alongU ? (glm::dot(at, ex) - u0) / std::max(u1 - u0, 1.0) : (glm::dot(at, ez) - v0) / std::max(v1 - v0, 1.0);
            runRoomUi("buildingRoom", {{"args", args}, {"room", a.room}, {"split", alongU ? "u" : "v"}, {"at", std::clamp(f, 0.1, 0.9)}});
        }
        else if (mode == "merge" && !same)
            runRoomUi("buildingRoom", {{"args", args}, {"room", a.room}, {"merge", b->room}});
        else if (mode == "wall" && !same)
        {
            const double by = std::max(std::abs(b->point.x - a.point.x), std::abs(b->point.z - a.point.z));
            runRoomUi("buildingRoom", {{"args", args}, {"room", a.room}, {"grow", b->room}, {"by", std::clamp(by * 0.5, 20.0, 2000.0)}});
        }
        else if (mode == "door")
        {
            if (!same)
            {
                runRoomUi("buildingDoor", {{"args", args}, {"add", {a.room, b->room}}, {"at", {b->point.x, b->point.y, b->point.z}}});
                return;
            }
            // A click beside a door takes it away.
            std::shared_ptr<const Interior> in = interiorOf(a.building);
            if (!in)
                return;
            const InteriorDoor* best = nullptr;
            double bd = 120.0;
            for (const InteriorDoor& d : in->doors)
                if (d.floor == gFloor && glm::length(Vec2(d.origin.x - a.point.x, d.origin.z - a.point.z)) < bd)
                {
                    bd = glm::length(Vec2(d.origin.x - a.point.x, d.origin.z - a.point.z));
                    best = &d;
                }
            if (best)
                runRoomUi("buildingDoor", {{"args", args}, {"remove", best->id}});
        }
    }

    std::optional<RoomHit> mPress, mHover;
};
} // namespace

void registerRoomTool(ext::Context& ctx)
{
    panels::add(ctx, {.id = kRoomPaletteId,
                      .title = "Edit Rooms",
                      .create = [](QWidget* parent) -> QWidget* { return gRoomPalette = new RoomPalette(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "layout-board",
                      .open = false,
                      .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                      .floatFirst = true,
                      .toolPalette = true});
    viewportTools::add(ctx, {.id = kRoomToolId, .label = "Edit Rooms", .icon = "layout-board", .create = [] { return std::make_unique<RoomTool>(); }});
    actions::add(ctx, {.id = "buildings.rooms",
                       .label = "Edit Rooms",
                       .category = "Buildings",
                       .run = [] { viewportTools::activate(kRoomToolId); },
                       .doc = "Edit an enterable building's rooms floor by floor: give a room a kind, split, merge, move the wall between two rooms, add or remove doors.",
                       .python = "buildingRoom('building:1a2b', room='l0/backroom', kind='kitchen')",
                       .checked = [] { return viewportTools::activeId() == kRoomToolId; },
                       .icon = "layout-board",
                       .keywords = {"interior", "room", "floor plan", "layout", "door", "wall", "partition", "enterable"}});
}
} // namespace buildings
