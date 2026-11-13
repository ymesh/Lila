// The OpenUSD extension: importUsd and exportUsd, live USD stages (the usdStage node and its commands), and the "usd"
// file translator File > Open, Import and Export use. Pixar's libraries live in OpenUSDCore beside this file, loaded on
// the first USD command (or when a scene holding a stage opens), so startup never pays for them.

#include "OpenUsd.h"
#include "UsdBackend.h"

#include <lila/AttrComponents.h>
#include <lila/Command.h>
#include <lila/Diagnostics.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/FileTranslator.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace lila;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
fs::path gPackageRoot;
std::atomic<bool> gLoaded{false};
/** True on the thread loading OpenUSDCore, whose start-up imports pxr and so reaches lilaUsdLoad again through lila.usd. */
thread_local bool tLoading = false;

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct Backend
{
    usd::BackendRun run = nullptr;
    void* lib = nullptr;
    std::string error;
};

/** Loads OpenUSDCore and registers Pixar's plugins once; it stays loaded, since OpenUSD does not unload cleanly. */
const Backend& backend()
{
    static Backend b;
    static std::once_flag once;
    std::call_once(once, [] {
        tLoading = true;
        struct Done
        {
            ~Done() { tLoading = false; }
        } done;
        const fs::path file = gPackageRoot / usd::kBackendFile;
        void* init = nullptr;
        void* run = nullptr;
#if defined(_WIN32)
        // Hydra's default graphics device is Vulkan, not OpenGL, which would need a window; read once, before Pixar's libraries load.
        if (!std::getenv("HGI_ENABLE_VULKAN"))
            _putenv_s("HGI_ENABLE_VULKAN", "1");
        // Search the package folder first, so Pixar's DLLs and TBB resolve from beside it.
        HMODULE lib = LoadLibraryExW(file.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!lib)
        {
            b.error = "can't load " + utf8(file) + " (error " + std::to_string(GetLastError()) + ")";
            return;
        }
        init = reinterpret_cast<void*>(GetProcAddress(lib, "lilaUsdInit"));
        run = reinterpret_cast<void*>(GetProcAddress(lib, "lilaUsdRun"));
#else
        void* lib = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib)
        {
            b.error = std::string("can't load ") + utf8(file) + ": " + dlerror();
            return;
        }
        init = dlsym(lib, "lilaUsdInit");
        run = dlsym(lib, "lilaUsdRun");
#endif
        if (!init || !run)
        {
            b.error = utf8(file) + " is missing its entry points";
            return;
        }
        std::string error;
        if (reinterpret_cast<usd::BackendInit>(init)(utf8(gPackageRoot).c_str(), &error) != 0)
        {
            b.error = error;
            return;
        }
        b.run = reinterpret_cast<usd::BackendRun>(run);
        b.lib = reinterpret_cast<void*>(lib);
        gLoaded = true;
    });
    return b;
}

using usd::runBackend;
using scene::Graph;
using scene::NodeId;

bool sceneHasStage()
{
    const Graph& g = Graph::current();
    for (NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->type().isA(usd::kStageType))
            return true;
    return false;
}

/** Scene events the stages follow; failures are logged, never thrown into the event loop. */
void forward(const char* event)
{
    try
    {
        runBackend(event, Args{});
    }
    catch (const std::exception& e)
    {
        log::error("OpenUSD: {}", e.what());
    }
}

scene::AttrDef stringAttr(const char* name, const char* nice, uint32_t flags, const char* doc)
{
    scene::AttrDef d;
    d.name = name;
    d.type = scene::PlugType::String;
    d.defaultValue = std::string();
    d.flags = scene::AttrInput | scene::AttrStorable | flags;
    d.niceName = nice;
    d.doc = doc;
    return d;
}

bool registerStageType(ext::Context& ctx)
{
    const scene::NodeType* transform = scene::types::find("transform");
    if (!transform)
        return false;
    using namespace usd::stageAttr;
    const uint32_t hidden = scene::AttrHidden;
    scene::NodeType t{.name = usd::kStageType,
                      .base = transform,
                      .dag = true,
                      .doc = "A live USD stage: the composed file, its prims as objects below it. Edits are written as overs to "
                             "the stage's edit layer; the source files are never changed.",
                      .icon = "stack-2",
                      .create = transform->create};
    t.ownAttrs = {
        stringAttr(kFile, "File", scene::AttrFile, "The USD file the stage composes. It is read, never written."),
        stringAttr(kEditLayer, "Edit layer", scene::AttrFile,
                   "Where edits are written. Empty keeps them inside the .lila; a .usda path writes them to that file, to share "
                   "it; one of the stage's own sublayers edits that file directly."),
        stringAttr(kEditData, "", hidden, "The edit layer's text, when it is kept inside the scene."),
        stringAttr(kMap, "", hidden, "Which object stands for which prim."),
        stringAttr(kBaseline, "", hidden, "What each prim held when its object was last built or written, to tell edits apart."),
        stringAttr(kMaterials, "", hidden, "The material prim each material came from."),
        stringAttr(kLoadRules, "", hidden, "Which payloads are loaded."),
        stringAttr(kStamps, "", hidden, "The source files' times and sizes at the last save, to reload when they change."),
        stringAttr(kMaterialX, "MaterialX", 0,
                   "The MaterialX surface new and edited materials get beside their UsdPreviewSurface: openPBR, standardSurface "
                   "or none. Empty follows the stage: OpenPBR when any of its materials uses MaterialX."),
    };
    lila::scene::relocate::registerNeutral(t.name);
    return scene::types::registerType(ctx, std::move(t)) != nullptr;
}
} // namespace

namespace lila::usd
{
json runBackend(const char* command, const Args& a)
{
    const Backend& b = backend();
    if (!b.run)
        throw CommandError(std::string(command) + ": OpenUSD failed to load: " + b.error);
    json result;
    std::string error;
    if (b.run(command, &a, &result, &error) != 0)
        throw CommandError(error);
    return result;
}

bool backendLoaded() { return gLoaded; }

void* backendSymbol(const char* name)
{
    const Backend& b = backend();
    if (!b.run)
        throw CommandError("OpenUSD failed to load: " + b.error);
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(b.lib), name));
#else
    return dlsym(b.lib, name);
#endif
}

bool isStageNode(NodeId node)
{
    const scene::Node* n = Graph::current().node(node);
    return n && n->type().isA(kStageType);
}

NodeId stageAbove(NodeId node)
{
    const Graph& g = Graph::current();
    for (NodeId p = g.parent(node); p != scene::kNoNode; p = g.parent(p))
        if (isStageNode(p))
            return p;
    return scene::kNoNode;
}
} // namespace lila::usd

USD_BACKEND_EXPORT const char* lilaUsdLoad()
{
    if (tLoading)
        return nullptr;
    const Backend& b = backend();
    return b.run ? nullptr : b.error.c_str();
}

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    gPackageRoot = ctx.root();
    using T = ArgType;
    if (!registerStageType(ctx))
        return 5;
    // Issues aimed at prim paths land on the nodes live stages built for those prims.
    ctx.add(scene::diagnostics::resolvers(), "diagnosticsResolver", "usdPrim",
            scene::diagnostics::Resolver([](std::string_view path) -> scene::NodeId {
                // No backend loaded means no live stage, so no prim has a node yet.
                if (!usd::backendLoaded() || path.empty() || path.front() != '/')
                    return scene::kNoNode;
                static auto find = reinterpret_cast<usd::BackendNodeForPrim>(usd::backendSymbol("lilaUsdNodeForPrim"));
                return find ? scene::NodeId(find(std::string(path).c_str())) : scene::kNoNode;
            }));
    // USD's API schemas in the Add popover. Listing them loads OpenUSDCore, so it waits for the first time Add opens.
    scene::attrComponents::addSource(ctx, "usd.schemas", [] {
        static std::vector<scene::attrComponents::Component> list = [] {
            std::vector<scene::attrComponents::Component> out;
            try
            {
                for (const nlohmann::json& s : usd::runBackend("usdComponentSchemas", Args{}))
                {
                    scene::attrComponents::Component c;
                    c.name = c.usdSchema = s.at("name").get<std::string>();
                    c.label = s.value("label", "");
                    c.source = "usd";
                    c.doc = s.value("doc", "");
                    for (const nlohmann::json& a : s.value("attrs", nlohmann::json::array()))
                        c.attrs.push_back(scene::attrDefFromJson(a));
                    out.push_back(std::move(c));
                }
            }
            catch (const std::exception& e)
            {
                log::warn("OpenUSD: no schema list for components: {}", e.what());
            }
            return out;
        }();
        return list;
    });
    if (!ctx.registerCommand({
            .name = "importUsd",
            .doc = "Import a USD file (.usd, .usda, .usdc or .usdz) into the scene. The stage is opened and composed first, so "
                   "references, payloads, sublayers and variant selections all resolve, and what arrives is the composed result.\n\n"
                   "Prims become nodes with their names and hierarchy: Xform, Scope and untyped prims transforms (every xformOp "
                   "kind and order resolved; translate, rotate in any order, scale and pivot land on the channels as authored), Mesh prims meshes with their n-gons, "
                   "normals, every UV set (st, st1, ... in order, then other texCoord primvars), displayColor/displayOpacity and "
                   "other color primvars as color sets, other numeric primvars as mesh attributes, edge creases, holes left out and "
                   "materialBind GeomSubsets as material slots. Cube, Sphere, Cylinder, Cone, Capsule and Plane are tessellated to meshes. "
                   "Cameras keep their lens, film back, clipping and projection; BasisCurves (linear, bezier, bspline, catmullRom; "
                   "periodic and pinned) and NurbsCurves become curve nodes. Invisible prims arrive hidden; proxy and guide prims are "
                   "skipped unless purposes asks for them.\n\n"
                   "UsdPreviewSurface materials become Lila materials (Unreal's default lit) with their textures: packed "
                   "occlusion/roughness/metallic textures as an ORM or MRO map, normal maps' green convention read from the texture's "
                   "scale, emissive intensity from its scale, opacity from the base color texture's alpha as masked or translucent, and "
                   "each texture's wrapS, wrapT and magFilter as its map's wrap and filter. Unlit materials Lila wrote become unlit "
                   "materials. MaterialX networks (outputs:mtlx:surface: OpenPBR, Standard Surface, glTF PBR, UsdPreviewSurface and "
                   "unlit surfaces, also from referenced .mtlx files) map the same way, preferred over a UsdPreviewSurface beside "
                   "them: image and tiledimage nodes with their UV set, address modes and filter, normalmap, and constant, multiply, "
                   "add, mix, extract, separate and convert nodes that fold to a constant or one texture's channels. What has no "
                   "Lila equivalent (coat, transmission, subsurface, other node kinds) is listed in the report's skipped entries. "
                   "Direct and collection bindings both resolve. Textures inside a "
                   ".usdz are unpacked beside it. Meshes with time-sampled points get a meshCache deformer playing every sample "
                   "(interpolated between them); a mesh whose topology changes over time keeps its rest frame.\n\n"
                   "UsdSkel skeletons become joints, SkelBindingAPI skins skinClusters with their weights and bind pose, BlendShapes "
                   "morph targets with their in-betweens, and SkelAnimation keys on the joints and target weights. Time samples become "
                   "keys (linear, or stepped on a held stage), and the stage's timeCodesPerSecond, framesPerSecond and time range set the "
                   "scene's fps and playback range. Instanceable prims and PointInstancers are realized: each instance its own "
                   "transform, sharing the prototype's mesh data.\n\n"
                   "Axes and units convert as FBX import does: a Z-up stage is read in Unreal's axes, a Y-up one as Maya's, and "
                   "metersPerUnit scales everything to cm. Lights, point clouds and anything else Lila has no node for are listed in "
                   "the report's skipped entries (lights and point clouds keep their place as transforms). Returns the new node names, "
                   "or with report=True a dict of nodes, roots, joints, skinClusters, cameras, curves, materials, take, takes, fps, "
                   "start, end, keyedCurves, instances, upAxis, metersPerUnit, lightingChannels, skipped and warnings.",
            .positionalDoc = "path",
            .flags = {nodeFlag({"parent", "p", T::String, nullptr, "Parent the imported roots under this node."}, "transform"),
                      {"prim", "", T::String, "", "Import only this prim and what is under it, by its path in the stage (\"/World/Hero\")."},
                      {"skins", "", T::Bool, true, "Rebuild skeleton bindings as skinClusters; False imports skinned meshes in their bind shape."},
                      {"animation", "anim", T::Bool, true, "Turn time samples (transforms, joints, blend shape weights, camera lens) into keys."},
                      {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the stage."},
                      {"materials", "", T::Bool, true, "Create materials from the bound UsdPreviewSurface networks and assign them."},
                      {"loadPayloads", "", T::Bool, true, "Load payloads; False leaves them out and lists them as skipped."},
                      {"georeference", "", T::Bool, true,
                       "Place it by the georeference it carries (a georeferenced Lila export, or a Cesium georeference prim)."},
                      {"variants", "", T::Any, nullptr,
                       "Variant selections before importing: {'set': 'variant'} for every prim with that set, or "
                       "{'/prim/path': {'set': 'variant'}} for one prim.",
                       {},
                       "importUsd(\"chair.usd\", variants={\"modelingVariant\": \"Rocking\"})"},
                      {"take", "", T::String, "", "For files Lila exported with several takes: the take to import (a variant of the root's 'take' set)."},
                      {"purposes", "", T::Any, nullptr,
                       "Prim purposes to import: a list of default, render, proxy and guide, or 'all'. Default and render by default."},
                      {"lightingChannels", "lc", T::Bool, true,
                       "Bake UsdLux light-link collections into lighting channels (Lila's own channel names when it wrote the file)."},
                      {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                      {"select", "", T::Bool, true, "Select the imported roots."}},
            .run = [](const Args& a) { return runBackend("importUsd", a); },
            .examples = {"importUsd(\"C:/assets/kitchen.usd\")", "importUsd(\"hero.usdz\", report=True)",
                         "importUsd(\"set.usd\", loadPayloads=False, variants={\"lod\": \"proxy\"})"},
        }))
        return 2;

    if (!ctx.registerCommand({
            .name = "exportUsd",
            .doc = "Export to a USD file. The extension picks the format: .usda text, .usdc binary, .usd binary (or text with "
                   "format='usda') and .usdz, a package holding the stage and its textures. Objects default to the selection (or "
                   "every root with selectionOnly=False); their descendants come too.\n\n"
                   "Defaults suit Unreal's USD importer: Z up, metersPerUnit 0.01 (centimetres), UsdPreviewSurface materials and "
                   "UsdSkel. The stage has one root prim named after the file, its defaultPrim, with the exported nodes under it and "
                   "the materials in a Materials scope. Transforms are written as translate, rotate (in the node's rotate order) and "
                   "scale ops, or a matrix when sheared. Meshes keep their n-gons (triangulate=True writes the viewport's triangles), "
                   "split normals, every UV set (as st, st1, ...), the current color set as displayColor/displayOpacity and the "
                   "others as color primvars, edge creases, and a GeomSubset per material slot when there are several. Materials map "
                   "to UsdPreviewSurface: a UsdUVTexture per map read through one UsdPrimvarReader_float2, packed ORM/MRO maps as "
                   "one texture feeding occlusion, roughness and metallic, DirectX normal maps flipped to USD's OpenGL convention by "
                   "the texture's scale and bias, emissive intensity as the texture's scale, masked and translucent opacity from the "
                   "base color's alpha, each map's wrap as wrapS and wrapT (nearest filtering as magFilter and minFilter), unlit "
                   "materials as their base color emitted over a black surface, and twoSided as doubleSided on the meshes using it. "
                   "materialX='openPBR' (or 'standardSurface') adds the same material as a MaterialX network under "
                   "outputs:mtlx:surface, which previewSurface=False writes alone. "
                   "Meshes a meshCache drives write their points as time samples.\n\n"
                   "Joints become a Skeleton (bind and rest transforms) under a SkelRoot shared with the meshes skinned to it; skins "
                   "write jointIndices and jointWeights (pruned to maxInfluences) and the mesh's geomBindTransform, in the bind pose. "
                   "Morph targets become BlendShapes with their in-betweens; a mesh with morphs and no skin gets a one-joint skeleton "
                   "of its own so Unreal imports them. Nodes parented to joints are written beside the skeleton, their joint's motion "
                   "baked into them. Cameras write their lens, film back and clipping. Curves write as BasisCurves (linear or cubic "
                   "bezier) or, for NURBS, NurbsCurves. LOD groups whose levels are plain meshes become Unreal's LOD variant set.\n\n"
                   "Animation is sampled every frame over the range and written as time samples, at the take's fps "
                   "(timeCodesPerSecond and framesPerSecond) with the stage's start and end time codes. One take goes straight "
                   "into the file. Several (takes='all' or a list of names) become a variant set named 'take' on the root prim, one "
                   "variant per take with its own samples and its range kept in the prim's customData; the first take is selected, "
                   "which is what Unreal and other tools play, and importUsd(take=...) picks another. splitLayers=True writes the "
                   "geometry, materials and animation into sublayers (<file>.geometry, .materials and .anim) that the file "
                   "composes. Lighting channels become UsdLux light links (lightingChannels). Returns the path, or with report=True "
                   "the files written, the warnings and lightingChannels: {channels, lights, objects, written}.",
            .positionalDoc = "path objects...",
            .flags = {{"format", "", T::String, "auto", "auto picks by extension; usda or usdc chooses the encoding of a .usd file.", {"auto", "usda", "usdc"}},
                      {"upAxis", "up", T::String, "z", "File up axis: z (Unreal) or y. Assets land the same way through either, as with FBX.", {"z", "y"}},
                      {"metersPerUnit", "mpu", T::Float, 0.01, "Scene unit of the file in metres: 0.01 is centimetres (Unreal), 1 metres. Lengths are scaled to it.", {}, {}, 1e-6},
                      {"materials", "mat", T::Bool, true, "Write UsdPreviewSurface materials and bind them."},
                      {"materialX", "mx", T::String, "none",
                       "Also write each material as a MaterialX network under outputs:mtlx:surface: openPBR (OpenPBR Surface, the "
                       "successor to Standard Surface that renderers are converging on) or standardSurface.",
                       {"none", "openPBR", "standardSurface"}},
                      {"previewSurface", "", T::Bool, true, "Write the UsdPreviewSurface network; False writes only the MaterialX one."},
                      {"textures", "tex", T::String, "relative",
                       "How texture files are referenced: relative to the file, absolute, or copy (copied into <name>_textures beside "
                       "the file). A .usdz packages them whatever this says.",
                       {"relative", "absolute", "copy"}},
                      {"animation", "anim", T::Bool, true, "Write the animation as time samples."},
                      {"takes", "", T::Any, nullptr, "Scene takes to write: 'all' or a list of names, one 'take' variant each. Default: the active take.",
                       {}, "exportUsd(\"hero.usd\", \"Hero\", takes=\"all\")"},
                      {"start", "st", T::Float, nullptr, "First frame; the playback start by default."},
                      {"end", "et", T::Float, nullptr, "Last frame; the playback end by default."},
                      {"normals", "", T::Bool, true, "Write the split normals; False leaves them for the importer to compute."},
                      {"triangulate", "", T::Bool, false, "Write triangles exactly as the viewport splits faces instead of the n-gons."},
                      {"maxInfluences", "mi", T::Int, 8, "Skin influences per point, heaviest kept and renormalised (4 for mobile).", {}, {}, 0.0},
                      {"deformOnly", "do", T::Bool, true, "Deform joints only (IK helpers and rig-only joints stay behind)."},
                      {"atOrigin", "", T::Bool, false, "Drop the exported roots' own transforms, keeping their children relative."},
                      {"children", "", T::Bool, true, "Include the descendants of the given objects."},
                      {"worldSpace", "ws", T::Bool, false, "Bake world transforms into the meshes and flatten the hierarchy (current pose, no skins or animation)."},
                      {"lods", "", T::Bool, true, "Write LOD groups as Unreal's LOD variant set; False writes every level as a plain child."},
                      {"splitLayers", "", T::Bool, false, "Geometry, materials and animation in their own sublayers, composed by the file."},
                      {"lightingChannels", "lc", T::Bool, true,
                       "Write the lighting channels as UsdLux light links: each light's lightLink collection holds the meshes "
                       "sharing a channel with it. The report and log list the channels, lights and objects either way."},
                      {"selectionOnly", "sl", T::Bool, true, "Without objects: the selection, or every root when False."},
                      {"report", "", T::Bool, false, "Return {files, warnings, lightingChannels} instead of the path."}},
            .undoable = false,
            .run = [](const Args& a) { return runBackend("exportUsd", a); },
            .examples = {"exportUsd(\"C:/out/hero.usd\", \"Hero\")", "exportUsd(\"C:/out/hero.usdz\", \"Hero\", textures=\"copy\")",
                         "exportUsd(\"C:/out/shot.usda\", selectionOnly=False, takes=\"all\", splitLayers=True)"},
        }))
        return 3;

    if (!ctx.registerCommand({
            .name = "importMaterialX",
            .doc = "Import the materials of a MaterialX document (.mtlx) as Lila materials, mapped as importUsd maps MaterialX "
                   "networks: OpenPBR, Standard Surface, glTF PBR, UsdPreviewSurface and unlit surfaces with their image "
                   "textures, UV sets, address modes and filters, normal maps and the constant and arithmetic nodes that fold "
                   "to a value or one texture's channels. Documents Lila wrote come back with every setting. A material whose "
                   "name the scene already has is left as it is. Returns the new material names, or with report=True a dict of "
                   "materials, created, skipped and warnings.",
            .positionalDoc = "path",
            .flags = {{"report", "", T::Bool, false, "Return the import report instead of just the names."}},
            .run = [](const Args& a) { return runBackend("importMaterialX", a); },
            .examples = {"importMaterialX(\"C:/library/metals.mtlx\")"},
        }))
        return 7;
    if (!ctx.registerCommand({
            .name = "exportMaterialX",
            .doc = "Write Lila materials to a MaterialX document (.mtlx): each a surfacematerial with its shader and a nodegraph "
                   "of image nodes (UV set as texcoord index, wrap as address modes, nearest filtering as closest), packed "
                   "ORM/MRO maps split by a separate3 node, normal maps through normalmap (DirectX maps flipped to MaterialX's "
                   "OpenGL convention), emissive and opacity from the base color's alpha. Unlit materials use surface_unlit. "
                   "Settings MaterialX has no input for are kept on the material node for importMaterialX. Objects stand for "
                   "the materials their meshes use; without any, the selection's, or every material. Returns the path, or "
                   "with report=True {path, materials, warnings}.",
            .positionalDoc = "path materialsOrObjects...",
            .flags = {{"surface", "", T::String, "openPBR",
                       "The lit surface: openPBR (OpenPBR Surface, where the industry is converging) or standardSurface.",
                       {"openPBR", "standardSurface"}},
                      {"textures", "tex", T::String, "relative",
                       "How texture files are referenced: relative to the file, absolute, or copy (into <name>_textures beside it).",
                       {"relative", "absolute", "copy"}},
                      {"selectionOnly", "sl", T::Bool, true, "Without names: the selection's materials; False writes every material."},
                      {"report", "", T::Bool, false, "Return {path, materials, warnings} instead of the path."}},
            .undoable = false,
            .run = [](const Args& a) { return runBackend("exportMaterialX", a); },
            .examples = {"exportMaterialX(\"C:/out/props.mtlx\", \"Brick\", \"Glass\")",
                         "exportMaterialX(\"C:/out/all.mtlx\", selectionOnly=False, surface=\"standardSurface\")"},
        }))
        return 8;

    const auto stageCommand = [&](CommandDef def) {
        def.run = [command = def.name](const Args& a) { return runBackend(command.c_str(), a); };
        return ctx.registerCommand(std::move(def)) != 0;
    };
    const bool stagesOk =
        stageCommand({
            .name = "usdStageOpen",
            .doc = "Open a USD file as a live stage: a usdStage node holding the composed stage, its prims built below it as "
                   "objects. Nothing is flattened: variants, payloads, references and sublayers stay live. Edits to the objects "
                   "are written as overs to the stage's edit layer when the scene saves (and before a reload or a variant or "
                   "payload switch); the source files are never changed. File > Open and Import of a USD file run this; "
                   "importUsd flattens instead.\n\n"
                   "Written: moving objects (with animation), hiding them, deleting them (the prim is deactivated), mesh edits, "
                   "material assignments and edits, new objects under the stage, objects moved to another parent, joint poses "
                   "and keys, new joints, skin weights, blend shapes and their weights, variant selections and payload "
                   "loading. What can't be written is listed as a warning when saving. Returns the stage node, or with "
                   "report=True a dict of stage, prims, nodes, materials, skipped and warnings.",
            .positionalDoc = "path",
            .flags = {nodeFlag({"parent", "p", T::String, nullptr, "Parent the stage node under this node."}, "transform"),
                      {"name", "n", T::String, nullptr, "The stage node's name; the file's name by default."},
                      {"editLayer", "el", T::String, "",
                       "Where edits go: empty keeps them inside the .lila, a .usda path writes them to that file, one of the "
                       "stage's sublayers edits it directly."},
                      {"loadPayloads", "", T::Bool, true, "Load payloads; False leaves them unloaded (usdStageLoad loads them)."},
                      {"materialX", "mx", T::String, "",
                       "The MaterialX surface new and edited materials get beside their UsdPreviewSurface: openPBR, "
                       "standardSurface or none. Empty follows the stage: OpenPBR when it already uses MaterialX."},
                      {"variants", "", T::Any, nullptr,
                       "Variant selections, written to the edit layer: {'set': 'variant'} for every prim with that set, or "
                       "{'/prim/path': {'set': 'variant'}} for one prim.",
                       {},
                       "usdStageOpen(\"chair.usd\", variants={\"modelingVariant\": \"Rocking\"})"},
                      {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the stage."},
                      {"report", "", T::Bool, false, "Return the report instead of just the stage node."},
                      {"select", "", T::Bool, true, "Select the stage node."}},
            .examples = {"usdStageOpen(\"C:/assets/kitchen.usd\")", "usdStageOpen(\"shot.usda\", editLayer=\"shot_lila.usda\")"},
        }) &&
        stageCommand({
            .name = "usdStageReload",
            .doc = "Read a stage's files again, keeping its edits. Edits made in Lila are written to the edit layer first; the "
                   "objects of the prims the files changed are rebuilt. A scene also reloads its stages on open when their "
                   "source files changed since it was saved. Returns {stage: {reloaded, updated}}.",
            .positionalDoc = "stages... (default all)",
            .undoable = false,
            .examples = {"usdStageReload()", "usdStageReload(\"kitchen\")"},
        }) &&
        stageCommand({
            .name = "usdStageUpdate",
            .doc = "Take in what other tools and scripts changed in the stages' layers: files changed on disk are read again "
                   "and the objects of the prims they touch rebuilt. Lila does this by itself while idle. Returns how many "
                   "prims were rebuilt.",
            .undoable = false,
        }) &&
        stageCommand({
            .name = "usdStageVariant",
            .doc = "Switch a variant on a stage, written to its edit layer. The prim's objects are rebuilt from the new "
                   "selection. Without prim, every prim with that variant set switches. Returns {prim: variant}.",
            .positionalDoc = "stage (default the selected one)",
            .flags = {{"set", "s", T::String, nullptr, "The variant set."},
                      {"variant", "v", T::String, nullptr, "The variant to select."},
                      {"prim", "", T::String, "", "The prim, by its path in the stage (\"/World/Chair\")."}},
            .examples = {"usdStageVariant(\"kitchen\", prim=\"/Kitchen/Chair\", set=\"style\", variant=\"metal\")"},
        }) &&
        stageCommand({
            .name = "usdStageLoad",
            .doc = "Load or unload the payloads at and below a prim of a stage, rebuilding its objects. The load state is "
                   "saved with the scene. Returns {loaded: [prim paths]}.",
            .positionalDoc = "stage (default the selected one)",
            .flags = {{"prim", "", T::String, "/", "The prim, by its path in the stage; / is every payload."},
                      {"load", "l", T::Bool, true, "False unloads."}},
            .examples = {"usdStageLoad(\"set\", prim=\"/Set/Building\")", "usdStageLoad(\"set\", load=False)"},
        }) &&
        stageCommand({
            .name = "usdStageExtract",
            .doc = "Turn stage objects into plain Lila objects: each one moves out of its stage, keeping its place, modifiers "
                   "and everything below it, and its prim is deactivated in the edit layer. Use it before adding modifiers "
                   "or deformers that should stay live, since a stage's layer holds only their result. Returns the objects.",
            .positionalDoc = "objects (default the selection)",
        }) &&
        stageCommand({
            .name = "usdStageWriteShot",
            .doc = "Write a stage's edits as a small .usda that sublayers the source, ready to open in Houdini or Maya or to "
                   "sublayer elsewhere. Edits made in Lila are written first. Returns the path.",
            .positionalDoc = "path stage (default the selected one)",
            .undoable = false,
            .examples = {"usdStageWriteShot(\"C:/shots/sh010_lila.usda\", \"kitchen\")"},
        }) &&
        stageCommand({
            .name = "usdStageAddLayer",
            .doc = "Add a layer to a stage, under its edits and over its source: a new .usda (with the stage's up axis and units) "
                   "when the file doesn't exist, else that file. It is listed as a sublayer of the edit layer, so the source "
                   "files stay unchanged, and usdStageWriteShot carries it along. Other tools write into it; Lila picks up "
                   "their changes. Returns the added layers, strongest first.",
            .positionalDoc = "path stage (default the selected one)",
            .flags = {{"position", "", T::Int, 0, "Where among the added layers: 0 is the strongest."}},
            .examples = {"usdStageAddLayer(\"C:/shots/sh010_lighting.usda\", \"kitchen\")"},
        }) &&
        stageCommand({
            .name = "usdStageRemoveLayer",
            .doc = "Take a layer usdStageAddLayer added out of a stage. The file stays on disk. Returns the added layers left.",
            .positionalDoc = "path stage (default the selected one)",
            .examples = {"usdStageRemoveLayer(\"C:/shots/sh010_lighting.usda\", \"kitchen\")"},
        }) &&
        stageCommand({
            .name = "usdStageInfo",
            .doc = "A stage's file, edit layer (mode embedded, file or sublayer, and path), the layers it reads, its variant "
                   "sets with their selections, its payloads and whether they are loaded, how many prims have objects, and "
                   "the warnings of its last save. edits=True adds the edit layer's text.",
            .positionalDoc = "stage (default the selected one)",
            .flags = {{"edits", "", T::Bool, false, "Include the edit layer as usda text."}},
            .undoable = false,
        }) &&
        stageCommand({
            .name = "usdStagePrim",
            .doc = "Every composed property of a stage object's prim, custom schemas included: {prim, type, schemas, "
                   "instanceProxy, properties: [{name, type, value, custom, timeSamples, authored, losesTo}]}. losesTo names the layer "
                   "whose stronger opinion hides the edit layer's. Values are usda text "
                   "at the current time; long arrays are summarized unless full=True; relationships list their targets. An "
                   "object inside an LOD level adds level, the level's variant.",
            .positionalDoc = "object",
            .flags = {{"full", "", T::Bool, false, "Every value in full, however long."}},
            .undoable = false,
        }) &&
        stageCommand({
            .name = "usdStageOpinions",
            .doc = "Every opinion on a stage object's prim, strongest first, to see why a value isn't the one you set: {prim, "
                   "specs: [{layer, path, arc, role, specifier, type, active, kind, instanceable, variantSelections}], "
                   "properties: [{name, opinions: [{layer, path, arc, role, value, timeSamples, targets, wins}], fallback, "
                   "losesTo}]}. arc is how the layer reaches the prim (local, reference, payload, variant, inherit, "
                   "specialize, relocate); role marks Lila's edit layer (edits), added layers (added) and the session layer. "
                   "wins marks the opinion an attribute's value comes from; fallback, that none has one and the schema's "
                   "default shows; losesTo, the layer that beats the edit layer's opinion. Give a property for that one only.",
            .positionalDoc = "object [property]",
            .flags = {{"full", "", T::Bool, false, "Every value in full, however long."}},
            .undoable = false,
            .examples = {"usdStageOpinions(\"Chair\", \"xformOp:translate\")"},
        }) &&
        stageCommand({
            .name = "usdStageSetProperty",
            .doc = "Set a property of a stage object's prim, written as an over to the edit layer; the object updates to "
                   "match. The value is usda text (\"(1, 2, 3)\", \"[1, 2]\") or a number, bool, string "
                   "or list. An attribute with time samples gets a sample at the current time. A property the prim lacks is "
                   "added, custom, when type gives its USD type (\"float\", \"token\", \"double3\"). Relationships take "
                   "a list of target paths. Returns the property as usdStagePrim lists it.",
            .positionalDoc = "object property value",
            .flags = {{"type", "", T::String, nullptr, "The USD value type, to add an attribute the prim doesn't have."}},
            .examples = {"usdStageSetProperty(\"Chair\", \"xformOp:translate\", \"(0, 0, 50)\")",
                         "usdStageSetProperty(\"Chair\", \"studio:assetId\", \"CH_014\", type=\"string\")"},
        });
    if (!stagesOk)
        return 6;

    ctx.subscribe(events::Event::SceneNew, [](const events::EventData&) {
        if (usd::backendLoaded())
            forward("usdStage.sceneNew");
    });
    ctx.subscribe(events::Event::SceneOpened, [](const events::EventData&) {
        if (usd::backendLoaded() || sceneHasStage())
            forward("usdStage.sceneOpened");
    });
    ctx.subscribe(events::Event::SceneBeforeSave, [](const events::EventData&) {
        if (sceneHasStage())
            forward("usdStage.beforeSave");
    });
    ctx.subscribe(events::Event::AppIdle, [](const events::EventData&) {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (!usd::backendLoaded() || now - last < std::chrono::seconds(1))
            return;
        last = now;
        forward("usdStage.idle");
    });

    if (!io::translators::add(ctx, {.id = "usd",
                                    .label = "USD",
                                    .doc = "Universal Scene Description through OpenUSD: Open and Import make a live stage (usdStage), "
                                           "Export writes meshes, materials, skeletons, cameras, lights, curves and animation.",
                                    .extensions = {".usd", ".usda", ".usdc", ".usdz"},
                                    .import = {.command = "usdStageOpen"},
                                    .exportSelection = {.command = "exportUsd"}}))
        return 4;
    if (!io::translators::add(ctx, {.id = "mtlx",
                                    .label = "MaterialX",
                                    .doc = "MaterialX documents: Import brings in their materials, Export writes the selection's materials.",
                                    .extensions = {".mtlx"},
                                    .import = {.command = "importMaterialX"},
                                    .exportSelection = {.command = "exportMaterialX"}}))
        return 9;
    usd::registerUi(ctx);
    usd::registerHydra(ctx);
    return 0;
}
