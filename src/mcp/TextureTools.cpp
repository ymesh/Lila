#include "mcp/McpServer.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <functional>

namespace lila::mcp
{
namespace
{
std::string text(const json& j) { return j.dump(1); }

scene::Node& setArg(const json& args)
{
    scene::Graph& g = scene::Graph::current();
    std::string name = args.value("set", std::string());
    if (name.empty())
    {
        const std::vector<std::string> sets = tex::setNames(g);
        if (sets.size() != 1)
            throw std::runtime_error(sets.empty() ? "the scene has no texture sets (texCreateSet makes one)" : "several texture sets; pass set");
        name = sets.front();
    }
    scene::Node* n = tex::findSet(g, name);
    if (!n)
        throw std::runtime_error("no texture set named " + name);
    return *n;
}

json meshesUsing(scene::Graph& g, const std::vector<std::string>& slots)
{
    json out = json::array();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            for (const auto& [s, count] : scene::materials::slotUsage(*m))
                if (count > 0 && std::ranges::find(slots, s) != slots.end())
                {
                    out.push_back(n->path());
                    break;
                }
    }
    return out;
}

/** The set as an agent needs it to edit: settings, the stack (or graph mode), what its masks bind to, and its paint. */
json describeTextureSet(scene::Node& set)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const tex::SetSettings s = tex::readSettings(set);
    const tex::Graph graph = tex::readGraph(set);
    const std::vector<std::string> users = tex::materialsUsing(g, set.name());
    const std::string slot = users.empty() ? std::string() : users.front();
    json channels = json::array();
    for (const tex::Channel& c : s.channels)
        channels.push_back({{"name", c.name}, {"type", tex::portTypeName(c.type)}, {"bits", c.bits}});
    json out = {{"set", set.name()},
                {"materials", users},
                {"meshes", meshesUsing(g, users)},
                {"channels", channels},
                {"settings", s.toJson()}};
    if (scene::Node* mat = scene::materials::find(g, slot))
    {
        const scene::AttrDef& d = mat->type().attrs[size_t(scene::material::kShadingModel)];
        const int64_t v = mat->as<int64_t>(scene::material::kShadingModel);
        out["shadingModel"] = v >= 0 && size_t(v) < d.enumNames.size() ? json(d.enumNames[size_t(v)]) : json(v);
    }

    const tex::StackState state = tex::readStack(graph, s);
    out["mode"] = state.graphMode ? (state.edited ? "graph (stack edited as nodes)" : "graph") : "layers";
    const json view = tex::stackView(state, s, graph);
    out["layers"] = view.value("layers", json::array());
    if (view.contains("base"))
        out["base"] = view["base"];

    // Bindings: every mesh map the graph reads, whether the bake project makes it, and each generator mask's sources.
    const std::vector<std::string> used = tex::externalsUsed(graph);
    json maps = json::array();
    for (const std::string& m : used)
        if (m.find(':') == std::string::npos)
            maps.push_back(m);
    json bindings = {{"bakeProject", s.bakeProject}, {"meshMaps", maps}, {"missingMaps", tex::missingBakeMaps(g, s.bakeProject, maps.get<std::vector<std::string>>())}};
    json masks = json::array();
    std::function<void(const std::vector<tex::Layer>&)> walk = [&](const std::vector<tex::Layer>& list) {
        for (const tex::Layer& l : list)
        {
            for (const tex::Mask& m : l.masks)
                if (m.kind == tex::MaskKind::Generator || m.kind == tex::MaskKind::Anchor)
                    masks.push_back({{"layer", l.id}, {"mask", m.id}, {"kind", tex::maskKindName(m.kind)}, {"type", m.type}, {"bindings", m.bindings},
                                     {"anchor", m.anchor}});
            walk(l.children);
        }
    };
    walk(state.stack.layers);
    bindings["masks"] = masks;
    out["bindings"] = bindings;

    json paint = json::array();
    std::function<void(const std::vector<tex::Layer>&)> paints = [&](const std::vector<tex::Layer>& list) {
        for (const tex::Layer& l : list)
        {
            if (l.kind == tex::LayerKind::Paint)
                paint.push_back({{"layer", l.id}, {"name", l.name}, {"images", l.paint}});
            paints(l.children);
        }
    };
    paints(state.stack.layers);
    if (ops::find("texPaintInfo"))
    {
        try
        {
            const json info = ops::execute("texPaintInfo", {{"args", {set.name()}}});
            for (json& p : paint)
                if (info.contains("layers") && info["layers"].contains(p["layer"].get<std::string>()))
                    p["strokes"] = info["layers"][p["layer"].get<std::string>()];
            out["activePaintLayer"] = info.value("active", json());
        }
        catch (const std::exception&)
        {
        }
    }
    out["paintLayers"] = paint;
    return out;
}
} // namespace

void registerTextureTools()
{
    Server& s = Server::get();

    s.addTool({
        .name = "describe_texture_set",
        .description = "A texture set as JSON: the materials using it and their meshes, channels and settings, mode (layers, or graph when edited as "
                       "nodes), the layer stack bottom first (each layer's kind, channels with mode and opacity, material values, masks), "
                       "bindings (mesh maps the graph reads, which the bake project doesn't make yet, each generator mask's sources) and "
                       "paint layers (images, stroke counts). Edit with run_command tex* commands (texAddLayer, texApplySmartMaterial, "
                       "texSetLayer...), check with material_lint, look with render_material_preview.",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"set", {{"type", "string"}, {"description", "Texture set; default the scene's only one."}}}}}},
        .run = [](const json& args) { return text(describeTextureSet(setArg(args))); },
    });

    s.addTool({
        .name = "material_lint",
        .description = "Check a material against physical ranges (materialLint): a texture set, a library id or a .lilamat path, evaluated "
                       "small. Albedo, pure black/white, metal reflectance, binary metallic, roughness, normal validity and convention, "
                       "height range, tile seams. fix=true clamps what it can with Levels nodes. Returns {passed, failures, warnings, "
                       "checks: [{id, status, message}]} (and fixed, after with fix).",
        .inputSchema = {{"type", "object"},
                        {"properties",
                         {{"material", {{"type", "string"}, {"description", "Texture set, library id or .lilamat path; default the scene's only set."}}},
                          {"fix", {{"type", "boolean"}}},
                          {"size", {{"type", "integer"}, {"description", "Evaluation size in px (default 256)."}}},
                          {"physical", {{"type", "string"}, {"description", "metal, dielectric, organic, emissive or stylised."}}},
                          {"backend", {{"type", "string"}, {"description", "gpu or cpu."}}}}}},
        .run = [](const json& args) {
            const std::string material = args.contains("material") ? args["material"].get<std::string>() : setArg(args).name();
            json call = {{"args", {material}}};
            for (const char* k : {"fix", "size", "physical", "backend"})
                if (args.contains(k))
                    call[k] = args[k];
            return text(ops::execute("materialLint", call, CallSource::UI));
        },
    });

    s.addTool({
        .name = "paint_stroke",
        .description = "Paint one stroke into a texture set's paint layer (texPaintStroke): a record with payload {channels {baseColor: [r, g, b], "
                       "roughness: 0.3...}, blend normal|erase|add|subtract|multiply}, brush {radius, strength, spacing, falloff...}, and either "
                       "space=surface dabs [{triangle, bary [u, v], radius, pressure}] or [{position [x, y, z], radius}] (object space), or "
                       "space=screen points [[x, y, pressure]] with view {width, height, camera}. One undo step; the record is kept so it can be "
                       "replayed. Returns {layer, stroke, dabs, pages}.",
        .inputSchema = {{"type", "object"},
                        {"properties",
                         {{"set", {{"type", "string"}, {"description", "Texture set; default the scene's only one."}}},
                          {"layer", {{"type", "string"}, {"description", "Paint layer id; default the active, else the top paint layer, else a new one."}}},
                          {"stroke", {{"type", "object"}, {"description", "The whole stroke record."}}},
                          {"mesh", {{"type", "string"}}},
                          {"payload", {{"type", "object"}}},
                          {"brush", {{"type", "object"}}},
                          {"space", {{"type", "string"}, {"enum", {"surface", "screen"}}}},
                          {"points", {{"type", "array"}}},
                          {"view", {{"type", "object"}}}}},
                        {"required", {"stroke"}}},
        .run = [](const json& args) {
            if (!ops::find("texPaintStroke"))
                throw std::runtime_error("texPaintStroke is not available in this build (painting needs the renderer)");
            json call = {{"args", {setArg(args).name()}}};
            for (const char* k : {"layer", "stroke", "mesh", "payload", "brush", "space", "points", "view"})
                if (args.contains(k))
                    call[k] = args[k];
            return text(ops::execute("texPaintStroke", call, CallSource::UI));
        },
    });
}
} // namespace lila::mcp
