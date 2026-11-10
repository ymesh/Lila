#pragma once

#include <string>
#include <vector>

namespace lila::groom
{
/**
 * Geometry nodes in the Hair category, one per GroomLab operator that works on what it is handed ("groomClump",
 * "groomCurl", ...): strands and guides as curves, the scalp as a mesh, the operator's params as sockets. A strand's root on
 * the scalp rides on its curve as the groomRootFace and groomRootUV attributes; float attributes are its channels. The
 * GN types registered.
 */
std::vector<std::string> registerGroomGnNodes();
} // namespace lila::groom
