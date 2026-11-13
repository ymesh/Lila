// OpenUSDCore: the half of the OpenUSD extension that links Pixar's libraries, loaded on the first USD command.

#include "UsdExport.h"
#include "UsdBackend.h"
#include "UsdCommon.h"
#include "UsdImport.h"
#include "UsdMaterialX.h"
#include "UsdStage.h"
#include "UsdAttributes.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/LightingChannels.h>
#include <lila/Log.h>

#include <cmath>
#include <format>

using namespace lila;

using json = nlohmann::json;

namespace
{
using scene::Graph;
using scene::NodeId;

std::vector<std::string> names(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        if (const scene::Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

std::vector<std::string> stringList(const json& j)
{
    std::vector<std::string> out;
    if (j.is_string())
        out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& v : j)
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

json importCommand(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("importUsd: path required");
    const std::string path = a.positional[0].get<std::string>();
    Graph& g = Graph::current();
    usd::ImportOptions opts;
    if (a.has("parent"))
    {
        scene::Node* p = g.find(a.get<std::string>("parent"));
        if (!p)
            throw CommandError("importUsd: no node named '" + a.get<std::string>("parent") + "'");
        opts.parent = p->id();
    }
    opts.prim = a.get<std::string>("prim", "");
    opts.skins = a.get<bool>("skins", true);
    opts.animation = a.get<bool>("animation", true);
    opts.setPlayback = a.get<bool>("setPlayback", true);
    opts.materials = a.get<bool>("materials", true);
    opts.loadPayloads = a.get<bool>("loadPayloads", true);
    opts.georeference = a.get<bool>("georeference", true);
    opts.take = a.get<std::string>("take", "");
    opts.lightingChannels = a.get<bool>("lightingChannels", true);
    if (a.has("variants"))
    {
        const json& v = a.flags["variants"];
        if (!v.is_object())
            throw CommandError("importUsd: variants takes a dict, {'set': 'variant'} or {'/prim/path': {'set': 'variant'}}");
        for (auto& [key, value] : v.items())
        {
            if (value.is_string())
                opts.variants[key] = value.get<std::string>();
            else if (value.is_object())
                for (auto& [set, sel] : value.items())
                    if (sel.is_string())
                        opts.primVariants[key][set] = sel.get<std::string>();
        }
    }
    if (a.has("purposes"))
    {
        std::vector<std::string> p = stringList(a.flags["purposes"]);
        opts.purposes.clear();
        for (const std::string& s : p)
        {
            if (s == "all")
                opts.purposes = {"default", "render", "proxy", "guide"};
            else if (s == "default" || s == "render" || s == "proxy" || s == "guide")
                opts.purposes.insert(s);
            else
                throw CommandError("importUsd: purposes are default, render, proxy, guide or all");
        }
        opts.purposes.insert("default");
    }

    usd::ImportResult r = usd::importUsd(g, usd::utf8Path(path), opts);
    if (!r.ok)
        throw CommandError("importUsd: " + r.error);
    for (const std::string& s : r.skipped)
        log::info("importUsd: skipped {}", s);
    for (const std::string& w : r.warnings)
        log::warn("importUsd: {}", w);
    if (a.get<bool>("select", true))
        g.select(r.roots);
    if (!a.get<bool>("report", false))
        return names(g, r.nodes);
    return {{"nodes", names(g, r.nodes)},
            {"roots", names(g, r.roots)},
            {"joints", names(g, r.joints)},
            {"skinClusters", names(g, r.skins)},
            {"cameras", names(g, r.cameras)},
            {"lights", names(g, r.lights)},
            {"curves", names(g, r.curves)},
            {"meshCaches", names(g, r.meshCaches)},
            {"materials", r.materials},
            {"take", r.take},
            {"takes", r.takes},
            {"fps", r.fps},
            {"start", r.start},
            {"end", r.end},
            {"keyedCurves", r.keyedCurves},
            {"instances", r.instances},
            {"upAxis", r.upAxis},
            {"metersPerUnit", r.metersPerUnit},
            {"lightingChannels", r.lightingChannels},
            {"skipped", r.skipped},
            {"warnings", r.warnings}};
}

json exportCommand(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("exportUsd: path required");
    const std::string path = a.positional[0].get<std::string>();
    std::string ext = usd::utf8(usd::utf8Path(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".usd" && ext != ".usda" && ext != ".usdc" && ext != ".usdz")
        throw CommandError("exportUsd: the file must end in .usd, .usda, .usdc or .usdz");
    Graph& g = Graph::current();

    usd::ExportOptions opts;
    const std::string up = a.get<std::string>("upAxis", "z");
    if (up != "z" && up != "y" && up != "Z" && up != "Y")
        throw CommandError("exportUsd: upAxis is z or y");
    opts.zUp = up == "z" || up == "Z";
    opts.metersPerUnit = a.get<double>("metersPerUnit", 0.01);
    if (!(opts.metersPerUnit > 0.0))
        throw CommandError("exportUsd: metersPerUnit must be positive");
    const std::string format = a.get<std::string>("format", "auto");
    if (format == "usda")
        opts.format = usd::Format::Usda;
    else if (format == "usdc")
        opts.format = usd::Format::Usdc;
    else if (format != "auto")
        throw CommandError("exportUsd: format is auto, usda or usdc");
    if ((format == "usda" && ext == ".usdc") || (format == "usdc" && ext == ".usda"))
        throw CommandError("exportUsd: format " + format + " doesn't match the " + ext + " extension");
    opts.materials = a.get<bool>("materials", true);
    if (!usd::mtlx::parseSurface(a.get<std::string>("materialX", "none"), &opts.materialX))
        throw CommandError("exportUsd: materialX is none, openPBR or standardSurface");
    opts.previewSurface = a.get<bool>("previewSurface", true);
    if (!opts.previewSurface && opts.materialX == usd::mtlx::Surface::None)
        throw CommandError("exportUsd: previewSurface=False needs a materialX surface to write instead");
    const std::string textures = a.get<std::string>("textures", "relative");
    if (textures == "absolute")
        opts.textures = usd::ExportOptions::Textures::Absolute;
    else if (textures == "copy")
        opts.textures = usd::ExportOptions::Textures::Copy;
    else if (textures != "relative")
        throw CommandError("exportUsd: textures is relative, absolute or copy");
    opts.animation = a.get<bool>("animation", true);
    if (a.has("takes"))
        opts.takes = stringList(a.flags["takes"]);
    if (a.has("start"))
        opts.start = scene::anim::secondsOf(a.get<double>("start"));
    if (a.has("end"))
        opts.end = scene::anim::secondsOf(a.get<double>("end"));
    opts.normals = a.get<bool>("normals", true);
    opts.triangulate = a.get<bool>("triangulate", false);
    opts.maxInfluences = a.get<int>("maxInfluences", 8);
    opts.deformOnly = a.get<bool>("deformOnly", true);
    opts.atOrigin = a.get<bool>("atOrigin", false);
    opts.children = a.get<bool>("children", true);
    opts.worldSpace = a.get<bool>("worldSpace", false);
    opts.lods = a.get<bool>("lods", true);
    opts.splitLayers = a.get<bool>("splitLayers", false);
    opts.lightingChannels = a.get<bool>("lightingChannels", true);

    std::vector<NodeId> ids;
    std::vector<std::string> objs;
    for (size_t k = 1; k < a.positional.size(); ++k)
        for (const std::string& s : stringList(a.positional[k]))
            objs.push_back(s);
    for (const std::string& name : objs)
    {
        scene::Node* n = g.find(name);
        if (!n)
            throw CommandError("exportUsd: no node named '" + name + "'");
        ids.push_back(n->id());
    }
    if (objs.empty())
        ids = a.get<bool>("selectionOnly", true) ? g.selection() : g.roots();

    usd::ExportResult r = usd::exportUsd(g, ids, usd::utf8Path(path), opts);
    for (const std::string& w : r.warnings)
        log::warn("exportUsd: {}", w);
    if (!r.ok)
        throw CommandError("exportUsd: " + r.error);
    const scene::lightingChannels::Usage& lc = r.lightingChannels;
    if (!lc.empty())
        log::info("exportUsd: lighting channels {} {}", scene::lightingChannels::summary(lc),
                  opts.lightingChannels ? "written as light links" : "left out (lightingChannels=False)");
    if (a.get<bool>("report", false))
        return {{"files", r.files}, {"warnings", r.warnings}, {"lightingChannels", scene::lightingChannels::report(g, lc, opts.lightingChannels && !lc.empty())}};
    return path;
}
} // namespace

USD_BACKEND_EXPORT int lilaUsdInit(const char* packageRoot, std::string* error)
{
    try
    {
#ifdef LILA_USD_PYTHON
        usd::preparePython(usd::utf8Path(packageRoot));
#endif
        return usd::registerPlugins(usd::utf8Path(packageRoot), error) ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        *error = e.what();
        return 1;
    }
}

USD_BACKEND_EXPORT void* lilaUsdStage(const char* node, std::string* error)
{
    try
    {
        return get_pointer(usd::stage::liveStage(node ? node : ""));
    }
    catch (const std::exception& e)
    {
        *error = e.what();
        return nullptr;
    }
}

USD_BACKEND_EXPORT uint64_t lilaUsdNodeForPrim(const char* primPath)
{
    try
    {
        return usd::stage::nodeForPrim(primPath ? primPath : "");
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

USD_BACKEND_EXPORT int lilaUsdRun(const char* command, const Args* args, json* result, std::string* error)
{
    try
    {
        const std::string_view name = command;
        if (name == "importUsd")
            *result = importCommand(*args);
        else if (name == "exportUsd")
            *result = exportCommand(*args);
        else if (name == "importMaterialX")
            *result = usd::importMaterialX(*args);
        else if (name == "exportMaterialX")
            *result = usd::exportMaterialX(*args);
        else if (name == "usdComponentSchemas")
            *result = usd::componentSchemaList();
        else if (!usd::stage::run(name, *args, *result))
            throw CommandError("OpenUSD: no command " + std::string(name));
        return 0;
    }
    catch (const std::exception& e)
    {
        *error = e.what();
        return 1;
    }
}
