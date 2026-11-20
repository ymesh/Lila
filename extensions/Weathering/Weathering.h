#pragma once

namespace lila::ext
{
class Context;
}

/**
 * The Weathering extension: age, damage and overgrow meshes, Blockwork levels, Masonry walls and Settlement buildings.
 * Each area registers its node types, commands and UI from its own file. Everything builds from the user's pieces
 * (Pieces.h), with procedural placeholders (Placeholders.h) so presets work out of the box, in any style.
 *
 * Commands that make something return a dict holding "group" (the group of what they made), so arrangePreset gathers
 * them into one Age group.
 */
namespace weathering
{
/** Age presets registered with Stagehand, the age command, base dressing helpers (Age.cpp). */
bool registerAge(lila::ext::Context& ctx);
/** Ground presets: scatter plus ground material and tint (Ground.cpp). */
bool registerGround(lila::ext::Context& ctx);
/** Live grunge masks (Masks.cpp). */
bool registerMasks(lila::ext::Context& ctx);
/** Edge decals and edge chipping (Edges.cpp). */
bool registerEdges(lila::ext::Context& ctx);
/** Ivy and other growth over surfaces (Ivy.cpp). */
bool registerIvy(lila::ext::Context& ctx);
/** Roof, window, wire, pile and rooftop generators (Generators.cpp). */
bool registerGenerators(lila::ext::Context& ctx);
} // namespace weathering
