// The viewport look of markers, and the Rig > Templates menu, actions and panel.

#include "RigTemplates.h"

#include <lila/Command.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Rig.h>
#include <lila/Transform.h>
#include <lila/UI.h>
#include <lila/Viewport.h>

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <numbers>
#include <optional>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace rigt
{
namespace
{
std::string gCurrent; // template the panel has selected
std::string gHose = "template"; // buildRigSkeleton's hose mode, from the panel's Limbs choice

std::string currentTemplate()
{
    if (findTemplate(gCurrent))
        return gCurrent;
    if (findTemplate("UE5_Manny"))
        return "UE5_Manny";
    std::vector<const Entry*> all = templates();
    return all.empty() ? std::string() : all.front()->tmpl.id;
}

glm::dvec3 axis(const std::string& a)
{
    try
    {
        return axisOf(a);
    }
    catch (const std::exception&)
    {
        return {0.0, 0.0, 1.0};
    }
}

/** The marker's aim frame as world axes (aim, up, third), from its aimAt child; nullopt for a marker that aims at nothing. */
std::optional<glm::dmat3> tripod(Graph& g, const Node& n, const glm::dvec3& at)
{
    const std::string aimAt = str(n, "aimAt");
    if (aimAt.empty())
        return std::nullopt;
    for (NodeId c : g.children(n.id()))
    {
        const Node* k = g.node(c);
        if (!k || !k->type().isA(kMarkerType) || str(*k, "marker") != aimAt)
            continue;
        glm::dvec3 aim = glm::dvec3(k->as<glm::dmat4>(Transform::kWorldMatrix)[3]) - at;
        if (glm::length(aim) < 1e-9)
            return std::nullopt;
        aim = glm::normalize(aim);
        glm::dvec3 up = n.as<glm::dvec3>(attrOf(n, "upVector"));
        up -= aim * glm::dot(up, aim);
        if (glm::length(up) < 1e-6)
            return std::nullopt;
        up = glm::normalize(up);
        return glm::dmat3(aim, up, glm::cross(aim, up));
    }
    return std::nullopt;
}

void drawMarker(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    const Node& n = c.node;
    const glm::dvec3 p(c.world[3]);
    const double r = std::max(0.05, n.as<double>(attrOf(n, "radius")));
    auto line = [&](const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& col) {
        c.frame.linesOnTop.push_back({glm::dvec3(a), col});
        c.frame.linesOnTop.push_back({glm::dvec3(b), col});
    };
    const bool lit = c.highlight != drawOverrides::Highlight::None;
    const glm::vec4 col = lit ? c.color : glm::vec4(0.55f, 0.55f, 0.55f, 1.0f);
    constexpr int kSegs = 20;
    for (int a = 0; a < 3; ++a)
    {
        glm::dvec3 u(0.0), v(0.0);
        u[(a + 1) % 3] = r;
        v[(a + 2) % 3] = r;
        for (int i = 0; i < kSegs; ++i)
        {
            double t0 = 2.0 * std::numbers::pi * i / kSegs, t1 = 2.0 * std::numbers::pi * (i + 1) / kSegs;
            line(p + u * std::cos(t0) + v * std::sin(t0), p + u * std::cos(t1) + v * std::sin(t1), col);
        }
    }
    // The limb line to the parent marker.
    if (NodeId parent = g.parent(n.id()); parent != kNoNode)
        if (const Node* pn = g.node(parent); pn && pn->type().isA(kMarkerType))
            line(glm::dvec3(pn->as<glm::dmat4>(Transform::kWorldMatrix)[3]), p, glm::vec4(0.35f, 0.35f, 0.35f, 1.0f));
    // The joint frame the build will give it: aim axis brightest, then up, then the third.
    if (std::optional<glm::dmat3> f = tripod(g, n, p))
    {
        const glm::dvec3 aimLocal = axis(str(n, "aimAxis")), upLocal = axis(str(n, "upAxis"));
        const glm::dmat3 local(aimLocal, upLocal, glm::cross(aimLocal, upLocal));
        const glm::dmat3 rot = *f * glm::transpose(local);
        const float shade[3] = {0.9f, 0.6f, 0.35f};
        for (int a = 0; a < 3; ++a)
        {
            glm::dvec3 e(0.0);
            e[a] = 1.0;
            // Shade by role: whichever local axis is aimed is brightest.
            const glm::dvec3 w = rot * e;
            const int role = std::abs(glm::dot(w, (*f)[0])) > 0.5 ? 0 : std::abs(glm::dot(w, (*f)[1])) > 0.5 ? 1 : 2;
            line(p, p + w * (r * 2.5), glm::vec4(shade[role], shade[role], shade[role], 1.0f));
        }
    }
    std::string label = str(n, "label");
    if (label.empty())
        label = str(n, "marker");
    if (std::optional<QPointF> at = c.view.project(p))
        c.painter.text(*at + QPointF(8, -8), QString::fromStdString(label), c.painter.ink(lit ? 1.0f : 0.75f));
}

std::optional<double> pickMarker(const drawOverrides::PickContext& c)
{
    const glm::dvec3 p(c.world[3]);
    std::optional<QPointF> at = c.view.project(p);
    if (!at)
        return std::nullopt;
    if (!c.click)
        return QRectF(c.a, c.b).normalized().contains(*at) ? std::optional<double>(0.0) : std::nullopt;
    const double r = c.node.as<double>(attrOf(c.node, "radius")) / std::max(1e-9, c.view.unitsPerPixel(p));
    return std::max(0.0, std::hypot(at->x() - c.a.x(), at->y() - c.a.y()) - std::max(4.0, r));
}

/** What the selection holds that the actions work on. */
struct Picked
{
    std::string instance;
    std::string joint;
    std::vector<std::string> meshes;
};

Picked picked()
{
    Graph& g = Graph::current();
    Picked p;
    for (NodeId id : g.selection())
    {
        const Node* n = g.node(id);
        if (!n)
            continue;
        if (NodeId root = instanceOf(g, id); root != kNoNode)
        {
            if (p.instance.empty())
                p.instance = g.node(root)->path();
        }
        else if (rig::isJoint(*n))
        {
            if (p.joint.empty())
                p.joint = n->path();
        }
        else
        {
            bool mesh = n->type().isA("mesh");
            for (NodeId c : g.children(id))
                mesh |= g.node(c)->type().isA("mesh");
            if (mesh)
                p.meshes.push_back(n->path());
        }
    }
    return p;
}

void run(const char* cmd, const json& call)
{
    try
    {
        ops::execute(cmd, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}

void place()
{
    Picked p = picked();
    json call = {{"args", {currentTemplate()}}};
    if (!p.meshes.empty())
        call["meshes"] = p.meshes;
    run("placeRigTemplate", call);
}

void build()
{
    Picked p = picked();
    json call = {{"args", {p.instance.empty() ? currentTemplate() : p.instance}}};
    if (!p.meshes.empty())
        call["meshes"] = p.meshes;
    if (!p.instance.empty() || !p.meshes.empty())
    {
        Graph& g = Graph::current();
        bool bound = !p.meshes.empty() || (!p.instance.empty() && !rig::centerMeshes(g, g.find(p.instance)->id()).empty());
        call["bindSkin"] = bound;
    }
    call["hose"] = gHose;
    run("buildRigSkeleton", call);
}

void orient()
{
    Picked p = picked();
    if (p.joint.empty())
    {
        log::warn("Orient To Template: select a joint of the skeleton");
        return;
    }
    json call = {{"args", {p.joint}}};
    if (!currentTemplate().empty())
        call["template"] = currentTemplate();
    run("orientRigSkeleton", call);
}

void fromSkeleton()
{
    Picked p = picked();
    if (p.joint.empty())
    {
        log::warn("Template From Skeleton: select a joint of the skeleton");
        return;
    }
    run("templateFromSkeleton", {{"args", {p.joint}}});
}

void bindMeshes()
{
    Picked p = picked();
    if (p.instance.empty() || p.meshes.empty())
    {
        log::warn("Bind Template: select a template instance (or marker) and the meshes");
        return;
    }
    json args = json::array({p.instance});
    for (const std::string& m : p.meshes)
        args.push_back(m);
    run("bindRigTemplate", {{"args", args}});
}

void withInstance(const char* cmd, const char* what)
{
    Picked p = picked();
    if (p.instance.empty())
    {
        log::warn("{}: select a template instance or one of its markers", what);
        return;
    }
    run(cmd, {{"args", {p.instance}}});
}

/** A labelled button running the action, styled by the app sheet's push role. */
QToolButton* rigtPush(QWidget* parent, const char* action, const QString& label)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(QString::fromStdString(actions::tooltip(action)));
    b->setProperty("role", "push");
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const std::string id = action;
    QObject::connect(b, &QToolButton::clicked, b, [id] { actions::run(json(id)); });
    return b;
}

QWidget* makePanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    auto* list = new QListWidget(w);
    auto* info = new QLabel(w);
    info->setWordWrap(true);
    info->setForegroundRole(QPalette::PlaceholderText);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(list, 1);
    v->addWidget(info);

    auto grid = [w](QGroupBox* box, std::initializer_list<std::pair<const char*, const char*>> items) {
        auto* g = new QGridLayout(box);
        g->setContentsMargins(0, 4, 0, 0);
        g->setSpacing(2);
        int i = 0;
        for (auto [action, label] : items)
        {
            g->addWidget(rigtPush(box, action, label), i / 2, i % 2);
            ++i;
        }
        return g;
    };
    auto* markers = new QGroupBox("Markers", w);
    grid(markers, {{"rigTemplates.place", "Place"}, {"rigTemplates.bind", "Bind to Meshes"}, {"rigTemplates.center", "Centre in Mesh"},
                   {"rigTemplates.mirror", "Mirror Left to Right"}, {"rigTemplates.unbind", "Unbind"}});
    v->addWidget(markers);

    auto* skeleton = new QGroupBox("Skeleton", w);
    QGridLayout* skeletonGrid = grid(skeleton, {{"rigTemplates.build", "Build Skeleton"}});
    auto* limbs = new QComboBox(w);
    const std::pair<const char*, const char*> modes[] = {{"Limbs as the Template Says", "template"}, {"Jointed Limbs", "none"}, {"Rubber Hose Limbs", "all"}};
    for (const auto& [label, mode] : modes)
    {
        limbs->addItem(label, QString(mode));
        if (gHose == mode)
            limbs->setCurrentIndex(limbs->count() - 1);
    }
    limbs->setToolTip("Limbs\nHow Build Skeleton makes the arms and legs: jointed elbows and knees, or rubber hoses");
    QObject::connect(limbs, &QComboBox::currentIndexChanged, w, [limbs](int) { gHose = limbs->currentData().toString().toStdString(); });
    limbs->setParent(skeleton);
    skeletonGrid->addWidget(limbs, 0, 1);
    v->addWidget(skeleton);

    auto* more = new QToolButton(w);
    more->setText("3 more");
    more->setToolTip("More template commands; each is also in Rig > Templates and Ctrl+T search");
    more->setPopupMode(QToolButton::InstantPopup);
    more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more->setIcon(icon("dots"));
    more->setProperty("role", "overflow");
    auto* moreMenu = new QMenu(more);
    moreMenu->setToolTipsVisible(true);
    for (const char* id : {"rigTemplates.orient", "rigTemplates.fromSkeleton", "rigTemplates.picker"})
    {
        const std::string action = id;
        QAction* a = moreMenu->addAction(QString::fromStdString(actions::find(action) ? actions::find(action)->label : action),
                                         [action] { actions::run(json(action)); });
        a->setToolTip(QString::fromStdString(actions::tooltip(action)));
    }
    more->setMenu(moreMenu);
    v->addWidget(more, 0, Qt::AlignLeft);

    QPointer<QListWidget> guard(list);
    auto describe = [info](const std::string& id) {
        const Entry* e = findTemplate(id);
        if (!e)
        {
            info->clear();
            return;
        }
        size_t free = 0;
        for (const Marker& m : e->tmpl.markers)
            free += m.isFree();
        info->setText(QString("%1\n%2 joints, %3 markers, %4 limbs, %5 poses\n%6")
                          .arg(QString::fromStdString(e->tmpl.description.empty() ? e->tmpl.name : e->tmpl.description))
                          .arg(e->tmpl.markers.size())
                          .arg(free)
                          .arg(e->tmpl.limbs.size())
                          .arg(e->tmpl.poses.size())
                          .arg(QString::fromStdString(e->file.empty() ? e->tier : e->tier + ": " + e->file.filename().string())));
    };
    auto refill = [guard, describe] {
        if (!guard)
            return;
        QSignalBlocker block(guard.data());
        guard->clear();
        const std::string cur = currentTemplate();
        for (const Entry* e : templates())
        {
            auto* item = new QListWidgetItem(QString::fromStdString(e->tmpl.name), guard.data());
            item->setData(Qt::UserRole, QString::fromStdString(e->tmpl.id));
            item->setToolTip(QString::fromStdString(e->tmpl.id));
            if (e->tmpl.id == cur)
                guard->setCurrentItem(item);
        }
        describe(cur);
    };
    QObject::connect(list, &QListWidget::currentItemChanged, w, [describe](QListWidgetItem* item) {
        if (!item)
            return;
        gCurrent = item->data(Qt::UserRole).toString().toStdString();
        describe(gCurrent);
    });
    refill();
    int listener = addListener(refill);
    QObject::connect(w, &QObject::destroyed, [listener] { removeListener(listener); });
    return w;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    drawOverrides::add(ctx, {.id = "rigTemplates.marker", .nodeType = kMarkerType, .draw = drawMarker, .pick = pickMarker});

    struct Def
    {
        const char* id;
        const char* label;
        void (*fn)();
        const char* doc;
    };
    const Def defs[] = {
        {"rigTemplates.place", "Place Template", place, "Place the current template's markers; bound to the selected meshes if any."},
        {"rigTemplates.build", "Build Skeleton From Template", build,
         "Build joints from the selected template instance (or the current template's defaults), binding its meshes."},
        {"rigTemplates.bind", "Bind Template to Meshes", bindMeshes, "Bind the selected template instance to the selected meshes, so its markers centre in them."},
        {"rigTemplates.unbind", "Unbind Template", [] { withInstance("unbindRigTemplate", "Unbind Template"); }, "Unbind the selected template instance from its meshes."},
        {"rigTemplates.center", "Centre Markers in Mesh", [] { withInstance("centerRigMarkers", "Centre Markers"); },
         "Centre the selected instance's markers in the bound meshes' cross-sections."},
        {"rigTemplates.mirror", "Mirror Markers Left to Right", [] { withInstance("mirrorRigMarkers", "Mirror Markers"); },
         "Copy the left markers' positions onto the right ones, reflected."},
        {"rigTemplates.orient", "Orient Skeleton to Template", orient,
         "Orient the selected skeleton's joints to the current template's rules, keeping its skin and animation."},
        {"rigTemplates.fromSkeleton", "Template From Skeleton", fromSkeleton, "Make a rig template from the selected skeleton."},
        {"rigTemplates.picker", "Make Picker From Template", [] {
             Picked p = picked();
             run("rigTemplatePicker", {{"args", {p.instance.empty() ? currentTemplate() : p.instance}}});
         },
         "Store a Control Picker document generated from the current template's layout in the scene."},
    };
    for (const Def& d : defs)
        actions::add(ctx, {.id = d.id, .label = d.label, .category = "Rig Templates", .run = d.fn, .doc = d.doc});

    panels::add(ctx, {.id = "rigTemplatesPanel", .title = "Rig Templates", .create = makePanel, .icon = "template", .open = false});

    menus::addItem(ctx, {.menu = "Rig", .kind = menus::Kind::Separator});
    menus::addItem(ctx, {.menu = "Rig", .kind = menus::Kind::Submenu, .label = "Templates"});
    for (const char* a : {"rigTemplates.place", "rigTemplates.bind", "rigTemplates.unbind", "rigTemplates.center", "rigTemplates.mirror",
                          "", "rigTemplates.build", "rigTemplates.orient", "rigTemplates.fromSkeleton", "rigTemplates.picker", "",
                          "window.rigTemplatesPanel"})
    {
        if (*a)
            menus::addItem(ctx, {.menu = "Rig/Templates", .action = a});
        else
            menus::addItem(ctx, {.menu = "Rig/Templates", .kind = menus::Kind::Separator});
    }
}
} // namespace rigt
