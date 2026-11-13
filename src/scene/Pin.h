#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <string>

/**
 * The pin node and its commands (Pin.cpp): a world frame riding a curve, a vertex, an edge, a face or a mesh's surface,
 * which drives a pin locator that objects and joints are constrained to. Several pins on one object blend like a
 * weighted parent constraint. bakePin turns pins into joints for Unreal.
 */
namespace lila::scene::pins
{
/**
 * The string attribute bakePin puts on a pinned prop: the joint (bone name) the prop attaches to with no offset.
 * FBX exports write it as the user property UnrealSocket; the Unreal link makes it a socket on the character's mesh.
 */
inline constexpr const char* kSocketAttr = "unrealSocket";
/** The prop's unrealSocket, empty when it has none. */
LILA_SCENE_API std::string socketOf(const Node& n);
/**
 * A prop's socket bone when it attaches to an existing bone with an offset (puppeteerPropUnreal) rather than to a joint
 * bakePin made: the socket (kSocketAttr) is then made on this bone with kSocketOffsetAttr, the prop's matrix in the
 * bone's rigid space.
 */
inline constexpr const char* kSocketBoneAttr = "unrealSocketBone";
inline constexpr const char* kSocketOffsetAttr = "unrealSocketOffset";
/** The prop's unrealSocketBone, empty when it has none. */
LILA_SCENE_API std::string socketBoneOf(const Node& n);
/** The prop's unrealSocketOffset; identity when it has none. */
LILA_SCENE_API glm::dmat4 socketOffsetOf(const Node& n);

void registerTypes();
void registerCommands();
} // namespace lila::scene::pins
