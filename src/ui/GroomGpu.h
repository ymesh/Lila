#pragma once

#include "core/Export.h"
#include "groom/LookDevice.h"

#include <memory>

namespace lila::ui
{
/** The groom look engine's device on the renderer's compute (gpu::Session). Null when the renderer isn't running. */
LILA_UI_API std::unique_ptr<groom::LookDevice> makeGpuLookDevice();
} // namespace lila::ui
