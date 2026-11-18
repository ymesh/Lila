#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "tex/Types.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * Texture node definitions. A `.lilatexnode` file holds one definition (or {"nodes": [...]} several):
 *
 *   { "format": "lilatexnode", "version": 1,
 *     "type": "levels", "label": "Levels", "category": "Filter", "doc": "...",
 *     "keywords": ["contrast"],           // more words the add-node search finds it by
 *     "kind": "slang",                    // slang | graph | native | buffer | output
 *     "inputs":  [{"id": "in", "type": "color", "default": [0.5, 0.5, 0.5, 1], "taps": 1, "doc": "..."}],
 *     "outputs": [{"id": "out", "type": "color"}],
 *     "params":  [{"id": "gamma", "type": "float", "default": 1, "min": 0.01, "softMax": 4}],
 *     "cost": 4,                          // rough ALU per output sample, for the scheduler
 *     "helpers": ["texColor"],            // shared Slang helpers it calls (deduplicated per pass)
 *     "externals": ["position"],          // caller-supplied maps, sampled as `position.sample(uv)`
 *     "code": "float myFn(float x) { ... }",  // functions only this type uses, emitted once per pass
 *     "body": {"out": "float4 c = in.sample(uv); return ...;"} }
 *
 * A body is the inside of a function of `float2 uv` returning the output's type. Inputs and image parameters are
 * samplers called with any coordinate (`in.sample(uv * 2.0)`), so transforms and warps are exact. Parameters are fields
 * of `p` (`p.gamma`; enums and seeds are ints, gradients have `p.colors.eval(t)`, curves `p.curve.eval(x)`). `texel()` is
 * the size of one output texel in uv. Ids can't be Slang keywords (`in`, `out`, `sample`...). An input's taps is how many times the body samples it per output sample.
 * An enum parameter with the id "operation" also lists each option in the add-node search (Comparison > Greater Than).
 *
 * kind "graph" carries "graph": a texture graph whose Group Input/Output and exposed inputs form the node's interface.
 * kind "native" runs as its own pass (blur, flood fill); its inputs are always buffered. kind "buffer" caches its input
 * in a buffer. kind "output" is the Material Output.
 *
 * Helper files: <tier>/helpers/<name>.slang, with "//! requires: a, b" lines naming helpers they call.
 *
 * Tiers, later winning on the same type: built-in (<resources>/texnodes), extensions, the user's
 * (<config>/texnodes), the workspace's (.lila/texnodes).
 */
namespace lila::tex
{
enum class NodeKind : uint8_t
{
    Slang,
    Graph,
    Native,
    Buffer,
    Output,
    GroupInput,
    GroupOutput,
    Group,
    /** Reroutes and portals (gn/Routing.h): spliced out before planning. */
    Route,
};

LILA_SCENE_API const char* nodeKindName(NodeKind k);

struct PortDecl
{
    std::string id;
    PortType type = PortType::Color;
    json defaultValue;
    std::string label;
    std::string doc;
    /** Samples of this input per output sample. */
    int taps = 1;
    /**
     * Texels either side of the output's own uv this input is read within, when not only there (neighbour taps); 0 for
     * an unknown reach. A changed region of the input then changes only that region grown by it.
     */
    int reach = 0;
    /** What an unlinked input without a set value reads instead of its default: "uv" is the texture coordinate. */
    std::string implicit;
};

struct ParamDecl
{
    std::string id;
    ParamType type = ParamType::Float;
    json defaultValue;
    std::string label;
    std::string doc;
    std::string panel;
    std::vector<std::string> options;
    std::optional<double> min, max, softMin, softMax, step;
};

struct LILA_SCENE_API NodeDef
{
    std::string type;
    std::string label;
    std::string category;
    std::string doc;
    /** Other words the add-node search finds it by ("rim", "facing ratio"). */
    std::vector<std::string> keywords;
    NodeKind kind = NodeKind::Slang;
    std::vector<PortDecl> inputs;
    std::vector<PortDecl> outputs;
    std::vector<ParamDecl> params;
    int cost = 1;
    std::vector<std::string> helpers;
    /** Maps the caller supplies by name (mesh maps such as "position"), sampled like inputs under that name. */
    std::vector<std::string> externals;
    std::string code;
    /** Output id -> Slang body. */
    std::map<std::string, std::string> body;
    /** kind Graph: the graph JSON. */
    json graph;
    /** builtin, extension, user, workspace, or core (Group Input, Group Output, Group). */
    std::string tier;
    std::string owner;
    std::filesystem::path file;

    const PortDecl* input(std::string_view id) const;
    const PortDecl* output(std::string_view id) const;
    const ParamDecl* param(std::string_view id) const;

    json toJson() const;
    /** Throws std::invalid_argument naming the problem. */
    static NodeDef fromJson(const json& j);
};

struct HelperDef
{
    std::string name;
    std::string code;
    std::vector<std::string> needs;
};

/** The winning definition of a type across the tiers, or null. Loads the folders on first use. */
LILA_SCENE_API const NodeDef* findNode(std::string_view type);
/** Every visible definition, by category then label. */
LILA_SCENE_API std::vector<const NodeDef*> allNodes();
LILA_SCENE_API const HelperDef* findHelper(std::string_view name);

/** Built-in, user, workspace folders (missing ones included). */
LILA_SCENE_API std::vector<std::filesystem::path> nodeDirs();
/** Rescans the folders. The workspace folder is also rescanned whenever the open workspace changes. */
LILA_SCENE_API void reloadLibrary();
/** Files that failed to load on the last scan, "path: why". */
LILA_SCENE_API std::vector<std::string> libraryErrors();

/** Definitions extensions registered, owner-tagged so unloading removes them. */
LILA_SCENE_API Registry<NodeDef>& extensionNodes();
/** Registers def as owned by the extension; replaced or removed with it. Throws std::invalid_argument for a bad def. */
LILA_SCENE_API uint64_t registerExtensionNode(ext::Context& ctx, NodeDef def);
/** Registers every .lilatexnode/.lilatexnodes in dir (and helpers under dir/helpers). Returns the count. */
LILA_SCENE_API size_t registerExtensionDir(ext::Context& ctx, const std::filesystem::path& dir);

/** Parses one node file (one def or {"nodes": [...]}). Throws std::invalid_argument. */
LILA_SCENE_API std::vector<NodeDef> readNodeFile(const std::filesystem::path& path);
} // namespace lila::tex
