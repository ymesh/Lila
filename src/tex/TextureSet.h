#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "scene/Graph.h"
#include "tex/Plan.h"
#include "tex/Ports.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * Texture sets: a scene node ("textureSet") holding the texture graph and the settings its maps are made with. Both are
 * JSON string attrs, so scenes save them (.lila and .lilaa alike) and every edit undoes. A set knows nothing of materials:
 * a material picks a set (or single maps of one) the way it picks a texture file.
 */
namespace lila::tex
{
inline constexpr const char* kTextureSetType = "textureSet";

struct Channel
{
    std::string name;
    PortType type = PortType::Gray;
    /** 8, 16 or 32 (float). */
    int bits = 8;
};

/** One UDIM tile's size; 0 takes the set's. */
struct Tile
{
    int udim = 1001;
    int width = 0;
    int height = 0;
};

struct LILA_SCENE_API SetSettings
{
    std::vector<Channel> channels;
    int width = 2048;
    int height = 2048;
    std::vector<Tile> tiles = {Tile{}};
    /** How the normal channel is written when no export preset decides: opengl (Y+) or directx (Y-). Graphs work in opengl. */
    std::string normalConvention = "opengl";
    /** The bakeProject whose maps feed the set's mesh inputs; empty for none. */
    std::string bakeProject;
    /** A bake target name (bake/Targets.h) texExport uses by default. */
    std::string exportPreset = "unreal";
    /** cm from height 0 to height 1: the relief the height channel stands for. */
    double heightDepth = 5.0;
    /** World size of the UV square along U and V, cm. With heightDepth, what turns height steps into slopes. */
    double uvWidth = 100.0;
    double uvHeight = 100.0;
    /**
     * Exports write the normal for a material that rebuilds the shape's normals from the height (Normals From Height,
     * Unreal's MF_LilaHeightNormal): the detail left once the height's own normals are rotated out, faded on slopes steeper
     * than normalFade degrees. The height is exported with it, at 16 bits.
     */
    bool normalFromHeight = false;
    double normalFade = 45.0;
    /** The height an engine rebuilds normals from: channel (the set's height) or mesh (the bake project's height map). */
    std::string heightSource = "channel";
    /** Named face sets for painting: {name: {mesh, faces: [...]}}, used as masks, fill targets and locks. */
    json faceGroups = json::object();
    /** Saved projection views by name: the screen view JSON texPaintStroke takes, for projecting again from the same place. */
    json projectionViews = json::object();
    /** The texture repeats (a trim sheet, a tiling material): strokes wrap round the uv square's edges. */
    bool tileable = false;

    /** heightDepth over the UV square's size: the height's slope per UV unit along u and v. */
    glm::dvec2 heightSlope() const { return {heightDepth / std::max(uvWidth, 1e-3), heightDepth / std::max(uvHeight, 1e-3)}; }

    const Channel* channel(std::string_view name) const;
    std::pair<int, int> tileSize(const Tile& t) const;

    json toJson() const;
    /** Missing keys keep their defaults. Throws std::invalid_argument on a bad value. */
    static SetSettings fromJson(const json& j);
    /** The standard channels: baseColor, roughness, metallic, normal, height (16 bit), ao, emissive. */
    static SetSettings makeDefault();
};

/** Node type and listeners. Call after the scene's builtins. */
LILA_SCENE_API void registerTypes();
/** texCreateSet, texAddNode, texConnect, ... */
LILA_SCENE_API void registerCommands();

LILA_SCENE_API scene::Node* findSet(scene::Graph& g, std::string_view name);
LILA_SCENE_API std::vector<std::string> setNames(scene::Graph& g);
/**
 * The set painting on the material's faces goes into: the set it picks whole, else the set of its first per-map pick;
 * empty when it uses none (or there is no such material).
 */
LILA_SCENE_API std::string setForMaterial(scene::Graph& g, std::string_view material);
/** The materials using the set, whole or for one of their maps, by name in scene order. */
LILA_SCENE_API std::vector<std::string> materialsUsing(scene::Graph& g, std::string_view set);
/** Points the materials at the whole set. Undoable (inside a command). Throws std::invalid_argument for a name that is not a material. */
LILA_SCENE_API void assignSet(scene::Graph& g, std::string_view set, const std::vector<std::string>& materials);
/** The name a material's new set gets: the material's name without its M_ or MI_ prefix, plus Tex. */
LILA_SCENE_API std::string defaultSetName(std::string_view material);
/** Throw std::invalid_argument when the stored JSON is unreadable. */
LILA_SCENE_API Graph readGraph(const scene::Node& set);
LILA_SCENE_API SetSettings readSettings(const scene::Node& set);
/** A hash of the set's stored graph and settings: equal stamps mean texGraphInfo would answer the same. */
LILA_SCENE_API size_t setStamp(const scene::Node& set);
/** Undoable writes (inside a command). */
LILA_SCENE_API void writeGraph(scene::Graph& g, scene::Node& set, const Graph& graph);

/**
 * "Set" names a set's graph, "Set/Group" one of its node groups. A set keeps every group in one flat subgroups map on its
 * own graph, and the graphs inside leave theirs empty so their Group nodes resolve against the set's.
 */
struct GraphAddress
{
    std::string set;
    std::string group;
};
LILA_SCENE_API GraphAddress splitAddress(std::string_view address);
LILA_SCENE_API std::string joinAddress(std::string_view set, std::string_view group);
/** True when the address names a set, and a group the set has. */
LILA_SCENE_API bool hasGraphAt(scene::Graph& g, std::string_view address);
/**
 * The graph at an address. A group comes back carrying the set's subgroups, so its Group nodes and new groups made in it
 * work as in the set's graph. Throws std::invalid_argument naming what is missing.
 */
LILA_SCENE_API Graph readGraphAt(scene::Graph& g, std::string_view address, scene::Node** set = nullptr);
/** Writes back what readGraphAt read, undoably: a group's subgroups go to the set's map. */
LILA_SCENE_API void writeGraphAt(scene::Graph& g, std::string_view address, const Graph& graph);
LILA_SCENE_API void writeSettings(scene::Graph& g, scene::Node& set, const SetSettings& settings);

/** The set's channels that aren't standard Material Output inputs, for portsOf. */
LILA_SCENE_API ChannelList extraChannels(const SetSettings& s);
/** One request per channel, read from the graph's Material Output. */
LILA_SCENE_API std::vector<OutputRequest> channelRequests(const Graph& g, const SetSettings& s);
/** UDIM tile's uv origin: 1001 is (0, 0), 1002 (1, 0), 1011 (0, 1). */
LILA_SCENE_API std::pair<int, int> udimOrigin(int udim);

struct EvaluateOptions
{
    /** Channel names; empty for all. */
    std::vector<std::string> channels;
    /** Overrides the tiles' sizes when > 0. */
    int width = 0;
    int height = 0;
    /** Relative folders resolve as bake output folders do. Empty: textures/<set>. */
    std::string folder;
    /** png, exr, tga, tif. */
    std::string format = "png";
    /** 0 uses each channel's bits (32 needs exr or tif, else 16). */
    int bits = 0;
    /** gpu or cpu; empty uses the GPU when there is one (see tex::evaluate). */
    std::string backend;
};

/**
 * Images for External buffers evaluateSet and exportSet can't make themselves (paint layers): the set, the buffer name
 * and the UDIM tile. An empty image leaves the buffer to the evaluator's fallback.
 */
using ExternalSource = std::function<image::Image(scene::Node& set, const std::string& name, int udim)>;
LILA_SCENE_API void setExternalSource(ExternalSource fn);

/**
 * Evaluates any outputs (channels, node sockets) of the set's graph for one tile (its udim; the first tile when it has no such
 * tile), at width x height or the tile's size, with the set's paint buffers and mesh maps. By request name. Throws.
 */
LILA_SCENE_API std::map<std::string, image::Image> evaluateOutputs(scene::Node& set, const std::vector<OutputRequest>& requests, int udim, int width,
                                                                   int height, const std::string& backend = {},
                                                                   std::vector<std::string>* warnings = nullptr);

/** As evaluateOutputs, for another graph in the set's place (its paint buffers and mesh maps): a stack not yet written. Throws. */
LILA_SCENE_API std::map<std::string, image::Image> evaluateGraphOutputs(scene::Node& set, const Graph& g, const std::vector<OutputRequest>& requests,
                                                                        int udim, int width, int height, const std::string& backend = {},
                                                                        std::vector<std::string>* warnings = nullptr);

/** Evaluates the set and writes <folder>/<set>_<channel>[.<udim>].<ext>. Returns {files, passes, milliseconds, compiled, cached, warnings, backend}. Throws. */
LILA_SCENE_API json evaluateSet(scene::Node& set, const EvaluateOptions& options);

struct ExportOptions
{
    /** A bake target name; empty uses the set's exportPreset. */
    std::string preset;
    std::string folder = "textures";
    int width = 0;
    int height = 0;
    /** Also write the preset's optional maps when the set has their channels. */
    bool includeOptional = true;
    /**
     * Skip files whose inputs (graph structure, values, mesh maps, paint, image files, size) hash the same as at the last export
     * into this folder, evaluating only the channels the rest read; a file whose pixels come out unchanged isn't rewritten.
     */
    bool incremental = false;
    /** The variant the set shows, for the {variant} token. */
    std::string variant;
    /** Names must tell variants apart: a template without {variant} gets _{variant} added. */
    bool needVariantToken = false;
    /** gpu or cpu; empty picks the GPU when there is one. */
    std::string backend;
    /** Overrides the set's normalFromHeight. */
    std::optional<bool> normalFromHeight;
};

/**
 * Writes the set's channels as a bake target lays them out: its maps (channel packs such as ORM composed per channel,
 * normals in the preset's convention), names, format and bits. Mesh maps the graph reads come from the set's bake project.
 * Written files are checked out and opened for add in the scene's changelist. Returns {files, outputs, written, unchanged,
 * warnings, preset, backend}: outputs are shaped like a bake's ({file, map, udim, width, height, format, bits, colorSpace,
 * normalFormat}) plus changed. Throws.
 */
LILA_SCENE_API json exportSet(scene::Node& set, const ExportOptions& options);

struct FlipbookOptions
{
    /** Channel names; empty for all. */
    std::vector<std::string> channels;
    /** Scene frames, inclusive, every step of them. */
    double startFrame = 1.0;
    double endFrame = 24.0;
    double step = 1.0;
    /** Cells per row; 0 lays the frames out as near a square as they go. */
    int columns = 0;
    /** One frame's width in the sheet; its height keeps the set's proportions. */
    int cellSize = 256;
    std::string folder;
    std::string format = "png";
    int bits = 0;
    std::string backend;
};

/**
 * Evaluates the set's first tile at each frame and writes one sheet per channel, frames left to right then top to bottom:
 * <folder>/<set>_<channel>_flipbook_<columns>x<rows>.<ext>. Returns {files: [{file, channel}], columns, rows, frames, fps,
 * cell: [w, h], warnings, backend}. Throws.
 */
LILA_SCENE_API json exportFlipbook(scene::Node& set, const FlipbookOptions& options);
} // namespace lila::tex
