#include "Plates.h"
#include "Plating.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_inverse.hpp>

namespace plating
{
using namespace lila;
using namespace lila::scene;

namespace
{
class PlatesNode final : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kLayout,
        kGap,
        kThickness,
        kElevation,
        kBevel,
        kBevelSegments,
        kLipWidth,
        kLipDepth,
        kClosed,
        kKeepBase,
        kOutMesh,
        kPlates,
        kWarning,
    };

    PlatesNode()
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
        if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in) || !resolved)
        {
            setOutput(kOutMesh, in);
            setOutput(kPlates, int64_t(0));
            setOutput(kWarning, std::string(resolved || !in ? "" : "No panel layout connected: draw one with the Layout Pencil"));
            return;
        }
        PlateOptions o;
        o.gap = as<double>(kGap);
        o.thickness = as<double>(kThickness);
        o.elevation = as<double>(kElevation);
        o.bevel = as<double>(kBevel);
        o.bevelSegments = int(as<int64_t>(kBevelSegments));
        o.lipWidth = as<double>(kLipWidth);
        o.lipDepth = as<double>(kLipDepth);
        o.closed = as<bool>(kClosed);
        try
        {
            PlatesResult r = makePlates(*resolved, o);
            auto out = std::make_shared<geo::Mesh>();
            if (as<bool>(kKeepBase))
                *out = *in;
            out->append(r.mesh);
            setOutput(kOutMesh, MeshPtr(out));
            setOutput(kPlates, int64_t(r.plates));
            std::string text;
            for (const std::string& w : r.warnings)
                text += (text.empty() ? "" : "\n") + w;
            setOutput(kWarning, std::move(text));
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

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

PlatesNode* platesOn(Graph& g, NodeId mesh)
{
    const std::vector<NodeId> s = modifiers::stack(g, mesh);
    for (auto it = s.rbegin(); it != s.rend(); ++it)
        if (Node* n = g.node(*it); n && n->type().name == kPlatesType)
            return static_cast<PlatesNode*>(n);
    return nullptr;
}

Node& meshArg(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    for (const std::string& name : a.objects())
        if (Node* n = g.find(name))
        {
            if (n->type().isA("mesh"))
                return *n;
            if (n->type().name == kPlatesType || n->type().name == layout::kLayoutType)
                if (Node* m = g.node(modifiers::meshOf(g, n->id())))
                    return *m;
        }
    throw CommandError(std::string(cmd) + ": give a mesh");
}
} // namespace

bool registerNodes(ext::Context& ctx)
{
    NodeType t;
    t.name = kPlatesType;
    t.icon = "layers-subtract";
    t.doc = "Plates: each panel of the mesh's Panel Layout as a thick plate, split from its neighbours by a gap along every line, "
            "with a bevelled top edge and an optional lip. Panels can be raised, sunk, thickened or hidden one by one (platesPanel).";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        param("enabled", PlugType::Bool, true, "Turn the plates off without removing them."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
        {.name = "layout", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The panel layout the plates follow (a panelLayoutModifier's outLayout)."},
        param("gap", PlugType::Float, 0.3, "Gap between plates along every line, cm.", 0.0),
        param("thickness", PlugType::Float, 1.0, "How far each plate stands up from the surface, cm.", 0.0),
        param("elevation", PlugType::Float, 0.0, "Moves each plate's base out along the surface first, cm; negative sinks it."),
        param("bevel", PlugType::Float, 0.15, "Bevel round each plate's top edge, cm.", 0.0),
        param("bevelSegments", PlugType::Int, int64_t(2), "Segments across the bevel.", 1.0, 8.0),
        param("lipWidth", PlugType::Float, 0.0, "Width of a raised rim inside each plate's edge, cm; 0 for none.", 0.0),
        param("lipDepth", PlugType::Float, 0.2, "How far the plate's middle sinks below its rim, cm.", 0.0),
        param("closed", PlugType::Bool, true, "Close each plate underneath so it is a solid."),
        param("keepBase", PlugType::Bool, false, "Keep the mesh under the plates (armour over a body) instead of only the plates."),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "plateCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Plates made."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<PlatesNode>(); };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "plates",
        .doc = "Turn each mesh's Panel Layout into plates: every panel a thick plate, split by a gap along the lines, bevelled, kept live "
               "as the layout changes. The lines that divide plates are best in a line set whose output is None. Returns the plates "
               "modifiers.",
        .positionalDoc = "meshes",
        .flags = {{"gap", "", T::Float, nullptr, "Gap between plates, cm.", {}, {}, 0.0},
                  {"thickness", "", T::Float, nullptr, "Plate thickness, cm.", {}, {}, 0.0},
                  {"elevation", "", T::Float, nullptr, "Base offset along the surface, cm."},
                  {"bevel", "", T::Float, nullptr, "Top edge bevel, cm.", {}, {}, 0.0},
                  {"lipWidth", "", T::Float, nullptr, "Rim width, cm.", {}, {}, 0.0},
                  {"keepBase", "", T::Bool, nullptr, "Keep the mesh under the plates."}},
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
                    throw CommandError("plates: " + mesh->name() + " has no panel layout; draw one with the Layout Pencil first");
                json params = json::object();
                for (const char* k : {"gap", "thickness", "elevation", "bevel", "lipWidth", "keepBase"})
                    if (a.has(k))
                        params[k] = a.flags[k];
                const std::string mod = ops::execute("addModifier", {{"args", {mesh->path()}}, {"kind", "plates"}, {"params", params}});
                Node* m = g.find(mod);
                g.connect({lay->id(), layout::PanelLayoutNode::kOutLayout, -1}, {m->id(), int(m->type().attrIndex("layout")), -1});
                out.push_back(mod);
            }
            if (out.empty())
                throw CommandError("plates: select or name a mesh with a panel layout");
            return out;
        },
        .examples = {"plates(\"Chestplate\", gap=0.4, thickness=1.5, bevel=0.2)"},
    });

    ctx.registerCommand({
        .name = "platesPanel",
        .doc = "Give the panel at a point its own plate values: hide it (a hole), raise or sink it (elevation), or change its "
               "thickness. clear drops its own values. Panels are found again by that point as the layout changes.",
        .positionalDoc = "mesh",
        .flags = {{"point", "p", T::Vec3, nullptr, "A world-space point inside the panel."},
                  {"hidden", "", T::Bool, nullptr, "Leave the panel out."},
                  {"elevation", "", T::Float, nullptr, "The panel's own elevation, cm."},
                  {"thickness", "", T::Float, nullptr, "The panel's own thickness, cm.", {}, {}, 0.0},
                  {"pattern", "", T::Bool, nullptr, "Pick the panel for surface patterns that only cover picked panels."},
                  {"clear", "", T::Bool, false, "Drop the panel's own values."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& mesh = meshArg(a, "platesPanel");
            Node* lay = layout::findLayoutNode(g, mesh.id());
            if (!lay || !a.has("point"))
                throw CommandError("platesPanel: give a mesh with a panel layout and point=");
            g.evaluate();
            const json pj = a.flags["point"];
            glm::dvec3 world{pj[0].get<double>(), pj[1].get<double>(), pj[2].get<double>()};
            const glm::dmat4 m = mesh.type().isA("transform") ? mesh.as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
            const glm::dvec3 local = glm::dvec3(glm::inverse(m) * glm::dvec4(world, 1.0));
            const std::shared_ptr<const layout::Resolved> r = layout::resolvedLayout(g, lay->id());
            if (!r)
                throw CommandError("platesPanel: the layout has no surface");
            const layout::PanelSample at = r->panelAt(local);
            if (at.panel < 0)
                throw CommandError("platesPanel: no panel there");
            const layout::PanelLayout* stored = layout::layoutOf(lay->as<DataPtr>(layout::PanelLayoutNode::kLayout));
            layout::PanelLayout l = stored ? *stored : layout::PanelLayout::withDefaultSets();
            json values = at.override >= 0 ? l.panels[size_t(at.override)].values : json::object();
            if (a.get<bool>("clear", false))
                values = json::object();
            for (const char* k : {"hidden", "elevation", "thickness", "pattern"})
                if (a.has(k))
                    values[k] = a.flags[k];
            if (at.override >= 0)
            {
                if (values.empty())
                    l.panels.erase(l.panels.begin() + at.override);
                else
                    l.panels[size_t(at.override)].values = values;
            }
            else if (!values.empty())
            {
                const layout::Surface surface(*r->mesh());
                l.panels.push_back({surface.anchorAt(local), values});
            }
            g.setValue({lay->id(), layout::PanelLayoutNode::kLayout, -1}, layout::makeLayoutData(std::move(l)));
            return {{"panel", at.panel}, {"values", values}};
        },
    });

    ctx.registerCommand({
        .name = "platesInfo",
        .doc = "A mesh's plates: how many, their settings and warnings.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& mesh = meshArg(a, "platesInfo");
            PlatesNode* p = platesOn(g, mesh.id());
            if (!p)
                throw CommandError("platesInfo: " + mesh.name() + " has no plates");
            g.evaluate();
            return {{"plates", p->as<int64_t>(PlatesNode::kPlates)},
                    {"gap", p->as<double>(PlatesNode::kGap)},
                    {"thickness", p->as<double>(PlatesNode::kThickness)},
                    {"warning", p->as<std::string>(PlatesNode::kWarning)}};
        },
    });
}
} // namespace plating
