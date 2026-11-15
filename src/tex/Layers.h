#pragma once

#include "core/Export.h"

#include "tex/Graph.h"
#include "tex/TextureSet.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * The layer stack: a Painter-style view over a texture set's graph, never a second system. Every stack edit compiles the
 * whole stack into the set's texGraph:
 *
 *   - each layer is a Group node (named by its id, "L3") whose subgroup makes the layer's channel values and its mask;
 *   - per channel, a chain of layerBlend nodes composites the layers bottom to top (layerBlendNormal for normals, RNM);
 *   - heights composite before height-to-normal, whose result is RNM-blended onto the composited normals;
 *   - the stack itself is stored on the Material Output node (values.layerStack), with stable ids.
 *
 * Reading the stack back (readStack) takes that metadata, picks up value edits made in the graph (blend modes, opacities,
 * fill values, generator settings: everything the stack has a field for) and recompiles; when the result differs from the
 * graph, the set was edited as nodes in a way the stack can't show and is in graph mode until rebuilt.
 *
 * Layer kinds: fill (a material and a mask), paint (placeholder material plus paint buffers per channel and a painted mask,
 * filled by the painting tools), procedural (any graph whose outputs are named after channels, optionally "mask"), folder
 * (children composited over the stack below, then mixed in through the folder's mask), instance (another layer's values
 * and mask, so one painted mask feeds several places) and filter (a filter node applied to the composite below).
 *
 * Masks stack bottom to top on 0 (on 1 when a layer has none): fill (a value), generator (a mask graph over mesh maps),
 * filter and levels (applied to the masks below), paint (an image) and anchor (a channel of the composite at an anchor).
 * An anchor is a name on a layer for the composite right after it; masks and generator inputs of layers above read it.
 */
namespace lila::tex
{
enum class LayerKind : uint8_t
{
    Fill,
    Paint,
    Procedural,
    Folder,
    Instance,
    Filter,
};

enum class MaskKind : uint8_t
{
    Fill,
    Generator,
    Filter,
    Levels,
    Paint,
    Anchor,
};

LILA_SCENE_API const char* layerKindName(LayerKind k);
LILA_SCENE_API bool parseLayerKind(std::string_view s, LayerKind* out);
LILA_SCENE_API const char* maskKindName(MaskKind k);
LILA_SCENE_API bool parseMaskKind(std::string_view s, MaskKind* out);

/** layerBlend's modes; normals take rnm or normal. */
LILA_SCENE_API const std::vector<std::string>& layerBlendModes();
LILA_SCENE_API const std::vector<std::string>& maskBlendModes();

/** One channel of a layer. An empty mode takes the channel's default (rnm for normal, addSigned for height, else normal). */
struct ChannelBlend
{
    bool enabled = true;
    std::string mode;
    double opacity = 1.0;
};

struct LILA_SCENE_API Mask
{
    std::string id;
    MaskKind kind = MaskKind::Fill;
    std::string name;
    bool enabled = true;
    /** How it combines with the masks below (maskBlendModes). */
    std::string mode = "normal";
    double opacity = 1.0;
    bool invert = false;
    /** Fill. */
    double value = 1.0;
    /** Generator or filter: the node type. */
    std::string type;
    /** Generator, filter, levels: parameter values by id. */
    json values = json::object();
    /** Generator: its inputs' sources by input id, a mesh map role (meshRoles) or "anchor:<name>.<channel>". Unlisted inputs bind to the role of their own name. */
    std::map<std::string, std::string> bindings;
    /** Paint: the image. */
    std::string image;
    /** Anchor: the anchor and the channel read. */
    std::string anchor;
    std::string channel = "height";

    json toJson() const;
    /** Throws std::invalid_argument. */
    static Mask fromJson(const json& j);
};

/** How a procedural layer's content repeats: tiles across the texture, turned (degrees) and shifted (in tiles). */
struct LayerTiling
{
    double scale = 1.0;
    double rotation = 0.0;
    double offsetX = 0.0;
    double offsetY = 0.0;

    bool identity() const { return scale == 1.0 && rotation == 0.0 && offsetX == 0.0 && offsetY == 0.0; }
};

struct HeightBlend
{
    bool enabled = false;
    double sharpness = 0.8;
    double influence = 1.0;
};

struct LILA_SCENE_API Layer
{
    std::string id;
    std::string name;
    LayerKind kind = LayerKind::Fill;
    bool enabled = true;
    /** Channel settings given explicitly; channels not listed take the kind's default (see channelOn). */
    std::map<std::string, ChannelBlend> channels;
    /** Fill and paint: channel values ([r, g, b, a] or a number). */
    json material = json::object();
    /** Paint: an image per channel (value in rgb, coverage in alpha: unpainted texels and channels show the stack below), and optionally "mask", a painted layer mask. */
    json paint = json::object();
    /** Procedural: a texture graph whose outputs are named after channels (and "mask"). */
    json graph;
    /** Instance: the layer whose values and mask it uses. */
    std::string source;
    /** Filter: the node type and its values. Its first input takes the composite below, its first output replaces it. */
    std::string filter;
    json filterValues = json::object();
    /** Names the composite right after this layer, for masks above. */
    std::string anchor;
    /** Mixes the layer in by height as well as mask (needs its height channel on). */
    HeightBlend heightBlend;
    /** Procedural: repeats the content. */
    LayerTiling tiling;
    std::vector<Mask> masks;
    /** Folder: bottom first. */
    std::vector<Layer> children;

    json toJson() const;
    /** Throws std::invalid_argument. */
    static Layer fromJson(const json& j);
};

struct LILA_SCENE_API LayerStack
{
    int nextId = 1;
    /** Channel values under every layer by channel name; missing ones take the channel's default. */
    json base = json::object();
    /** Strength of the composited height's normal, RNM-blended onto the composited normals (0 leaves normals alone). */
    double heightNormal = 1.0;
    /** Bottom first. */
    std::vector<Layer> layers;

    Layer* find(std::string_view id);
    const Layer* find(std::string_view id) const;
    /** The list holding the layer and its index there; null when there is no such layer. */
    std::vector<Layer>* listOf(std::string_view id, size_t* index);
    /** "L<n>" or "M<n>", unique in the stack. */
    std::string newId(char prefix);
    /** Every anchor name and the layer carrying it. */
    std::map<std::string, std::string> anchors() const;

    json toJson() const;
    /** Throws std::invalid_argument. */
    static LayerStack fromJson(const json& j);
};

/** Whether the layer writes the channel: its explicit setting, else by kind (fill: has a material value; paint: has a painted image; procedural: has the output; others: yes). */
LILA_SCENE_API bool channelOn(const LayerStack& stack, const Layer& layer, const std::string& channel);
/** The mode a layer uses on a channel. */
LILA_SCENE_API std::string channelMode(const Layer& layer, const std::string& channel);
/** A channel's value under every layer, when the stack's base doesn't set it. */
LILA_SCENE_API json channelDefault(const std::string& name, PortType type);

struct StackCompile
{
    Graph graph;
    std::vector<std::string> warnings;
};

/**
 * The graph a stack compiles to, for a set with these settings. previous keeps its Material Output's name and the positions
 * of nodes that still exist. Throws std::invalid_argument for a stack that can't compile (an instance of a missing layer).
 */
LILA_SCENE_API StackCompile compileStack(const LayerStack& stack, const SetSettings& settings, const Graph& previous);

struct StackState
{
    LayerStack stack;
    /** The graph has a stack (or is empty). */
    bool layered = false;
    /** The graph holds things the stack can't show: hand-made graph, or a stack edited as nodes. */
    bool graphMode = false;
    /** graphMode with stack metadata present: the stack was edited by hand as nodes. */
    bool edited = false;
};

/** The set's stack as its graph stands, with value edits made in the graph folded in. */
LILA_SCENE_API StackState readStack(const Graph& g, const SetSettings& settings);

/**
 * A graph made by hand, as a procedural layer: the Material Output becomes the Group Output and its linked channels the
 * layer's outputs. Unlinked channel values land in base.
 */
LILA_SCENE_API Layer graphAsLayer(const Graph& g, const SetSettings& settings, json* base);

/** Every external map name the graph reads (through any node, group or graph node). */
LILA_SCENE_API std::vector<std::string> externalsUsed(const Graph& g);

/** The stack as JSON for texLayerStack: layers with their resolved channels, plus mode, channels and the maps it reads. */
LILA_SCENE_API json stackView(const StackState& state, const SetSettings& settings, const Graph& g);

/** texAddLayer, texRemoveLayer, texMoveLayer, texSetLayer, texAddMask, ... (called by registerCommands). */
LILA_SCENE_API void registerLayerCommands();
} // namespace lila::tex
