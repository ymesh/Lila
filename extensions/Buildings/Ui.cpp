// Buildings in the UI: Level > Buildings, the Level Toolkit's Buildings section, and the Attribute Editor section on
// building meshes (the building being edited, its style, floors, bays, seed, palette, LOD and its interior).

#include "BuildingsScene.h"

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
#include <lila/UI.h>

#include <QCheckBox>
#include <QPointer>
#include <QTimer>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

using namespace lila;
using namespace lila::ui;

namespace buildings
{
std::string& currentBuilding()
{
    static std::string id;
    return id;
}

namespace
{
const std::vector<std::string> kKeywords = {"building", "facade", "procedural building", "cityengine", "buildify", "house", "city"};
constexpr const char* kMenu = "Level/Buildings";

json runUi(const char* cmd, json call)
{
    try
    {
        return ops::execute(cmd, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return json();
    }
}

/** The Buildings modifier feeding a mesh node, if it is a building mesh. */
scene::Node* modifierOf(scene::NodeId mesh)
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId m : scene::modifiers::stack(g, mesh))
        if (scene::Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            if (scene::Node* grp = gn::groupOfModifier(g, *mod); grp && grp->name() == kGroupName)
                return mod;
    return nullptr;
}

/** Building ids of the selected building meshes. */
json selectedBuildings()
{
    json ids = json::array();
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
        if (scene::Node* mod = modifierOf(id))
            if (auto p = published(uint64_t(mod->id())))
                for (const Spec& s : p->specs)
                    ids.push_back(s.id);
    return ids;
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

struct SectionHost
{
    QPointer<QWidget> box;
    scene::NodeId mesh;
};
std::vector<SectionHost>& hosts()
{
    static std::vector<SectionHost> h;
    return h;
}

QWidget* buildingForm(scene::NodeId meshId);

void populate(QWidget* box, scene::NodeId mesh)
{
    auto* lay = box->layout();
    while (QLayoutItem* it = lay->takeAt(0))
    {
        if (QWidget* w = it->widget())
            w->deleteLater();
        delete it;
    }
    if (QWidget* form = buildingForm(mesh))
        lay->addWidget(form);
}
} // namespace

void refreshSections()
{
    // Deferred: the widget asking may be one of those rebuilt.
    QTimer::singleShot(0, [] {
        auto& h = hosts();
        std::erase_if(h, [](const SectionHost& s) { return s.box.isNull(); });
        for (SectionHost& s : h)
            if (scene::Graph::current().node(s.mesh))
                populate(s.box, s.mesh);
    });
}

namespace
{
QWidget* buildingSection(scene::NodeId meshId)
{
    if (!modifierOf(meshId))
        return nullptr;
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    hosts().push_back({box, meshId});
    populate(box, meshId);
    return box;
}

QWidget* buildingForm(scene::NodeId meshId)
{
    scene::Node* mod = modifierOf(meshId);
    if (!mod)
        return nullptr;
    scene::Graph::current().evaluate();
    std::shared_ptr<const Published> pub = published(uint64_t(mod->id()));
    if (!pub || pub->specs.empty())
        return nullptr;
    // The building being edited: the one picked last if it is here, else the first.
    std::string id = currentBuilding();
    int index = pub->byId.count(id) ? pub->byId.at(id) : 0;
    id = pub->specs[size_t(index)].id;
    const Spec& spec = pub->specs[size_t(index)];
    const Result& res = *pub->results[size_t(index)];
    const json edits = [&] {
        try
        {
            const json all = json::parse(ops::execute("getAttr", {{"args", {mod->path() + ".edits"}}}).get<std::string>());
            return all.value(id, json::object());
        }
        catch (const std::exception&)
        {
            return json::object();
        }
    }();

    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    f->setContentsMargins(0, 0, 0, 0);
    f->setSpacing(4);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* which = new QComboBox(w);
    for (size_t i = 0; i < pub->specs.size(); ++i)
    {
        const Spec& s = pub->specs[i];
        const std::string label = s.name.empty() ? "Building " + std::to_string(i + 1) : s.name;
        std::string styleLabel = pub->results[i]->style;
        if (std::shared_ptr<const Style> st = findStyle(styleLabel))
            styleLabel = st->label;
        const std::string text = label + "  (" + styleLabel + ")";
        which->addItem(qs(text), qs(s.id));
        which->setItemData(int(i), qs(s.id), Qt::ToolTipRole);
    }
    which->setCurrentIndex(index);
    which->setToolTip("The building the fields below edit; Paint Bays picks one by clicking it");
    QObject::connect(which, &QComboBox::activated, w, [which, meshId](int i) {
        currentBuilding() = which->itemData(i).toString().toStdString();
        refreshSections();
        (void)meshId;
    });
    f->addRow("Building", which);

    auto edit = [id](json flags) {
        flags["args"] = json::array({id});
        runUi("buildingEdit", flags);
        refreshSections();
    };

    auto* style = new QComboBox(w);
    for (const auto& s : allStyles())
        style->addItem(qs(s->label), qs(s->id));
    style->setCurrentIndex(std::max(0, style->findData(qs(res.style))));
    style->setToolTip(qs("Style. Picked from " + (edits.contains("style") ? std::string("an edit") : spec.styleFrom.empty() ? "the program" : spec.styleFrom)));
    QObject::connect(style, &QComboBox::activated, w, [style, edit](int i) { edit({{"style", style->itemData(i).toString().toStdString()}}); });
    f->addRow("Style", style);

    auto* floors = new QSpinBox(w);
    floors->setRange(1, 120);
    floors->setValue(res.floors);
    floors->setAlignment(Qt::AlignRight);
    floors->setButtonSymbols(QAbstractSpinBox::NoButtons);
    QObject::connect(floors, &QSpinBox::editingFinished, w, [floors, edit, was = res.floors] {
        if (floors->value() != was)
            edit({{"floors", floors->value()}});
    });
    f->addRow("Floors", floors);

    auto* bay = controls::valueField(80.0, 3000.0, 0, " cm", w);
    bay->setValue(edits.value("bayWidth", findStyle(res.style) ? findStyle(res.style)->bayWidth : 320.0));
    QObject::connect(bay, &QDoubleSpinBox::editingFinished, w, [bay, edit] { edit({{"bayWidth", bay->value()}}); });
    f->addRow("Bay Width", bay);

    auto* seed = new QSpinBox(w);
    seed->setRange(0, 9999);
    seed->setValue(edits.value("seed", 0));
    seed->setAlignment(Qt::AlignRight);
    seed->setButtonSymbols(QAbstractSpinBox::NoButtons);
    seed->setToolTip("Another seed gives other colours, balconies, awnings and clutter");
    QObject::connect(seed, &QSpinBox::editingFinished, w, [seed, edit] { edit({{"seed", seed->value()}}); });
    f->addRow("Seed", seed);

    // Palette swatches: render and trim.
    auto* colours = new QWidget(w);
    auto* ch = new QHBoxLayout(colours);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(4);
    auto swatch = [&](const char* key, glm::vec3 linear, const QString& tip) {
        auto* b = new QToolButton(colours);
        b->setProperty("lilaRawColors", true);
        const QString hex = qs(colourHex(linear));
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #555; min-width: 36px; min-height: 18px; }").arg(hex));
        b->setToolTip(tip + " " + hex);
        QObject::connect(b, &QToolButton::clicked, b, [b, key, hex, edit] {
            const QColor c = colorPopup::getColor(QColor(hex), b, "Building colour");
            if (c.isValid())
                edit({{"palette", {{key, c.name().toStdString()}}}});
        });
        ch->addWidget(b);
    };
    swatch("render", res.palette.render, "Render");
    swatch("trim", res.palette.trim, "Trim");
    swatch("roof", res.palette.roof, "Roof");
    ch->addStretch(1);
    f->addRow("Colours", colours);

    auto* enter = new QCheckBox(w);
    enter->setChecked(res.enterable);
    enter->setToolTip("Rooms inside, generated from the building; off goes back to fake interiors behind the windows");
    QObject::connect(enter, &QCheckBox::clicked, w, [id](bool on) {
        runUi(on ? "buildingMakeEnterable" : "buildingMakeExterior", {{"args", {id}}});
        refreshSections();
    });
    f->addRow("Enterable", enter);
    if (res.enterable)
        if (std::shared_ptr<const Interior> in = interiorOf(id))
        {
            auto* program = new QComboBox(w);
            for (const std::string& p : interiorPrograms())
            {
                QString label = qs(p);
                label[0] = label[0].toUpper();
                program->addItem(label, qs(p));
            }
            program->setCurrentIndex(std::max(0, program->findData(qs(in->program))));
            program->setToolTip(qs("Interior program, from " + (in->programFrom == "given" ? std::string("an edit") : in->programFrom == "fallback"
                                                                      ? std::string("nothing it knows: generic rooms")
                                                                      : std::string("the building's use"))));
            QObject::connect(program, &QComboBox::activated, w, [program, id](int i) {
                runUi("buildingRoom", {{"args", {id}}, {"program", program->itemData(i).toString().toStdString()}});
                refreshSections();
            });
            f->addRow("Program", program);
            int floorCount = 0, unreachable = 0;
            for (const Room& r : in->rooms)
            {
                floorCount = std::max(floorCount, r.floor + 1);
                unreachable += r.reachable ? 0 : 1;
            }
            std::string summary = std::to_string(in->rooms.size()) + " rooms, " + std::to_string(in->doors.size()) + " doors, " + std::to_string(floorCount) +
                                  (floorCount == 1 ? " floor" : " floors");
            if (unreachable > 0)
                summary += ", " + std::to_string(unreachable) + " unreachable";
            auto* rooms = new QLabel(qs(summary), w);
            rooms->setEnabled(unreachable > 0);
            f->addRow("Rooms", rooms);
        }

    auto* lod = new controls::Segmented(w);
    lod->add("Near", {}, "Shell and modules", 0);
    lod->add("HLOD", {}, "Simplified proxy: tiers and roofs", 1);
    lod->add("Far", {}, "Flat-roof massing", 2);
    lod->setCurrent(pub->lod == "hlod" ? 1 : pub->lod == "far" ? 2 : 0);
    const std::string meshPath = scene::Graph::current().node(meshId)->path();
    QObject::connect(lod, &controls::Segmented::picked, w, [meshPath](int i) {
        runUi("buildingLod", {{"args", {meshPath}}, {"lod", i == 1 ? "hlod" : i == 2 ? "far" : "near"}});
    });
    f->addRow("LOD", lod);

    auto* info = new QLabel(qs(std::to_string(pub->specs.size()) + " buildings, " + std::to_string(pub->slots) + " slots, " +
                               std::to_string(int(pub->ms)) + " ms"),
                            w);
    info->setEnabled(false);
    f->addRow("Mesh", info);

    auto* row = new QWidget(w);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    h->addWidget(controls::push("buildings.paint", row, "Paint Bays"));
    if (res.enterable)
        h->addWidget(controls::push("buildings.rooms", row, "Edit Rooms"));
    auto* reset = new QToolButton(row);
    reset->setText("Reset");
    reset->setToolTip("Drop every edit of this building: style, floors, bays, colours");
    controls::setRole(reset, "push");
    QObject::connect(reset, &QToolButton::clicked, reset, [edit] { edit({{"clear", true}}); });
    h->addWidget(reset);
    auto* alone = new QToolButton(row);
    alone->setText("Standalone");
    alone->setToolTip("Move this building to a mesh of its own");
    controls::setRole(alone, "push");
    QObject::connect(alone, &QToolButton::clicked, alone, [id] { runUi("buildingStandalone", {{"args", {id}}}); });
    h->addWidget(alone);
    h->addStretch(1);
    f->addRow("", row);
    return w;
}

void warnOnThrow(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}
} // namespace

void registerUi(ext::Context& ctx)
{
    registerPaintTool(ctx);
    registerRoomTool(ctx);
    auto add = [&](Action a) {
        a.keywords.insert(a.keywords.end(), kKeywords.begin(), kKeywords.end());
        a.category = "Buildings";
        actions::add(ctx, std::move(a));
    };
    add({.id = "buildings.create",
         .label = "Create Buildings",
         .run =
             [] {
                 warnOnThrow([] {
                     // The selection decides the input: a settlement, footprint curves or a mesh.
                     json call = json::object();
                     scene::Graph& g = scene::Graph::current();
                     for (scene::NodeId id : g.selection())
                         if (scene::Node* n = g.node(id))
                         {
                             if (n->type().isA("curve"))
                                 call["curve"] = n->path();
                             else
                             {
                                 bool settlement = false;
                                 for (scene::NodeId m : scene::modifiers::stack(g, id))
                                     if (scene::Node* mod = g.node(m); mod && mod->type().attrIndex("ground") >= 0 && mod->type().attrIndex("edits") >= 0)
                                         settlement = true;
                                 call[settlement ? "settlement" : "mesh"] = n->path();
                             }
                             break;
                         }
                     actions::runWithOptions("buildingCreate", call);
                 });
             },
         .doc = "Make live procedural buildings from the selected settlement, footprint curves or mesh.",
         .python = "buildingCreate(settlement='Town')",
         .command = "buildingCreate",
         .icon = "building-community"});
    add({.id = "buildings.refresh",
         .label = "Refresh Buildings",
         .run =
             [] {
                 const json r = runUi("buildingRefresh", json::object());
                 if (r.is_null())
                     return;
                 if (r["refreshed"].empty())
                     log::info("buildingRefresh: every settlement's buildings are up to date");
                 for (const json& g : r["refreshed"])
                     log::info("buildingRefresh: {} rebuilt from {} ({} buildings)", g["group"].get<std::string>(), g["settlement"].get<std::string>(), g.value("buildings", 0));
             },
         .doc = "Rebuild the buildings of settlements that changed since they were made, keeping edits, overrides and pins.",
         .python = "buildingRefresh()",
         .icon = "refresh",
         .keywords = {"update", "regenerate", "rebuild", "sync"}});
    add({.id = "buildings.style",
         .label = "Building Style",
         .run = [] { warnOnThrow([] { actions::runWithOptions("buildingStyle", {{"building", selectedBuildings()}}); }); },
         .doc = "Give the selected buildings, a district or every building a style.",
         .python = "buildingStyle('Town_Buildings_0_0', style='seafront')",
         .command = "buildingStyle",
         .icon = "palette"});
    add({.id = "buildings.check",
         .label = "Check Buildings",
         .run =
             [] {
                 const json c = runUi("buildingCheck", json::object());
                 if (c.is_null())
                     return;
                 if (c.value("ok", false))
                     log::info("buildingCheck: {} buildings, no issues", c.value("buildings", 0));
                 else
                     for (const json& i : c["issues"])
                         log::warn("buildingCheck: {} {}: {}", i["kind"].get<std::string>(), i["building"].get<std::string>(), i["what"].get<std::string>());
             },
         .doc = "List modules meeting, doors off the ground, overlapping neighbours, floating slots and real names on signs.",
         .python = "buildingCheck()",
         .icon = "list-check"});
    add({.id = "buildings.standalone",
         .label = "Make Standalone",
         .run = [] { warnOnThrow([] { runUi("buildingStandalone", {{"building", currentBuilding().empty() ? selectedBuildings() : json(currentBuilding())}}); }); },
         .doc = "Move the building being edited (or the selection's) out of its cell into a mesh of its own.",
         .python = "buildingStandalone('building:1a2b')",
         .icon = "box-multiple"});
    add({.id = "buildings.age",
         .label = "Age Buildings",
         .run = [] { warnOnThrow([] { actions::runWithOptions("buildingAge", {{"building", selectedBuildings()}}); }); },
         .doc = "Weather buildings with a Weathering preset, per building or per district, and set their dirt.",
         .python = "buildingAge(polygon=[[0, 0], [8000, 0], [8000, 8000], [0, 8000]], dirt=0.7)",
         .command = "buildingAge",
         .icon = "sun-wind"});
    add({.id = "buildings.lod",
         .label = "Building LOD",
         .doc = "Show building meshes as near (shell and modules), HLOD proxies or far massing.",
         .python = "buildingLod(lod='far')",
         .command = "buildingLod",
         .call = json::object(),
         .icon = "stack-2"});
    add({.id = "buildings.proxies",
         .label = "Make Proxies",
         .doc = "Make each building mesh's HLOD and far proxies as meshes of their own, for export.",
         .python = "buildingProxies()",
         .command = "buildingProxies",
         .call = json::object(),
         .icon = "layers-subtract"});
    add({.id = "buildings.trimSheet",
         .label = "Building Trim Sheet",
         .doc = "Copy the LilaBuilding trim sheet into the workspace and bake it with Trim Sheets.",
         .python = "buildingTrimSheet()",
         .command = "buildingTrimSheet",
         .call = json::object(),
         .icon = "artboard"});
    add({.id = "buildings.saveStyle",
         .label = "Save Building Style",
         .doc = "Save a style of your own from a base style and changes, for you or the workspace.",
         .python = "buildingStyleSave(id='pinkCentro', base='colonial', spec={'palette': {'render': ['#e8a0b4']}})",
         .command = "buildingStyleSave",
         .icon = "device-floppy"});
    add({.id = "buildings.enterable",
         .label = "Make Enterable",
         .run = [] { warnOnThrow([] { actions::runWithOptions("buildingMakeEnterable", {{"building", currentBuilding().empty() ? selectedBuildings() : json(currentBuilding())}}); }); },
         .doc = "Give the building being edited (or the selection's) rooms inside: stairs, rooms by its use, doors, furnishing slots.",
         .python = "buildingMakeEnterable('building:1a2b')",
         .command = "buildingMakeEnterable",
         .icon = "door-enter",
         .keywords = {"interior", "rooms", "enter", "floor plan"}});
    add({.id = "buildings.exterior",
         .label = "Make Exterior",
         .run = [] { warnOnThrow([] { actions::runWithOptions("buildingMakeExterior", {{"building", currentBuilding().empty() ? selectedBuildings() : json(currentBuilding())}}); }); },
         .doc = "Take a building's interior away again; its windows go back to fake interiors.",
         .python = "buildingMakeExterior('building:1a2b')",
         .command = "buildingMakeExterior",
         .icon = "door-exit",
         .keywords = {"interior", "rooms"}});
    add({.id = "buildings.profile",
         .label = "Character Profile",
         .doc = "The character interiors are built and checked for: height, radius, step and slope.",
         .python = "buildingCharacterProfile(height=180, radius=35, maxStep=45, maxSlope=44)",
         .command = "buildingCharacterProfile",
         .icon = "walk",
         .keywords = {"navigation", "capsule", "stairs", "nav mesh"}});
    add({.id = "buildings.reloadStyles",
         .label = "Reload Building Styles",
         .run = [] { runUi("buildingStyles", {{"reload", true}}); },
         .doc = "Read the shipped, user and workspace style folders again and regenerate.",
         .python = "buildingStyles(reload=True)",
         .icon = "refresh"});

    menus::addItem(ctx, {.id = "buildings.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Buildings",
                         .before = levelToolkit::kDressSection});
    int sep = 0;
    for (const char* a : {"buildings.create", "buildings.refresh", "buildings.paint", "buildings.style", "buildings.check", "", "buildings.enterable", "buildings.rooms",
                          "buildings.exterior", "buildings.profile", "", "buildings.age", "buildings.standalone", "buildings.lod", "buildings.proxies", "",
                          "buildings.saveStyle", "buildings.reloadStyles", "buildings.trimSheet"})
        if (!*a)
            menus::addItem(ctx, {.id = "buildings.menu.sep" + std::to_string(++sep), .menu = kMenu, .kind = menus::Kind::Separator});
        else
            menus::addItem(ctx, {.id = std::string("buildings.menu.") + a, .menu = kMenu, .action = a});

    levelToolkit::add(ctx, {.id = "buildings",
                            .title = "Buildings",
                            .items = {{.action = "buildings.create", .icon = "building-community", .text = "Create", .shelf = "Buildings"},
                                      {.action = "buildings.paint", .icon = "brush", .text = "Paint Bays", .shelf = "Bays"},
                                      {.action = "buildings.style", .icon = "palette", .text = "Style"},
                                      {.action = "buildings.check", .icon = "list-check", .text = "Check"},
                                      {.action = "buildings.enterable", .icon = "door-enter", .text = "Enterable"},
                                      {.action = "buildings.rooms", .icon = "layout-board", .text = "Rooms", .shelf = "Rooms"},
                                      {.action = "buildings.age", .text = "Age"},
                                      {.action = "buildings.standalone", .text = "Standalone"},
                                      {.action = "buildings.exterior", .text = "Exterior", .rare = true},
                                      {.action = "buildings.profile", .text = "Character Profile", .rare = true},
                                      {.action = "buildings.refresh", .text = "Refresh", .rare = true},
                                      {.action = "buildings.lod", .text = "LOD", .rare = true},
                                      {.action = "buildings.proxies", .text = "Proxies", .rare = true},
                                      {.action = "buildings.saveStyle", .text = "Save Style", .rare = true},
                                      {.action = "buildings.reloadStyles", .text = "Reload Styles", .rare = true},
                                      {.action = "buildings.trimSheet", .text = "Trim Sheet", .rare = true}},
                            .order = 22,
                            .docs = "buildings"});

    attributeEditor::addSection(ctx, "mesh", "Buildings", buildingSection);

    // Option boxes: style names as labels, sections, lists that only scripts fill hidden.
    std::vector<std::pair<std::string, std::string>> styleLabels;
    for (const auto& st : allStyles())
        styleLabels.push_back({st->id, st->label});
    ops::options::setOverride("buildingCreate", {.order = {"settlement", "curve", "mesh", "style", "cell", "standalone", "floors", "height", "roof", "ground", "name", "culture"},
                                                 .hidden = {"envelopes", "lots"},
                                                 .labels = {{"cell", "Cell Size"}},
                                                 .sections = {{"floors", "Curves and Meshes"}, {"height", "Curves and Meshes"}, {"roof", "Curves and Meshes"},
                                                              {"ground", "Ground"}, {"name", "Output"}, {"culture", "Output"}},
                                                 .choices = {{"style", styleLabels}, {"roof", {{"flat", "Flat"}, {"hip", "Hip"}, {"gable", "Gable"}, {"dome", "Dome"}, {"sheet", "Sheet"}}}},
                                                 .ranges = {{"cell", {0.0, 51200.0, 800.0, " cm"}}, {"height", {100.0, 30000.0, 50.0, " cm"}}}});
    ops::options::setOverride("buildingStyle", {.order = {"style", "all", "clear"}, .hidden = {"building", "polygon"}, .choices = {{"style", styleLabels}}});
    ops::options::setOverride("buildingEdit", {.hidden = {"building", "bays", "toggles", "facadeBays", "clearBays", "palette"}, .choices = {{"style", styleLabels}}});
    ops::options::setOverride("buildingAge", {.order = {"preset", "dirt", "seed"}, .hidden = {"building", "polygon", "meshes"}});
    std::vector<std::pair<std::string, std::string>> programLabels;
    for (const std::string& p : interiorPrograms())
    {
        std::string l = p;
        l[0] = char(std::toupper(static_cast<unsigned char>(l[0])));
        programLabels.push_back({p, l});
    }
    ops::options::setOverride("buildingMakeEnterable", {.order = {"program", "furnish", "entry", "pin", "reset"}, .hidden = {"building"},
                                                        .labels = {{"entry", "Add Entry"}, {"reset", "Reset Hand Edits"}}, .choices = {{"program", programLabels}}});
    ops::options::setOverride("buildingMakeExterior", {.hidden = {"building"}, .labels = {{"clear", "Clear Layout"}}});
    ops::options::setOverride("buildingCharacterProfile", {.order = {"height", "radius", "maxStep", "maxSlope", "reset"},
                                                           .labels = {{"maxStep", "Max Step"}, {"maxSlope", "Max Slope"}},
                                                           .ranges = {{"height", {30.0, 400.0, 5.0, " cm"}}, {"radius", {5.0, 150.0, 1.0, " cm"}},
                                                                      {"maxStep", {1.0, 100.0, 1.0, " cm"}}, {"maxSlope", {5.0, 85.0, 1.0, "\u00b0"}}}});
    ops::options::setOverride("buildingLod", {.choices = {{"lod", {{"near", "Near"}, {"hlod", "HLOD"}, {"far", "Far"}}}}});
    ops::options::setOverride("buildingStyleSave", {.order = {"id", "base", "label", "description", "scope"}, .hidden = {"spec"}, .labels = {{"id", "Name"}},
                                                    .choices = {{"base", styleLabels}, {"scope", {{"user", "User"}, {"workspace", "Workspace"}}}}});
}
} // namespace buildings
