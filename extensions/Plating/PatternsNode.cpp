#include "Patterns.h"
#include "Plating.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>

namespace plating
{
using namespace lila;
using namespace lila::scene;

namespace
{
const std::vector<std::string> kPatterns = {"hex", "squares", "diamondPlate", "slots", "dots", "grille"};
const std::vector<std::string> kProjections = {"panel", "triplanar"};

class PatternNode final : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kLayout,
        kPattern,
        kProjection,
        kScale,
        kDepth,
        kWidth,
        kRotation,
        kMargin,
        kPickedOnly,
        kOutMesh,
        kWarning,
    };

    PatternNode()
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
        setOutput(kWarning, std::string());
        if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in) || !resolved)
        {
            setOutput(kOutMesh, in);
            if (in && !resolved)
                setOutput(kWarning, std::string("No panel layout connected: draw one with the Layout Pencil"));
            return;
        }
        PatternOptions o;
        o.kind = PatternKind(std::clamp<int64_t>(as<int64_t>(kPattern), 0, int64_t(kPatterns.size()) - 1));
        o.projection = Projection(std::clamp<int64_t>(as<int64_t>(kProjection), 0, 1));
        o.scale = as<double>(kScale);
        o.depth = as<double>(kDepth);
        o.width = as<double>(kWidth);
        o.rotation = as<double>(kRotation);
        o.margin = as<double>(kMargin);
        o.pickedOnly = as<bool>(kPickedOnly);
        try
        {
            PatternResult r = applyPattern(*resolved, *in, o);
            setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
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

AttrDef patternParam(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef patternEnum(std::string name, const std::vector<std::string>& names, std::string doc)
{
    AttrDef a = patternParam(std::move(name), PlugType::Enum, int64_t(0), std::move(doc));
    a.flags = AttrInput | AttrStorable;
    a.enumNames = names;
    return a;
}

int64_t enumIndex(const std::vector<std::string>& names, const std::string& v, const char* flag)
{
    auto it = std::find(names.begin(), names.end(), v);
    if (it == names.end())
        throw CommandError(std::string("surfacePattern: unknown ") + flag + " '" + v + "'");
    return int64_t(it - names.begin());
}
} // namespace

bool registerPatternNodes(ext::Context& ctx)
{
    NodeType t;
    t.name = kPatternType;
    t.icon = "grid-pattern";
    t.doc = "Surface Pattern: hex, squares, diamond plate, slots, dots or grille pressed into the panels of the mesh's Panel Layout, "
            "following each panel's own direction and fading out near the lines. Every panel, or only those picked in the Layout "
            "Pencil. Needs a mesh dense enough to show the pattern.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        patternParam("enabled", PlugType::Bool, true, "Turn the pattern off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply when exporting."},
        {.name = "layout", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The panel layout the pattern follows (a panelLayoutModifier's outLayout)."},
        patternEnum("pattern", kPatterns, "Hex, squares, diamond plate, slots, dots or grille."),
        patternEnum("projection", kProjections, "Panel: rows follow each panel. Triplanar: the mesh's own axes."),
        patternParam("scale", PlugType::Float, 2.0, "Size of one cell, cm.", 0.01),
        patternParam("depth", PlugType::Float, 0.1, "How far the pattern presses in, cm; negative raises it."),
        patternParam("width", PlugType::Float, 0.12, "Line width or feature size, as a fraction of a cell.", 0.01, 0.9),
        patternParam("rotation", PlugType::Float, 0.0, "Turn of the pattern, degrees."),
        patternParam("margin", PlugType::Float, 0.5, "The pattern fades out this far from the lines, cm.", 0.0),
        patternParam("pickedOnly", PlugType::Bool, false, "Only panels picked for a pattern in the Layout Pencil."),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<PatternNode>(); };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}

void registerPatternCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "surfacePattern",
        .doc = "Press a pattern into the panels of each mesh's Panel Layout: hex, squares, diamond plate, slots, dots or grille, "
               "following each panel. Kept live. Returns the pattern modifiers.",
        .positionalDoc = "meshes",
        .flags = {{"pattern", "", T::String, nullptr, "hex, squares, diamondPlate, slots, dots or grille.", kPatterns},
                  {"projection", "", T::String, nullptr, "panel or triplanar.", kProjections},
                  {"scale", "", T::Float, nullptr, "Cell size, cm.", {}, {}, 0.01},
                  {"depth", "", T::Float, nullptr, "How far it presses in, cm; negative raises."},
                  {"width", "", T::Float, nullptr, "Line width or feature size, fraction of a cell.", {}, {}, 0.01, 0.9},
                  {"rotation", "", T::Float, nullptr, "Degrees."},
                  {"margin", "", T::Float, nullptr, "Fade distance from the lines, cm.", {}, {}, 0.0},
                  {"pickedOnly", "", T::Bool, nullptr, "Only panels picked for a pattern."}},
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
                    throw CommandError("surfacePattern: " + mesh->name() + " has no panel layout; draw one with the Layout Pencil first");
                json params = json::object();
                for (const char* k : {"scale", "depth", "width", "rotation", "margin", "pickedOnly"})
                    if (a.has(k))
                        params[k] = a.flags[k];
                if (a.has("pattern"))
                    params["pattern"] = enumIndex(kPatterns, a.get<std::string>("pattern"), "pattern");
                if (a.has("projection"))
                    params["projection"] = enumIndex(kProjections, a.get<std::string>("projection"), "projection");
                const std::string mod = ops::execute("addModifier", {{"args", {mesh->path()}}, {"kind", "pattern"}, {"params", params}});
                Node* m = g.find(mod);
                g.connect({lay->id(), layout::PanelLayoutNode::kOutLayout, -1}, {m->id(), int(m->type().attrIndex("layout")), -1});
                out.push_back(mod);
            }
            if (out.empty())
                throw CommandError("surfacePattern: select or name a mesh with a panel layout");
            return out;
        },
        .examples = {"surfacePattern(\"Hull\", pattern=\"hex\", scale=1.5, depth=0.08)"},
    });
}
} // namespace plating
