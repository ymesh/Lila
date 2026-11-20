#pragma once

#include <lila/Command.h>
#include <lila/Graph.h>

#include <cstdint>

namespace lila::ext
{
class Context;
}

/** Live grunge masks (W3): the grungeModifier node, its commands, the Unreal send and the Level > Weathering entries. */
namespace weathering::masks
{
/** The node type addModifier makes for kind "grunge". */
inline constexpr const char* kGrungeType = "grungeModifier";
/** The colour set the masks go into for Unreal: R edge, G cavity, B occlusion, A streaks. */
extern const char* kColorSet;

/** grungeMask's work: adds or updates the masks on the meshes a names, wiring their neighbours. */
nlohmann::json addMasks(lila::scene::Graph& g, const lila::Args& a, const char* cmd);

bool registerMaskNode(lila::ext::Context& ctx);
void registerMaskCommands(lila::ext::Context& ctx);
/** grungeSend (MasksUnreal.cpp). */
void registerMaskUnreal(lila::ext::Context& ctx);
/** Level > Weathering menu entries and option boxes (MasksUi.cpp). */
void registerMaskUi(lila::ext::Context& ctx);
} // namespace weathering::masks
