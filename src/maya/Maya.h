#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Maya scene import, without any Autodesk library: .ma is parsed as the MEL subset Maya writes, .mb is decoded from
 * its IFF chunks into the same statements (MayaFile.h), and one converter turns those into Lila nodes.
 *
 * What converts: transforms (translate, rotate, scale, rotate order, pivots and their translates, visibility),
 * hierarchy, meshes (points, tweaks, faces, uv sets, color sets, locked normals, hard edges, edge creases), primitive
 * creators (polyCube, polySphere, ... as live Lila generators when nothing else follows them), a few history nodes
 * evaluated (polyTweak, polyTweakUV, transformGeometry, polySmoothFace, polyTriangulate, polySoftEdge), joints
 * (jointOrient, radius, segment scale compensate, inverse scale), skinClusters (weights, bind pre-matrices, geometry
 * matrix), blendShapes (as morphs: inline and live targets, in-betweens, target and base weights, animated and
 * set-driven-key weights), locators, cameras (lens, film back, clipping, orthographic, Lock Camera, animated focal length and clipping),
 * image planes (free ones, ones on the default cameras as Lila's attached planes, ones on other cameras placed against
 * their lens), lambert / blinn / phong / standardSurface / aiStandardSurface / openPBRSurface materials with file
 * textures and per-face shading group assignments, time animation curves on transform and camera channels,
 * display layers (visibility, hide on playback, template / reference, colour, members), namespaces, and referenced
 * Maya, FBX and OBJ files.
 *
 * Rigs (MayaRig.h): offset parent matrices, numeric custom attributes, parent / point / orient / aim / scale / pole
 * vector constraints with their offsets and weights, ikHandles (rotate plane and single chain, pole vectors, twist,
 * stretch by driven joint lengths), cluster, lattice, nonLinear, softMod, deltaMush, tension, wrap, proximityWrap,
 * jiggle, shrinkWrap and textureDeformer, utility and matrix node networks (MathNodes.h), set driven keys alone or
 * summed by blendWeighted, curves on any converted plug, follicles, uvPins and proximityPins. MEL expressions, spline
 * IK, motion paths and other curve or surface nodes, wire and sculpt deformers, HumanIK and plugin nodes stay as
 * their last pose and keys, listed in the report.
 *
 * Maya's axes are right-handed, Y up unless the scene's default cameras say Z; units come from currentUnit.
 */
namespace lila::maya
{
namespace fs = std::filesystem;

struct File;

struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Prefix every imported name with this namespace ("ns" gives "ns:name"). */
    std::string nameSpace;
    /** "auto" (from the default cameras), "y" or "z". */
    std::string upAxis = "auto";
    bool skins = true;
    /** blendShapes as morphs, their weight curves and set driven keys as anim curves and driven channels. */
    bool morphs = true;
    bool animation = true;
    bool materials = true;
    /**
     * "reference": each referenced file becomes a Lila reference under its namespace (Maya files are converted to a
     * .lila beside them first), with the scene's reference edits applied as reference overrides. "import": their
     * nodes come in as ordinary scene nodes, edits applied. "none": skipped.
     */
    std::string references = "reference";
    /** Keep a primitive creator with nothing after it as a live generator node instead of a baked mesh. */
    bool history = true;
    /** Set the scene fps and playback range from the file. */
    bool setPlayback = true;
    /** Bake the lightLinker's light links into lighting channels. */
    bool lightingChannels = true;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every DAG node created, parents first
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins;
    std::vector<scene::NodeId> morphs;
    std::vector<scene::NodeId> cameras;
    std::vector<scene::NodeId> lights;
    std::vector<scene::NodeId> imagePlanes;
    std::vector<std::string> materials;
    std::vector<std::string> displayLayers;
    /** Lighting channels made from the file's light links. */
    std::vector<std::string> lightingChannels;
    /** Lila references created (their namespaces), and .lila files written for referenced files. */
    std::vector<std::string> references;
    std::vector<std::string> converted;
    /** Maya DAG path of each converted node to its Lila name. */
    std::map<std::string, std::string> mayaNames;
    int curves = 0;
    /** Things in the file that were not converted, one line each. */
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    std::string format; // "ascii" or "binary"
    std::string version;
    std::string upAxis;
    double unitCm = 1.0;
    double fps = 0.0;
};

/** Main thread. Undoable as one step when run inside a command. */
LILA_SCENE_API ImportResult importMaya(scene::Graph& graph, const fs::path& path, const ImportOptions& opts = {});
/** The same from a file already read with readFile (MayaFile.h), the slow part that touches nothing and can run first, off the main thread. */
LILA_SCENE_API ImportResult importMaya(scene::Graph& graph, const fs::path& path, const File& file, const ImportOptions& opts = {});

/**
 * The .lila a reference to this Maya file loads: <stem>.lila beside it, converted now unless a current conversion is
 * already there. Leaves the scene alone. Empty with report.error set on failure.
 */
LILA_SCENE_API fs::path lilaForReference(const fs::path& path, const ImportOptions& opts, ImportResult& report);

/** mayaImport, mayaOpen, mayaConvert, mayaInfo, mayaReference. Needs the scene and io commands. */
LILA_SCENE_API void registerCommands();

/** True for .ma / .mb (case-insensitive). */
LILA_SCENE_API bool isMayaFile(const fs::path& path);
} // namespace lila::maya
