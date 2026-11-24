#include "tex/GraphFiles.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/GraphProfile.h"
#include "tex/NodeDef.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace lila::tex
{
namespace
{
namespace fs = std::filesystem;

constexpr const char* kMaterialFormat = "lilamat";

std::string lowerExt(const fs::path& p)
{
    std::string e = p.extension().string();
    for (char& c : e)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

json readJson(const fs::path& p, const char* cmd)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw CommandError(std::string(cmd) + ": can't read " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    try
    {
        return json::parse(ss.str());
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + p.string() + " isn't JSON: " + e.what());
    }
}

void writeJson(const fs::path& p, const json& j, const char* cmd)
{
    std::error_code ec;
    if (p.has_parent_path())
        fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out)
        throw CommandError(std::string(cmd) + ": can't write " + p.string());
    out << j.dump(2) << "\n";
}

/** name made a valid identifier no scene node has yet. */
std::string freeName(std::string name)
{
    std::string clean;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            clean += c;
    if (clean.empty() || std::isdigit(static_cast<unsigned char>(clean.front())))
        clean = "Material" + clean;
    std::string out = clean;
    for (int i = 1; !scene::Graph::current().findAll(out).empty(); ++i)
        out = clean + std::to_string(i);
    return out;
}
} // namespace

void registerFileCommands()
{
    registerGroupCommands();
    registerParamCommands();
    registerProfileCommands();
    using T = ArgType;
    ops::registerCommand({
        .name = "texExportGraph",
        .doc = "Write a texture set to a file: .lilamat holds its graph and settings (import it to make the set again); "
               ".lilatexnodes holds its node groups as graph nodes for the node library, or only the one named as \"Set/Group\". "
               "Returns the path.",
        .positionalDoc = "set path",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
                throw CommandError("texExportGraph: set and path required");
            const GraphAddress at = splitAddress(a.positional[0].get<std::string>());
            scene::Node* n = findSet(scene::Graph::current(), at.set);
            if (!n)
                throw CommandError("texExportGraph: no texture set named '" + at.set + "'");
            fs::path path = paths::fromUtf8(a.positional[1].get<std::string>());
            Graph g = readGraph(*n);
            std::string ext = lowerExt(path);
            if (!at.group.empty() && !g.subgroups.contains(at.group))
                throw CommandError("texExportGraph: " + at.set + " has no node group named '" + at.group + "'");
            if (!at.group.empty() && ext != ".lilatexnodes" && ext != ".lilatexnode")
                throw CommandError("texExportGraph: a node group exports as .lilatexnodes; .lilamat holds a whole set");
            if (ext == ".lilatexnodes" || ext == ".lilatexnode")
            {
                json nodes = json::array();
                for (const auto& [name, sub] : g.subgroups)
                {
                    if (!at.group.empty() && name != at.group)
                        continue;
                    json def = {{"type", name}, {"label", sub.value("name", name)}, {"category", "Graphs"}, {"kind", "graph"}, {"graph", sub}};
                    if (sub.contains("description"))
                        def["doc"] = sub["description"];
                    try
                    {
                        NodeDef::fromJson(def);
                    }
                    catch (const std::exception& e)
                    {
                        throw CommandError("texExportGraph: group " + name + ": " + e.what());
                    }
                    nodes.push_back(def);
                }
                if (nodes.empty())
                    throw CommandError("texExportGraph: " + n->name() + " has no node groups to put in a library; export it as .lilamat");
                writeJson(path, {{"format", "lilatexnode"}, {"version", 1}, {"nodes", nodes}}, "texExportGraph");
            }
            else
                writeJson(path, {{"format", kMaterialFormat}, {"version", 1}, {"name", n->name()}, {"graph", g.toJson()}, {"settings", readSettings(*n).toJson()}},
                          "texExportGraph");
            return a.positional[1];
        },
    });

    ops::registerCommand({
        .name = "texImportGraph",
        .doc = "Read a texture graph file. A .lilamat becomes a new texture set (named after the file's set, made unique); a "
               ".lilatexnode(s) library is copied into the user's node folder and its nodes join the library. Returns the new "
               "set names.",
        .positionalDoc = "path",
        .flags = {{"name", "", T::String, nullptr, ".lilamat: the new set's name instead of the file's."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("texImportGraph: path required");
            fs::path path = paths::fromUtf8(a.positional[0].get<std::string>());
            std::string ext = lowerExt(path);
            if (ext == ".lilatexnodes" || ext == ".lilatexnode")
            {
                std::vector<NodeDef> defs;
                try
                {
                    defs = readNodeFile(path);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("texImportGraph: ") + e.what());
                }
                fs::path dir = nodeDirs().at(1);
                std::error_code ec;
                fs::create_directories(dir, ec);
                fs::copy_file(path, dir / path.filename(), fs::copy_options::overwrite_existing, ec);
                if (ec)
                    throw CommandError("texImportGraph: can't copy into " + dir.string() + ": " + ec.message());
                reloadLibrary();
                for (const NodeDef& d : defs)
                    log::info("texture node '{}' added to the library", d.type);
                return json::array();
            }
            json j = readJson(path, "texImportGraph");
            if (j.value("format", "") != kMaterialFormat || !j.contains("graph"))
                throw CommandError("texImportGraph: " + path.string() + " isn't a .lilamat file");
            Graph g;
            SetSettings s = SetSettings::makeDefault();
            try
            {
                g = Graph::fromJson(j["graph"]);
                if (j.contains("settings"))
                    s = SetSettings::fromJson(j["settings"]);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texImportGraph: ") + e.what());
            }
            std::string name = freeName(a.get<std::string>("name", j.value("name", path.stem().string())));
            scene::Graph& sg = scene::Graph::current();
            scene::Node* n = sg.createNode(*scene::types::find(kTextureSetType), name, scene::kNoNode, false);
            g.name = n->name();
            writeGraph(sg, *n, g);
            writeSettings(sg, *n, s);
            return json::array({n->name()});
        },
    });
}
} // namespace lila::tex
