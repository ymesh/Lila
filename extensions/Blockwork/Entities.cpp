#include "Entities.h"

#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Prefs.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>

#include <QUuid>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace blockwork
{
using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace
{
constexpr const char* kFilesPref = "/blockwork/entityFiles";
constexpr const char* kClassAttr = "entityClass";
constexpr const char* kCategory = "Entity";

std::filesystem::path gStarter;
std::vector<EntityClass> gClasses;
bool gLoaded = false;

const std::vector<std::string> kPropTypes = {"number", "int", "bool", "string", "color", "enum", "node"};

EntityClass parseClass(const json& c, const std::string& file)
{
    EntityClass e;
    e.name = c.value("name", std::string());
    if (e.name.empty())
        throw std::runtime_error(file + ": a class has no name");
    e.kind = c.value("kind", std::string("point"));
    if (e.kind != "point" && e.kind != "volume")
        throw std::runtime_error(file + ": " + e.name + " kind is '" + e.kind + "'; use point or volume");
    e.unreal = c.value("unreal", std::string());
    e.doc = c.value("doc", std::string());
    e.file = file;
    if (c.contains("size") && c["size"].is_array() && c["size"].size() == 3)
        e.size = {c["size"][0].get<double>(), c["size"][1].get<double>(), c["size"][2].get<double>()};
    if (c.contains("color") && c["color"].is_array() && c["color"].size() == 3)
        e.color = {c["color"][0].get<float>(), c["color"][1].get<float>(), c["color"][2].get<float>()};
    for (const json& p : c.value("properties", json::array()))
    {
        EntityProperty prop;
        prop.name = p.value("name", std::string());
        prop.type = p.value("type", std::string("string"));
        if (prop.name.empty() || std::find(kPropTypes.begin(), kPropTypes.end(), prop.type) == kPropTypes.end())
            throw std::runtime_error(file + ": " + e.name + " has a property with no name or an unknown type '" + prop.type + "'");
        prop.defaultValue = p.value("default", json());
        prop.unreal = p.contains("unreal") ? p["unreal"].get<std::string>() : prop.name;
        prop.doc = p.value("doc", std::string());
        for (const json& ch : p.value("choices", json::array()))
            prop.choices.push_back(ch.get<std::string>());
        e.properties.push_back(std::move(prop));
    }
    return e;
}

std::vector<std::string> definitionFiles()
{
    std::vector<std::string> out;
    for (const json& f : PrefStore::get().value("general", kFilesPref, json::array()))
        if (f.is_string())
            out.push_back(f.get<std::string>());
    return out;
}

void load()
{
    gClasses.clear();
    std::vector<std::string> files = {gStarter.generic_string()};
    for (const std::string& f : definitionFiles())
        files.push_back(f);
    for (const std::string& f : files)
    {
        std::ifstream in{std::filesystem::path(f)};
        if (!in)
        {
            log::warn("Blockwork: entity definitions {} not found", f);
            continue;
        }
        try
        {
            const json doc = json::parse(in);
            for (const json& c : doc.value("classes", json::array()))
            {
                EntityClass e = parseClass(c, f);
                // A later file's class replaces an earlier one of the same name.
                auto it = std::find_if(gClasses.begin(), gClasses.end(), [&](const EntityClass& k) { return k.name == e.name; });
                if (it != gClasses.end())
                    *it = std::move(e);
                else
                    gClasses.push_back(std::move(e));
            }
        }
        catch (const std::exception& e)
        {
            log::warn("Blockwork: {} ({})", e.what(), f);
        }
    }
    gLoaded = true;
}

std::string str(const Node& n, const char* attr)
{
    const int i = n.type().attrIndex(attr);
    return i >= 0 && std::holds_alternative<std::string>(n.value(i)) ? n.as<std::string>(i) : std::string();
}

/** The entity's class name, or empty for a node that is not one. */
std::string classOf(const Node& n) { return str(n, kClassAttr); }

glm::dvec3 lilaForward()
{
    const math::Vector f = math::fromUnreal(math::Vector(1.0, 0.0, 0.0));
    return {f.x, f.y, f.z};
}

void addProperty(Graph& g, const std::string& path, const EntityProperty& p)
{
    json call = {{"args", {path}}, {"longName", p.name}, {"category", kCategory}, {"keyable", true}};
    if (!p.doc.empty())
        call["doc"] = p.doc;
    if (p.type == "number")
        call["attributeType"] = "double", call["defaultValue"] = p.defaultValue.is_number() ? p.defaultValue.get<double>() : 0.0;
    else if (p.type == "int")
        call["attributeType"] = "long", call["defaultValue"] = p.defaultValue.is_number() ? p.defaultValue.get<int64_t>() : 0;
    else if (p.type == "bool")
        call["attributeType"] = "bool", call["defaultValue"] = p.defaultValue.is_boolean() && p.defaultValue.get<bool>();
    else if (p.type == "color")
        call["attributeType"] = "double3", call["usedAsColor"] = true;
    else if (p.type == "enum")
    {
        std::string names;
        for (const std::string& c : p.choices)
            names += (names.empty() ? "" : ":") + c;
        call["attributeType"] = "enum", call["enumName"] = names.empty() ? std::string("none") : names;
    }
    else
        call["dataType"] = "string";
    if (g.find(path)->type().attrIndex(p.name) >= 0)
        return;
    ops::execute("addAttr", call);
    if (p.type == "string" || p.type == "node")
        if (p.defaultValue.is_string() && !p.defaultValue.get<std::string>().empty())
            ops::execute("setAttr", {{"args", {path + "." + p.name, p.defaultValue}}});
    if (p.type == "color" && p.defaultValue.is_array())
        ops::execute("setAttr", {{"args", {path + "." + p.name, p.defaultValue}}});
}

/** Makes a node an entity of the class: the class attr and its properties. */
void stamp(Graph& g, NodeId id, const EntityClass& c)
{
    const std::string path = g.fullPath(id);
    if (g.node(id)->type().attrIndex(kClassAttr) < 0)
        ops::execute("addAttr", {{"args", {path}}, {"longName", kClassAttr}, {"dataType", "string"}, {"category", kCategory},
                                 {"doc", "The entity class from the definition files: what it becomes in the engine."}});
    ops::execute("setAttr", {{"args", {path + "." + kClassAttr, c.name}}});
    for (const EntityProperty& p : c.properties)
        addProperty(g, path, p);
}

/** The material volumes of a class show in: its colour, made once. */
std::string volumeMaterial(Graph& g, const EntityClass& c)
{
    const std::string name = "Volume" + c.name;
    if (!materials::find(g, name))
    {
        Node* m = materials::create(g, name);
        ops::execute("setAttr", {{"args", {g.fullPath(m->id()) + ".baseColor", {c.color.x, c.color.y, c.color.z}}}});
    }
    return name;
}

void drawEntity(drawOverrides::DrawContext& c)
{
    const EntityClass* k = findEntityClass(classOf(c.node));
    const glm::dvec3 half = (k ? k->size : glm::dvec3(32.0)) * 0.5;
    const glm::vec4 col = c.highlight != drawOverrides::Highlight::None ? c.color : glm::vec4(k ? k->color : glm::vec3(0.8f), 1.0f);
    auto at = [&](double x, double y, double z) { return glm::dvec3(c.world * glm::dvec4(x, y, z, 1.0)); };
    // A box standing on the entity's origin, and an arrow along its forward.
    const glm::dvec3 lo(-half.x, 0.0, -half.z), hi(half.x, half.y * 2.0, half.z);
    const glm::dvec3 corners[8] = {at(lo.x, lo.y, lo.z), at(hi.x, lo.y, lo.z), at(hi.x, lo.y, hi.z), at(lo.x, lo.y, hi.z),
                                   at(lo.x, hi.y, lo.z), at(hi.x, hi.y, lo.z), at(hi.x, hi.y, hi.z), at(lo.x, hi.y, hi.z)};
    const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges)
    {
        c.frame.lines.push_back({corners[e[0]], col});
        c.frame.lines.push_back({corners[e[1]], col});
    }
    const glm::dvec3 f = lilaForward(), mid(0.0, half.y, 0.0);
    const double len = std::max(half.x, half.z) * 1.6;
    const glm::dvec3 tip = mid + f * len, side = glm::normalize(glm::cross(f, glm::dvec3(0.0, 1.0, 0.0))) * len * 0.25;
    for (auto [a, b] : {std::pair{mid, tip}, {tip, tip - f * len * 0.3 + side}, {tip, tip - f * len * 0.3 - side}})
    {
        c.frame.lines.push_back({at(a.x, a.y, a.z), col});
        c.frame.lines.push_back({at(b.x, b.y, b.z), col});
    }
    // Node references as lines to what they name.
    if (!k)
        return;
    Graph& g = graph();
    for (const EntityProperty& p : k->properties)
        if (p.type == "node")
            if (Node* to = g.find(str(c.node, p.name.c_str())))
            {
                const glm::dvec3 from = at(0.0, half.y, 0.0);
                const glm::dvec3 end = glm::dvec3(to->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
                const glm::vec4 dim(glm::vec3(col), 0.6f);
                c.frame.linesOnTop.push_back({from, dim});
                c.frame.linesOnTop.push_back({end, dim});
            }
}

std::optional<double> pickEntity(const drawOverrides::PickContext& c)
{
    const EntityClass* k = findEntityClass(classOf(c.node));
    const glm::dvec3 half = (k ? k->size : glm::dvec3(32.0)) * 0.5;
    QRectF r;
    bool any = false;
    for (int i = 0; i < 8; ++i)
    {
        const glm::dvec3 p(i & 1 ? half.x : -half.x, i & 2 ? half.y * 2.0 : 0.0, i & 4 ? half.z : -half.z);
        if (auto px = c.view.project(glm::dvec3(c.world * glm::dvec4(p, 1.0))))
        {
            r = any ? r.united(QRectF(*px, QSizeF(1, 1))) : QRectF(*px, QSizeF(1, 1));
            any = true;
        }
    }
    if (!any)
        return std::nullopt;
    if (!c.click)
        return QRectF(c.a, c.b).normalized().contains(r.center()) ? std::optional<double>(0.0) : std::nullopt;
    return r.adjusted(-4, -4, 4, 4).contains(c.a) ? std::optional<double>(0.0) : std::nullopt;
}

/** A Python literal for a json value. */
std::string py(const json& v)
{
    if (v.is_boolean())
        return v.get<bool>() ? "True" : "False";
    if (v.is_null())
        return "None";
    if (v.is_string())
        return json(v.get<std::string>()).dump();
    if (v.is_array() || v.is_object())
    {
        std::string s = v.dump();
        for (auto [from, to] : {std::pair<std::string, std::string>{"true", "True"}, {"false", "False"}, {"null", "None"}})
            for (size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size()))
                s.replace(i, from.size(), to);
        return s;
    }
    return v.dump();
}

json propertyValue(const Node& n, const EntityProperty& p)
{
    const int i = n.type().attrIndex(p.name);
    if (i < 0)
        return nullptr;
    const PlugValue& v = n.value(i);
    if (const double* d = std::get_if<double>(&v))
        return *d;
    if (const int64_t* d = std::get_if<int64_t>(&v))
        return p.type == "enum" && size_t(*d) < p.choices.size() ? json(p.choices[size_t(*d)]) : json(*d);
    if (const bool* d = std::get_if<bool>(&v))
        return *d;
    if (const std::string* d = std::get_if<std::string>(&v))
        return *d;
    if (const glm::dvec3* d = std::get_if<glm::dvec3>(&v))
        return {{"color", {d->x, d->y, d->z}}};
    return nullptr;
}

/** Python that spawns or updates each entity's actor, tagged with its link so a second send moves it instead of adding one. */
const char* kSendScript = R"PY(
import unreal, json
data = json.loads(DATA)
sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
tagged = {}
for a in sub.get_all_level_actors():
    for t in a.tags:
        s = str(t)
        if s.startswith("LilaEntity:"):
            tagged[s[11:]] = a
report = {"placed": [], "updated": [], "problems": []}
for e in data:
    cls = unreal.load_class(None, e["class"])
    if not cls:
        report["problems"].append(e["name"] + ": no class " + e["class"])
        continue
    loc = unreal.Vector(*e["location"])
    rot = unreal.Rotator(roll=0.0, pitch=e["pitch"], yaw=e["yaw"])
    a = tagged.get(e["id"])
    if a and a.get_class() != cls:
        sub.destroy_actor(a)
        a = None
    if a:
        report["updated"].append(e["name"])
    else:
        a = sub.spawn_actor_from_class(cls, loc, rot)
        if not a:
            report["problems"].append(e["name"] + ": Unreal spawned nothing for " + e["class"])
            continue
        a.tags = [unreal.Name("LilaEntity:" + e["id"])]
        report["placed"].append(e["name"])
    a.set_actor_location_and_rotation(loc, rot, False, False)
    if "scale" in e:
        a.set_actor_scale3d(unreal.Vector(*e["scale"]))
    a.set_actor_label(e["name"])
    for k, v in e["properties"].items():
        try:
            if isinstance(v, dict) and "color" in v:
                v = unreal.LinearColor(v["color"][0], v["color"][1], v["color"][2], 1.0)
            a.set_editor_property(k, v)
        except Exception as ex:
            report["problems"].append(e["name"] + "." + k + ": " + str(ex))
print(json.dumps(report))
)PY";
} // namespace

const std::vector<EntityClass>& entityClasses()
{
    if (!gLoaded)
        load();
    return gClasses;
}

const EntityClass* findEntityClass(const std::string& name)
{
    for (const EntityClass& c : entityClasses())
        if (c.name == name)
            return &c;
    return nullptr;
}

bool isEntity(const Node& n) { return !classOf(n).empty(); }

void registerEntities(ext::Context& ctx)
{
    using T = ArgType;
    gStarter = ctx.resources() / "entities" / "starter.lilaentities";

    const NodeType* locator = types::find("locator");
    if (locator)
    {
        NodeType t;
        t.name = kEntityType;
        t.base = locator;
        t.dag = true;
        t.icon = "map-pin";
        t.doc = "A Blockwork point entity: a player start, light, sound, target or any class from the entity definition files, drawn as "
                "its class's box and sent to Unreal as that class's actor.";
        t.ownAttrs = {{.name = kClassAttr, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
                       .doc = "The entity class from the definition files: what it becomes in the engine."}};
        t.create = locator->create;
        relocate::registerNeutral(t.name);
        types::registerType(ctx, std::move(t));
        drawOverrides::add(ctx, {.id = "blockwork.entity", .nodeType = kEntityType, .draw = drawEntity, .pick = pickEntity});
    }

    ctx.registerCommand({
        .name = "blockworkEntityClasses",
        .doc = "The entity classes from the starter definitions and the files added with blockworkEntityFile: name, kind (point or "
               "volume), Unreal class, size, colour and properties.",
        .flags = {{"reload", "r", T::Bool, false, "Read the files again."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("reload", false))
                load();
            json out = json::array();
            for (const EntityClass& c : entityClasses())
            {
                json props = json::array();
                for (const EntityProperty& p : c.properties)
                    props.push_back({{"name", p.name}, {"type", p.type}, {"default", p.defaultValue}, {"unreal", p.unreal}});
                out.push_back({{"name", c.name}, {"kind", c.kind}, {"unreal", c.unreal}, {"size", {c.size.x, c.size.y, c.size.z}},
                               {"color", {c.color.x, c.color.y, c.color.z}}, {"properties", props}, {"file", c.file}, {"doc", c.doc}});
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "blockworkEntityFile",
        .doc = "Add an entity definition file (.lilaentities JSON: {classes: [{name, kind, unreal, size, color, properties: [{name, type, "
               "default, unreal, choices}]}]}) or remove one. Classes in later files replace same-named earlier ones. Returns the files.",
        .positionalDoc = "file",
        .flags = {{"remove", "", T::Bool, false, "Remove the file instead."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> files = definitionFiles();
            if (!a.positional.empty())
            {
                const std::string f = a.positional[0].get<std::string>();
                files.erase(std::remove(files.begin(), files.end(), f), files.end());
                if (!a.get<bool>("remove", false))
                {
                    if (!std::filesystem::exists(std::filesystem::path(f)))
                        throw CommandError("blockworkEntityFile: no file " + f);
                    files.push_back(f);
                }
                PrefStore::get().set("general", kFilesPref, files);
                load();
            }
            return files;
        },
    });

    ctx.registerCommand({
        .name = "blockworkEntity",
        .doc = "Place an entity. A point class becomes an entity node at position; a volume class a block filling origin and size. "
               "Its properties become attributes. Returns the node.",
        .flags = {{"class", "c", T::String, nullptr, "The entity class (blockworkEntityClasses lists them)."},
                  {"position", "p", T::Vec3, json::array({0, 0, 0}), "Point classes: where it stands, world cm."},
                  {"turn", "t", T::Float, 0.0, "Point classes: turn around up, degrees."},
                  {"origin", "o", T::Vec3, json::array({0, 0, 0}), "Volume classes: the box's low corner, world cm."},
                  {"size", "sz", T::Vec3, json::array({256, 256, 256}), "Volume classes: the box's size, cm."},
                  {"name", "n", T::String, nullptr, "Node name; default the class's."},
                  {"parent", "", T::String, kDefaultGroup, "Group it goes under, made when missing; empty for the world."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const EntityClass* c = findEntityClass(a.get<std::string>("class", ""));
            if (!c)
                throw CommandError("blockworkEntity: no class '" + a.get<std::string>("class", "") + "'; blockworkEntityClasses lists them");
            const std::string parentName = a.get<std::string>("parent", kDefaultGroup);
            const NodeId parent = parentName.empty() ? kNoNode : levelGroup(g, parentName);
            const std::string name = a.get<std::string>("name", c->name + "1");
            NodeId id;
            if (c->kind == "volume")
            {
                const json o = a.flags["origin"], s = a.flags["size"];
                Frame f;
                f.origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
                f.size = {s[0].get<double>(), s[2].get<double>(), s[1].get<double>()};
                geo::Mesh m = buildShape(Shape::Box, f, {});
                if (m.empty())
                    throw CommandError("blockworkEntity: give the volume a size");
                id = createBlock(g, std::move(m), name, parent, volumeMaterial(g, *c));
            }
            else
            {
                Node* n = g.createNode(*types::find(kEntityType), name, parent);
                id = n->id();
                const json p = a.flags["position"];
                g.setValue({id, Transform::kTranslate, -1}, glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
                g.setValue({id, Transform::kRotate, -1}, glm::dvec3(0.0, a.get<double>("turn", 0.0), 0.0));
            }
            stamp(g, id, *c);
            g.select({id});
            return g.fullPath(id);
        },
        .examples = {"blockworkEntity(class='PlayerStart', position=[0, 0, 0], turn=90)",
                     "blockworkEntity(class='Trigger', origin=[0, 0, 0], size=[256, 192, 256])"},
    });

    ctx.registerCommand({
        .name = "blockworkVolume",
        .doc = "Turn blocks into volume entities of a class (a trigger, a blocking volume), keeping their shape; clear=True turns "
               "them back into plain blocks. Returns the blocks.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"class", "c", T::String, nullptr, "A volume class."}, {"clear", "", T::Bool, false, "Make them plain blocks again."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            const bool clear = a.get<bool>("clear", false);
            const EntityClass* c = clear ? nullptr : findEntityClass(a.get<std::string>("class", ""));
            if (!clear && (!c || c->kind != "volume"))
                throw CommandError("blockworkVolume: give a volume class");
            for (NodeId id : blockArgs(a, "blockworkVolume"))
            {
                const std::string path = g.fullPath(id);
                if (clear)
                {
                    const EntityClass* was = findEntityClass(classOf(*g.node(id)));
                    if (was)
                        for (const EntityProperty& p : was->properties)
                            if (g.node(id)->type().attrIndex(p.name) >= 0)
                                ops::execute("deleteAttr", {{"args", {path + "." + p.name}}});
                    if (g.node(id)->type().attrIndex(kClassAttr) >= 0)
                        ops::execute("deleteAttr", {{"args", {path + "." + kClassAttr}}});
                    geo::Mesh m = baseMesh(g, id);
                    materials::assign(m, materials::kDefaultSlot, {});
                    setBaseMesh(g, id, std::move(m));
                }
                else
                {
                    stamp(g, id, *c);
                    geo::Mesh m = baseMesh(g, id);
                    materials::assign(m, volumeMaterial(g, *c), {});
                    setBaseMesh(g, id, std::move(m));
                }
                out.push_back(path);
            }
            return out;
        },
        .examples = {"blockworkVolume('block3', class='Trigger')"},
    });

    ctx.registerCommand({
        .name = "blockworkSendEntities",
        .doc = "Send entities to the open Unreal level: each becomes its class's actor (volumes sized to their block's bounds), its "
               "properties set on it. Sending again moves and updates the same actors. Returns what Unreal placed, updated and any "
               "problems; dryRun returns what would be sent.",
        .positionalDoc = "entities (default the selection, or every entity)",
        .flags = {{"dryRun", "", T::Bool, false, "Return the data instead of sending it."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            std::vector<NodeId> ids;
            for (const std::string& n : a.objects())
                if (Node* k = g.find(n); k && isEntity(*k))
                    ids.push_back(k->id());
            if (ids.empty())
                for (NodeId id : g.nodes())
                    if (Node* k = g.node(id); k && isEntity(*k))
                        ids.push_back(id);
            if (ids.empty())
                throw CommandError("blockworkSendEntities: no entities; place some with the Entity tool or blockworkEntity");
            json data = json::array();
            std::vector<std::string> skipped;
            for (NodeId id : ids)
            {
                Node* n = g.node(id);
                const EntityClass* c = findEntityClass(classOf(*n));
                if (!c || c->unreal.empty())
                {
                    skipped.push_back(n->name() + ": class '" + classOf(*n) + "' has no Unreal class");
                    continue;
                }
                const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
                std::string link = n->as<std::string>(Transform::kLinkId);
                if (link.empty())
                {
                    // The id the actor is found by on the next send.
                    link = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                    g.setValue({id, Transform::kLinkId, -1}, link);
                }
                json e = {{"id", link}, {"name", n->name()}, {"class", c->unreal}};
                glm::dvec3 pos(w[3]);
                if (c->kind == "volume")
                {
                    const geo::Bounds b = worldMesh(g, id).bounds();
                    pos = b.center();
                    const math::Vector s = math::scaleToUnreal(math::Vector(b.size()));
                    // Unreal makes a volume with a 200 cm cube brush; scale sizes it.
                    e["scale"] = {s.x / 200.0, s.y / 200.0, s.z / 200.0};
                    e["yaw"] = 0.0, e["pitch"] = 0.0;
                }
                else
                {
                    const glm::dvec3 f = glm::normalize(glm::dmat3(w) * lilaForward());
                    const math::Vector uf = math::toUnreal(math::Vector(f));
                    e["yaw"] = glm::degrees(std::atan2(uf.y, uf.x));
                    e["pitch"] = glm::degrees(std::asin(std::clamp(uf.z, -1.0, 1.0)));
                }
                const math::Vector up = math::toUnreal(math::Vector(pos));
                e["location"] = {up.x, up.y, up.z};
                json props = json::object();
                for (const EntityProperty& p : c->properties)
                    if (!p.unreal.empty() && p.type != "node")
                        if (json v = propertyValue(*n, p); !v.is_null())
                            props[p.unreal] = v;
                e["properties"] = props;
                data.push_back(e);
            }
            if (a.get<bool>("dryRun", false))
                return {{"entities", data}, {"skipped", skipped}};
            const std::string code = "DATA = " + py(data.dump()) + "\n" + kSendScript;
            json r = ops::execute("unrealCall", {{"args", {"runPython"}}, {"params", {{"code", code}, {"mode", "exec"}}}});
            if (!r.value("ok", false))
                throw CommandError("blockworkSendEntities: " + r.value("error", std::string("Unreal is not connected")));
            json report = {{"skipped", skipped}};
            const json res = r.value("result", json::object());
            const std::string log = res.is_object() ? res.value("log", std::string()) : std::string();
            const size_t at = log.rfind('{');
            if (at != std::string::npos)
                try
                {
                    report.update(json::parse(log.substr(at)));
                }
                catch (const std::exception&)
                {
                    report["log"] = log;
                }
            return report;
        },
        .examples = {"blockworkSendEntities()", "blockworkSendEntities(dryRun=True)"},
    });
}
} // namespace blockwork
