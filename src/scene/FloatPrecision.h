#pragma once

#include "core/Export.h"
#include "scene/Node.h"

namespace lila::scene
{
class Graph;

/**
 * Meshes too big for the viewport's float precision. The viewport draws each mesh in float relative to the camera, so a
 * mesh whose points reach far from its own origin (a huge mesh, or a small one scaled up to planet size) has its surface
 * rounded to steps that grow with that reach, and it jitters as the camera moves. Such meshes get a warning in the
 * Outliner, and tileMesh splits them into tiles that each keep their points near their own origin.
 */
namespace floatPrecision
{
/** Reach (cm) past which a mesh is warned about: about 1 mm of jitter. */
inline constexpr double kWarnReach = 1.0e6;
/** tileMesh's default tile edge, cm: tiles reach about 3.5 km, well under a tenth of a millimetre of jitter. */
inline constexpr double kDefaultTileSize = 5.0e5;

/** How far (cm, in world units) the mesh's points reach from its own origin; 0 for a node without a mesh. */
LILA_SCENE_API double reach(Graph& g, NodeId mesh);
/** The size of the steps (cm) float rounds a point at that reach to. */
LILA_SCENE_API double jitter(double reach);

/** tileMesh, and the live check that warns about big meshes. */
LILA_SCENE_API void registerCommands();
} // namespace floatPrecision
} // namespace lila::scene
