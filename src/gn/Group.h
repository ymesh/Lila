#pragma once

#include "core/Export.h"

#include "gn/Geometry.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lila::gn
{
using json = nlohmann::json;

/** One node in a group. Values hold socket and property overrides by id; anything missing takes its default. */
struct GNode
{
    std::string name; // unique in the group
    std::string type;
    glm::dvec2 position{0.0};
    json values = json::object();
    /** Bypassed: evaluation passes its first input of a matching type through to each output (gn/Routing.h). */
    bool muted = false;
    /** Drawn as its header only. */
    bool collapsed = false;
    /** Drawn with only its linked sockets. */
    bool hideUnused = false;
};

/** Writes a node's flags into its JSON object ("muted", "collapsed", "hideUnused"), leaving out the ones that are off. */
LILA_SCENE_API void putNodeFlags(const GNode& n, json& o);
LILA_SCENE_API void getNodeFlags(GNode& n, const json& o);
template <class Json> void putNodeFlagsTo(const GNode& n, Json& o)
{
    if (n.muted)
        o["muted"] = true;
    if (n.collapsed)
        o["collapsed"] = true;
    if (n.hideUnused)
        o["hideUnused"] = true;
}

struct GLink
{
    std::string fromNode;
    std::string fromSocket;
    std::string toNode;
    std::string toSocket;

    bool operator==(const GLink&) const = default;
    auto operator<=>(const GLink&) const = default;
};

/**
 * A zip tie: links gathered into a short parallel ribbon through a band on the canvas. Display only; evaluation never
 * reads it. angle is the ribbon's direction in degrees, 0 running left to right. color is a palette name, empty for the
 * theme's default.
 */
struct GTie
{
    std::vector<GLink> links;
    glm::dvec2 position{0.0};
    double angle = 0.0;
    std::string color;

    bool operator==(const GTie&) const = default;
};

/**
 * A frame: a labelled, coloured box on the canvas holding nodes, which move with it. Display only. position is its
 * top-left corner; color is a palette name, empty for the theme's default.
 */
struct GFrame
{
    std::string label;
    glm::dvec2 position{0.0};
    glm::dvec2 size{300.0, 200.0};
    std::string color;
    bool collapsed = false;
    std::vector<std::string> nodes;

    bool operator==(const GFrame&) const = default;
};

/** [{label, position: [x, y], size: [w, h], color, collapsed, nodes: [names]}]; color and collapsed left out at their defaults. */
LILA_SCENE_API json framesToJson(const std::vector<GFrame>& frames);
/** Skips malformed entries. */
LILA_SCENE_API std::vector<GFrame> framesFromJson(const json& j);
/** Drops member names that aren't in nodes, and repeats. */
LILA_SCENE_API void pruneFrames(std::vector<GFrame>& frames, const std::vector<std::string>& nodes);
/** Frames holding any of nodes, with only those members, renamed through renamed and shifted by offset. */
LILA_SCENE_API std::vector<GFrame> framesAmong(const std::vector<GFrame>& frames, const std::set<std::string>& nodes,
                                               const std::map<std::string, std::string>& renamed = {}, glm::dvec2 offset = glm::dvec2(0.0));

/** Bookmarks with only well-formed [x, y, zoom] entries; anything else reads as none. */
LILA_SCENE_API json cleanBookmarks(const json& j);

/** [{links: [[fromNode, fromSocket, toNode, toSocket]], position: [x, y], angle, color}]; color left out when empty. */
LILA_SCENE_API json tiesToJson(const std::vector<GTie>& ties);
/** Skips malformed entries rather than failing, since ties never affect what a graph computes. */
LILA_SCENE_API std::vector<GTie> tiesFromJson(const json& j);
/** Drops tied links missing from links (and repeats), then ties left with fewer than two. */
LILA_SCENE_API void pruneTies(std::vector<GTie>& ties, const std::vector<GLink>& links);
/**
 * The ties among a set of nodes, for copy, paste and duplicate: only links with both ends in nodes, renamed through
 * renamed when it is given, positions shifted by offset; ties left with fewer than two links are dropped.
 */
LILA_SCENE_API std::vector<GTie> tiesAmong(const std::vector<GTie>& ties, const std::set<std::string>& nodes,
                                           const std::map<std::string, std::string>& renamed = {}, glm::dvec2 offset = glm::dvec2(0.0));

/**
 * An exposed parameter: an output of the Group Input node, a socket on Group nodes using the group, and a keyable attr
 * on the group's modifier type. Everything after defaultValue is presentation: editors show the label, tooltip and
 * limits, and group inputs by panel in interface order.
 */
struct GInput
{
    std::string name;
    SocketType type = SocketType::Float;
    json defaultValue;
    std::string label;       // nice name; empty derives one from name
    std::string description; // tooltip
    /** "", factor, percentage, angle (float in degrees, fed to the graph as radians), distance, color. */
    std::string subtype;
    std::optional<double> min, max, softMin, softMax, step;
    std::string panel;

    /** label, or name split into words. */
    LILA_SCENE_API std::string displayName() const;
};

/** A value a nested group hands back through its Group Output. Only geometry reaches the modifier stack. */
struct GOutput
{
    std::string name;
    SocketType type = SocketType::Float;
    std::string description;
};

/**
 * A node-group definition. Saved as JSON in the scene (on a gnGroup node) and in library folders:
 *   { "name", "typeVersion", "inputs": [{name, type, default, label, description, subtype, min, max, softMin, softMax,
 *     step, panel}], "outputs": [{name, type, description}], "nodes": [{name, type, position, values, muted, collapsed, hideUnused}],
 *     "links": [[fromNode, fromSocket, toNode, toSocket]], "ties": [see tiesToJson], "frames": [see framesToJson],
 *     "bookmarks": {slot: [x, y, zoom]}, "subgroups": {name: definition}, "tool": true when a tool }
 */
struct LILA_SCENE_API Group
{
    std::string name;
    int typeVersion = 1;
    std::vector<GInput> inputs;
    std::vector<GOutput> outputs;
    std::vector<GNode> nodes;
    std::vector<GLink> links;
    std::vector<GTie> ties;
    std::vector<GFrame> frames;
    /** Saved canvas views by slot ("1".."5"): [centre x, centre y, zoom]. */
    json bookmarks = json::object();
    std::string description;
    /** Runs on the selected meshes as a viewport tool (gn/Tool.h) rather than only as a modifier. */
    bool tool = false;
    /** Definitions of the groups its Group nodes use, by name, each self-contained (with its own subgroups). */
    std::map<std::string, json> subgroups;

    json toJson() const;
    /** Throws std::invalid_argument on malformed input. */
    static Group fromJson(const json& j);

    GNode* node(std::string_view name);
    const GNode* node(std::string_view name) const;
    const GInput* input(std::string_view name) const;
    GInput* input(std::string_view name);
    const GOutput* output(std::string_view name) const;
    /** Panel names in first-use order; "" (no panel) is left out. */
    std::vector<std::string> panels() const;
    /** First node of a type, or null. */
    const GNode* firstOfType(std::string_view type) const;
    /** base, base1, base2... whichever is free. */
    std::string uniqueNodeName(std::string_view base) const;
    std::vector<GLink> linksInto(std::string_view node, std::string_view socket) const;

    /** A new group: Group Input feeding Group Output. */
    static Group makeDefault(std::string name);
};

/** Whether the group or a group nested in it reads the scene time. */
LILA_SCENE_API bool usesSceneTime(const Group& g);
/** Whether the group or a group nested in it reads Is Viewport or the active camera. */
LILA_SCENE_API bool usesViewState(const Group& g);
/** Names of the groups its Group nodes use directly. */
LILA_SCENE_API std::vector<std::string> referencedGroups(const Group& g);
/** Whether the definition is, or uses at any depth, the named group. */
LILA_SCENE_API bool dependsOn(const Group& g, std::string_view group);

inline constexpr const char* kGroupNodeDeclType = "Group";
/** Group, Repeat and For Each Element: node types whose "group" property names a subgroup they run. */
LILA_SCENE_API bool runsGroup(std::string_view type);

/** Identifier rules for group names, node names and exposed inputs: letters, digits and underscores, not a digit first. */
LILA_SCENE_API bool validIdentifier(std::string_view s);
/** An identifier that doesn't clash with the modifier's own attrs, so it can be an exposed input. */
LILA_SCENE_API bool validInputName(std::string_view s);
} // namespace lila::gn
