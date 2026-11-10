#include "groom/GroomPresets.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "curve/CurveNodes.h"
#include "groom/Attrs.h"
#include "groom/GroomEdit.h"
#include "groom/GroomNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace lila::groom::presets
{
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace scene;
using T = ArgType;

namespace
{
constexpr const char* kFormat = "lilagroom";

std::optional<json> readPreset(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_object() && j.value("format", "") == kFormat)
        return j;
    return std::nullopt;
}

fs::path userDir() { return PrefStore::get().userDir() / "groom"; }

fs::path projectDir() { return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / "groom" : fs::path(); }

/** Attr values of a node's editable inputs, by name. */
json attrsOf(const Node& n)
{
    json out = json::object();
    for (const AttrDef& d : n.type().ownAttrs)
        if ((d.flags & AttrInput) && !(d.flags & AttrHidden))
            out[d.name] = toJson(n.value(n.type().attrIndex(d.name)));
    return out;
}

void setAttrs(Graph& g, Node& n, const json& attrs)
{
    if (!attrs.is_object())
        return;
    for (const AttrDef& d : n.type().ownAttrs)
        if ((d.flags & AttrInput) && !(d.flags & AttrHidden) && attrs.contains(d.name))
            try
            {
                g.setValue({n.id(), n.type().attrIndex(d.name), -1}, fromJson(d, attrs[d.name]));
            }
            catch (const std::exception& e)
            {
                log::warn("groom preset: {}: {}", d.name, e.what());
            }
}

json vec(const D3& v) { return json::array({v.x, v.y, v.z}); }

D3 vecOf(const json& j, const D3& def) { return j.is_array() && j.size() == 3 ? D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : def; }

Node& groomArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.positional.empty())
    {
        Node* n = g.find(a.positional[0].get<std::string>());
        if (Node* gr = n ? findGroom(g, *n) : nullptr)
            return *gr;
        throw CommandError(std::string(cmd) + ": " + a.positional[0].get<std::string>() + " is not part of a groom");
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = findGroom(g, *n))
                return *gr;
    throw CommandError(std::string(cmd) + ": select a groom");
}

std::string slug(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return out.empty() ? "preset" : out;
}

/** Bounds of the scalp the guides grow from, in the guides' space. */
bool scalpBounds(Graph& g, Node& guides, D3& lo, D3& hi)
{
    std::unique_ptr<Surface> s = scalpOf(g, guides);
    if (!s)
        return false;
    const geo::Mesh& m = s->mesh();
    lo = D3(1e300);
    hi = D3(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    return lo.x <= hi.x;
}
} // namespace

std::vector<Folder> folders()
{
    std::vector<Folder> out = {{paths::resourcesDir() / "groom", "lila", false}};
    const std::vector<fs::path> studio = PrefStore::get().studioDirs();
    for (size_t i = 0; i < studio.size(); ++i)
        out.push_back({studio[i] / "groom", i == 0 ? std::string("studio") : "studio" + std::to_string(i + 1), false});
    out.push_back({userDir(), "user", true});
    if (fs::path p = projectDir(); !p.empty())
        out.push_back({p, "project", true});
    return out;
}

std::vector<Preset> list()
{
    std::vector<Preset> out;
    std::error_code ec;
    for (const Folder& f : folders())
    {
        if (!fs::is_directory(f.dir, ec))
            continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(f.dir, ec))
            if (e.path().extension() == kExtension)
                files.push_back(e.path());
        std::ranges::sort(files);
        for (const fs::path& file : files)
            if (auto j = readPreset(file))
                out.push_back({f.source + "/" + paths::toUtf8(file.stem()), j->value("name", paths::toUtf8(file.stem())), j->value("doc", std::string()), f.source,
                               j->contains("guides"), file});
    }
    return out;
}

json capture(Graph& g, Node& groom, bool withGuides)
{
    json j = {{"format", kFormat}, {"version", 1}, {"lock", attrsOf(groom)}};
    if (Node* guides = withGuides ? guidesOf(g, groom) : nullptr)
    {
        json splines = json::array();
        for (const Spline& s : guideSplines(g, *guides))
            splines.push_back(curve::splineToJson(s));
        D3 lo, hi;
        if (scalpBounds(g, *guides, lo, hi))
            j["guides"] = {{"min", vec(lo)}, {"max", vec(hi)}, {"splines", splines}};
        else
            j["guides"] = {{"splines", splines}};
    }
    return j;
}

int apply(Graph& g, Node& groom, const json& preset, bool guides, bool replaceGuides)
{
    setAttrs(g, groom, preset.value("lock", json::object()));
    Node* guideNode = guidesOf(g, groom);
    if (!guides || !guideNode || !preset.contains("guides"))
        return 0;
    const json& gj = preset["guides"];
    Splines add;
    for (const json& s : gj.value("splines", json::array()))
        add.push_back(curve::splineFromJson(s));
    // Fitted from the bounds of the scalp they were made on to this one's.
    D3 lo, hi;
    std::unique_ptr<Surface> scalp = scalpOf(g, *guideNode);
    if (gj.contains("min") && gj.contains("max") && scalpBounds(g, *guideNode, lo, hi))
    {
        const D3 from0 = vecOf(gj["min"], D3(0.0)), from1 = vecOf(gj["max"], D3(1.0));
        const D3 scale = (hi - lo) / glm::max(from1 - from0, D3(1e-9));
        for (Spline& s : add)
        {
            for (D3& p : s.points)
                p = lo + (p - from0) * scale;
            for (D3& p : s.handleL)
                p = lo + (p - from0) * scale;
            for (D3& p : s.handleR)
                p = lo + (p - from0) * scale;
        }
    }
    if (scalp)
        attachRoots(add, *scalp);
    Splines s = replaceGuides ? Splines{} : guideSplines(g, *guideNode);
    int64_t next = attr::nextId(s);
    for (Spline& sp : add)
    {
        attr::set(sp, attr::kId, double(next++));
        s.push_back(std::move(sp));
    }
    storeGuides(g, *guideNode, std::move(s));
    return int(add.size());
}

void registerCommands()
{
    ops::registerCommand({
        .name = "groomPresets",
        .doc = "Hairstyle presets in the groom folders (Lila's, the studio's, yours and the project's). Returns [{id, name, doc, "
               "source, guides, file}, ...].",
        .flags = {},
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Preset& p : list())
                out.push_back({{"id", p.id}, {"name", p.name}, {"doc", p.doc}, {"source", p.source}, {"guides", p.guides}, {"file", paths::toUtf8(p.file)}});
            return out;
        },
    });
    ops::registerCommand({
        .name = "groomSavePreset",
        .doc = "Saves a groom's lock settings and style stack as a hairstyle preset, with its guides when guides is on. Returns the "
               "preset id.",
        .positionalDoc = "groom",
        .flags = {{"name", "n", T::String, nullptr, "Preset name."},
                  {"doc", "", T::String, nullptr, "One line on what it looks like."},
                  {"guides", "g", T::Bool, false, "Keep the guides too, so loading it draws the hairstyle."},
                  {"scope", "", T::String, "user", "Where it goes.", {"user", "project"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = groomArg(g, a, "groomSavePreset");
            const std::string name = a.get<std::string>("name");
            if (name.empty())
                throw CommandError("groomSavePreset: give a name");
            fs::path dir = a.get<std::string>("scope", "user") == "project" ? projectDir() : userDir();
            if (dir.empty())
                throw CommandError("groomSavePreset: no project is open");
            std::error_code ec;
            fs::create_directories(dir, ec);
            json j = capture(g, groom, a.get<bool>("guides"));
            j["name"] = name;
            if (a.has("doc"))
                j["doc"] = a.get<std::string>("doc");
            const fs::path file = dir / (slug(name) + kExtension);
            std::ofstream out(file, std::ios::binary);
            out << j.dump(2);
            if (!out)
                throw CommandError("groomSavePreset: can't write " + paths::toUtf8(file));
            return a.get<std::string>("scope", "user") + "/" + slug(name);
        },
    });
    ops::registerCommand({
        .name = "groomLoadPreset",
        .doc = "Applies a hairstyle preset to a groom: its lock settings, its style stack in place of the groom's, and its guides "
               "fitted to the scalp when it has them. Returns how many guides it added.",
        .positionalDoc = "groom",
        .flags = {{"preset", "p", T::String, nullptr, "Preset id (groomPresets) or a .lilagroom file."},
                  {"guides", "g", T::Bool, true, "Add the preset's guides when it has them."},
                  {"replace", "", T::Bool, false, "The preset's guides replace the groom's."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groom = groomArg(g, a, "groomLoadPreset");
            const std::string want = a.get<std::string>("preset");
            std::optional<json> j;
            for (const Preset& p : list())
                if (p.id == want || p.name == want)
                {
                    j = readPreset(p.file);
                    break;
                }
            if (!j && fs::exists(paths::fromUtf8(want)))
                j = readPreset(paths::fromUtf8(want));
            if (!j)
                throw CommandError("groomLoadPreset: no preset '" + want + "'");
            return apply(g, groom, *j, a.get<bool>("guides", true), a.get<bool>("replace"));
        },
    });
}
} // namespace lila::groom::presets
