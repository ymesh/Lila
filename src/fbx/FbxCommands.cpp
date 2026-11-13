#include "fbx/Fbx.h"
#include "scene/GeoSidecar.h"
#include "scene/Georef.h"
#include "fbx/GameExport.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/LightingChannels.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/UnrealCheck.h"

#include <chrono>
#include <charconv>
#include <future>
#include <set>

namespace lila::fbx
{
namespace
{
using scene::Graph;
using scene::NodeId;

std::vector<std::string> positionalNames(const Args& a, size_t from)
{
    std::vector<std::string> out;
    for (size_t k = from; k < a.positional.size(); ++k)
    {
        const json& v = a.positional[k];
        if (v.is_string())
            out.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(x.get<std::string>());
    }
    return out;
}

fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::vector<std::string> names(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        out.push_back(g.node(id)->path());
    return out;
}

/**
 * Skeletal when any of the nodes (or, with children, their descendants) is a skinned mesh, sequence when every node is
 * a camera, else static.
 */
ExportKind autoKind(Graph& g, const std::vector<NodeId>& ids, bool children)
{
    bool cameras = !ids.empty();
    for (NodeId id : ids)
        cameras = cameras && g.node(id) && g.node(id)->type().isA("camera");
    if (cameras)
        return ExportKind::Sequence;
    for (NodeId id : ids)
    {
        std::vector<NodeId> all{id};
        if (children)
            for (NodeId d : g.descendants(id))
                all.push_back(d);
        for (NodeId n : all)
            if (g.node(n) && g.node(n)->type().isA("mesh") && scene::skin::find(g, n) != scene::kNoNode)
                return ExportKind::Skeletal;
    }
    return ExportKind::Static;
}
} // namespace

void unrealCheck(Graph& g, const std::vector<NodeId>& ids, const ExportOptions& opts, const std::string& command)
{
    if (opts.kind != ExportKind::Skeletal && opts.kind != ExportKind::Animation)
        return;
    // Without animation the file holds the pose at the current time only.
    const bool range = opts.animation || opts.kind == ExportKind::Animation;
    const double now = g.time();
    const scene::uecheck::ExportCheck c = scene::uecheck::checkExport(g, ids, opts.children, range ? opts.start : now, range ? opts.end : now,
                                                                       0.0, command);
    if (!c.error.empty())
        throw CommandError(c.error);
}

bool exportForUnreal(const std::vector<scene::NodeId>& nodes, const fs::path& path, const std::string& profile, std::string* error,
                     const std::string& kind, bool atOrigin, const std::string& take, bool children)
{
    LILA_ZONE();

    ExportOptions opts;
    if (!parseProfile(profile.empty() ? "ue5" : profile, &opts.profile) || opts.profile == AxisProfile::Generic)
    {
        if (error)
            *error = "unknown Unreal profile '" + profile + "' (ue5 or ue6)";
        return false;
    }
    if (!parseKind(kind.empty() ? "static" : kind, &opts.kind))
    {
        if (error)
            *error = "unknown kind '" + kind + "' (static, skeletal, animation or sequence)";
        return false;
    }
    opts.atOrigin = atOrigin;
    opts.children = children;
    opts.collision = true;
    if (!take.empty())
        opts.takes = {take};

    progress::Scope scope("Exporting " + path.filename().string(), 3.0);
    // Read here: a SkipScope lives on the calling thread, not on the main thread that gathers.
    const bool check = !scene::uecheck::skipping();
    auto gather = [nodes, opts, check] {
        if (check)
            unrealCheck(Graph::current(), nodes, opts, "Unreal export");
        return gatherExport(Graph::current(), nodes, opts);
    };
    ExportScene snapshot;
    if (mainq::isMainThread())
        try
        {
            snapshot = gather();
        }
        catch (const std::exception& e)
        {
            if (error)
                *error = e.what();
            return false;
        }
    else
    {
        auto promise = std::make_shared<std::promise<ExportScene>>();
        std::future<ExportScene> result = promise->get_future();
        mainq::post([promise, gather] {
            try
            {
                promise->set_value(gather());
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
            }
        });
        if (result.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
        {
            if (error)
                *error = "timed out waiting for the main thread to snapshot the scene";
            return false;
        }
        try
        {
            snapshot = result.get();
        }
        catch (const std::exception& e)
        {
            if (error)
                *error = e.what();
            return false;
        }
    }
    for (const std::string& w : snapshot.warnings)
        mainq::post([w] { log::warn("FBX export: {}", w); });
    scope.step();
    progress::Scope write("Writing " + path.filename().string(), 0.0, progress::None, 2.0);
    return writeFbx(snapshot, path, opts, error);
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "importFbx",
        .doc = "Import an FBX file: its node hierarchy as transforms, joints and meshes, converted to the scene's axes and "
               "cm. LimbNodes become joints (PreRotation into jointOrient), skins become skinClusters with the file's "
               "weights and bind matrices, morph channels become a morph with a target per channel (in-betweens kept), and one take is "
               "baked onto the channels. Cameras become camera nodes with their lens, film back and clipping (focal length, film back and "
               "clipping curves keyed per frame); lights are reported, not imported. Returns the new node names, or with report=True a dict of nodes, roots, "
               "joints, skinClusters, take, fps, skipped and warnings.",
        .positionalDoc = "path",
        .flags = {nodeFlag({"parent", "p", T::String, nullptr, "Parent the imported roots under this node."}, "transform"),
                  {"skins", "", T::Bool, true, "Rebuild skins as joints and skinClusters with their weights and bind pose."},
                  {"animation", "anim", T::Bool, true, "Bake the take onto translate/rotate/scale, one key per frame."},
                  {"take", "", T::String, "", "AnimStack to import; the file's active one (or first) by default."},
                  {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the take."},
                  {"instances", "", T::Bool, true, "Objects sharing one geometry come in as instances of the first."},
                  {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                  {"select", "", T::Bool, true, "Select the imported roots."},
                  {"georeference", "", T::Bool, true,
                   "Place it by the georeference it carries (a georeferenced Lila export) or the files beside it (metadata.xml, _offset.xyz, .rcinfo, .prj), as importObj does."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importFbx: path required");
            std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            ImportOptions opts;
            if (a.has("parent"))
            {
                scene::Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("importFbx: no node named '" + a.get<std::string>("parent") + "'");
                opts.parent = p->id();
            }
            opts.skins = a.get<bool>("skins", true);
            opts.animation = a.get<bool>("animation", true);
            opts.take = a.get<std::string>("take", "");
            opts.setPlayback = a.get<bool>("setPlayback", true);
            opts.instances = a.get<bool>("instances", true);
            opts.georeference = a.get<bool>("georeference", true);
            ImportResult r = importFbx(g, utf8Path(path), opts);
            if (!r.ok)
                throw CommandError("importFbx: " + r.error);
            // A georeferenced photogrammetry export: its scene is east, north, up from the sidecar's offset.
            if (a.get<bool>("georeference", true) && !r.georeferenced)
                if (const auto found = scene::geoSidecar::find(utf8Path(path)))
                {
                    const bool zUp = scene::georef::sceneZUp();
                    const scene::georef::CrsInfo info = scene::georef::crsInfo(found->georeference.crs);
                    const double perUnit = 100.0 * info.unitToMetre;
                    // The importer's scene axes back to east, north, height in CRS units (it turned a Z-up file into the scene's up).
                    glm::dmat4 toEnu(1.0 / perUnit);
                    toEnu[3][3] = 1.0;
                    if (!zUp)
                        toEnu = glm::dmat4(glm::dvec4(1, 0, 0, 0), glm::dvec4(0, 0, 1, 0), glm::dvec4(0, -1, 0, 0), glm::dvec4(0, 0, 0, 1)) *
                                glm::dmat4(glm::dvec4(1.0 / perUnit, 0, 0, 0), glm::dvec4(0, 1.0 / perUnit, 0, 0), glm::dvec4(0, 0, 1.0 / perUnit, 0), glm::dvec4(0, 0, 0, 1));
                    const scene::georef::Placement placed = scene::georef::place(found->georeference);
                    const glm::dmat4 m = placed.matrix * scene::georef::localFrame(found->georeference, zUp) * toEnu;
                    std::vector<uint64_t> roots(r.roots.begin(), r.roots.end());
                    scene::georef::placeRoots(g, roots, m);
                    log::info("importFbx: {} ({})", placed.message, found->source);
                }
            for (const std::string& s : r.skipped)
                log::info("importFbx: skipped {}", s);
            for (const std::string& w : r.warnings)
                log::warn("importFbx: {}", w);
            if (a.get<bool>("select", true))
                g.select(r.roots);
            if (!a.get<bool>("report", false))
                return names(g, r.nodes);
            return {{"nodes", names(g, r.nodes)}, {"roots", names(g, r.roots)}, {"joints", names(g, r.joints)},
                    {"skinClusters", names(g, r.skins)}, {"cameras", names(g, r.cameras)}, {"take", r.take}, {"fps", r.fps}, {"start", r.start}, {"end", r.end},
                    {"curves", r.curves}, {"skipped", r.skipped}, {"warnings", r.warnings}, {"sourceAxes", r.sourceAxes},
                    {"sourceUnitMeters", r.sourceUnitMeters}};
        },
    });

    ops::registerCommand({
        .name = "exportFbx",
        .doc = "Export the evaluated meshes and transforms to a binary FBX 7.4 file. Objects default to the selection "
               "(or the whole scene with selectionOnly=False). Names go out intact, so UCX_/UBX_/USP_/UCP_ collision and "
               "SOCKET_ empties work in Unreal. kind=skeletal writes joints, skins (bind pose mesh, clusters, BindPose), "
               "morph targets (in-betweens as their own targets) with their weight curves, driven channel curveNames as named curves on the root joint, and the animation baked over the range; kind=animation writes only the skeleton and its curves. "
               "kind=sequence writes cameras and transforms with their animation and no meshes, for Unreal Sequencer's Import FBX: "
               "cameras go out as FBX cameras (focal length aperture mode, film back, clipping, focal length and clipping curves) "
               "and arrive as CineCameras. Cameras are written as FBX cameras in every kind. "
               "start/end are frames. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {{"profile", "", T::String, "ue5", "Axis profile: ue5 (Z up), ue6 (Y up) or generic (the scene's own axes)."},
                  {"kind", "k", T::String, "auto", "auto | static | skeletal | animation | sequence. auto is skeletal when a skinned mesh is exported, sequence when only cameras are."},
                  {"maxInfluences", "mi", T::Int, 8, "Skin influences per vertex, heaviest kept and renormalised (4 for mobile).", {}, {}, 0.0},
                  {"animation", "anim", T::Bool, true, "Write the take for skeletal and sequence exports (always for kind=animation)."},
                  {"bakeAnimation", "bake", T::Bool, true, "Sample every frame; False writes plain translate/rotate/scale curves as keys."},
                  {"deformOnly", "do", T::Bool, true, "Skeletal and animation kinds: deform joints only (IK helpers and rig-only joints stay behind)."},
                  {"start", "st", T::Float, nullptr, "First frame; the playback start by default."},
                  {"end", "et", T::Float, nullptr, "Last frame; the playback end by default."},
                  {"take", "", T::String, "", "Take (AnimStack) name; the active take's, else Take 001."},
                  {"takes", "", T::Any, nullptr, "Scene takes to write, one AnimStack each: 'all' or a list of names."},
                  {"triangulate", "", T::Bool, true, "Write triangles exactly as the viewport splits faces."},
                  {"tangents", "", T::Bool, true, "Write MikkTSpace tangents and binormals from uv0."},
                  {"worldSpace", "ws", T::Bool, false, "Bake world transforms into the meshes and flatten the hierarchy."},
                  {"atOrigin", "", T::Bool, false, "Drop the exported roots' own transforms, keeping their children relative."},
                  {"children", "", T::Bool, true, "Include the descendants of the given objects."},
                  {"smoothingGroups", "sg", T::Bool, true, "Write edge smoothing (hard and soft edges)."},
                  {"compress", "", T::Bool, true, "Deflate the larger arrays as the FBX SDK does; False writes them raw."},
                  {"textures", "", T::Bool, false,
                   "Write the materials' colors and base color, normal and emissive maps as file textures with their wrap modes "
                   "(repeat or clamp). Off writes plain named materials."},
                  {"collision", "col", T::Bool, false,
                   "Static kind: write each mesh's simple collision (a Stagehand body's shape) as UCX_<mesh>_NN meshes for Unreal."},
                  {"units", "u", T::String, "cm", "File units: cm, m, mm, in or ft. Lengths are scaled to them.", {"cm", "m", "mm", "in", "ft"}},
                  {"selectionOnly", "sl", T::Bool, true, "Without objects: the selection, or every root when False."},
                  {"unrealCheck", "uc", T::Bool, true,
                   "Skeletal and animation kinds: refuse when a skeleton fails unrealCheck over the range (sampled). False exports anyway."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("exportFbx: path required");
            std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();

            ExportOptions opts;
            std::string profile = a.get<std::string>("profile", "ue5");
            if (!parseProfile(profile, &opts.profile))
                throw CommandError("exportFbx: unknown profile '" + profile + "' (ue5, ue6 or generic)");
            opts.triangulate = a.get<bool>("triangulate", true);
            opts.tangents = a.get<bool>("tangents", true);
            opts.worldSpace = a.get<bool>("worldSpace", false);
            opts.atOrigin = a.get<bool>("atOrigin", false);
            opts.children = a.get<bool>("children", true);
            opts.smoothingGroups = a.get<bool>("smoothingGroups", true);
            opts.compress = a.get<bool>("compress", true);
            opts.unitScale = game::cmPerUnit(a.get<std::string>("units", "cm"));
            if (opts.unitScale <= 0.0)
                throw CommandError("exportFbx: units must be cm, m, mm, in or ft");
            opts.maxInfluences = a.get<int>("maxInfluences", 8);
            opts.textures = a.get<bool>("textures", false);
            opts.collision = a.get<bool>("collision", false);
            opts.animation = a.get<bool>("animation", true);
            opts.bakeAnimation = a.get<bool>("bakeAnimation", true);
            opts.deformOnly = a.get<bool>("deformOnly", true);
            opts.takeName = a.get<std::string>("take", "");
            if (a.has("takes"))
            {
                const json& t = a.flags["takes"];
                if (t.is_string())
                    opts.takes.push_back(t.get<std::string>());
                else if (t.is_array())
                    for (const json& n : t)
                        if (n.is_string())
                            opts.takes.push_back(n.get<std::string>());
            }
            if (a.has("start"))
                opts.start = scene::anim::secondsOf(a.get<double>("start"));
            if (a.has("end"))
                opts.end = scene::anim::secondsOf(a.get<double>("end"));

            std::vector<NodeId> ids;
            std::vector<std::string> objs = positionalNames(a, 1);
            for (const std::string& name : objs)
            {
                scene::Node* n = g.find(name);
                if (!n)
                    throw CommandError("exportFbx: no node named '" + name + "'");
                ids.push_back(n->id());
            }
            if (objs.empty())
                ids = a.get<bool>("selectionOnly", true) ? g.selection() : g.roots();
            std::string kind = a.get<std::string>("kind", "auto");
            if (kind == "auto")
                opts.kind = autoKind(g, ids, opts.children);
            else if (!parseKind(kind, &opts.kind))
                throw CommandError("exportFbx: unknown kind '" + kind + "' (auto, static, skeletal, animation or sequence)");

            if (a.get<bool>("unrealCheck", true))
                unrealCheck(g, ids, opts, "exportFbx");

            progress::Scope scope("Exporting " + utf8Path(path).filename().string() + "...", 3.0);
            ExportScene snapshot;
            {
                progress::Scope gather("Gathering the scene");
                snapshot = gatherExport(g, ids, opts);
            }
            for (const std::string& w : snapshot.warnings)
                log::warn("exportFbx: {}", w);
            if (snapshot.nodes.empty())
                throw CommandError("exportFbx: nothing to export");
            std::vector<NodeId> sources;
            for (const ExportNode& e : snapshot.nodes)
                sources.push_back(e.source);
            if (const scene::lightingChannels::Usage lc = scene::lightingChannels::usage(g, sources); !lc.empty())
                log::warn("exportFbx: lighting channels {} not written: FBX has no light linking (USD and Unreal placements carry them)",
                          scene::lightingChannels::summary(lc));
            std::string err;
            bool ok = false;
            {
                progress::Scope write("Writing " + utf8Path(path).filename().string(), 0.0, progress::None, 2.0);
                progress::runOnWorker([&] { ok = writeFbx(snapshot, utf8Path(path), opts, &err); });
            }
            if (!ok)
                throw CommandError("exportFbx: " + err);
            return path;
        },
    });

    ops::registerCommand({
        .name = "fixDuplicateJointNames",
        .doc = "Rename joints that share a name with another joint in the same skeleton (the given roots and their "
               "descendants, or the selection), so the skeleton exports with unique bone names. Returns [[old, new], ...].",
        .positionalDoc = "roots",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> joints;
            std::set<NodeId> seen;
            for (const std::string& name : a.objects())
            {
                scene::Node* root = g.find(name);
                if (!root)
                    throw CommandError("fixDuplicateJointNames: no node named '" + name + "'");
                std::vector<NodeId> all{root->id()};
                for (NodeId d : g.descendants(root->id()))
                    all.push_back(d);
                for (NodeId id : all)
                    if (scene::rig::isJoint(*g.node(id)) && seen.insert(id).second)
                        joints.push_back(id);
            }
            std::set<std::string> used;
            for (NodeId id : joints)
                used.insert(g.node(id)->name());
            json out = json::array();
            std::set<std::string> kept;
            for (NodeId id : joints)
            {
                scene::Node* n = g.node(id);
                if (kept.insert(n->name()).second)
                    continue;
                std::string base = n->name();
                size_t digits = base.size();
                while (digits > 0 && base[digits - 1] >= '0' && base[digits - 1] <= '9')
                    --digits;
                std::string stem = base.substr(0, digits);
                int k = 1;
                if (digits < base.size())
                    std::from_chars(base.data() + digits, base.data() + base.size(), k);
                std::string candidate;
                do
                    candidate = stem + std::to_string(++k);
                while (used.contains(candidate) || g.availableName(candidate, g.parent(id)) != candidate);
                std::string was = n->path();
                std::string now = g.rename(id, candidate);
                used.insert(now);
                kept.insert(now);
                out.push_back({was, n->path()});
            }
            return out;
        },
    });

    game::registerCommands();
}
} // namespace lila::fbx
