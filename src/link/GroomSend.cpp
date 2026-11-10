#include "link/GroomSend.h"

#include "core/Log.h"
#include "link/RpcClient.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <filesystem>

namespace lila::link
{
namespace
{
namespace fs = std::filesystem;

scene::Node* groomOf(scene::Graph& g, const std::string& name)
{
    scene::Node* n = g.find(name);
    if (!n)
        return nullptr;
    if (n->type().isA("groom"))
        return n;
    // A groom's strands, locks or guides stand for it.
    for (scene::NodeId id : g.nodes())
        if (scene::Node* gr = g.node(id); gr && gr->type().isA("groom"))
            for (const char* out : {"outStrands", "outMesh", "outGuides", "outCards"})
                for (const scene::PlugRef& to : g.destinations({gr->id(), gr->type().attrIndex(out), -1}))
                    if (to.node == n->id())
                        return gr;
    return nullptr;
}

std::string assetName(const scene::Node& groom)
{
    std::string base = groom.name();
    if (base.size() > 5 && base.compare(base.size() - 5, 5, "Groom") == 0)
        base.resize(base.size() - 5);
    std::string out = "GR_";
    for (char c : base)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    return out;
}
} // namespace

void registerGroomSendCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "unrealSendGroom",
        .doc = "Sends a groom to the connected Unreal editor as a groom asset: its strands and the primary guides Unreal simulates "
               "on (exportGroom's Unreal Alembic), with M_LilaHair on it, built on the MF_LilaHair material function for your own "
               "hair materials. The groom stays mapped, so sending again reimports the same asset. Returns Unreal's reply with "
               "wait=True.",
        .positionalDoc = "groom (default: the selected groom)",
        .flags = {{"folder", "f", T::String, nullptr, "Content folder, like /Game/Characters/Hair (default /Game/Grooms, or the mapped one)."},
                  {"name", "n", T::String, nullptr, "Asset name (default GR_<groom>, or the mapped one)."},
                  {"start", "s", T::Float, nullptr, "First frame for an animated groom; the current frame when left out."},
                  {"end", "", T::Float, nullptr, "Last frame; start when left out."},
                  {"material", "", T::Bool, true, "Set M_LilaHair on the groom."},
                  {"bindTo", "b", T::String, nullptr,
                   "Bind the groom to a skeletal mesh so it follows its skinning: an asset like /Game/Characters/SK_Hero, or a Lila "
                   "node sent to Unreal as a skeletal mesh (its mapping). Remembered for the next send."},
                  {"bindSource", "", T::String, nullptr, "The skeletal mesh the groom was made on when it differs from bindTo (transfers it)."},
                  {"actor", "", T::String, nullptr, "With bindTo: the level actor (its label) that gets a groom component on its skeletal mesh."},
                  {"interpolationPoints", "", T::Int, 100, "Binding: points the hair follows on the mesh.", {}, {}, 1.0, 1000.0},
                  {"wait", "wt", T::Bool, false, "Block until Unreal answers and return its reply."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            scene::Node* groom = nullptr;
            for (const std::string& name : a.objects())
                if ((groom = groomOf(g, name)))
                    break;
            if (!groom)
                throw CommandError("unrealSendGroom: select a groom");
            json mapping = mappingOf(groom->fullPath());
            if (!mapping.is_object() || mapping.value("kind", std::string()) != "groom")
                mapping = {{"node", groom->fullPath()}, {"kind", "groom"}};
            const std::string folder = a.has("folder") ? a.get<std::string>("folder") : mapping.value("folder", std::string("/Game/Grooms"));
            const std::string name = a.has("name") ? a.get<std::string>("name") : mapping.value("name", assetName(*groom));

            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("unrealSendGroom: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<RpcClient> client = Link::get().client();
            if (!client)
                throw CommandError("unrealSendGroom: not connected to Unreal");
            const json features = Link::get().status().value("features", json::array());
            if (!features.is_array() || std::ranges::find(features, json("groom")) == features.end())
                throw CommandError("unrealSendGroom: the LilaLink plugin in this editor is too old for importGroom; rebuild it from Lila's ue/LilaLink");

            const fs::path dir = fs::temp_directory_path() / "lila_link_grooms";
            fs::create_directories(dir);
            const std::string file = (dir / (name + ".abc")).generic_string();
            json call = {{"args", {groom->path()}}, {"path", file}, {"engine", "unreal"}};
            if (a.has("start"))
                call["start"] = a.get<double>("start");
            if (a.has("end"))
                call["end"] = a.get<double>("end");
            const json exported = ops::execute("exportGroom", call);

            mapping["folder"] = folder;
            mapping["name"] = name;
            mapping["contentPath"] = folder + "/" + name;
            setMapping(mapping);

            json params = {{"file", file}, {"folder", folder}, {"name", name}, {"material", a.get<bool>("material", true)}};
            // A binding target given as a Lila node is the asset it was sent as.
            auto assetOf = [&](const std::string& what) {
                if (what.empty() || what.front() == '/')
                    return what;
                scene::Node* n = g.find(what);
                const json m = n ? mappingOf(n->fullPath()) : json();
                if (!m.is_object() || m.value("contentPath", std::string()).empty())
                    throw CommandError("unrealSendGroom: " + what + " has not been sent to Unreal; send it first or give its asset path");
                const std::string path = m["contentPath"];
                return path.find('.') == std::string::npos ? path + "." + path.substr(path.rfind('/') + 1) : path;
            };
            const std::string bindTo = a.has("bindTo") ? assetOf(a.get<std::string>("bindTo")) : mapping.value("bindTo", std::string());
            if (!bindTo.empty())
            {
                params["bindTo"] = bindTo;
                params["interpolationPoints"] = a.get<int>("interpolationPoints", 100);
                if (a.has("bindSource"))
                    params["bindSource"] = assetOf(a.get<std::string>("bindSource"));
                const std::string actor = a.has("actor") ? a.get<std::string>("actor") : mapping.value("actor", std::string());
                if (!actor.empty())
                    params["actor"] = actor;
                mapping["bindTo"] = bindTo;
                if (!actor.empty())
                    mapping["actor"] = actor;
                setMapping(mapping);
            }
            if (a.get<bool>("wait", false))
            {
                RpcResult r = client->call("importGroom", params, importCallOptions());
                if (!r.ok)
                    throw CommandError("unrealSendGroom: " + r.error);
                return r.result;
            }
            const std::string what = groom->name();
            client->callOnMain("importGroom", params, importCallOptions(), [what](RpcResult r) {
                if (!r.ok)
                    return log::error("Unreal: sending {} failed: {}", what, r.error);
                for (const json& w : r.result.value("warnings", json::array()))
                    log::warn("Unreal: {}: {}", what, w.get<std::string>());
                log::info("Unreal: {} -> {}", what, r.result.value("groom", std::string()));
            });
            return {{"queued", true}, {"file", file}, {"strands", exported.value("strands", 0)}, {"contentPath", folder + "/" + name}};
        },
    });
}
} // namespace lila::link
