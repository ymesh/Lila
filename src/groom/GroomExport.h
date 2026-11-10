#pragma once

#include "scene/Graph.h"

#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <memory>

namespace lila::groom
{
/** One frame of a groom as exports write it: its strands with every channel, its primary guides as curves, its scalp. */
struct GroomExportFrame
{
    std::shared_ptr<const groomlab::StrandSet> strands;
    /** The strands the Cards nodes made cards from, with their card layers; null without Cards nodes. */
    std::shared_ptr<const groomlab::StrandSet> cardStrands;
    groomlab::StrandSet guides;
    std::shared_ptr<const groomlab::ScalpMesh> scalp;
};

/** The groom node evaluated at `seconds` for an export, whether or not anything reads its strands. Throws for a non-groom. */
GroomExportFrame groomExportFrame(scene::Graph& g, scene::NodeId groom, double seconds);

/** Registers exportGroom and importGroom (GroomIo.cpp). */
void registerIoCommands();
} // namespace lila::groom
