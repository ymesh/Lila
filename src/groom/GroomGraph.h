#pragma once

#include "core/Export.h"
#include "groom/Style.h"

#include <optional>
#include <string>
#include <vector>

/**
 * A groom's graph (GroomLab JSON) as its lock tools see it: the guides run through a chain of lock styles (LockSplit,
 * LockCurl, ...) into the Locks node, whose guides become the locks mesh; the strands are scattered and interpolated
 * from the same locks. Edits keep every other node where it is.
 */
namespace lila::groom::groomGraph
{
/** Node names of the default graph. */
inline constexpr const char* kGuides = "guides";
inline constexpr const char* kLocks = "locks";

/** A new groom's graph: guides into locks, strands interpolated from them. */
LILA_SCENE_API std::string defaultGraph();

struct StyleEntry
{
    /** The graph node's name. */
    std::string node;
    Style kind = Style::Curl;
    bool enabled = true;
};

/** The lock styles from the guides to the Locks node, first applied first. Throws std::runtime_error for unreadable JSON. */
LILA_SCENE_API std::vector<StyleEntry> styles(const std::string& graph);
/** A style added at `position` of the chain (-1: last), its params set from `params` (a JSON object). Returns its name. */
LILA_SCENE_API std::string addStyle(std::string& graph, Style kind, int position, const std::string& params = "{}");
LILA_SCENE_API void removeStyle(std::string& graph, const std::string& node);
LILA_SCENE_API void moveStyle(std::string& graph, const std::string& node, int position);
/** Sets params of a node (a JSON object merged into its params), and its bypass. */
LILA_SCENE_API void setParams(std::string& graph, const std::string& node, const std::string& params);
LILA_SCENE_API void setEnabled(std::string& graph, const std::string& node, bool enabled);
/** A node's params as JSON (empty object when it has none). */
LILA_SCENE_API std::string params(const std::string& graph, const std::string& node);
} // namespace lila::groom::groomGraph
