// Site kits in the UI: an action per kind (its option box shows only that kind's settings), the Site Kits palette that
// picks the kind, Level > Open World > Site Kit, the Open World toolkit's Sites group, and the Attribute Editor's Site
// section that edits a site live.

#include "OpenWorld.h"
#include "Sites.h"

#include <lila/Actions.h>
#include <lila/AttributeEditor.h>
#include <lila/Command.h>
#include <lila/Controls.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Panels.h>
#include <lila/Transform.h>

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>

using namespace lila;
using namespace lila::ui;

namespace openworld
{
namespace
{
using json = nlohmann::json;
namespace opt = ops::options;

constexpr const char* kSiteMenu = "Level/Open World/Site Kit";

std::string kindAction(const std::string& kind) { return "openworld.site." + kind; }

/** The site modifier on a node or in its stack, or null. */
scene::Node* siteModifierFor(scene::Graph& g, scene::Node* n)
{
    if (!n)
        return nullptr;
    auto is = [](const scene::Node& m) { return gn::isGnModifier(m) && m.type().attrIndex("siteSpec") >= 0; };
    if (is(*n))
        return n;
    for (scene::NodeId id : scene::modifiers::stack(g, n->id()))
        if (scene::Node* m = g.node(id); m && is(*m))
            return m;
    return nullptr;
}

/** The selected site's mesh, or empty (the commands then take the only site). */
std::string selectedSite()
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && siteModifierFor(g, n))
            return n->path();
    return {};
}

void warnOnThrowSites(const std::function<void()>& f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}

/** siteCreate's option box: each kind's settings in its own section, enabled only for that kind, hidden when the kind is fixed. */
void siteOptionBox()
{
    opt::Override o;
    o.order = {"kind", "name", "terrain", "at", "heading", "ground", "culture", "seed", "waterLevel"};
    o.hidden = {"params", "id"};
    o.shown = {"at"};
    o.labels = {{"at", "At (X, Z)"}, {"waterLevel", "Water Level"}};
    o.ranges["heading"] = {-360.0, 360.0, 5.0, " deg"};
    o.editors["kind"] = "grouped";
    for (const sites::Kind& k : sites::kinds())
    {
        o.choices["kind"].push_back({k.id, k.label});
        o.choiceDocs["kind"][k.id] = k.doc;
        o.choiceGroups["kind"][k.id] = k.group;
    }
    for (const char* f : {"name", "terrain", "at", "heading", "ground", "culture", "seed", "waterLevel"})
        o.sections[f] = "Site";
    for (const sites::Param& p : sites::params())
    {
        std::vector<std::string> takers;
        std::string section;
        for (const sites::Kind& k : sites::kinds())
            if (std::ranges::find(k.params, p.name) != k.params.end())
            {
                takers.push_back(k.id);
                section = section.empty() ? k.label : section;
            }
        // A setting two kinds share sits under the first one's heading; the other kind's box shows it all the same.
        o.sections[p.name] = takers.size() > 1 ? std::string("Compound") : section;
        o.enabledWhen[p.name] = {"kind", takers};
        o.order.push_back(p.name);
        const bool share = p.type == sites::Param::Type::Float && p.max == 1.0;
        const bool length = p.type == sites::Param::Type::Float && !share;
        if (share)
            o.ranges[p.name] = {0.0, 1.0, 0.05, ""};
        else if (length)
            o.ranges[p.name] = {p.min, p.max, 100.0, " cm"};
    }
    o.fills.push_back({"From Selection", "At the selected object's position, on the selected terrain", [] {
                           json v = json::object();
                           scene::Graph& g = scene::Graph::current();
                           for (scene::NodeId id : g.selection())
                               if (scene::Node* n = g.node(id); n && n->type().isA("transform"))
                               {
                                   bool terrain = false;
                                   if (n->type().isA("mesh") && ops::find("terrainInfo"))
                                       try
                                       {
                                           ops::execute("terrainInfo", {{"args", {n->path()}}});
                                           terrain = true;
                                       }
                                       catch (const std::exception&)
                                       {
                                       }
                                   if (terrain)
                                       v["terrain"] = n->path();
                                   else
                                   {
                                       const glm::dmat4 m = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                                       v["at"] = json::array({m[3].x, m[3].z});
                                   }
                               }
                           return v;
                       }});
    o.docs = "open-world?id=site-kits";
    opt::setOverride("siteCreate", std::move(o));
}

/** The Site Kits palette: every kind as a row, grouped Coast, Inland, Compounds; Check and Regrade (the selected site, else every one) under them. */
QWidget* makePalette(QWidget* parent)
{
    auto* root = new QWidget(parent);
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(4);
    for (const char* group : {"Coast", "Inland", "Compounds"})
    {
        auto* box = new QWidget(root);
        std::vector<QWidget*> rows;
        for (const sites::Kind& k : sites::kinds())
            if (k.group == group)
                rows.push_back(controls::row(kindAction(k.id), k.icon.c_str(), box, QString::fromStdString(k.label)));
        auto* g = controls::grid(rows, 2, box);
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->addWidget(g);
        v->addWidget(controls::section(group, box, root, (std::string("openworld.siteKits.") + group).c_str()));
    }
    auto* tools = new QWidget(root);
    auto* tl = new QVBoxLayout(tools);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->addWidget(controls::grid({controls::row("openworld.siteCheck", "list-check", tools, "Check"), controls::row("openworld.siteGround", "shovel", tools, "Regrade")}, 2,
                                 tools));
    v->addWidget(controls::section("Sites", tools, root, "openworld.siteKits.tools"));
    v->addStretch(1);
    return root;
}

/** The Attribute Editor's Site section on a site mesh: name and kind, its settings live through siteEdit, Regrade and Check. */
QWidget* makeSiteSection(scene::NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* node = g.node(id);
    scene::Node* mod = siteModifierFor(g, node);
    if (!mod)
        return nullptr;
    sites::Spec spec;
    try
    {
        const json text = ops::execute("getAttr", {{"args", {mod->path() + ".siteSpec"}}});
        spec = sites::specFromJson(json::parse(text.get<std::string>()));
    }
    catch (const std::exception& e)
    {
        auto* l = new QLabel(QString::fromStdString(std::string("Broken site spec: ") + e.what()));
        l->setWordWrap(true);
        return l;
    }
    const sites::Kind* kind = sites::findKind(spec.kind);
    const std::string mesh = node->path();
    auto* w = new QWidget();
    auto* grid = new QGridLayout(w);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(3);
    int r = 0;
    auto info = [&](const QString& label, const QString& value) {
        auto* l = new QLabel(label, w);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        controls::setRole(l, "dim");
        grid->addWidget(l, r, 0);
        grid->addWidget(new QLabel(value, w), r, 1);
        ++r;
    };
    info("Kit", QString::fromStdString(kind ? kind->label : spec.kind));
    info("Id", QString::fromStdString(spec.id));

    const json fixed = {{"kind", spec.kind}};
    std::vector<std::string> wanted = {"name", "heading", "seed"};
    if (kind)
        wanted.insert(wanted.end(), kind->params.begin(), kind->params.end());
    const std::vector<opt::Field> all = opt::fields("siteCreate", fixed);
    for (const std::string& flag : wanted)
    {
        auto it = std::ranges::find_if(all, [&](const opt::Field& f) { return f.flag == flag; });
        if (it == all.end())
            continue;
        json value = flag == "name" ? json(spec.name) : flag == "heading" ? json(spec.heading) : flag == "seed" ? json(spec.seed) : spec.params.value(flag, json());
        FlagFieldWidgets f = makeFlagField(*it, value, [mesh, flag](const json& v) {
            warnOnThrowSites([&] { ops::execute("siteEdit", {{"args", {mesh}}, {flag, v}}, CallSource::UI); });
        }, w);
        if (f.label)
            grid->addWidget(f.label, r, 0);
        grid->addWidget(f.field, r, f.label ? 1 : 0, 1, f.label ? 1 : 2);
        ++r;
    }
    auto* buttons = controls::grid({controls::push("openworld.siteGround", w, "Regrade"), controls::push("openworld.siteCheck", w, "Check")}, 2, w);
    grid->addWidget(buttons, r, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    return w;
}
} // namespace

void registerSitesUi(ext::Context& ctx)
{
    siteOptionBox();
    opt::Override edit;
    edit.hidden = {"params"};
    opt::setOverride("siteEdit", std::move(edit));

    static const std::vector<std::string> kKeywords = {"site kit", "preset", "open world", "gta", "level", "place", "location"};
    std::vector<std::string> kindIds;
    for (const sites::Kind& k : sites::kinds())
    {
        std::vector<std::string> keywords = kKeywords;
        keywords.insert(keywords.end(), k.keywords.begin(), k.keywords.end());
        actions::add(ctx, {.id = kindAction(k.id),
                           .label = k.label,
                           .category = "Open World",
                           .doc = k.doc,
                           .python = "siteCreate(kind='" + k.id + "', terrain='Terrain', at=[0, 0])",
                           .command = "siteCreate",
                           .call = {{"kind", k.id}},
                           .icon = k.icon,
                           .keywords = keywords});
        kindIds.push_back(kindAction(k.id));
    }
    actions::add(ctx, {.id = "openworld.siteCreate",
                       .label = "Site Kit",
                       .category = "Open World",
                       .doc = "Make any site kit: pick the kind and its settings in the option box.",
                       .python = "siteCreate(kind='port', terrain='Terrain', at=[0, -20000])",
                       .command = "siteCreate",
                       .icon = "map-pin-plus",
                       .keywords = kKeywords});
    actions::add(ctx, {.id = "openworld.siteGround",
                       .label = "Regrade Site",
                       .category = "Open World",
                       .run = [] { warnOnThrowSites([] { ops::execute("siteGround", {{"args", json::array({selectedSite()})}}, CallSource::UI); }); },
                       .doc = "Grade the terrain under the selected site again from its settings.",
                       .python = "siteGround('port1')",
                       .icon = "shovel",
                       .keywords = {"site", "grade", "terrain", "flatten", "pad"}});
    actions::add(ctx, {.id = "openworld.siteCheck",
                       .label = "Check Sites",
                       .category = "Open World",
                       .run =
                           [] {
                               warnOnThrowSites([] {
                                   const json c = ops::execute("siteCheck", {{"args", json::array({selectedSite()})}}, CallSource::UI);
                                   if (c.value("ok", false))
                                       log::info("siteCheck: no issues");
                                   for (const json& i : c["issues"])
                                       log::warn("siteCheck: {} {} at [{:.0f}, {:.0f}]: {}", i["site"].get<std::string>(), i["kind"].get<std::string>(),
                                                 i["at"][0].get<double>(), i["at"][1].get<double>(), i["what"].get<std::string>());
                                   for (const json& n : c["notes"])
                                       log::info("siteCheck: {}", n.get<std::string>());
                               });
                           },
                       .doc = "Check the selected site (or every site): real names, overlaps, water, steep ground, stale grading.",
                       .python = "siteCheck()",
                       .icon = "list-check",
                       .keywords = {"site", "validate", "check", "overlap", "real names"}});

    panels::add(ctx, {.id = "openworld.siteKits",
                      .title = "Site Kits",
                      .create = makePalette,
                      .area = Qt::RightDockWidgetArea,
                      .icon = "building-warehouse",
                      .open = false,
                      .toggleAction = "window.siteKits",
                      .keywords = {"site kit", "port", "airstrip", "cemetery", "farm", "village", "compound", "island", "lighthouse", "gta"},
                      .floatFirst = true,
                      .floatWidth = 380,
                      .floatHeight = 460,
                      .docs = "open-world?id=site-kits"});

    std::vector<levelToolkit::Item> items = {{.action = "window.siteKits", .icon = "building-warehouse", .text = "Kits", .shelf = "Sites", .group = "Sites"},
                                             {.action = "openworld.siteGround", .icon = "shovel", .text = "Regrade", .group = "Sites"},
                                             {.action = "openworld.siteCheck", .icon = "list-check", .text = "Check", .group = "Sites"}};
    for (const std::string& a : kindIds)
        items.push_back({.action = a, .rare = true});
    addTools(ctx, {"", "openworld.siteGround", "openworld.siteCheck"}, items);

    // Level > Open World > Site Kit: the palette first, every kind, then the any-kind box.
    menus::addItem(ctx, {.id = "openworld.menu.siteKit", .menu = kMenu, .kind = menus::Kind::Submenu, .label = "Site Kit",
                         .before = "openworld.menu.openworld.siteGround"});
    menus::addItem(ctx, {.id = "openworld.siteKit.palette", .menu = kSiteMenu, .action = "window.siteKits"});
    menus::addItem(ctx, {.id = "openworld.siteKit.sep1", .menu = kSiteMenu, .kind = menus::Kind::Separator});
    for (const std::string& a : kindIds)
        menus::addItem(ctx, {.id = "openworld.siteKit." + a, .menu = kSiteMenu, .action = a});
    menus::addItem(ctx, {.id = "openworld.siteKit.sep2", .menu = kSiteMenu, .kind = menus::Kind::Separator});
    menus::addItem(ctx, {.id = "openworld.siteKit.any", .menu = kSiteMenu, .action = "openworld.siteCreate"});

    attributeEditor::addSection(ctx, "mesh", "Site", makeSiteSection);
}
} // namespace openworld
