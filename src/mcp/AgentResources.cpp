#include "mcp/McpServer.h"

#include "ops/Command.h"
#include "core/Paths.h"

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace lila::mcp
{
namespace
{
std::filesystem::path agentsDir() { return paths::resourcesDir() / "agents"; }

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("missing " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** Replaces {{name}} with args[name] (or the fallback when it is missing). */
std::string fill(std::string text, const json& args, const std::map<std::string, std::string>& fallback)
{
    for (auto& [key, def] : fallback)
    {
        std::string value = args.contains(key) && args[key].is_string() && !args[key].get<std::string>().empty() ? args[key].get<std::string>() : def;
        std::string tag = "{{" + key + "}}";
        for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
            text.replace(at, tag.size(), value);
    }
    return text;
}

/** Every command name grouped by its first word, appended to the API guide so it never goes stale. */
std::string commandIndex()
{
    std::string out = "\n\n## Every registered command\n\nCall any of these as `lila.ops.<name>(...)` (run `help(lila.ops.<name>)` or the `list_commands` tool with `filter` for flags).\n\n";
    std::string line;
    for (const CommandDef* d : ops::all())
    {
        if (line.size() + d->name.size() > 110)
        {
            out += line + "\n";
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + d->name;
    }
    return out + line + "\n";
}

std::string socketLine(const json& s)
{
    std::string out = s.value("id", "") + " " + s.value("type", "");
    if (s.value("field", false))
        out += " field";
    if (s.contains("implicit"))
        out += " (implicit " + s["implicit"].get<std::string>() + ")";
    else if (s.contains("default") && !s["default"].is_null())
        out += " = " + s["default"].dump();
    return out;
}

/** The geometry-node catalogue from the live node registry, appended to the geometry-nodes guide. */
std::string nodeCatalogue()
{
    const CommandDef* d = ops::find("gnNodeTypes");
    if (!d)
        return {};
    Args a;
    a.flags = {{"detail", true}};
    json types;
    try
    {
        types = d->run(a);
    }
    catch (const std::exception&)
    {
        return {};
    }
    std::map<std::string, std::vector<const json*>> byCategory;
    for (const json& t : types)
        byCategory[t.value("category", "Other")].push_back(&t);
    std::string out = "\n\n## Node catalogue\n\nSockets are listed as they are with default properties; nodes marked \"sockets follow props\" change "
                      "their sockets with their properties (gnGroupInfo shows the resolved ones).\n";
    for (auto& [cat, list] : byCategory)
    {
        out += "\n### " + cat + "\n\n";
        for (const json* t : list)
        {
            out += "- `" + t->value("type", "") + "`: " + t->value("doc", "");
            if (t->value("dynamicSockets", false))
                out += " (sockets follow props)";
            out += "\n";
            std::string props;
            for (const json& p : t->value("props", json::array()))
            {
                props += (props.empty() ? "" : "; ") + p.value("id", "");
                if (!p["options"].empty())
                {
                    std::string opts;
                    for (const json& o : p["options"])
                        opts += (opts.empty() ? "" : "|") + o.get<std::string>();
                    props += " " + opts;
                }
                props += " (default " + p["default"].dump() + ")";
            }
            if (!props.empty())
                out += "  - props: " + props + "\n";
            std::string ins, outs;
            for (const json& s : t->value("inputs", json::array()))
                ins += (ins.empty() ? "" : ", ") + socketLine(s);
            for (const json& s : t->value("outputs", json::array()))
                outs += (outs.empty() ? "" : ", ") + s.value("id", "") + " " + s.value("type", "");
            if (!ins.empty())
                out += "  - in: " + ins + "\n";
            if (!outs.empty())
                out += "  - out: " + outs + "\n";
        }
    }
    return out;
}

/** The texture-node catalogue from the live node library, appended to the materials guide. */
std::string textureCatalogue()
{
    const CommandDef* d = ops::find("texNodeTypes");
    if (!d)
        return {};
    Args a;
    a.flags = {{"detail", true}};
    json types;
    try
    {
        types = d->run(a);
    }
    catch (const std::exception&)
    {
        return {};
    }
    std::map<std::string, std::vector<const json*>> byCategory;
    for (const json& t : types)
        byCategory[t.value("category", "Other")].push_back(&t);
    std::string out = "\n\n## Node catalogue\n\nParameters go in a node's `params`, unlinked input values in `values`.\n";
    for (auto& [cat, list] : byCategory)
    {
        out += "\n### " + cat + "\n\n";
        for (const json* t : list)
        {
            out += "- `" + t->value("type", "") + "`: " + t->value("doc", "") + "\n";
            std::string params;
            for (const json& p : t->value("params", json::array()))
            {
                params += (params.empty() ? "" : "; ") + p.value("id", "") + " " + p.value("type", "");
                if (p.contains("options") && !p["options"].empty())
                {
                    std::string opts;
                    for (const json& o : p["options"])
                        opts += (opts.empty() ? "" : "|") + o.get<std::string>();
                    params += " " + opts;
                }
                params += " = " + p["default"].dump();
            }
            if (!params.empty())
                out += "  - params: " + params + "\n";
            std::string ins, outs;
            for (const json& s : t->value("inputs", json::array()))
                ins += (ins.empty() ? "" : ", ") + socketLine(s);
            for (const json& s : t->value("outputs", json::array()))
                outs += (outs.empty() ? "" : ", ") + s.value("id", "") + " " + s.value("type", "");
            if (!ins.empty())
                out += "  - in: " + ins + "\n";
            if (!outs.empty())
                out += "  - out: " + outs + "\n";
        }
    }
    return out;
}

/** An agent guide: an MCP resource, and a read_guide topic for clients that can't read resources. */
struct Doc
{
    const char* uri;
    const char* topic;
    const char* file;
    const char* name;
    const char* description;
    /** Generated from the live registries and appended, so it never goes stale. */
    std::string (*appendix)() = nullptr;
};

const Doc kDocs[] = {
    {"lila://guide/modeling", "modeling", "guide-modeling.md", "Modeling guide",
     "How to model in Lila from code: one connected mesh grown from one primitive with symmetry and extrusion, recipes for "
     "limbs, head, snout, eyes, ears and tail, selecting components by position and normal, checking with mesh_check and "
     "captures, cheap token use. Read before modeling anything."},
    {"lila://guide/api", "api", "guide-api.md", "API guide", "Where Lila's commands and Python modules live and how to call them, plus an index of every command.",
     commandIndex},
    {"lila://guide/geometry-nodes", "geometryNodes", "guide-geometry-nodes.md", "Geometry nodes guide",
     "Authoring geometry-node groups from code: the one-call gnBuildGroup spec, group interfaces, nested groups, colours and "
     "normals for export, sharing as .lilanodes, and the full node catalogue with sockets and options.",
     nodeCatalogue},
    {"lila://guide/materials", "materials", "guide-materials.md", "Materials guide",
     "Procedural materials as texture graphs: the one-call texBuildGraph spec, channels, mesh maps from bakes, previewing, "
     "exporting maps and sending to Unreal, and the full texture node catalogue. Read before building a material node setup.",
     textureCatalogue},
    {"lila://guide/set-dressing", "setDressing", "guide-set-dressing.md", "Set dressing guide",
     "Dressing scenes with props: library search and placement, physics settle/push/drop, shelf fills, scatters and piles, "
     "modular kit snapping, and dressCheck to verify. Read before placing or arranging props."},
    {"lila://guide/terrain", "terrain", "guide-terrain.md", "Terrain guide",
     "Building terrain from code: recipes first, one-JSON specs and presets, named places (terrainQuery, region:<name>), wedges, "
     "create, import and erode, sample and route, sculpt strokes, building pads and plaza stamps "
     "(cut and fill, walls, piles), and sending the landscape with its paint layers to Unreal. Read before any terrain work."},
    {"lila://guide/settlement", "settlement", "guide-settlement.md", "Settlement guide",
     "Building villages, towns, cities and regions from code: recipes first, settlementCreate on flat ground or a Terrain "
     "(settlementSuggest for the site, settlementGrade to build it into the ground), styles, eras, walls and districts, editing "
     "streets and lots by stable id, settlementCheck, settlementExplain and framed captures to verify, and settlementSendToUnreal "
     "or settlementExport to ship. Read before any settlement work."},
    {"lila://guide/masonry", "masonry", "guide-masonry.md", "Masonry guide",
     "Building masonry from code: recipes first (ruined round tower, building from one spec, stone over a blockout, walls on "
     "terrain), the masonryBuilding spec keys, ruin, age, wedges, masonryCheck and captures, and shipping instanced stones to "
     "Unreal on MF_LilaMasonry. Read before building walls, towers or ruins."},
    {"lila://guide/foliage", "foliage", "guide-foliage.md", "Foliage guide",
     "Growing trees and plants from code: recipes first (windswept pines, broadleaf, bush), the presets and the settings that "
     "matter (lengths in metres), leaf cards and detail levels, bark and leaf texture sets, checking with meshInfo and captures, "
     "and sending trees to Unreal or exporting the bake pair. Read before making any tree or plant."},
    {"lila://examples/low-poly-dog", "example", "examples/low_poly_dog.py", "Example: low-poly dog",
     "A complete script: a very low-poly dog as one closed symmetric mesh (legs, head, snout, nose, eyes, floppy ears, tail "
     "all extruded), checked with meshCheck and captures."},
};

std::string readDoc(const Doc& d) { return readFile(agentsDir() / d.file) + (d.appendix ? d.appendix() : std::string()); }
} // namespace

std::string agentInstructions()
{
    try
    {
        return readFile(agentsDir() / "instructions.md");
    }
    catch (const std::exception&)
    {
        return "You are connected to Lila, a Maya-style 3D app. Do the 3D work here with these tools, not in Blender or Maya. Read "
               "the lila://guide/modeling resource (or the read_guide tool) before modeling.";
    }
}

void registerAgentResources()
{
    Server& s = Server::get();
    s.setInstructions(agentInstructions());

    std::string topicDocs;
    for (const Doc& d : kDocs)
    {
        const std::string file = d.file;
        s.addResource({
            .uri = d.uri,
            .name = d.name,
            .description = d.description,
            .mimeType = file.ends_with(".py") ? "text/x-python" : "text/markdown",
            .read = [&d] { return readDoc(d); },
        });
        topicDocs += std::string(topicDocs.empty() ? "" : "; ") + d.topic + " (" + d.name + ")";
    }

    s.addTool({
        .name = "read_guide",
        .description = "Lila's agent guides, for clients that cannot read MCP resources. Read the one for the work before starting. "
                       "topic: " + topicDocs + "; extensions add more (any lila://guide/<name>). Leave topic out for the list.",
        .inputSchema = {{"type", "object"}, {"properties", {{"topic", {{"type", "string"}}}}}},
        .run = [](const json& args) -> std::string {
            const std::string topic = args.value("topic", std::string());
            for (const Doc& d : kDocs)
                if (topic == d.topic)
                    return readDoc(d);
            // Extension guides: "geometryNodes" and "geometry-nodes" both name lila://guide/geometry-nodes.
            std::string kebab;
            for (char c : topic)
                kebab += std::isupper(static_cast<unsigned char>(c)) ? std::string("-") + char(std::tolower(c)) : std::string(1, c);
            const std::vector<Resource> guides = Server::get().resources("lila://guide/");
            for (const Resource& r : guides)
                if (r.uri == "lila://guide/" + topic || r.uri == "lila://guide/" + kebab)
                    return r.read();
            std::string list = topic.empty() ? "Lila's guides:\n" : "No guide '" + topic + "'. Lila's guides:\n";
            for (const Resource& r : guides)
                list += "- " + r.uri.substr(std::string("lila://guide/").size()) + ": " + r.description + "\n";
            return list;
        },
    });

    s.addPrompt({
        .name = "dress_scene",
        .description = "Dress a space with props: pick from the library, place and arrange them, settle with physics, and check "
                       "until nothing floats, overlaps or would fall.",
        .arguments = json::array({
            {{"name", "brief"}, {"description", "What to dress and how, e.g. \"a cluttered study: full bookshelf, papers on the desk\""}, {"required", true}},
            {{"name", "area"}, {"description", "Where: mesh names or a bounding box in cm"}, {"required", false}},
        }),
        .get = [](const json& args) {
            return fill(readFile(agentsDir() / "prompts" / "dress_scene.md"), args, {{"brief", "the requested set"}, {"area", "the selection"}});
        },
    });

    s.addPrompt({
        .name = "model_asset",
        .description = "Model a game asset in Lila step by step: block out one symmetric mesh, refine with edge loops, check captures "
                       "from four angles, iterate.",
        .arguments = json::array({
            {{"name", "subject"}, {"description", "What to model, e.g. \"a small cute dog with big round eyes\""}, {"required", true}},
            {{"name", "style"}, {"description", "Style and density, e.g. \"very low poly, flat shaded\""}, {"required", false}},
            {{"name", "budget"}, {"description", "Triangle budget, e.g. \"500\""}, {"required", false}},
        }),
        .get = [](const json& args) {
            return fill(readFile(agentsDir() / "prompts" / "model_asset.md"), args,
                        {{"subject", "the requested asset"}, {"style", "low poly, clean quads"}, {"budget", "800"}});
        },
    });
}
} // namespace lila::mcp
