#pragma once

#include "core/Export.h"

#include "scene/Graph.h"
#include "tex/Graph.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * Time in texture graphs. Image Sequence nodes play numbered files over the timeline, and any number parameter of a node
 * can be driven by a keyable attribute on the texture set. Both resolve into plain values on a copy of the graph before
 * it is planned, so a new frame changes values and image files, never the plan's passes.
 */
namespace lila::tex::sequence
{
inline constexpr const char* kType = "imageSequence";
/** Node value {param: set attribute}: the parameters driven by attributes on the set. */
inline constexpr const char* kDriversKey = "drivers";

/**
 * The files of the sequence a frame's path belongs to, in frame order, UTF-8 and absolute: every file in its folder whose
 * name differs from it only in the frame number (a run of #, else the last run of digits). Only the path itself when it
 * has no frame number; empty when nothing matches.
 */
LILA_SCENE_API std::vector<std::string> files(const std::string& path, const std::filesystem::path& root = {});

/** The files a position in the sequence shows: files[first], crossfading into files[second] by mix. */
struct Frames
{
    int first = 0;
    int second = 0;
    double mix = 0.0;
};
/** position in sequence frames over count of them; loop is the node's After End (loop, hold, pingPong). */
LILA_SCENE_API Frames framesAt(double position, int count, std::string_view loop, bool blend);

/** The set attribute driving a node's parameter; empty for none. */
LILA_SCENE_API std::string driver(const GNode& node, std::string_view param);
/** The attribute name a driver of node.param gets. */
LILA_SCENE_API std::string driverName(std::string_view node, std::string_view param);
/** Whether the graph changes with time: it has an Image Sequence node or a driven parameter. */
LILA_SCENE_API bool animated(const Graph& g);

/**
 * Puts the graph at a scene time, for evaluation: driven parameters take their attribute's value (its curve's, at times
 * other than the scene's), and Image Sequence nodes the files of their frame. True when anything changed.
 */
LILA_SCENE_API bool followTime(Graph& g, scene::Graph& scene, const scene::Node& set, double seconds, const std::filesystem::path& root = {});
} // namespace lila::tex::sequence
