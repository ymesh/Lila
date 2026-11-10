#pragma once

#include "core/Export.h"

/**
 * Soft Net (softNet deformer): a Panel Layout's lines as Bezier curves that shape the surface after the skin. The lines
 * are bound to the mesh as it is, each sample a barycentric soft constraint on its uncut triangles; every junction is a
 * knot with a keyable control in a frame riding the surface under the net, and every line end has a tangent handle
 * under its knot. Moving, turning or scaling a knot, or pulling a handle, reshapes the lines, and the surface follows
 * them with a smooth displacement that fades over the net's reach. One net can bind several meshes (a body and its
 * garments), each moved by the same lines.
 */
namespace lila::layout
{
inline constexpr const char* kSoftNetType = "softNet";
inline constexpr const char* kSoftNetKnotsType = "softNetKnots";
inline constexpr const char* kSoftNetKnotType = "softNetKnot";
inline constexpr const char* kSoftNetTangentType = "softNetTangent";

LILA_SCENE_API void registerSoftNetTypes();
LILA_SCENE_API void registerSoftNetCommands();
} // namespace lila::layout
