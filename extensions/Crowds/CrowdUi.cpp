// Crowds UI: actions with search keywords, menus under Animation, Level and Create, the Crowds panel and the crowd
// node's Attribute Editor section (one set of controls for both).

#include "CrowdDraw.h"

#include <lila/Actions.h>
#include <lila/AttributeEditor.h>
#include <lila/Command.h>
#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/FileDialog.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/OptionBox.h>
#include <lila/Panels.h>
#include <lila/UI.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <span>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace crowds
{
namespace
{
const std::vector<std::string> kWords = {"crowd", "crowds", "agents", "population", "pedestrians", "golaem", "miarmy", "mass", "masscrowd", "horde", "extras"};

struct BehaviourRow
{
    const char* behaviour;
    const char* label;
    const char* icon;
    const char* doc;
};

const BehaviourRow kMove[] = {
    {"wander", "Wander", "walk", "Agents walk to random goals in an area (or on the network) and pause between them."},
    {"goTo", "Go To", "map-pin", "Agents walk to a goal node or point and stand there; an animated goal is followed."},
    {"followPath", "Follow Path", "route", "Agents walk along a curve, once or round and round."},
    {"flee", "Flee", "run", "Agents run from a threat node when it comes within range."},
    {"chase", "Chase", "target-arrow", "Agents run after a target node, or the nearest agent of another group."},
};
const BehaviourRow kGather[] = {
    {"queue", "Queue", "list-numbers", "Agents line up along a curve, its end the head; the head is served and leaves in turn."},
    {"idle", "Idle Group", "users-group", "Agents stand in a ring and turn to each other now and then."},
    {"spot", "Spot", "armchair", "Agents walk to interaction spots (sit, pick up) and perform them."},
    {"stand", "Stand", "man", "Agents stand where they are spawned."},
};

const std::span<const BehaviourRow> kSets[] = {kMove, kGather};

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

/** The crowd the UI acts on: the selected one, or the only one in the scene. */
NodeId currentCrowd()
{
    Graph& g = Graph::current();
    std::vector<std::string> sel;
    for (NodeId id : g.selection())
        sel.push_back(g.path(id));
    try
    {
        return crowdArg(g, sel, "crowds");
    }
    catch (const std::exception&)
    {
        return kNoNode;
    }
}

std::string statusText(NodeId id, std::string* warning)
{
    Graph& g = Graph::current();
    std::shared_ptr<const CrowdView> v = viewOf(g, id);
    if (!v)
    {
        const std::string err = sim::info(g, id).error;
        if (warning)
            *warning = err;
        return err.empty() ? "Not set up" : "";
    }
    const Setup& s = v->runtime->setup();
    const sim::Info i = sim::info(g, id);
    auto count = [](size_t n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
    std::string t = count(s.agents.size(), "agent") + ", " + count(s.groups.size(), "group");
    const Stats& st = v->runtime->stats();
    t += "\nMotion: " + (st.device == "none" ? std::string("cycles") : st.device);
    if (i.cachedTo >= i.cachedFrom)
        t += "\nCached " + std::to_string(i.cachedFrom) + " to " + std::to_string(i.cachedTo);
    if (st.batches > 0)
        t += "\n" + std::to_string(st.generations) + " generations in " + std::to_string(st.batches) + " batches";
    if (warning)
    {
        *warning = st.note;
        for (const std::string& c : v->characters)
            if (std::shared_ptr<const CharacterParts> p = partsFor(g, c); p && !p->problem.empty())
                *warning += (warning->empty() ? "" : "\n") + c + ": " + p->problem;
        if (v->characters.empty())
            *warning += std::string(warning->empty() ? "" : "\n") + "No characters: agents draw as proxies";
    }
    return t;
}

QWidget* box(QWidget* parent, QVBoxLayout** layout, int spacing = 2)
{
    auto* w = new QWidget(parent);
    *layout = new QVBoxLayout(w);
    (*layout)->setContentsMargins(0, 0, 0, 0);
    (*layout)->setSpacing(spacing);
    return w;
}

/** A crowd's controls: status, spawn, groups, display and output. The panel and the Attribute Editor share them. */
QWidget* controlsFor(NodeId id, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    QWidget* banner = controls::warningBanner(w);
    v->addWidget(banner);
    auto* status = new QLabel(w);
    status->setWordWrap(true);
    status->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(status);

    QVBoxLayout* sl = nullptr;
    QWidget* spawn = box(w, &sl, 6);
    for (std::span<const BehaviourRow> set : kSets)
    {
        std::vector<QWidget*> rows;
        for (const BehaviourRow& b : set)
            rows.push_back(controls::row(std::string("crowds.spawn.") + b.behaviour, b.icon, spawn, b.label));
        sl->addWidget(controls::grid(rows, 2, spawn));
    }
    v->addWidget(controls::section("Spawn", spawn, w, "crowds"));

    QVBoxLayout* gl = nullptr;
    QWidget* groupsBox = box(w, &gl);
    auto* groups = new QListWidget(groupsBox);
    groups->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* noGroups = new QLabel("No groups. Pick a behaviour under Spawn.", groupsBox);
    noGroups->setWordWrap(true);
    noGroups->setForegroundRole(QPalette::PlaceholderText);
    gl->addWidget(groups);
    gl->addWidget(noGroups);
    v->addWidget(controls::section("Groups", groupsBox, w, "crowds"));

    QVBoxLayout* dl = nullptr;
    QWidget* display = box(w, &dl, 4);
    auto* quality = new controls::Segmented(display);
    quality->add("Auto", {}, "Auto\nThe nearest agents as characters, the rest as proxies", 0);
    quality->add("Full", {}, "Full\nEvery agent as its character", 1);
    quality->add("Proxy", {}, "Proxy\nA capsule skeleton per agent", 2);
    quality->add("Points", {}, "Points\nA marker per agent, the cheapest", 3);
    dl->addWidget(quality);
    std::vector<QWidget*> displayRows = {controls::row("crowds.showPaths", "route-2", display, "Paths"),
                                         controls::row("crowds.setCharacters", "users", display, "Characters")};
    dl->addWidget(controls::grid(displayRows, 2, display));
    v->addWidget(controls::section("Display", display, w, "crowds"));

    QVBoxLayout* ol = nullptr;
    QWidget* output = box(w, &ol);
    ol->addWidget(controls::grid({controls::row("crowds.simulate", "player-play", output, "Simulate"),
                                  controls::row("crowds.bake", "stack-2", output, "Bake"),
                                  controls::row("crowds.sendToUnreal", "send", output, "Sequence to Unreal"),
                                  controls::row("crowds.sendMassToUnreal", "route", output, "Mass to Unreal"),
                                  controls::row("crowds.export", "file-export", output, "Export")},
                                 2, output));
    ol->addWidget(controls::overflow({"crowds.clearCache"}, output));
    v->addWidget(controls::section("Output", output, w, "crowds"));

    QPointer<QLabel> statusP(status);
    QPointer<QWidget> bannerP(banner);
    QPointer<QListWidget> groupsP(groups);
    QPointer<QLabel> noGroupsP(noGroups);
    QPointer<controls::Segmented> qualityP(quality);
    auto refresh = [=] {
        if (!statusP)
            return;
        Graph& g = Graph::current();
        const Node* n = g.node(id);
        if (!n || !n->type().isA(kCrowdType))
            return;
        std::string warning;
        statusP->setText(QString::fromStdString(statusText(id, &warning)));
        controls::setWarning(bannerP, QString::fromStdString(warning));
        const int64_t q = n->as<int64_t>(n->type().attrIndex("quality"));
        if (qualityP->current() != int(q))
            qualityP->setCurrent(int(q));
        json gs = json::parse(n->as<std::string>(n->type().attrIndex("groups")).empty() ? "[]" : n->as<std::string>(n->type().attrIndex("groups")), nullptr, false);
        if (!gs.is_array())
            gs = json::array();
        QStringList rows;
        for (const json& gr : gs)
            rows << QString::fromStdString(gr.value("name", "") + "    " + gr.value("behaviour", "") + "    " +
                                           std::to_string(gr.value("agents", json::array()).size()));
        QStringList have;
        for (int k = 0; k < groupsP->count(); ++k)
            have << groupsP->item(k)->text();
        if (have != rows)
        {
            groupsP->clear();
            groupsP->addItems(rows);
        }
        // As tall as its rows, up to five, so one group doesn't hold a column of empty space.
        const int rowH = std::max(groupsP->sizeHintForRow(0), groupsP->fontMetrics().height() + 4);
        groupsP->setFixedHeight(rowH * std::clamp(int(rows.size()), 1, 5) + 2 * groupsP->frameWidth() + 2);
        groupsP->setVisible(!rows.isEmpty());
        noGroupsP->setVisible(rows.isEmpty());
    };
    auto path = [id] {
        const Node* n = Graph::current().node(id);
        return n ? n->path() : std::string();
    };
    QObject::connect(quality, &controls::Segmented::picked, w, [path](int q) { runCommand("setAttr", {{"args", {path() + ".quality", q}}}); });
    auto removeGroup = [groups, path] {
        if (QListWidgetItem* it = groups->currentItem())
        {
            const std::string name = it->text().split(' ').front().toStdString();
            runCommand("crowdGroups", {{"args", {path()}}, {"remove", name}});
        }
    };
    ui::deletion::claim(groups, removeGroup);
    QObject::connect(groups, &QListWidget::customContextMenuRequested, w, [groups, removeGroup](const QPoint& at) {
        if (!groups->itemAt(at))
            return;
        QMenu menu;
        menu.addAction("Remove Group", removeGroup);
        menu.exec(groups->viewport()->mapToGlobal(at));
    });
    auto* timer = new QTimer(w);
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, w, [w, refresh] {
        if (w->isVisible())
            refresh();
    });
    timer->start();
    const auto sub = events::subscribe(events::Event::UndoStackChanged, [refresh](const events::EventData&) { refresh(); }, "Crowds");
    QObject::connect(w, &QObject::destroyed, [sub] { events::unsubscribe(sub); });
    refresh();
    return w;
}

/** The Crowds panel: the scene's crowds and the chosen one's controls. */
QWidget* makePanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(8);
    v->addWidget(controls::grid({controls::row("crowds.create", "user-plus", w, "New Crowd")}, 2, w));
    auto* list = new QListWidget(w);
    list->setMaximumHeight(90);
    v->addWidget(list);
    auto* empty = new QLabel("No crowds. Click New Crowd, with characters selected to give it them.", w);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(empty);
    auto* holder = new QVBoxLayout;
    v->addLayout(holder);
    v->addStretch(1);

    QPointer<QListWidget> listP(list);
    QPointer<QLabel> emptyP(empty);
    auto fill = [listP, emptyP] {
        if (!listP)
            return;
        Graph& g = Graph::current();
        const QString current = listP->currentItem() ? listP->currentItem()->text() : QString();
        QStringList names;
        for (NodeId id : sim::simulations(g))
            if (Node* n = g.node(id); n && n->type().isA(kCrowdType))
                names << QString::fromStdString(n->path());
        QStringList have;
        for (int k = 0; k < listP->count(); ++k)
            have << listP->item(k)->text();
        if (have != names)
        {
            listP->clear();
            listP->addItems(names);
        }
        const QString want = !current.isEmpty() && names.contains(current) ? current : names.isEmpty() ? QString() : names.front();
        for (int i = 0; i < listP->count(); ++i)
            if (listP->item(i)->text() == want && listP->currentRow() != i)
                listP->setCurrentRow(i);
        listP->setVisible(names.size() > 1);
        emptyP->setVisible(names.isEmpty());
    };
    QObject::connect(list, &QListWidget::currentTextChanged, w, [holder, w](const QString& name) {
        while (QLayoutItem* item = holder->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        Graph& g = Graph::current();
        std::vector<NodeId> found = name.isEmpty() ? std::vector<NodeId>{} : g.findAll(name.toStdString());
        if (found.size() == 1)
            holder->addWidget(controlsFor(found[0], w));
    });
    fill();
    std::vector<events::SubscriptionId> subs;
    for (events::Event e : {events::Event::NodeCreated, events::Event::NodeDeleted, events::Event::NodeRenamed, events::Event::SceneNew,
                            events::Event::SceneOpened})
        subs.push_back(events::subscribe(e, [fill](const events::EventData&) { fill(); }, "Crowds"));
    const auto sel = events::subscribe(events::Event::SelectionChanged, [listP](const events::EventData&) {
        const NodeId c = currentCrowd();
        if (!listP || !c)
            return;
        const QString name = QString::fromStdString(Graph::current().path(c));
        for (int i = 0; i < listP->count(); ++i)
            if (listP->item(i)->text() == name && listP->currentRow() != i)
                listP->setCurrentRow(i);
    }, "Crowds");
    subs.push_back(sel);
    QObject::connect(w, &QObject::destroyed, [subs] {
        for (events::SubscriptionId s : subs)
            events::unsubscribe(s);
    });
    return w;
}

void createFromSelection()
{
    Graph& g = Graph::current();
    json chars = json::array();
    for (NodeId id : g.selection())
        if (characterJoint(g, id))
            chars.push_back(g.path(id));
    json call = json::object();
    if (!chars.empty())
        call["characters"] = chars;
    runCommand("crowdCreate", call);
}

void setCharactersFromSelection()
{
    Graph& g = Graph::current();
    const NodeId c = currentCrowd();
    if (!c)
        return log::write(LogLevel::Warning, "Characters: select the crowd and the characters it should wear");
    json chars = json::array();
    for (NodeId id : g.selection())
        if (id != c && characterJoint(g, id))
            chars.push_back(g.path(id));
    if (chars.empty())
        return log::write(LogLevel::Warning, "Characters: select skinned characters along with the crowd");
    runCommand("crowdCharacters", {{"args", {g.path(c)}}, {"characters", chars}});
}

void exportCrowd()
{
    const NodeId c = currentCrowd();
    if (!c)
        return log::write(LogLevel::Warning, "Export Crowd: select a crowd");
    const QString dir = fileDialog::getExistingDirectory(nullptr, "Export Crowd Agents");
    if (dir.isEmpty())
        return;
    runCommand("crowdExport", {{"args", {Graph::current().path(c)}}, {"path", dir.toStdString()}});
}
} // namespace

void registerUi(ext::Context& ctx)
{
    registerDraw(ctx);
    const char* cat = "Crowds";
    auto words = [](std::vector<std::string> extra) {
        extra.insert(extra.end(), kWords.begin(), kWords.end());
        return extra;
    };
    actions::add(ctx, {.id = "crowds.create", .label = "New Crowd", .category = cat, .run = createFromSelection,
                       .doc = "A crowd node, wearing the selected characters. Then spawn groups of agents into it.", .icon = "user-plus",
                       .keywords = words({"create", "new"})});
    for (std::span<const BehaviourRow> set : kSets)
        for (const BehaviourRow& b : set)
            actions::add(ctx, {.id = std::string("crowds.spawn.") + b.behaviour, .label = std::string("Spawn ") + b.label, .category = cat,
                               .doc = b.doc, .command = "crowdSpawn", .call = {{"behaviour", b.behaviour}}, .icon = b.icon,
                               .keywords = words({"spawn", b.behaviour, b.label})});
    actions::add(ctx, {.id = "crowds.setCharacters", .label = "Set Crowd Characters", .category = cat, .run = setCharactersFromSelection,
                       .doc = "The selected characters become the ones the selected crowd's agents wear.", .icon = "users",
                       .keywords = words({"characters", "variety"})});
    actions::add(ctx, {.id = "crowds.showPaths", .label = "Show Crowd Paths", .category = cat,
                       .run = [] {
                           const NodeId c = currentCrowd();
                           if (!c)
                               return;
                           const Node* n = Graph::current().node(c);
                           const bool on = n->as<bool>(n->type().attrIndex("showPaths"));
                           runCommand("setAttr", {{"args", {n->path() + ".showPaths", !on}}});
                       },
                       .doc = "Draw each moving agent's line to its goal.",
                       .checked =
                           [] {
                               const NodeId c = currentCrowd();
                               const Node* n = c ? Graph::current().node(c) : nullptr;
                               return n && n->as<bool>(n->type().attrIndex("showPaths"));
                           },
                       .icon = "route-2", .keywords = words({"paths", "goals", "debug"})});
    actions::add(ctx, {.id = "crowds.simulate", .label = "Simulate Crowd", .category = cat,
                       .doc = "Simulate the crowd to the end of the playback range, so it plays and scrubs from the cache.", .command = "crowdSimulate",
                       .icon = "player-play", .keywords = words({"simulate", "cache", "generate"})});
    actions::add(ctx, {.id = "crowds.bake", .label = "Bake Crowd", .category = cat,
                       .doc = "Bake every agent onto its own copy of its character, keyed every frame.", .command = "crowdBake", .icon = "stack-2",
                       .keywords = words({"bake", "keys", "characters"})});
    actions::add(ctx, {.id = "crowds.export", .label = "Export Crowd...", .category = cat, .run = exportCrowd,
                       .doc = "Bake the agents and write one FBX per agent into a folder.", .icon = "file-export",
                       .keywords = words({"export", "fbx", "gltf"})});
    actions::add(ctx, {.id = "crowds.sendToUnreal", .label = "Send Crowd to Unreal", .category = cat,
                       .doc = "Bake the agents and send them to Unreal as a Level Sequence: one skeletal mesh actor per agent with its animation.",
                       .command = "crowdSendToUnreal", .icon = "send", .keywords = words({"unreal", "sequencer", "level sequence", "ue5"})});
    actions::add(ctx, {.id = "crowds.sendMassToUnreal", .label = "Send Mass Crowd to Unreal", .category = cat,
                       .doc = "Send the crowd to Unreal as a live Mass crowd: its sidewalks and crossings as ZoneGraph lanes, the groups walking "
                              "them as Mass agents that wander the lanes and cross at the crossings. Needs the LilaLinkCrowd plugin.",
                       .command = "crowdSendToUnreal", .call = {{"mass", true}}, .icon = "route",
                       .keywords = words({"unreal", "mass", "masscrowd", "mass ai", "zonegraph", "statetree", "pedestrians", "city sample", "ue5"})});
    actions::add(ctx, {.id = "crowds.clearCache", .label = "Clear Crowd Cache", .category = cat,
                       .doc = "Throw the crowd's cache away; the next frame simulates from the start.", .command = "clearSimCache", .icon = "trash",
                       .keywords = words({"cache", "clear"})});

    panels::add(ctx, {.id = "crowds", .title = "Crowds", .create = makePanel, .icon = "users-group", .open = false,
                      .keywords = words({"panel"})});

    auto submenu = [&](const std::string& under, const std::string& id) {
        const std::string menu = under + "/Crowds";
        menus::addItem(ctx, {.id = id, .menu = under, .kind = menus::Kind::Submenu, .label = "Crowds"});
        int sep = 0;
        auto add = [&](const std::string& a) {
            if (a.empty())
                menus::addItem(ctx, {.id = id + ".sep" + std::to_string(sep++), .menu = menu, .kind = menus::Kind::Separator});
            else
                menus::addItem(ctx, {.id = id + "." + a, .menu = menu, .action = a});
        };
        add("crowds.create");
        add("crowds.setCharacters");
        add("");
        for (std::span<const BehaviourRow> set : kSets)
        {
            for (const BehaviourRow& b : set)
                add(std::string("crowds.spawn.") + b.behaviour);
            add("");
        }
        for (const char* a : {"crowds.simulate", "crowds.bake", "crowds.export", "crowds.sendToUnreal", "crowds.sendMassToUnreal", "crowds.clearCache", "", "crowds.showPaths", "window.crowds"})
            add(a);
    };
    submenu("Animation", "crowds.menu.anim");
    submenu("Level", "crowds.menu.level");
    menus::addItem(ctx, {.id = "crowds.menu.create", .menu = "Create", .action = "crowds.create"});

    attributeEditor::addSection(ctx, kCrowdType, "Crowd", [](NodeId id) { return controlsFor(id, nullptr); });
}
} // namespace crowds
