// Export and import through GroomLab's writers and file format (github.com/thekesslereffect/groomlab, Apache-2.0, see
// src/groom/groomlab/LICENSE and NOTICE).
#include "groom/GroomExport.h"

#include "core/Log.h"
#include "geo/Mesh.h"
#include "groom/GroomGraph.h"
#include "groom/GroomNodes.h"
#include "groom/YetiImport.h"
#include "groom/YetiImport.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <groomlab/card_files.h>
#include <groomlab/card_presets.h>
#include <groomlab/engine_export.h>
#include <groomlab/flow.h>
#include <groomlab/groom_file.h>
#include <groomlab/guide_hierarchy.h>
#include <groomlab/parts.h>
#include <groomlab/scalp_maps.h>
#include <groomlab/usd_writer.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace lila::groom
{
using T = ArgType;

namespace
{
using namespace scene;
namespace fs = std::filesystem;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** The export's name: the file's stem as an identifier, as GroomLab's hosts name it. */
std::string exportName(const std::string& path)
{
    std::string name;
    for (char c : fs::path(path).stem().string())
        name += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
        name = "groom" + name;
    return name;
}

Node& groomArg(Graph& g, const Args& a, const char* command)
{
    if (!a.positional.empty())
    {
        Node* n = g.find(a.positional[0].get<std::string>());
        if (!n)
            throw CommandError(std::string(command) + ": no node " + a.positional[0].get<std::string>());
        if (Node* gr = findGroom(g, *n))
            return *gr;
        throw CommandError(std::string(command) + ": " + n->name() + " is not part of a groom");
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = findGroom(g, *n))
                return *gr;
    throw CommandError(std::string(command) + ": select a groom");
}

std::string text(const Node& n, const char* attr)
{
    const int a = n.type().attrIndex(attr);
    return a < 0 ? std::string() : std::get<std::string>(n.value(a));
}

/** The frames written: start to end inclusive, by step, as scene frames. */
std::vector<double> framesOf(const Args& a, double fps, double now)
{
    const double start = a.has("start") ? a.get<double>("start") : now;
    const double end = a.has("end") ? a.get<double>("end") : start;
    const double step = std::max(1e-6, a.get<double>("step", 1.0));
    if (end < start)
        throw CommandError("exportGroom: end is before start");
    std::vector<double> out;
    for (double f = start; f <= end + 1e-9; f += step)
        out.push_back(f);
    (void)fps;
    return out;
}

json exportFile(const Args& a)
{
    Graph& g = Graph::current();
    Node& gr = groomArg(g, a, "exportGroom");
    const std::string path = a.get<std::string>("path", "");
    if (path.empty())
        throw CommandError("exportGroom: give a path (.abc, .usd, .usda, .usdc or .groom)");
    const std::string ext = lower(fs::path(path).extension().string());
    const double fps = anim::playback().fps > 0.0 ? anim::playback().fps : 24.0;
    const double was = g.time();
    const std::vector<double> frames = framesOf(a, fps, was * fps);
    if (!fs::path(path).parent_path().empty())
        fs::create_directories(fs::path(path).parent_path());
    json out = {{"file", path}, {"frames", frames.size()}};
    struct RestoreTime
    {
        Graph& g;
        double seconds;
        ~RestoreTime() { g.setTime(seconds); }
    } restore{g, was};
    if (ext == ".groom")
    {
        groomlab::GroomFile file;
        const GroomExportFrame frame = groomExportFrame(g, gr.id(), was);
        groomlab::GroomData& d = file.data;
        d.graphJson = text(gr, "graph").empty() ? groomGraph::defaultGraph() : text(gr, "graph");
        d.hierarchyJson = text(gr, "hierarchy");
        d.partsJson = text(gr, "parts");
        d.flowJson = text(gr, "flow");
        d.correctivesJson = text(gr, "correctives");
        d.sculptsJson = text(gr, "sculpts");
        d.scalp = frame.scalp;
        d.maps = groomlab::ScalpMaps::fromJson(text(gr, "scalpMaps")).catalog();
        file.framesPerSecond = fps;
        file.units = "cm";
        file.upAxis = "Y";
        file.source = "Lila";
        try
        {
            groomlab::writeGroomFile(path, file);
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string("exportGroom: ") + e.what());
        }
        out["strands"] = frame.strands ? frame.strands->strandCount() : 0;
        return out;
    }
    std::size_t strands = 0;
    try
    {
        if (ext == ".abc")
        {
            groomlab::EngineExportOptions o;
            const std::string engine = a.get<std::string>("engine", "unreal");
            o.engine = engine == "unity" ? groomlab::EngineExportOptions::Engine::Unity : groomlab::EngineExportOptions::Engine::Unreal;
            o.groups = a.get<bool>("groups", false) ? std::string(groomlab::kGroupIdChannel) : std::string();
            o.upAxis = "Y";
            o.centimetersPerUnit = 1.0;
            if (const std::string problem = o.problem(); !problem.empty())
                throw CommandError("exportGroom: " + problem);
            const bool guides = a.get<bool>("guides", true) && o.engine == groomlab::EngineExportOptions::Engine::Unreal;
            groomlab::EngineGroomWriter writer(path, exportName(path), frames.front() / fps, (frames.size() > 1 ? frames[1] - frames[0] : 1.0) / fps, o);
            for (double f : frames)
            {
                const GroomExportFrame frame = groomExportFrame(g, gr.id(), f / fps);
                writer.writeFrame(*frame.strands, guides ? &frame.guides : nullptr, frame.scalp.get());
                strands = frame.strands->strandCount();
                out["guides"] = guides ? frame.guides.strandCount() : 0;
            }
            writer.close();
        }
        else if (ext == ".usd" || ext == ".usda" || ext == ".usdc")
        {
            groomlab::UsdCurvesWriter::Options o;
            o.framesPerSecond = fps;
            o.metersPerUnit = 0.01;
            o.upAxis = "Y";
            o.comment = "Lila";
            groomlab::UsdCurvesWriter writer(path, exportName(path), o);
            for (double f : frames)
            {
                const GroomExportFrame frame = groomExportFrame(g, gr.id(), f / fps);
                writer.writeFrame(f, *frame.strands, frame.scalp.get());
                strands = frame.strands->strandCount();
            }
            writer.close();
        }
        else
            throw CommandError("exportGroom: " + ext + " is not a groom format: .abc (Unreal or Unity), .usd, .usda, .usdc or .groom");
    }
    catch (const CommandError&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("exportGroom: ") + e.what());
    }
    out["strands"] = strands;
    return out;
}

/** A GroomLab scalp as a Lila mesh: its points and triangles. */
geo::Mesh meshOf(const groomlab::ScalpMesh& scalp)
{
    geo::Mesh m;
    std::vector<geo::Index> ids;
    ids.reserve(scalp.points().size());
    for (const groomlab::Vec3f& p : scalp.points())
        ids.push_back(m.addPoint(glm::dvec3(p.x, p.y, p.z)));
    for (const groomlab::ScalpMesh::Triangle& t : scalp.triangles())
        m.addFace({ids[size_t(t[0])], ids[size_t(t[1])], ids[size_t(t[2])]});
    return m;
}

/** The graph's Cards node, one added after the hair (before the output) when it has none. */
std::string cardsNode(json& graph)
{
    for (const json& n : graph["nodes"])
        if (n.value("type", std::string()) == "Cards")
            return n["name"];
    json& connections = graph["connections"];
    std::string from;
    for (json& c : connections)
        if (c["to"] == graph.value("output", std::string("output")))
        {
            from = c["from"];
            c["from"] = "cards";
        }
    if (from.empty())
        throw CommandError("groomCards: the graph has no output to put cards before");
    graph["nodes"].push_back({{"name", "cards"}, {"type", "Cards"}});
    connections.push_back({{"from", from}, {"to", "cards"}, {"input", "strands"}});
    return "cards";
}

json paramJson(const groomlab::ParamValue& v)
{
    if (const auto* b = std::get_if<bool>(&v))
        return *b;
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return *i;
    if (const auto* d = std::get_if<double>(&v))
        return *d;
    if (const auto* s = std::get_if<std::string>(&v))
        return *s;
    return nullptr;
}

json cardsPreset(const Args& a)
{
    Graph& g = Graph::current();
    Node& gr = groomArg(g, a, "groomCards");
    const std::string stored = text(gr, "graph");
    json graph = json::parse(stored.empty() ? groomGraph::defaultGraph() : stored);
    const std::string node = cardsNode(graph);
    const std::string lod = a.get<std::string>("lod", "mid");
    std::vector<std::pair<std::string, groomlab::ParamValue>> params;
    try
    {
        const std::string kind = groomlab::cardNodeKind(graph.dump(), node);
        params = groomlab::cardPresetParams(lod, kind.empty() ? "hair" : kind,
                                            a.has("triangles") ? std::optional<int>(int(a.get<int64_t>("triangles"))) : std::nullopt);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("groomCards: ") + e.what());
    }
    for (json& n : graph["nodes"])
        if (n["name"] == node)
            for (const auto& [name, value] : params)
                if (json j = paramJson(value); !j.is_null())
                    n["params"][name] = j;
    g.setValue({gr.id(), gr.type().attrIndex("graph"), -1}, graph.dump());
    // The cards show on a mesh of their own, made the first time.
    const PlugRef out{gr.id(), gr.type().attrIndex("outCards"), -1};
    std::string mesh;
    if (const std::vector<PlugRef> to = g.destinations(out); !to.empty())
        mesh = g.node(to.front().node)->path();
    else
    {
        std::string base = gr.name();
        if (base.size() > 5 && base.compare(base.size() - 5, 5, "Groom") == 0)
            base.resize(base.size() - 5);
        Node* m = g.createNode(*types::find("mesh"), base + "Cards");
        g.connect(out, {m->id(), MeshNode::kInMesh, -1});
        mesh = m->path();
    }
    return {{"node", node}, {"lod", lod}, {"mesh", mesh}};
}

json bakeCards(const Args& a)
{
    Graph& g = Graph::current();
    Node& gr = groomArg(g, a, "groomBakeCards");
    const std::string folder = a.get<std::string>("folder", "");
    if (folder.empty())
        throw CommandError("groomBakeCards: give a folder");
    const std::string name = a.get<std::string>("name", gr.name());
    const GroomExportFrame frame = groomExportFrame(g, gr.id(), g.time());
    std::string stamp;
    groomlab::CardBakePaths at = groomlab::CardBakePaths::in(folder, name);
    try
    {
        const groomlab::CardBake bake = groomlab::bakeCardsInMemory(frame.cardStrands ? frame.cardStrands : frame.strands, frame.scalp, int(a.get<int64_t>("resolution", 0)),
                                                                    float(a.get<double>("occlusion", 1.0)));
        stamp = groomlab::writeCardBake(bake, at, true);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("groomBakeCards: ") + e.what());
    }
    const std::string maps = groomlab::formatCardMaps(at.prefix, stamp, 1);
    g.setValue({gr.id(), gr.type().attrIndex("cardMaps"), -1}, maps);
    json files = json::array();
    for (const std::string& m : groomlab::cardMapNames())
        files.push_back(at.map(m));
    return {{"prefix", at.prefix}, {"maps", files}};
}

json addMap(const Args& a)
{
    Graph& g = Graph::current();
    Node& gr = groomArg(g, a, "groomAddMap");
    const std::string name = groomlab::sanitizeMapName(a.get<std::string>("name", "mask"));
    const std::string kind = a.get<std::string>("kind", "vertices");
    groomlab::ScalpMaps maps;
    try
    {
        maps = groomlab::ScalpMaps::fromJson(text(gr, "scalpMaps"));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("groomAddMap: ") + e.what());
    }
    if (maps.maps.count(name))
        throw CommandError("groomAddMap: " + gr.name() + " already has a map named " + name);
    groomlab::ScalpMapEntry e;
    e.kind = kind;
    e.channels = a.get<bool>("color", false) ? 4 : 1;
    if (kind == "texture")
    {
        e.texture = a.get<std::string>("file", "");
        if (e.texture.empty())
            throw CommandError("groomAddMap: a texture map needs a file (a PNG painting writes)");
    }
    else
    {
        const GroomExportFrame frame = groomExportFrame(g, gr.id(), g.time());
        e.values.assign(frame.scalp ? frame.scalp->points().size() * size_t(e.channels) : 0, float(a.get<double>("value", 0.0)));
    }
    maps.maps[name] = e;
    g.setValue({gr.id(), gr.type().attrIndex("scalpMaps"), -1}, maps.toJson());
    return {{"name", name}, {"kind", kind}};
}

json importFile(const Args& a)
{
    Graph& g = Graph::current();
    if (a.positional.empty())
        throw CommandError("importGroom: give a .groom file");
    const std::string path = a.positional[0].get<std::string>();
    groomlab::GroomFile file;
    try
    {
        file = groomlab::readGroomFile(path);
        if (file.upAxis != "Y")
            groomlab::convertUpAxis(file, "Y");
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("importGroom: ") + e.what());
    }
    if (file.units != "cm")
        log::warn("importGroom: {} is in {}, read as cm", path, file.units);
    Node* scalp = nullptr;
    if (a.has("mesh"))
    {
        scalp = g.find(a.get<std::string>("mesh"));
        if (!scalp || !scalp->type().isA("mesh"))
            throw CommandError("importGroom: no mesh " + a.get<std::string>("mesh"));
    }
    else
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
            {
                scalp = n;
                break;
            }
    const std::string base = a.get<std::string>("name", exportName(path));
    if (!scalp)
    {
        if (!file.data.scalp || file.data.scalp->points().empty())
            throw CommandError("importGroom: the file holds no scalp; give the mesh it grows on");
        scalp = g.createNode(*types::find("mesh"), base + "Scalp");
        g.setValue({scalp->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(meshOf(*file.data.scalp))));
    }
    const json made = ops::execute("createGroom", {{"args", {scalp->path()}}, {"strands", a.get<bool>("strands", true)}, {"name", base}});
    Node* gr = g.find(made.value("groom", std::string()));
    if (!gr)
        throw CommandError("importGroom: the groom was not made");
    auto set = [&](const char* attr, const std::string& value) {
        if (!value.empty())
            g.setValue({gr->id(), gr->type().attrIndex(attr), -1}, value);
    };
    set("hierarchy", file.data.hierarchyJson);
    set("graph", file.data.graphJson);
    set("parts", file.data.partsJson);
    set("flow", file.data.flowJson);
    set("correctives", file.data.correctivesJson);
    set("sculpts", file.data.sculptsJson);
    const groomlab::ScalpMaps maps = groomlab::scalpMapsFromGroomData(file.data);
    if (!maps.maps.empty())
        set("scalpMaps", maps.toJson());
    json out = made;
    out["scalp"] = scalp->path();
    return out;
}
} // namespace

void registerIoCommands()
{
    yeti::registerCommands();
    yeti::registerCommands();
    ops::registerCommand({
        .name = "exportGroom",
        .doc = "Writes a groom's strands to a file by its extension: .abc for Unreal (groom_* attributes, its primary guides to "
               "simulate) or Unity, .usd/.usda/.usdc curves, or .groom (GroomLab's file: guides, graph, parts, flow, maps and the "
               "scalp). Frames start to end (scene frames; the current one by default). Returns {file, frames, strands}.",
        .positionalDoc = "groom",
        .flags = {{"path", "p", T::String, nullptr, "The file to write."},
                  {"engine", "e", T::String, "unreal", "For .abc: the engine whose hair importer reads it.", {"unreal", "unity"}},
                  {"start", "s", T::Float, nullptr, "First frame; the current frame when left out."},
                  {"end", "", T::Float, nullptr, "Last frame; start when left out."},
                  {"step", "", T::Float, 1.0, "Frames between written frames.", {}, {}, 0.001},
                  {"guides", "", T::Bool, true, "Unreal: write the primary guides it simulates."},
                  {"groups", "", T::Bool, false, "One groom group per groupId (the Attribute node's groups)."}},
        .run = exportFile,
    });
    ops::registerCommand({
        .name = "importGroom",
        .doc = "Reads a GroomLab .groom file into a new groom: its guides, graph, parts, flow, maps and sculpts, on the mesh given or "
               "selected, else on the file's own scalp as a new mesh. Returns {groom, guides, locks, strands, scalp}.",
        .positionalDoc = "file",
        .flags = {{"mesh", "m", T::String, nullptr, "The scalp mesh to grow on; the selected mesh, else the file's scalp, when left out."},
                  {"strands", "", T::Bool, true, "Also a curve showing the strands."},
                  {"name", "n", T::String, nullptr, "Base name of the new nodes; the file's name when left out."}},
        .run = importFile,
    });
    ops::registerCommand({
        .name = "groomAddMap",
        .doc = "Adds a map to paint on the groom's scalp for Map nodes and map params to read: kind vertices (a value per scalp "
               "vertex) or texture (an image at the scalp's uv0, written to file as a PNG when painted). Paint it with Groom Brushes' "
               "Paint, Paint On Scalp or Texture. The head's color sets are maps already. Returns {name, kind}.",
        .positionalDoc = "groom",
        .flags = {{"name", "n", T::String, "mask", "The map's name."},
                  {"kind", "k", T::String, "vertices", "Where it lives.", {"vertices", "texture"}},
                  {"file", "f", T::String, nullptr, "texture: the image file it paints into."},
                  {"color", "", T::Bool, false, "A color map (RGBA) instead of gray."},
                  {"value", "v", T::Float, 0.0, "vertices: the value it starts at.", {}, {}, 0.0, 1.0}},
        .run = addMap,
    });
    ops::registerCommand({
        .name = "groomCards",
        .doc = "Gives the groom a Cards node (after the hair) set from a level-of-detail preset: hero, mid or low, its card count "
               "from the preset's triangle budget or triangles, and a mesh showing the cards (made once, fed by outCards). Returns {node, lod, mesh}.",
        .positionalDoc = "groom",
        .flags = {{"lod", "l", T::String, "mid", "The preset.", {"hero", "mid", "low"}},
                  {"triangles", "t", T::Int, nullptr, "The triangle budget instead of the preset's.", {}, {}, 1.0}},
        .run = cardsPreset,
    });
    ops::registerCommand({
        .name = "groomBakeCards",
        .doc = "Bakes the groom's cards into texture maps (Depth, Coverage, Tangent, Attribute, Color, AO, Normal as .tga), the "
               "atlas layout and a preview, in folder as <name>_<Map>.tga; the cards keep their atlas cells from it after. "
               "Needs a Cards node (groomCards). Returns {prefix, maps}.",
        .positionalDoc = "groom",
        .flags = {{"folder", "f", T::String, nullptr, "Folder to write into."},
                  {"name", "n", T::String, nullptr, "File name prefix; the groom's name when left out."},
                  {"resolution", "r", T::Int, 0, "Map size in pixels; 0: the Cards node's atlas size.", {}, {}, 0.0, 8192.0},
                  {"occlusion", "", T::Float, 1.0, "Ambient occlusion strength, 0 to 1.", {}, {}, 0.0, 1.0}},
        .run = bakeCards,
    });
}
} // namespace lila::groom
