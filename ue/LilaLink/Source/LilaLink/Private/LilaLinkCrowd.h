#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's crowdSendToUnreal: createCrowdSequence {path, fps, frames, agents: [{name, mesh, animation}]}
 * makes (or remakes) a Level Sequence at path with one skeletal mesh actor per agent in the open level, each bound as a
 * possessable playing its animation from frame 0. The actors go in an outliner folder named after the sequence; ones
 * from an earlier send of the same sequence are replaced.
 */
void AddCrowdMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
