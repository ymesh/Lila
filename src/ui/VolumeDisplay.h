#pragma once

#include "core/Export.h"

namespace lila::geo
{
class Mesh;
}

namespace lila::render
{
struct FrameState;
}

namespace lila::ui::volumeDisplay
{
/** A mesh whose material is a volume: it draws as the medium filling it, not as a surface. */
LILA_UI_API bool isVolumeMesh(const geo::Mesh& mesh);

/**
 * The scene's volumes into the frame: each visible mesh with a volume material, as its geometry nodes' density grid when it
 * has one, else filled from its closed surface. Grids are cut down to the renderer's size limit.
 */
LILA_UI_API void apply(render::FrameState& s);
} // namespace lila::ui::volumeDisplay
