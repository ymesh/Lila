#pragma once

#include "groom/Groom.h"

#include <memory>
#include <string>

namespace lila::scene
{
class Graph;
class Node;
} // namespace lila::scene

/** Graph edits the groom commands share. Internal to src/groom. */
namespace lila::groom
{
/** The guides' scalp in the guides' object space, or null. */
std::unique_ptr<Surface> scalpOf(scene::Graph& g, scene::Node& guides);
/** The guides' own splines, object space. */
Splines guideSplines(scene::Graph& g, scene::Node& guides);
/** The groom's graph JSON, the default one when it has none of its own. */
std::string groomGraphText(const scene::Node& groom);
} // namespace lila::groom
