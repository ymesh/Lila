#pragma once

#include "Crowd.h"

#include <lila/Motion.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::render
{
struct RenderMesh;
}

namespace crowds
{
/**
 * A character cut for crowd drawing: each skinned mesh's faces split by the joint that weighs most on them, each piece
 * in that joint's space, so one instanced draw per piece shows it on every agent (rigid skinning, joints crack a
 * little; far cheaper than skinning hundreds of meshes on the CPU).
 */
struct CharacterParts
{
    struct Piece
    {
        std::shared_ptr<const lila::render::RenderMesh> mesh;
        int joint = -1; // index in the map's skeleton
    };
    std::vector<Piece> pieces;
    std::optional<lila::motion::CharacterMap> map;
    /** Why there are no pieces (no skinned mesh, not characterised), for the panel. */
    std::string problem;
    uint64_t key = 0;
};

/** The parts of a character root node, built on first use and when its skins change. */
std::shared_ptr<const CharacterParts> partsFor(Graph& g, const std::string& character);

/** The joint a character node names: itself, the first joint under it, or the skeleton it points at; kNoNode if none. */
NodeId characterJoint(Graph& g, NodeId node);

void registerDraw(lila::ext::Context& ctx);
} // namespace crowds
