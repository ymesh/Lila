// Overlap's UI: the simulated chains and the colliders in the viewport, actions and a menu, a panel, and Attribute Editor
// sections on the Overlap node.

#include "Overlap.h"

#include <lila/Command.h>
#include <lila/OptionBox.h>
#include <lila/Events.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/SimPreview.h>
#include <lila/Simulation.h>
#include <lila/UI.h>
#include <lila/Viewport.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <map>
#include <optional>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace overlap
{
namespace
{
constexpr double kTau = 6.28318530717958647692;
/** Simulated chains are data, so they get a colour of their own (linear). */
const glm::vec4 kChainColor{1.0f, 0.42f, 0.08f, 1.0f};
const glm::vec4 kChainSelectedColor{1.0f, 0.75f, 0.35f, 1.0f};

bool flagOf(const Node& n, const char* name)
{
    const int a = n.type().attrIndex(name);
    return a >= 0 && n.as<bool>(a);
}

double numberOf(const Node& n, const char* name)
{
    const int a = n.type().attrIndex(name);
    return a >= 0 ? n.as<double>(a) : 0.0;
}

/** Runs a command from the UI; failures go to the log rather than out of a Qt slot. */
json runCommand(const char* name, const json& call = json::object())
{
    if (optionBox::shiftOpens(name, call))
        return nullptr;
    try
    {
        return ops::execute(name, ops::options::callWith(name, call), CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Error, e.what());
        return nullptr;
    }
}

void drawSimulation(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    if (!flagOf(c.node, "preview") || !flagOf(c.node, "enabled"))
        return;
    std::shared_ptr<const sim::Frame> f;
    try
    {
        f = simPreviewFrame(g, c.node.id());
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!f)
        return;
    const sim::Info info = sim::info(g, c.node.id());
    std::map<NodeId, size_t> index;
    for (size_t i = 0; i < info.outputs.size(); ++i)
        index[info.outputs[i]] = i;
    const glm::vec4 color = c.highlight == drawOverrides::Highlight::Selected ? kChainSelectedColor : kChainColor;
    for (size_t i = 0; i < info.outputs.size() && i < f->world.size(); ++i)
    {
        const glm::dvec3 p(f->world[i][3]);
        if (auto it = index.find(g.parent(info.outputs[i])); it != index.end())
        {
            c.frame.linesOnTop.push_back({glm::dvec3((f->world[it->second][3])), color});
            c.frame.linesOnTop.push_back({glm::dvec3(p), color});
        }
        const double r = 3.0 * c.view.unitsPerPixel(p);
        for (int axis = 0; axis < 3; ++axis)
        {
            glm::dvec3 d(0.0);
            d[axis] = r;
            c.frame.linesOnTop.push_back({glm::dvec3(p - d), color});
            c.frame.linesOnTop.push_back({glm::dvec3(p + d), color});
        }
    }
    if (info.loop && !info.converged && !f->world.empty())
        if (std::optional<QPointF> at = c.view.project(glm::dvec3(f->world.front()[3])))
            c.painter.label(*at + QPointF(10, -12), "Overlap loop did not settle: seam cross-faded");
}

/** A circle of radius r around centre in the plane of the unit axes u and v, in the node's space. */
void circle(drawOverrides::DrawContext& c, const glm::dvec3& centre, const glm::dvec3& u, const glm::dvec3& v, double r, double from = 0.0,
            double span = kTau)
{
    constexpr int kSegments = 32;
    glm::dvec3 prev(0.0);
    for (int k = 0; k <= kSegments; ++k)
    {
        const double a = from + span * double(k) / kSegments;
        const glm::dvec3 p = glm::dvec3(c.world * glm::dvec4(centre + (u * std::cos(a) + v * std::sin(a)) * r, 1.0));
        if (k > 0)
        {
            c.frame.lines.push_back({glm::dvec3(prev), c.color});
            c.frame.lines.push_back({glm::dvec3(p), c.color});
        }
        prev = p;
    }
}

void drawCollider(drawOverrides::DrawContext& c)
{
    const double r = numberOf(c.node, "radius");
    const bool capsule = c.node.as<int64_t>(c.node.type().attrIndex("shape")) == 1;
    const glm::dvec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    if (!capsule)
    {
        circle(c, {}, x, y, r);
        circle(c, {}, y, z, r);
        circle(c, {}, z, x, r);
        return;
    }
    const double half = numberOf(c.node, "length") * 0.5;
    const glm::dvec3 a(-half, 0, 0), b(half, 0, 0);
    circle(c, a, y, z, r);
    circle(c, b, y, z, r);
    // End caps as half circles, opening toward the body.
    circle(c, a, -x, y, r, -kTau / 4, kTau / 2);
    circle(c, a, -x, z, r, -kTau / 4, kTau / 2);
    circle(c, b, x, y, r, -kTau / 4, kTau / 2);
    circle(c, b, x, z, r, -kTau / 4, kTau / 2);
    for (const glm::dvec3& side : {y, -y, z, -z})
    {
        c.frame.lines.push_back({glm::dvec3((c.world * glm::dvec4(a + side * r, 1.0))), c.color});
        c.frame.lines.push_back({glm::dvec3((c.world * glm::dvec4(b + side * r, 1.0))), c.color});
    }
}

std::optional<double> pickCollider(const drawOverrides::PickContext& c)
{
    const glm::dvec3 centre(c.world[3]);
    std::optional<QPointF> at = c.view.project(centre);
    if (!at)
        return std::nullopt;
    if (!c.click)
        return QRectF(c.a, c.b).normalized().contains(*at) ? std::optional<double>(0.0) : std::nullopt;
    const double radius = numberOf(c.node, "radius") * glm::length(glm::dvec3(c.world[0])) / std::max(c.view.unitsPerPixel(centre), 1e-12);
    return std::max(0.0, std::hypot(at->x() - c.a.x(), at->y() - c.a.y()) - radius);
}

std::string infoText(NodeId id)
{
    try
    {
        const sim::Info i = sim::info(Graph::current(), id);
        if (!i.error.empty())
            return "Error: " + i.error;
        std::string s = std::to_string(i.outputs.size()) + " joints, " + std::to_string(i.substeps) + " steps per frame";
        if (i.cachedTo >= i.cachedFrom)
            s += "\nCached frames " + std::to_string(i.cachedFrom) + " to " + std::to_string(i.cachedTo);
        if (i.loop)
            s += "\nLoop " + std::to_string(i.loopStart) + " to " + std::to_string(i.loopEnd) + ": " +
                 (i.converged ? "settled in " + std::to_string(i.passes) + " passes" : "did not settle, seam cross-faded");
        return s;
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
}

/** A labelled button in the app sheet's push style. */
QToolButton* overlapPush(QWidget* parent, const QString& label, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(tip);
    b->setProperty("role", "push");
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

/** Simulate / Record / Clear buttons and a status line for one Overlap node. */
QWidget* controls(NodeId id, QWidget* parent = nullptr)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    auto* status = new QLabel(w);
    status->setWordWrap(true);
    status->setForegroundRole(QPalette::PlaceholderText);
    auto name = [id] {
        Node* n = Graph::current().node(id);
        return n ? n->path() : std::string();
    };
    auto refresh = [status, id] { status->setText(QString::fromStdString(infoText(id))); };
    auto button = [&](const char* label, const char* tip, std::function<void()> fn) {
        QToolButton* b = overlapPush(w, label, tip);
        QObject::connect(b, &QToolButton::clicked, w, [fn = std::move(fn), refresh] {
            fn();
            refresh();
        });
        row->addWidget(b);
    };
    button("Simulate", "Simulate\nSimulate to the end of the playback range (or the loop)", [name] {
        runCommand("simulate", {{"args", {name()}}, {"frame", sim::frameOf(lila::scene::Graph::current().time())}});
    });
    button("Record", "Record\nBake to a new additive animation layer", [name] { runCommand("recordSimulation", {{"args", {name()}}}); });
    button("Clear Cache", "Clear Cache\nThrow the cache away; the next frame simulates from the start", [name] { runCommand("clearSimCache", {{"args", {name()}}}); });
    v->addLayout(row);
    v->addWidget(status);
    refresh();
    return w;
}

/** The Overlap panel: the scene's Overlap nodes, setup buttons, and the selected one's controls. */
QWidget* makePanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    auto* setup = new QHBoxLayout;
    setup->setSpacing(2);
    auto add = [&](const QString& label, const char* action) {
        QToolButton* b = overlapPush(w, label, QString::fromStdString(actions::tooltip(action)));
        const std::string id = action;
        QObject::connect(b, &QToolButton::clicked, w, [id] { actions::run(json(id)); });
        optionBox::withOptionBox(b, id, w);
        setup->addWidget(b);
    };
    add("Add Chains", "overlap.create");
    add("Add Collider", "overlap.addCollider");
    add("Joint Settings", "overlap.addJoint");
    v->addLayout(setup);
    auto* list = new QListWidget(w);
    v->addWidget(list, 1);
    auto* empty = new QLabel("No Overlap chains. Select the root joints and click Add Chains.", w);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(empty);
    auto* holder = new QVBoxLayout;
    v->addLayout(holder);

    auto fill = [list, empty] {
        Graph& g = Graph::current();
        const QString current = list->currentItem() ? list->currentItem()->text() : QString();
        list->clear();
        for (NodeId id : sim::simulations(g))
            if (Node* n = g.node(id); n && n->type().isA(kOverlapType))
                list->addItem(QString::fromStdString(n->path()));
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == current)
                list->setCurrentRow(i);
        list->setVisible(list->count() > 0);
        empty->setVisible(list->count() == 0);
    };
    ui::deletion::claim(list, [list] {
        if (QListWidgetItem* it = list->currentItem())
            runCommand("delete", {{"args", {it->text().toStdString()}}});
    });
    QObject::connect(list, &QListWidget::currentTextChanged, w, [holder, w](const QString& name) {
        while (QLayoutItem* item = holder->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        Graph& g = Graph::current();
        std::vector<NodeId> found = g.findAll(name.toStdString());
        if (Node* n = found.size() == 1 ? g.node(found[0]) : nullptr)
        {
            g.select({n->id()});
            holder->addWidget(controls(n->id(), w));
        }
    });
    fill();
    std::vector<events::SubscriptionId> subs;
    for (events::Event e : {events::Event::NodeCreated, events::Event::NodeDeleted, events::Event::NodeRenamed, events::Event::SceneNew,
                            events::Event::SceneOpened})
        subs.push_back(events::subscribe(e, [fill](const events::EventData&) { fill(); }, "Overlap"));
    QObject::connect(w, &QObject::destroyed, [subs] {
        for (events::SubscriptionId s : subs)
            events::unsubscribe(s);
    });
    return w;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    drawOverrides::add(ctx, {.id = "overlap.simulation", .nodeType = kOverlapType, .draw = drawSimulation});
    drawOverrides::add(ctx, {.id = "overlap.collider", .nodeType = kColliderType, .draw = drawCollider, .pick = pickCollider});
    // The settings nodes have nothing to show in the viewport.
    drawOverrides::add(ctx, {.id = "overlap.settings", .nodeType = kChainType, .draw = [](drawOverrides::DrawContext&) {}});
    drawOverrides::add(ctx, {.id = "overlap.jointSettings", .nodeType = kJointType, .draw = [](drawOverrides::DrawContext&) {}});

    const char* cat = "Animation";
    actions::add(ctx, {.id = "overlap.create", .label = "Add Overlap Chains", .category = cat,
                       .doc = "Overlap (procedural secondary motion) on chains from the selected joints.", .command = "createOverlap"});
    actions::add(ctx, {.id = "overlap.addCollider", .label = "Add Overlap Collider", .category = cat,
                       .doc = "A sphere or capsule the Overlap chains stay out of, parented to the selection.", .command = "createOverlapCollider"});
    actions::add(ctx, {.id = "overlap.addJoint", .label = "Overlap Joint Settings", .category = cat,
                       .doc = "Per-joint Overlap settings (keyable pin, delay) for the selected joints.", .command = "addOverlapJoint"});
    actions::add(ctx, {.id = "overlap.simulate", .label = "Simulate Overlap", .category = cat,
                       .doc = "Simulate the selected (or every) Overlap to the current frame.", .command = "simulate"});
    actions::add(ctx, {.id = "overlap.record", .label = "Record Overlap", .category = cat,
                       .doc = "Bake the selected Overlap to a new additive animation layer.", .command = "recordSimulation"});
    actions::add(ctx, {.id = "overlap.clearCache", .label = "Clear Simulation Cache", .category = cat,
                       .doc = "Throw away simulation caches.", .command = "clearSimCache"});

    panels::add(ctx, {.id = "overlap", .title = "Overlap", .create = makePanel, .icon = "wave-sine", .open = false});

    menus::addItem(ctx, {.id = "overlap.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Overlap"});
    for (const char* a : {"overlap.create", "overlap.addJoint", "overlap.addCollider", "", "overlap.simulate", "overlap.record",
                          "overlap.clearCache", "", "window.overlap"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("overlap.menu.") + a, .menu = "Animation/Overlap", .action = a});
        else
            menus::addItem(ctx, {.id = "overlap.menu.sep" + std::to_string(menus::itemsIn("Animation/Overlap").size()),
                                 .menu = "Animation/Overlap", .kind = menus::Kind::Separator});
    }

    attributeEditor::addSection(ctx, kOverlapType, "Simulation", [](NodeId id) { return controls(id); });
}
} // namespace overlap
