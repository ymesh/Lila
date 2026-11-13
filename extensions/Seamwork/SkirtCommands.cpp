// S4: the seamSkirt node (a live filleted blend mesh between each object and the ground) and the seamSkirt command.

#include "Seamwork.h"
#include "Skirt.h"

#include <lila/Relocate.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
using T = ArgType;

namespace
{
int sk(const Node& n, std::string_view name) { return n.type().attrIndex(name); }

SkirtOptions skirtOptions(const Node& n, const SeamData& d)
{
    SkirtOptions o;
    o.width = n.as<double>(sk(n, "width"));
    o.radius = n.as<double>(sk(n, "radius"));
    o.segments = int(n.as<int64_t>(sk(n, "segments")));
    o.spacing = n.as<double>(sk(n, "spacing"));
    o.method = SkirtMethod(std::clamp<int64_t>(n.as<int64_t>(sk(n, "method")), 0, 2));
    o.straightAngle = n.as<double>(sk(n, "straightAngle"));
    o.kinds = int(n.as<int64_t>(sk(n, "kinds")));
    o.up = d.up;
    const std::string object = n.as<std::string>(sk(n, "object"));
    if (!object.empty())
    {
        o.object = -2;
        for (size_t i = 0; i < d.objects.size(); ++i)
            if (d.objects[i].name == object)
                o.object = int(i);
    }
    const json colors = json::parse(n.as<std::string>(sk(n, "layerColors")), nullptr, false);
    if (colors.is_object())
        for (const auto& [name, c] : colors.items())
            if (c.is_array() && c.size() == 3)
                o.layerColors[name] = {c[0].get<double>(), c[1].get<double>(), c[2].get<double>()};
    return o;
}

class SkirtNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        json stats = json::object();
        std::string warning;
        MeshPtr out = std::make_shared<geo::Mesh>();
        try
        {
            const SeamData* d = seamDataOf(value(sk(*this, "seams")));
            if (!d || !d->result)
                warning = "no seam data: connect a seam node's outSeams";
            else
            {
                const SkirtOptions o = skirtOptions(*this, *d);
                if (o.object == -2)
                    warning = "the seam node has no object named " + as<std::string>(sk(*this, "object"));
                else
                {
                    SkirtResult r = buildSkirt(*d->result, d->objects, d->ground.get(), o);
                    json curves = json::array();
                    for (const SkirtCurve& c : r.curves)
                        curves.push_back({{"curve", c.curve}, {"method", c.method}, {"reason", c.reason}});
                    stats = {{"curves", curves}, {"faces", r.mesh.numFaces()}};
                    for (const std::string& w : r.warnings)
                        warning += (warning.empty() ? "" : "; ") + w;
                    out = std::make_shared<geo::Mesh>(std::move(r.mesh));
                }
            }
        }
        catch (const std::exception& e)
        {
            warning = e.what();
        }
        // An unconnected node (still being wired up) only says so on its warning attr.
        if (!warning.empty() && warning != mLogged && std::get<DataPtr>(value(sk(*this, "seams"))))
            log::warn("{}: {}", name(), warning);
        mLogged = warning;
        setOutput(sk(*this, "outMesh"), out);
        setOutput(sk(*this, "stats"), stats.dump());
        setOutput(sk(*this, "warning"), warning);
    }

private:
    std::string mLogged;
};

AttrDef kp(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

/** "#rrggbb" (sRGB) to linear. */
glm::dvec3 linearFromHex(const std::string& hex)
{
    if (hex.size() != 7 || hex[0] != '#')
        return glm::dvec3(0.5);
    glm::dvec3 c;
    for (int k = 0; k < 3; ++k)
    {
        const double s = std::stoi(hex.substr(size_t(1 + 2 * k), 2), nullptr, 16) / 255.0;
        c[k] = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    }
    return c;
}

std::string identifierOf(const std::string& path)
{
    std::string s = path.substr(path.find_last_of('|') == std::string::npos ? 0 : path.find_last_of('|') + 1);
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    return s.empty() ? std::string("skirt") : s;
}

bool sourceNanite(const std::string& node)
{
    const json m = link::mappingOf(node);
    return m.is_object() && m.value("nanite", false);
}
} // namespace

bool registerSkirtNodes(ext::Context& ctx)
{
    NodeType t;
    t.name = kSkirtType;
    t.icon = "wave-square";
    t.doc = "A filleted band bridging an object and the ground along their contact, its borders lying on both surfaces with their "
            "normals, so it disappears at its edges while its material blends wall into ground (MF_LilaSkirtBlend). Follows the "
            "contacts live.";
    AttrDef method = kp("method", PlugType::Enum, int64_t(0), "auto sweeps straight runs and unions the rest; union or sweep forces one.");
    method.enumNames = skirtMethodNames();
    t.ownAttrs = {
        {.name = "seams", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden, .doc = "A seam node's outSeams."},
        {.name = "object", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Only this object's contacts; every object when empty."},
        kp("width", PlugType::Float, 40.0, "How far the skirt reaches up the wall and over the ground, cm.", 0.5),
        kp("radius", PlugType::Float, 0.0, "Fillet radius, cm; 0 follows width.", 0.0),
        kp("segments", PlugType::Int, int64_t(6), "Rows across the fillet.", 2.0, 32.0),
        kp("spacing", PlugType::Float, 15.0, "Rows along swept contacts every this many cm.", 0.5),
        method,
        kp("straightAngle", PlugType::Float, 25.0, "Auto sweeps contacts turning no more than this, degrees.", 0.0, 180.0),
        kp("kinds", PlugType::Int, int64_t(1), "Contact kinds taken: 1 ground, 2 mesh, added.", 0.0, 3.0),
        {.name = "layerColors", .type = PlugType::String, .defaultValue = std::string("{}"), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Terrain layer colours baked into the ground side, {layer: [r, g, b]} linear."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "stats", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Method per contact and why, as JSON."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<SkirtNode>(); };
    if (!types::registerType(ctx, std::move(t)))
        return false;
    lila::scene::relocate::registerWorldAttrs(kSkirtType, {.worldOutput = true});
    return true;
}

void registerSkirtCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "seamSkirt",
        .doc = "Contact skirts: a blend mesh bridging each object and the ground, the alternative to screen-space mesh blending that "
               "works in mobile forward too.\n\n"
               "Each skirt is a live seamSkirt node and its mesh, one per object. method='auto' sweeps a fillet profile along "
               "contacts that run straight (turning less than straightAngle) and builds the rest by a boolean union with a fillet cut "
               "back to the band; the method used per contact and why is in the result. Borders lie on the source surfaces with "
               "their normals; point attr side runs 0 (wall) to 1 (ground), also in vertex alpha, with the terrain colour baked into "
               "vertex RGB. Each skirt mesh is mapped for Unreal (non-Nanite LODs, complex-as-simple collision, Nanite when its "
               "object's mapping has it) with material slot LilaSkirtBlend (M_LilaSkirtBlend / MF_LilaSkirtBlend).",
        .positionalDoc = "seam node",
        .flags = {{"width", "w", T::Float, 40.0, "How far the skirt reaches up the wall and over the ground, cm.", {}, {}, 0.5},
                  {"radius", "r", T::Float, 0.0, "Fillet radius, cm; 0 follows width.", {}, {}, 0.0},
                  {"method", "m", T::String, "auto", "auto, union or sweep.", skirtMethodNames()},
                  {"segments", "sg", T::Int, int64_t(6), "Rows across the fillet.", {}, {}, 2.0, 32.0},
                  {"spacing", "sp", T::Float, 15.0, "Rows along swept contacts every this many cm.", {}, {}, 0.5},
                  {"straightAngle", "sa", T::Float, 25.0, "Auto sweeps contacts turning no more than this, degrees.", {}, {}, 0.0, 180.0},
                  {"perObject", "po", T::Bool, true, "One skirt per object (for export); off makes one for all."},
                  {"map", "mp", T::Bool, true, "Map each skirt for Unreal under /Game/LilaLink/Contacts."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::string cmd = "seamSkirt";
            const auto names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one seam node");
            NodeId sid = kNoNode;
            const auto d = seamDataNamed(g, names[0], cmd, &sid);
            const std::string methodName = a.get<std::string>("method", "auto");
            const auto mi = std::find(skirtMethodNames().begin(), skirtMethodNames().end(), methodName);

            // Terrain layer colours for the ground side.
            json colors = json::object();
            const std::string terrain = g.node(sid)->as<std::string>(g.node(sid)->type().attrIndex("terrainName"));
            if (!terrain.empty() && ops::find("terrainMaterialRules") && !d->result->layers.empty())
                try
                {
                    const json q = ops::execute("terrainMaterialRules", {{"args", {terrain}}, {"query", true}});
                    for (const json& l : q.value("displayLayers", json::array()))
                    {
                        const glm::dvec3 c = linearFromHex(l.value("color", std::string("#808080")));
                        colors[l.value("name", std::string())] = {c.x, c.y, c.z};
                    }
                }
                catch (const std::exception& e)
                {
                    log::warn("seamSkirt: terrain colours unread, the ground side bakes grey: {}", e.what());
                }

            std::vector<std::string> targets;
            if (a.get<bool>("perObject", true))
            {
                for (const Curve& c : d->result->curves)
                    if (c.kind == Kind::Ground && c.a >= 0)
                    {
                        const std::string& n = d->objects[size_t(c.a)].name;
                        if (std::find(targets.begin(), targets.end(), n) == targets.end())
                            targets.push_back(n);
                    }
            }
            else
                targets.push_back("");
            if (targets.empty())
                throw CommandError(cmd + ": " + g.node(sid)->path() + " has no ground contacts to skirt");

            const std::string slot = "LilaSkirtBlend";
            if (!materials::find(g, slot))
                materials::create(g, slot);
            json made = json::array();
            for (const std::string& target : targets)
            {
                const std::string stem = target.empty() ? g.node(sid)->name() : identifierOf(target);
                Node* sn = g.createNode(*types::find(kSkirtType), stem + "Skirt");
                const NodeId kid = sn->id();
                g.connect({sid, g.node(sid)->type().attrIndex("outSeams"), -1}, {kid, sk(*sn, "seams"), -1}, true);
                g.setValue({kid, sk(*sn, "object"), -1}, target);
                g.setValue({kid, sk(*sn, "width"), -1}, a.get<double>("width", 40.0));
                g.setValue({kid, sk(*sn, "radius"), -1}, a.get<double>("radius", 0.0));
                g.setValue({kid, sk(*sn, "segments"), -1}, a.get<int64_t>("segments", 6));
                g.setValue({kid, sk(*sn, "spacing"), -1}, a.get<double>("spacing", 15.0));
                g.setValue({kid, sk(*sn, "method"), -1}, int64_t(mi == skirtMethodNames().end() ? 0 : mi - skirtMethodNames().begin()));
                g.setValue({kid, sk(*sn, "straightAngle"), -1}, a.get<double>("straightAngle", 25.0));
                g.setValue({kid, sk(*sn, "layerColors"), -1}, colors.dump());
                Node* mesh = g.createNode(*types::find("mesh"), stem + "_skirt");
                g.connect({kid, sk(*sn, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1}, true);
                const int tag = g.addAttr(mesh->id(), {.name = "seamworkOutput", .type = PlugType::Bool, .defaultValue = true,
                                                       .flags = AttrInput | AttrStorable | AttrUser | AttrHidden, .doc = "Seamwork."});
                g.setValue({mesh->id(), tag, -1}, true);
                g.evaluate();
                json mapping = nullptr;
                if (a.get<bool>("map", true))
                {
                    mapping = {{"node", mesh->path()},
                               {"contentPath", "/Game/LilaLink/Contacts/SM_" + identifierOf(mesh->name())},
                               {"kind", "static"},
                               {"lodGroup", "SmallProp"},
                               {"collision", "complexAsSimple"},
                               {"nanite", !target.empty() && sourceNanite(target)}};
                    link::setMapping(mapping);
                }
                made.push_back({{"node", g.node(kid)->path()},
                                {"mesh", mesh->path()},
                                {"object", target},
                                {"stats", json::parse(sn->as<std::string>(sk(*sn, "stats")), nullptr, false)},
                                {"warning", sn->as<std::string>(sk(*sn, "warning"))},
                                {"mapping", mapping}});
            }
            return {{"skirts", made}, {"material", slot}};
        },
        .examples = {"r = seamSkirt('contacts', width=40)\nfor s in r['skirts']: print(s['object'], s['stats']['curves'])",
                     "seamSkirt('contacts', method='sweep', radius=15)"},
    });
}
} // namespace seamwork
