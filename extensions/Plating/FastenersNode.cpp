#include "Fasteners.h"
#include "Plating.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>

namespace plating
{
using namespace lila;
using namespace lila::scene;

namespace
{
const std::vector<std::string> kKinds = {"rivet", "hexBolt", "screw", "washer"};
const std::vector<std::string> kPlacements = {"onLines", "bothSides", "corners"};

class FastenersNode final : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kLayout,
        kKind,
        kPlacement,
        kSize,
        kHeight,
        kSpacing,
        kInset,
        kSet,
        kSink,
        kRecess,
        kOutMesh,
        kCount,
        kWarning,
    };

    FastenersNode()
    {
        inAttr = kInMesh;
        enabledAttr = kEnabled;
        outAttr = kOutMesh;
    }

    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        mError.clear();
        const MeshPtr& in = as<MeshPtr>(kInMesh);
        const std::shared_ptr<const layout::Resolved> resolved = layout::resolvedOf(as<DataPtr>(kLayout));
        setOutput(kCount, int64_t(0));
        setOutput(kWarning, std::string());
        if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in) || !resolved)
        {
            setOutput(kOutMesh, in);
            if (in && !resolved)
                setOutput(kWarning, std::string("No panel layout connected: draw one with the Layout Pencil"));
            return;
        }
        FastenerOptions o;
        o.kind = FastenerKind(std::clamp<int64_t>(as<int64_t>(kKind), 0, 3));
        o.placement = Placement(std::clamp<int64_t>(as<int64_t>(kPlacement), 0, 2));
        o.size = as<double>(kSize);
        o.height = as<double>(kHeight);
        o.spacing = as<double>(kSpacing);
        o.inset = as<double>(kInset);
        o.set = as<std::string>(kSet);
        o.sink = as<double>(kSink);
        o.recess = as<double>(kRecess);
        try
        {
            FastenersResult r = makeFasteners(*resolved, *in, o);
            auto out = std::make_shared<geo::Mesh>(r.surface.numFaces() ? std::move(r.surface) : *in);
            out->append(r.mesh);
            setOutput(kOutMesh, MeshPtr(out));
            setOutput(kCount, int64_t(r.count));
            if (!r.warnings.empty())
                setOutput(kWarning, r.warnings.front());
            if (r.count == 0)
                setOutput(kWarning, std::string(o.set.empty() ? "No fasteners fit: draw lines, or lower the spacing or inset"
                                                              : "No fasteners fit on the lines in set " + o.set));
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, std::string(name()) + ": " + e.what());
            mError = e.what();
            setOutput(kOutMesh, in);
            setOutput(kWarning, std::string(e.what()));
        }
    }
};

AttrDef fastenerParam(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef fastenerEnum(std::string name, const std::vector<std::string>& names, std::string doc)
{
    AttrDef a = fastenerParam(std::move(name), PlugType::Enum, int64_t(0), std::move(doc));
    a.flags = AttrInput | AttrStorable;
    a.enumNames = names;
    return a;
}
} // namespace

bool registerFastenerNodes(ext::Context& ctx)
{
    NodeType t;
    t.name = kFastenersType;
    t.icon = "circle-dot";
    t.doc = "Fasteners: rivets, hex bolts, screws or washers set along the mesh's Panel Layout lines, beside them on both sides, "
            "or into panel corners, seated on whatever is under them (plates included). Kept live as the lines change.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        fastenerParam("enabled", PlugType::Bool, true, "Turn the fasteners off without removing them."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
        {.name = "layout", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The panel layout the fasteners follow (a panelLayoutModifier's outLayout)."},
        fastenerEnum("kind", kKinds, "Rivet, hex bolt, screw or washer."),
        [] {
            AttrDef a = fastenerEnum("placement", kPlacements, "On the lines, beside them on both sides (plate borders), or in panel corners.");
            a.defaultValue = int64_t(1);
            return a;
        }(),
        fastenerParam("size", PlugType::Float, 0.6, "Head diameter, cm.", 0.01),
        fastenerParam("height", PlugType::Float, 0.25, "Head height, cm.", 0.0),
        fastenerParam("spacing", PlugType::Float, 3.0, "Distance between fasteners along a line, cm.", 0.05),
        fastenerParam("inset", PlugType::Float, 0.8, "Distance from the line or the corner, cm.", 0.0),
        {.name = "set", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Only lines in this line set; empty for every line."},
        fastenerParam("sink", PlugType::Float, 0.05, "How far each head sinks into the surface, cm."),
        fastenerParam("recess", PlugType::Float, 0.0, "Depth of a round recess pressed under each head, cm; 0 for none. Needs a dense mesh.", 0.0),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "fastenerCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Fasteners placed."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<FastenersNode>(); };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}

void registerFastenerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "fasteners",
        .doc = "Set fasteners along each mesh's Panel Layout: rivets, bolts, screws or washers on the lines, beside them, or in panel "
               "corners, seated on plates when there are some. Kept live. Returns the fasteners modifiers.",
        .positionalDoc = "meshes",
        .flags = {{"kind", "", T::String, nullptr, "rivet, hexBolt, screw or washer.", kKinds},
                  {"placement", "", T::String, nullptr, "onLines, bothSides or corners.", kPlacements},
                  {"size", "", T::Float, nullptr, "Head diameter, cm.", {}, {}, 0.01},
                  {"height", "", T::Float, nullptr, "Head height, cm.", {}, {}, 0.0},
                  {"spacing", "", T::Float, nullptr, "Distance between fasteners, cm.", {}, {}, 0.05},
                  {"inset", "", T::Float, nullptr, "Distance from the line or corner, cm.", {}, {}, 0.0},
                  {"set", "", T::String, nullptr, "Only lines in this line set."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                Node* mesh = g.find(name);
                if (!mesh || !mesh->type().isA("mesh"))
                    continue;
                Node* lay = layout::findLayoutNode(g, mesh->id());
                if (!lay)
                    throw CommandError("fasteners: " + mesh->name() + " has no panel layout; draw one with the Layout Pencil first");
                json params = json::object();
                for (const char* k : {"size", "height", "spacing", "inset", "set"})
                    if (a.has(k))
                        params[k] = a.flags[k];
                for (auto [flag, names] : {std::pair{"kind", &kKinds}, std::pair{"placement", &kPlacements}})
                    if (a.has(flag))
                    {
                        auto it = std::find(names->begin(), names->end(), a.get<std::string>(flag));
                        if (it == names->end())
                            throw CommandError(std::string("fasteners: unknown ") + flag + " '" + a.get<std::string>(flag) + "'");
                        params[flag] = int64_t(it - names->begin());
                    }
                const std::string mod = ops::execute("addModifier", {{"args", {mesh->path()}}, {"kind", "fasteners"}, {"params", params}});
                Node* m = g.find(mod);
                g.connect({lay->id(), layout::PanelLayoutNode::kOutLayout, -1}, {m->id(), int(m->type().attrIndex("layout")), -1});
                out.push_back(mod);
            }
            if (out.empty())
                throw CommandError("fasteners: select or name a mesh with a panel layout");
            return out;
        },
        .examples = {"fasteners(\"Hull\", kind=\"rivet\", placement=\"bothSides\", spacing=2.5)"},
    });

    ctx.registerCommand({
        .name = "fastenersToInstances",
        .doc = "Turn a mesh's fasteners into instances of one fastener mesh, grouped under the mesh: they export to Unreal as one "
               "mesh placed many times (instanced static meshes). The fasteners modifier is turned off, not removed, so turning it "
               "back on and deleting the group undoes it. Returns the group.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* mesh = nullptr;
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && n->type().isA("mesh"))
                    mesh = n;
            if (!mesh)
                throw CommandError("fastenersToInstances: select or name a mesh with fasteners");
            g.evaluate();
            Node* mod = nullptr;
            for (NodeId id : modifiers::stack(g, mesh->id()))
                if (Node* n = g.node(id); n && n->type().name == kFastenersType)
                    mod = n;
            if (!mod)
                throw CommandError("fastenersToInstances: " + mesh->name() + " has no fasteners");
            const std::shared_ptr<const layout::Resolved> r = layout::resolvedOf(mod->as<DataPtr>(FastenersNode::kLayout));
            const MeshPtr surface = mod->as<MeshPtr>(FastenersNode::kInMesh);
            if (!r || !surface)
                throw CommandError("fastenersToInstances: the fasteners have no panel layout");
            FastenerOptions o;
            o.kind = FastenerKind(std::clamp<int64_t>(mod->as<int64_t>(FastenersNode::kKind), 0, 3));
            o.placement = Placement(std::clamp<int64_t>(mod->as<int64_t>(FastenersNode::kPlacement), 0, 2));
            o.size = mod->as<double>(FastenersNode::kSize);
            o.height = mod->as<double>(FastenersNode::kHeight);
            o.spacing = mod->as<double>(FastenersNode::kSpacing);
            o.inset = mod->as<double>(FastenersNode::kInset);
            o.set = mod->as<std::string>(FastenersNode::kSet);
            o.sink = mod->as<double>(FastenersNode::kSink);
            o.recess = mod->as<double>(FastenersNode::kRecess);
            const std::vector<glm::dmat4> at = fastenerPlacements(*r, *surface, o);
            if (at.empty())
                throw CommandError("fastenersToInstances: no fasteners to convert");
            Node* group = g.createNode(*types::find("transform"), mesh->name() + "_Fasteners", mesh->id(), false);
            Node* source = g.createNode(*types::find("mesh"), mesh->name() + "_Fastener", group->id(), false);
            geo::Mesh shape = fastenerMesh(o);
            geo::setHardEdgesByAngle(shape, 40.0);
            g.setValue({source->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(shape))));
            auto place = [&](Node& n, const glm::dmat4& frame) {
                g.setValue({n.id(), Transform::kTranslate, -1}, glm::dvec3(frame[3]));
                g.setValue({n.id(), Transform::kRotate, -1}, xformops::closestEuler(glm::dmat3(frame), RotateOrder::XYZ, glm::dvec3(0.0)));
            };
            place(*source, at.front());
            for (size_t k = 1; k < at.size(); ++k)
            {
                const json made = ops::execute("instance", {{"args", {source->path()}}});
                const std::string path = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
                if (Node* copy = g.find(path))
                    place(*copy, at[k]);
            }
            g.setValue({mod->id(), FastenersNode::kEnabled, -1}, false);
            return group->path();
        },
    });
}
} // namespace plating
