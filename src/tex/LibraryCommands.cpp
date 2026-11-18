#include "tex/Library.h"

#include "core/Paths.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "tex/Layers.h"
#include "tex/Lint.h"
#include "uv/Metrics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace lila::tex::library
{
namespace
{
namespace fs = std::filesystem;
using T = ArgType;
using scene::Node;
using scene::NodeId;

scene::Graph& graph() { return scene::Graph::current(); }

std::string positionalString(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

/** name made a valid identifier no scene node has yet. */
std::string freeName(const std::string& name, const std::string& fallback)
{
    std::string clean;
    bool upper = false;
    for (char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        {
            clean += upper ? char(std::toupper(static_cast<unsigned char>(c))) : c;
            upper = false;
        }
        else
            upper = !clean.empty();
    }
    if (clean.empty() || std::isdigit(static_cast<unsigned char>(clean.front())))
        clean = fallback + clean;
    std::string out = clean;
    for (int i = 1; !graph().findAll(out).empty(); ++i)
        out = clean + std::to_string(i);
    return out;
}

/** A library id, or a path to a .lilamat file. */
std::pair<json, std::optional<Entry>> loadMaterial(const std::string& idOrPath, const char* cmd)
{
    if (std::optional<Entry> e = find(idOrPath))
    {
        try
        {
            return {document(*e), e};
        }
        catch (const std::exception& ex)
        {
            throw CommandError(std::string(cmd) + ": " + ex.what());
        }
    }
    fs::path p = paths::fromUtf8(idOrPath);
    std::error_code ec;
    if (p.extension() == ".lilamat" && fs::is_regular_file(p, ec))
    {
        std::ifstream in(p, std::ios::binary);
        try
        {
            json doc = json::parse(in);
            if (doc.value("format", "") != "lilamat")
                throw std::invalid_argument("not a .lilamat file");
            return {doc, std::nullopt};
        }
        catch (const std::exception& ex)
        {
            throw CommandError(std::string(cmd) + ": " + idOrPath + ": " + ex.what());
        }
    }
    throw CommandError(std::string(cmd) + ": no library material '" + idOrPath + "' (materialLibrary lists them)");
}

/** The target meshes and their faces, as assignMaterial picks them: faces= of one object, the face selection, else objects. */
std::map<NodeId, std::vector<uint32_t>> targets(scene::Graph& g, const Args& a, const char* cmd)
{
    std::map<NodeId, std::vector<uint32_t>> out;
    std::vector<std::string> named;
    for (size_t i = 1; i < a.positional.size(); ++i)
    {
        const json& v = a.positional[i];
        if (v.is_string())
            named.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    named.push_back(x.get<std::string>());
    }
    if (a.has("faces"))
    {
        if (named.size() != 1)
            throw CommandError(std::string(cmd) + ": faces= needs exactly one object");
        Node* n = g.find(named[0]);
        if (!n || !n->type().isA("mesh"))
            throw CommandError(std::string(cmd) + ": " + named[0] + " is not a mesh");
        for (const json& f : a.flags["faces"])
            out[n->id()].push_back(f.get<uint32_t>());
        return out;
    }
    if (named.empty() && scene::components::mode() == scene::ComponentMode::Face)
        for (const auto& [node, ids] : scene::components::selected(scene::ComponentMode::Face))
            if (Node* n = g.node(node); n && n->type().isA("mesh") && !ids.empty())
                out[node] = ids;
    if (!out.empty())
        return out;
    if (named.empty())
        for (NodeId id : g.selection())
            named.push_back(g.node(id)->path());
    for (const std::string& name : named)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no object '" + name + "'");
        if (n->type().isA("mesh"))
            out[n->id()];
    }
    return out;
}

scene::MeshPtr meshOf(Node& n) { return n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh); }

/** UV units per cm of world surface over the targets, area weighted; 0 without uvs. */
double uvPerCm(scene::Graph& g, const std::map<NodeId, std::vector<uint32_t>>& tgts)
{
    double sum = 0.0, weight = 0.0;
    for (const auto& [id, faces] : tgts)
    {
        Node* n = g.node(id);
        scene::MeshPtr m = n ? meshOf(*n) : nullptr;
        if (!m)
            continue;
        glm::dmat3 lin(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        double d = uv::texelDensity(*m, geo::attr::uvName(0), faces, 1.0, lin);
        if (d <= 0.0)
            continue;
        double w = faces.empty() ? double(m->numFaces()) : double(faces.size());
        sum += d * w;
        weight += w;
    }
    return weight > 0.0 ? sum / weight : 0.0;
}

/** Meshes (and their faces) whose slots bind to the material, for a set applied without naming targets. */
std::map<NodeId, std::vector<uint32_t>> facesUsing(scene::Graph& g, const std::string& material)
{
    std::map<NodeId, std::vector<uint32_t>> out;
    for (NodeId nid : g.nodes())
    {
        Node* n = g.node(nid);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr m = meshOf(*n);
        if (!m)
            continue;
        const auto& names = m->materialNames();
        auto it = std::find(names.begin(), names.end(), material);
        if (it == names.end())
            continue;
        const int32_t slot = int32_t(it - names.begin());
        std::span<const int32_t> ids = m->faceAttrs().get<int32_t>(geo::attr::kMaterial);
        std::vector<uint32_t>& faces = out[n->id()];
        for (uint32_t f = 0; f < m->numFaces(); ++f)
            if ((ids.empty() ? 0 : ids[f]) == slot)
                faces.push_back(f);
        if (faces.empty())
            out.erase(n->id());
    }
    return out;
}

/** The texture set the material uses; null for none. */
Node* setFor(scene::Graph& g, const std::string& material)
{
    const std::string set = setForMaterial(g, material);
    return set.empty() ? nullptr : findSet(g, set);
}

/** Takes out the tiling applyLibraryMaterial added, so a saved material carries no mesh's scale. */
void removeTiling(Graph& g)
{
    if (!g.node(kTilingNode))
        return;
    std::vector<GLink> in = g.linksInto(kTilingNode, "input");
    std::vector<GLink> outs;
    for (const GLink& l : g.links)
        if (l.fromNode == kTilingNode)
            outs.push_back(l);
    std::erase_if(g.links, [](const GLink& l) { return l.fromNode == kTilingNode || l.toNode == kTilingNode; });
    std::erase_if(g.nodes, [](const GNode& n) { return n.name == kTilingNode; });
    if (!in.empty())
        for (const GLink& o : outs)
            g.links.push_back({in.front().fromNode, in.front().fromSocket, o.toNode, o.toSocket});
}

json entryList(const std::vector<Entry>& es)
{
    json out = json::array();
    for (const Entry& e : es)
        out.push_back(e.toJson());
    return out;
}

Meta metaFromFlags(Meta m, const Args& a)
{
    for (auto [k, v] : {std::pair{"name", &m.name}, {"category", &m.category}, {"physical", &m.physical}, {"seedParam", &m.seedParam}, {"author", &m.author},
                        {"licence", &m.licence}, {"version", &m.version}, {"description", &m.description}})
        if (a.has(k))
            *v = a.get<std::string>(k);
    if (a.has("tags"))
        m.tags = a.flags["tags"].get<std::vector<std::string>>();
    if (a.has("variationParams"))
        m.variationParams = a.flags["variationParams"].get<std::vector<std::string>>();
    if (a.has("scaleCm"))
        m.scaleCm = a.get<double>("scaleCm");
    if (a.has("tileable"))
        m.tileable = a.get<bool>("tileable");
    return m;
}

/** A lint target: a scene texture set, a library id or a .lilamat path. */
struct LintTarget
{
    Node* set = nullptr;
    json doc;
    std::optional<Entry> entry;
    fs::path file;
    Meta meta;
};

LintTarget lintTarget(const std::string& name)
{
    LintTarget t;
    if (Node* n = findSet(graph(), name))
    {
        t.set = n;
        // A set made from the library keeps its material's meta when it's still in the library under the set's name.
        t.meta.name = n->name();
        if (std::optional<Entry> e = find(n->name()))
            t.meta = e->meta;
        return t;
    }
    auto [doc, entry] = loadMaterial(name, "materialLint");
    t.doc = doc;
    t.entry = entry;
    t.meta = entry ? entry->meta : Meta::fromJson(doc.value("meta", json::object()));
    if (entry)
        t.file = entry->file;
    else
        t.file = paths::fromUtf8(name);
    return t;
}

json lintOne(LintTarget& t, const Args& a)
{
    lint::Options o{.size = a.get<int>("size", 256), .backend = a.get<std::string>("backend", "")};
    if (a.has("physical"))
        t.meta.physical = a.get<std::string>("physical");
    Graph g;
    SetSettings s;
    try
    {
        g = t.set ? readGraph(*t.set) : graphOf(t.doc);
        s = t.set ? readSettings(*t.set) : settingsOf(t.doc);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("materialLint: ") + e.what());
    }
    lint::Report r;
    try
    {
        r = lint::run(g, s, t.meta, o);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("materialLint: ") + e.what());
    }
    json out = r.toJson();
    if (!a.get<bool>("fix", false) || (r.count("fail") == 0 && r.count("warn") == 0))
        return out;
    std::vector<std::string> changes = lint::fix(g, r, t.meta);
    out["fixed"] = changes;
    if (t.set)
        writeGraph(graph(), *t.set, g);
    else
    {
        if (t.entry && t.entry->tier != "user" && t.entry->tier != "workspace")
            throw CommandError("materialLint: " + t.entry->id + " is a " + t.entry->tier + " material; save a copy (saveLibraryMaterial) to fix it");
        t.doc["graph"] = g.toJson();
        std::ofstream f(t.file, std::ios::binary);
        if (!f)
            throw CommandError("materialLint: can't write " + paths::toUtf8(t.file));
        f << t.doc.dump(2) << "\n";
        reload();
    }
    try
    {
        out["after"] = lint::run(g, s, t.meta, o).toJson();
    }
    catch (const std::exception& e)
    {
        out["after"] = {{"error", e.what()}};
    }
    return out;
}
} // namespace

void registerCommands()
{
    ops::registerCommand({
        .name = "materialLibrary",
        .doc = "List or search the material library (built-in, extension packs, your materials, the workspace's and the scene's "
               "texture sets). Returns [{id, name, category, tags, physical, scaleCm, tileable, tier, file, favourite, ...}]; "
               "with id=, that material's full document and presets; with categories=True, {categories, tags}.",
        .flags = {{"search", "s", T::String, nullptr, "Words that must all appear in the name, category, tags or description."},
                  {"category", "", T::String, nullptr, "A category or a parent of it (Metal matches Metal/Steel)."},
                  {"tag", "", T::String, nullptr, "Only materials with this tag."},
                  {"physical", "", T::String, nullptr, "metal, dielectric, organic, emissive or stylised.",
                   {"metal", "dielectric", "organic", "emissive", "stylised"}},
                  {"tier", "", T::String, nullptr, "builtin, extension, user, workspace or scene.", {"builtin", "extension", "user", "workspace", "scene"}},
                  {"favourites", "", T::Bool, nullptr, "Only favourites."},
                  {"id", "", T::String, nullptr, "One material: its document and presets."},
                  {"categories", "", T::Bool, nullptr, "Return the category and tag lists instead."},
                  {"reload", "", T::Bool, nullptr, "Rescan the folders first."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("reload", false))
                reload();
            if (a.get<bool>("categories", false))
                return {{"categories", categories()}, {"tags", tags()}, {"errors", errors()}};
            if (a.has("id"))
            {
                std::string id = a.get<std::string>("id");
                std::optional<Entry> e = find(id);
                if (!e)
                    throw CommandError("materialLibrary: no material '" + id + "'");
                json j = e->toJson();
                try
                {
                    j["document"] = document(*e);
                }
                catch (const std::exception& ex)
                {
                    throw CommandError(std::string("materialLibrary: ") + ex.what());
                }
                j["presets"] = presets(id);
                return j;
            }
            Query q{.text = a.get<std::string>("search", ""),
                    .category = a.get<std::string>("category", ""),
                    .tag = a.get<std::string>("tag", ""),
                    .physical = a.get<std::string>("physical", ""),
                    .tier = a.get<std::string>("tier", ""),
                    .favouritesOnly = a.get<bool>("favourites", false)};
            return entryList(search(q));
        },
    });

    ops::registerCommand({
        .name = "saveLibraryMaterial",
        .doc = "Save a texture set to the material library as a .lilamat with its metadata, in your materials (tier='user') or the "
               "workspace's. The tiling applyLibraryMaterial added is left out. Returns {id, file, tier}.",
        .positionalDoc = "set",
        .flags = {{"tier", "", T::String, "user", "user or workspace.", {"user", "workspace"}},
                  {"id", "", T::String, nullptr, "Path under the tier folder without extension (Metal/myRust); default category/name."},
                  {"path", "", T::String, nullptr, "Write here instead of a tier folder."},
                  {"name", "", T::String, nullptr, "Display name (default the set's)."},
                  {"category", "", T::String, nullptr, "Category path, Metal/Steel."},
                  {"tags", "", T::List, nullptr, "Search tags."},
                  {"physical", "", T::String, nullptr, "metal, dielectric, organic, emissive or stylised.",
                   {"metal", "dielectric", "organic", "emissive", "stylised"}},
                  {"scaleCm", "", T::Float, nullptr, "Real-world size of one tile, cm.", {}, {}, 0.0},
                  {"tileable", "", T::Bool, nullptr, "Whether the texture tiles."},
                  {"seedParam", "", T::String, nullptr, "Node.param of the seed."},
                  {"variationParams", "", T::List, nullptr, "Node.param values worth varying (wear, age, colour)."},
                  {"author", "", T::String, nullptr, ""},
                  {"licence", "", T::String, nullptr, ""},
                  {"version", "", T::String, nullptr, ""},
                  {"description", "", T::String, nullptr, ""},
                  {"overwrite", "", T::Bool, true, "Replace an existing file."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string setName = positionalString(a, 0, "saveLibraryMaterial", "texture set");
            Node* n = findSet(graph(), setName);
            if (!n)
                throw CommandError("saveLibraryMaterial: no texture set named '" + setName + "'");
            Meta base;
            if (std::optional<Entry> e = find(setName); e && e->tier != "scene")
                base = e->meta;
            if (base.name.empty())
                base.name = setName;
            Meta m = metaFromFlags(base, a);
            Graph g = readGraph(*n);
            removeTiling(g);
            std::string tier = a.get<std::string>("tier", "user");
            fs::path file;
            std::string id;
            if (a.has("path"))
                file = paths::fromUtf8(a.get<std::string>("path"));
            else
            {
                fs::path dir = tierDir(tier);
                if (dir.empty())
                    throw CommandError("saveLibraryMaterial: no " + tier + " material folder (open a workspace for tier='workspace')");
                id = a.get<std::string>("id", "");
                if (id.empty())
                    id = (m.category.empty() ? std::string() : m.category + "/") + freeName(m.name, "Material");
                file = dir / paths::fromUtf8(id + ".lilamat");
            }
            std::error_code ec;
            if (fs::exists(file, ec) && !a.get<bool>("overwrite", true))
                throw CommandError("saveLibraryMaterial: " + paths::toUtf8(file) + " exists");
            fs::create_directories(file.parent_path(), ec);
            json doc = {{"format", "lilamat"}, {"version", 1}, {"name", m.name}, {"meta", m.toJson()}, {"graph", g.toJson()},
                        {"settings", readSettings(*n).toJson()}};
            std::ofstream f(file, std::ios::binary);
            if (!f)
                throw CommandError("saveLibraryMaterial: can't write " + paths::toUtf8(file));
            f << doc.dump(2) << "\n";
            f.close();
            reload();
            return {{"id", id}, {"file", paths::toUtf8(file)}, {"tier", a.has("path") ? "" : tier}};
        },
    });

    ops::registerCommand({
        .name = "applyLibraryMaterial",
        .doc = "Put a library material on meshes: faces= of one object, the face selection, else whole objects (default the "
               "selection). Faces get a new material slot; whole objects keep their one bound slot or get a new one. The slot's "
               "texture set takes the material's graph, tiled so one tile spans the material's scaleCm on the surface (from the "
               "meshes' uv texel density). With set= and no objects, replaces that set's graph. Returns {set, material, tiles, uvPerCm}.",
        .positionalDoc = "material [objects...]",
        .flags = {{"faces", "f", T::List, nullptr, "Face ids of the one object given."},
                  {"slot", "", T::String, nullptr, "Bind to this existing material slot instead of picking one."},
                  {"set", "", T::String, nullptr, "Apply to this texture set (and the meshes using its material)."},
                  {"preset", "", T::String, nullptr, "A preset of the material's parameters to apply."},
                  {"values", "", T::Any, nullptr, "{\"Node.param\": value} to set after the preset."},
                  {"scale", "", T::Bool, true, "Tile to real-world size; False keeps one tile per uv unit."},
                  {"tiles", "", T::Float, nullptr, "Tiles per uv unit, overriding the real-world scale.", {}, {}, 1e-3},
                  {"layer", "", T::Bool, false, "Add it as a layer on top of the set's layer stack instead of replacing the set's graph."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "applyLibraryMaterial";
            std::string id = positionalString(a, 0, cmd, "material");
            auto [doc, entry] = loadMaterial(id, cmd);
            Meta meta = entry ? entry->meta : Meta::fromJson(doc.value("meta", json::object()));
            if (meta.name.empty())
                meta.name = doc.value("name", "Material");
            Graph lib;
            SetSettings libSettings;
            try
            {
                lib = graphOf(doc);
                libSettings = settingsOf(doc);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }

            scene::Graph& g = graph();
            g.evaluate();
            UndoChunk chunk("Apply Library Material");
            std::map<NodeId, std::vector<uint32_t>> tgts;
            std::string material;
            Node* set = nullptr;
            if (a.has("set"))
            {
                set = findSet(g, a.get<std::string>("set"));
                if (!set)
                    throw CommandError(std::string(cmd) + ": no texture set named '" + a.get<std::string>("set") + "'");
                const std::vector<std::string> users = materialsUsing(g, set->name());
                if (a.positional.size() <= 1 && users.empty())
                    throw CommandError(std::string(cmd) + ": no material uses " + set->name() + "; name the meshes, or pick the set in a material's Texture Set");
                material = users.empty() ? std::string() : users.front();
                tgts = a.positional.size() > 1 ? targets(g, a, cmd) : facesUsing(g, material);
            }
            else
            {
                tgts = targets(g, a, cmd);
                if (tgts.empty())
                    throw CommandError(std::string(cmd) + ": select meshes or faces, or name them");
                bool faces = std::any_of(tgts.begin(), tgts.end(), [](const auto& t) { return !t.second.empty(); });
                if (a.has("slot"))
                    material = a.get<std::string>("slot");
                else if (!faces)
                {
                    // Whole objects keep the one slot they all share when a material node already binds it.
                    std::string shared;
                    for (const auto& [nid, f] : tgts)
                    {
                        scene::MeshPtr m = meshOf(*g.node(nid));
                        std::vector<std::pair<std::string, uint32_t>> used;
                        if (m)
                            used = scene::materials::slotUsage(*m);
                        std::string only = used.size() == 1 ? used.front().first : std::string();
                        if (only.empty() || (!shared.empty() && only != shared))
                        {
                            shared.clear();
                            break;
                        }
                        shared = only;
                    }
                    if (!shared.empty() && scene::materials::find(g, shared))
                        material = shared;
                }
                if (material.empty())
                {
                    material = freeName("M_" + meta.name, "M_Material");
                    ops::execute("createMaterial", {{"args", {material}}});
                    for (const auto& [nid, f] : tgts)
                    {
                        json call = {{"args", {material, g.node(nid)->path()}}};
                        if (!f.empty())
                            call["faces"] = f;
                        ops::execute("assignMaterial", call);
                    }
                }
                else if (!scene::materials::find(g, material))
                    ops::execute("createMaterial", {{"args", {material}}});
                set = setFor(g, material);
            }

            if (a.has("preset"))
            {
                json p = presets(id);
                const std::string name = a.get<std::string>("preset");
                if (!p.contains(name))
                    throw CommandError(std::string(cmd) + ": " + id + " has no preset '" + name + "'");
                applyValues(lib, p[name]);
            }
            std::vector<std::string> missed;
            if (a.has("values"))
                missed = applyValues(lib, a.flags["values"]);

            double density = uvPerCm(g, tgts);
            double tiles = 1.0;
            if (a.has("tiles"))
                tiles = std::max(1e-3, a.get<double>("tiles"));
            else if (a.get<bool>("scale", true) && meta.tileable)
                tiles = tilesFor(meta.scaleCm, density);
            if (std::abs(tiles - 1.0) > 1e-6 || a.has("tiles"))
            {
                try
                {
                    setTiling(lib, tiles, tiles);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string(cmd) + ": " + e.what());
                }
            }

            if (a.get<bool>("layer", false))
            {
                if (!set)
                    throw CommandError(std::string(cmd) + ": layer=True needs set= (or a slot that already has a texture set)");
                json base = json::object();
                Layer layer = graphAsLayer(lib, readSettings(*set), &base);
                json call = {{"name", meta.name}};
                if (Graph::fromJson(layer.graph).outputs.empty())
                    call.update({{"args", {set->name(), "fill"}}, {"material", base}});
                else
                    call.update({{"args", {set->name(), "procedural"}}, {"graph", layer.graph}});
                json added = ops::execute("texAddLayer", call);
                return {{"set", set->name()}, {"material", material}, {"layer", added}, {"tiles", tiles}, {"uvPerCm", density}};
            }

            if (!set)
            {
                std::string name = freeName(meta.name, "Material");
                set = g.createNode(*scene::types::find(kTextureSetType), name, scene::kNoNode, false);
                writeSettings(g, *set, libSettings);
                assignSet(g, set->name(), {material});
            }
            else
            {
                // The set keeps its own resolution and export settings; the material may bring channels it lacks.
                SetSettings s = readSettings(*set);
                for (const Channel& c : libSettings.channels)
                    if (!s.channel(c.name))
                        s.channels.push_back(c);
                writeSettings(g, *set, s);
            }
            lib.name = set->name();
            writeGraph(g, *set, lib);
            json out = {{"set", set->name()}, {"material", material}, {"tiles", tiles}, {"uvPerCm", density}, {"scaleCm", meta.scaleCm}};
            json objs = json::array();
            for (const auto& [nid, f] : tgts)
                objs.push_back(g.node(nid)->path());
            out["objects"] = objs;
            if (!missed.empty())
                out["unmatched"] = missed;
            return out;
        },
    });

    ops::registerCommand({
        .name = "materialFavourite",
        .doc = "Mark a library material as a favourite (on=False to unmark). Returns the favourites.",
        .positionalDoc = "material",
        .flags = {{"on", "", T::Bool, true, "Favourite or not."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string id = positionalString(a, 0, "materialFavourite", "material");
            setFavourite(id, a.get<bool>("on", true));
            return favourites();
        },
    });

    ops::registerCommand({
        .name = "materialPresets",
        .doc = "A library material's parameter presets: {name: {\"Node.param\": value}}. save= stores one from values= or from "
               "set='s current values of the material's seed and variation parameters; delete= removes one of yours.",
        .positionalDoc = "material",
        .flags = {{"save", "", T::String, nullptr, "Preset name to save."},
                  {"values", "", T::Any, nullptr, "{\"Node.param\": value} for save."},
                  {"set", "", T::String, nullptr, "Texture set to capture the values from for save."},
                  {"delete", "", T::String, nullptr, "Preset name to delete."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string id = positionalString(a, 0, "materialPresets", "material");
            if (a.has("save"))
            {
                json values = a.has("values") ? a.flags["values"] : json();
                if (values.is_null())
                {
                    std::optional<Entry> e = find(id);
                    if (!e)
                        throw CommandError("materialPresets: no material '" + id + "'");
                    if (!a.has("set"))
                        throw CommandError("materialPresets: save needs values= or set=");
                    Node* n = findSet(graph(), a.get<std::string>("set"));
                    if (!n)
                        throw CommandError("materialPresets: no texture set named '" + a.get<std::string>("set") + "'");
                    values = captureValues(readGraph(*n), e->meta);
                    if (values.empty())
                        throw CommandError("materialPresets: " + id + " names no seed or variation parameters to capture");
                }
                savePreset(id, a.get<std::string>("save"), values);
            }
            if (a.has("delete") && !deletePreset(id, a.get<std::string>("delete")))
                throw CommandError("materialPresets: no preset of yours named '" + a.get<std::string>("delete") + "'");
            return presets(id);
        },
    });

    ops::registerCommand({
        .name = "materialLint",
        .doc = "Check a material's values against physical ranges: a texture set, a library id or a .lilamat path, evaluated small. "
               "Checks albedo (dielectrics 30 to 240 sRGB), pure black/white, metal reflectance (per metal), binary metallic, "
               "roughness, normal validity and convention, height range and tile seams; stylised materials relax the albedo "
               "checks. fix=True clamps what it can with Levels nodes (sets undoably, user/workspace files in place). "
               "all=True checks every library material (tier= to narrow). Returns {passed, failures, warnings, checks}.",
        .positionalDoc = "material",
        .flags = {{"fix", "", T::Bool, false, "Clamp what failed with Levels nodes."},
                  {"size", "", T::Int, 256, "Evaluation size, px.", {}, {}, 1.0},
                  {"backend", "", T::String, nullptr, "gpu or cpu (default: the GPU when there is one).", {"gpu", "cpu"}},
                  {"physical", "", T::String, nullptr, "Override the meta's physical category.", {"metal", "dielectric", "organic", "emissive", "stylised"}},
                  {"all", "", T::Bool, false, "Every library material."},
                  {"tier", "", T::String, nullptr, "With all: only this tier."}},
        .run = [](const Args& a) -> json {
            if (a.get<bool>("all", false))
            {
                json out = json::array();
                Query q{.tier = a.get<std::string>("tier", "")};
                for (const Entry& e : search(q))
                {
                    if (e.tier == "scene")
                        continue;
                    json r;
                    try
                    {
                        LintTarget t = lintTarget(e.id);
                        r = lintOne(t, a);
                    }
                    catch (const std::exception& ex)
                    {
                        r = {{"passed", false}, {"error", ex.what()}};
                    }
                    r["id"] = e.id;
                    out.push_back(r);
                }
                return out;
            }
            LintTarget t = lintTarget(positionalString(a, 0, "materialLint", "material"));
            return lintOne(t, a);
        },
    });
}
} // namespace lila::tex::library
