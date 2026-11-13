// S2: the seamStrip node (live contact strips as a mesh) and seamDecal, which makes it or sends decal actors to Unreal.

#include "Seamwork.h"
#include "Strip.h"

#include <lila/Relocate.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <algorithm>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
using T = ArgType;

namespace
{
int ai(const Node& n, std::string_view name) { return n.type().attrIndex(name); }

/** A node turning a seam node's data into a mesh; build may add facts to stats. */
class SeamMeshNode final : public Node
{
public:
    std::function<geo::Mesh(const SeamData&, const Node&, json& stats)> build;

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        json stats = json::object();
        std::string warning;
        MeshPtr out;
        try
        {
            const SeamData* d = seamDataOf(value(ai(*this, "seams")));
            if (!d || !d->result)
                warning = "no seam data: connect a seam node's outSeams";
            else
                out = std::make_shared<geo::Mesh>(build(*d, *this, stats));
        }
        catch (const std::exception& e)
        {
            warning = e.what();
        }
        // An unconnected node (still being wired up) only says so on its warning attr.
        if (!warning.empty() && warning != mLogged && std::get<DataPtr>(value(ai(*this, "seams"))))
            log::warn("{}: {}", name(), warning);
        mLogged = warning;
        setOutput(ai(*this, "outMesh"), out ? out : std::make_shared<geo::Mesh>());
        setOutput(ai(*this, "stats"), stats.dump());
        setOutput(ai(*this, "warning"), warning);
    }

private:
    std::string mLogged;
};

AttrDef sp(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

StripOptions stripOptions(const Node& n)
{
    StripOptions o;
    o.side = Side(std::clamp<int64_t>(n.as<int64_t>(ai(n, "side")), 0, 2));
    o.width = n.as<double>(ai(n, "width"));
    o.divisions = int(n.as<int64_t>(ai(n, "divisions")));
    o.offset = n.as<double>(ai(n, "offset"));
    o.spacing = n.as<double>(ai(n, "spacing"));
    o.tile = n.as<double>(ai(n, "tile"));
    o.kinds = int(n.as<int64_t>(ai(n, "kinds")));
    o.material = n.as<std::string>(ai(n, "material"));
    o.band.falloff = Falloff(std::clamp<int64_t>(n.as<int64_t>(ai(n, "falloff")), 0, int64_t(falloffNames().size()) - 1));
    o.band.curve = n.as<std::string>(ai(n, "falloffCurve"));
    o.band.noise = n.as<double>(ai(n, "noise"));
    o.band.noiseScale = n.as<double>(ai(n, "noiseScale"));
    return o;
}

int kindsMask(const json& kinds)
{
    if (!kinds.is_array())
        return 3;
    int m = 0;
    for (const json& k : kinds)
    {
        Kind kind = Kind::Ground;
        if (k.is_string() && parseKind(k.get<std::string>(), &kind))
            m |= 1 << int(kind);
    }
    return m == 0 ? 3 : m;
}

json unrealDir(const glm::dvec3& v)
{
    const math::Vector u = math::toUnreal(math::Vector(v));
    return json::array({u.x, u.y, u.z});
}

bool truthy(const json& v)
{
    if (v.is_number())
        return v.get<double>() != 0.0;
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_string())
    {
        const std::string s = v.get<std::string>();
        return !s.empty() && s != "0" && s != "False" && s != "false";
    }
    return false;
}
} // namespace

bool registerStripNodes(ext::Context& ctx)
{
    NodeType t;
    t.name = kStripType;
    t.icon = "road";
    t.doc = "Ribbons along a seam node's contacts, lying on the ground, up the wall or both as a V, for grime and moss strips in "
            "any renderer: UVs run along the length, vertex alpha fades across. Follows the contacts live.";
    AttrDef side = sp("side", PlugType::Enum, int64_t(0), "Ground, wall, or both as a V.");
    side.enumNames = sideNames();
    AttrDef fall = sp("falloff", PlugType::Enum, int64_t(1), "How the alpha fades across.");
    fall.enumNames = falloffNames();
    t.ownAttrs = {
        {.name = "seams", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden, .doc = "A seam node's outSeams."},
        side,
        sp("width", PlugType::Float, 40.0, "Width of each side, cm.", 0.1),
        sp("divisions", PlugType::Int, int64_t(3), "Rows across each side.", 1.0, 32.0),
        sp("offset", PlugType::Float, 0.2, "Lift off the surface, cm.", 0.0),
        sp("spacing", PlugType::Float, 20.0, "Rows along the contact every this many cm.", 0.5),
        sp("tile", PlugType::Float, 100.0, "cm of strip per texture repeat.", 0.1),
        sp("kinds", PlugType::Int, int64_t(3), "Contact kinds taken: 1 ground, 2 mesh, 4 crease, added.", 0.0, 7.0),
        {.name = "material", .type = PlugType::String, .defaultValue = std::string("LilaSeamStrip"), .flags = AttrInput | AttrStorable,
         .doc = "Material slot of the strip faces."},
        fall,
        {.name = "falloffCurve", .type = PlugType::String, .defaultValue = std::string("0:1 0.5:0.6 1:0"), .flags = AttrInput | AttrStorable,
         .doc = "Custom falloff \"distance:weight ...\"."},
        sp("noise", PlugType::Float, 0.3, "Edge width varies by up to this fraction with world noise.", 0.0, 4.0),
        sp("noiseScale", PlugType::Float, 80.0, "Size of the edge noise, cm.", 1.0),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "stats", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Strips made, as JSON."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] {
        auto n = std::make_unique<SeamMeshNode>();
        n->build = [](const SeamData& d, const Node& node, json& stats) {
            size_t strips = 0;
            geo::Mesh m = buildStrips(*d.result, stripOptions(node), [&d](const glm::dvec3& from, const glm::dvec3& dir, glm::dvec3& hit, glm::dvec3& nrm) {
                return raySurface(d, from, dir, hit, nrm, 30.0);
            }, &strips);
            stats = {{"strips", strips}, {"faces", m.numFaces()}};
            return m;
        };
        return n;
    };
    if (!types::registerType(ctx, std::move(t)))
        return false;
    lila::scene::relocate::registerWorldAttrs(kStripType, {.worldOutput = true});
    return true;
}

void registerStripCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "seamDecal",
        .doc = "Grime, moss or wet strips along a seam node's contacts.\n\n"
               "decals='mesh' (default) makes a live strip mesh (seamStrip node) that works in every renderer, mobile forward "
               "included: masked by default (blend='translucent' for soft edges), material slot LilaSeamStrip, which "
               "M_LilaSeamStrip / MF_LilaSeamStrip render in Unreal. decals='dbuffer' sends decal actors to Unreal instead, a "
               "desktop opt-in (desktop forward and deferred, not mobile). decals='actor' sends decal actors for mobile forward and "
               "needs ForwardLevelUp's clustered decals switched on, which LilaLink's rendererInfo must report.",
        .positionalDoc = "seam node",
        .flags = {{"side", "si", T::String, "ground", "Lay the strip on the ground, up the wall, or both as a V.", sideNames()},
                  {"width", "w", T::Float, 40.0, "Width of each side, cm.", {}, {}, 0.1},
                  {"divisions", "dv", T::Int, int64_t(3), "Rows across each side.", {}, {}, 1.0, 32.0},
                  {"offset", "of", T::Float, 0.2, "Lift off the surface, cm.", {}, {}, 0.0},
                  {"spacing", "sp", T::Float, 20.0, "Rows along the contact every this many cm.", {}, {}, 0.5},
                  {"tile", "ti", T::Float, 100.0, "cm of strip per texture repeat.", {}, {}, 0.1},
                  {"kinds", "k", T::List, json::array({"ground", "mesh"}), "Contact kinds taken: ground, mesh, crease."},
                  {"noise", "no", T::Float, 0.3, "Edge width varies by up to this fraction with world noise.", {}, {}, 0.0},
                  {"blend", "b", T::String, "masked", "How strip meshes render: masked or translucent.", {"masked", "translucent"}},
                  {"decals", "dc", T::String, "mesh", "mesh (any renderer), dbuffer (desktop decal actors) or actor (ForwardLevelUp clustered decals).",
                   {"mesh", "dbuffer", "actor"}},
                  {"segment", "sg", T::Float, 100.0, "Length of each decal actor along the contact, cm.", {}, {}, 1.0},
                  {"name", "n", T::String, nullptr, "Name of the strip mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::string cmd = "seamDecal";
            const auto names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one seam node");
            NodeId sid = kNoNode;
            const auto d = seamDataNamed(g, names[0], cmd, &sid);
            const std::string mode = a.get<std::string>("decals", "mesh");
            const std::string sideName = a.get<std::string>("side", "ground");
            const auto sideIt = std::find(sideNames().begin(), sideNames().end(), sideName);
            const int64_t side = sideIt == sideNames().end() ? 0 : sideIt - sideNames().begin();
            const int kinds = kindsMask(a.flags.value("kinds", json()));
            const std::string seamName = g.node(sid)->name();

            if (mode == "mesh")
            {
                const bool translucent = a.get<std::string>("blend", "masked") == "translucent";
                const std::string slot = translucent ? "LilaSeamStripTranslucent" : "LilaSeamStrip";
                Node* strip = g.createNode(*types::find(kStripType), seamName + "Strip");
                const NodeId tid = strip->id();
                g.connect({sid, g.node(sid)->type().attrIndex("outSeams"), -1}, {tid, ai(*strip, "seams"), -1}, true);
                g.setValue({tid, ai(*strip, "side"), -1}, side);
                g.setValue({tid, ai(*strip, "width"), -1}, a.get<double>("width", 40.0));
                g.setValue({tid, ai(*strip, "divisions"), -1}, a.get<int64_t>("divisions", 3));
                g.setValue({tid, ai(*strip, "offset"), -1}, a.get<double>("offset", 0.2));
                g.setValue({tid, ai(*strip, "spacing"), -1}, a.get<double>("spacing", 20.0));
                g.setValue({tid, ai(*strip, "tile"), -1}, a.get<double>("tile", 100.0));
                g.setValue({tid, ai(*strip, "kinds"), -1}, int64_t(kinds));
                g.setValue({tid, ai(*strip, "noise"), -1}, a.get<double>("noise", 0.3));
                g.setValue({tid, ai(*strip, "material"), -1}, slot);
                Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", seamName + "Strips"));
                g.connect({tid, ai(*strip, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1}, true);
                const int out = g.addAttr(mesh->id(), {.name = "seamworkOutput", .type = PlugType::Bool, .defaultValue = true,
                                                       .flags = AttrInput | AttrStorable | AttrUser | AttrHidden, .doc = "Seamwork."});
                g.setValue({mesh->id(), out, -1}, true);
                if (!materials::find(g, slot))
                    if (Node* m = materials::create(g, slot))
                        if (const int bm = m->type().attrIndex("blendMode"); bm >= 0)
                            g.setValue({m->id(), bm, -1}, int64_t(translucent ? 2 : 1));
                g.evaluate();
                json stats = json::parse(strip->as<std::string>(ai(*strip, "stats")), nullptr, false);
                return {{"strip", g.node(tid)->path()}, {"mesh", mesh->path()}, {"material", slot}, {"stats", stats},
                        {"warning", strip->as<std::string>(ai(*strip, "warning"))}};
            }

            // Decal actors in Unreal.
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": decals='" + mode + "' sends to Unreal, which is not connected (" + e.what() + ")");
            }
            auto client = link::Link::get().client();
            json out = {{"decals", mode}};
            if (mode == "actor")
            {
                link::RpcResult info = client->call("rendererInfo", json::object(), 10000);
                if (!info.ok)
                    throw CommandError(cmd + ": rendererInfo: " + info.error);
                const json clustered = info.result.value("forwardLevelUpSettings", json::object()).value("r.Mobile.ClusteredDecals", json());
                if (!truthy(clustered))
                    throw CommandError(cmd + ": decals='actor' needs ForwardLevelUp's clustered decals (r.Mobile.ClusteredDecals 1), which this "
                                             "Unreal reports off; use decals='mesh', which works everywhere, or 'dbuffer' for desktop only");
            }
            else
                out["note"] = "DBuffer decal actors render in desktop forward and deferred only, not on mobile";
            StripOptions o;
            o.side = Side(side);
            o.width = a.get<double>("width", 40.0);
            o.kinds = kinds;
            json decals = json::array();
            for (const DecalBox& b : decalBoxes(*d->result, o, a.get<double>("segment", 100.0)))
            {
                const math::Vector c = math::toUnreal(math::Vector(b.center));
                decals.push_back({{"key", b.key}, {"location", {c.x, c.y, c.z}}, {"xAxis", unrealDir(b.project)}, {"zAxis", unrealDir(b.along)},
                                  {"size", {b.size.x, b.size.y, b.size.z}}});
            }
            link::RpcResult r = client->call("seamDecals", {{"group", g.node(sid)->fullPath()}, {"decals", decals}}, link::importCallOptions());
            if (!r.ok)
                throw CommandError(cmd + ": seamDecals: " + r.error);
            out["result"] = r.result;
            out["count"] = decals.size();
            return out;
        },
        .examples = {"seamDecal('contacts', side='both', width=30)", "seamDecal('contacts', decals='dbuffer')"},
    });
}
} // namespace seamwork
