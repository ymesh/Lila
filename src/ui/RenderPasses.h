#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "render/FrameState.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"

#include <functional>
#include <string>
#include <string_view>

/**
 * Render passes (a modest MPxRenderOverride): callbacks every viewport runs at fixed points while it builds a frame.
 * A pass adds to the frame like a drawer does, 3D into frame (lines, linesOnTop, meshes, tris3d) and 2D into painter,
 * but chooses where it lands relative to Lila's own content.
 */
class QWidget;

namespace lila::ui::renderPasses
{
enum class Stage : uint8_t
{
    Scene,   // after Lila's meshes, locators and components, before drawers and tools
    Overlay, // after drawers, the active tool and the gizmo, before the HUD
    Final,   // last: after the HUD, view indicator and cursor badges
};

struct Context
{
    render::FrameState& frame;
    OverlayPainter& painter;
    const ViewInfo& view;
    /** Which viewport panel, as hud prefs key them. */
    int panel = 0;
    /** The viewport widget being drawn, to tell the view under the cursor from the others. */
    QWidget* widget = nullptr;
};

struct Pass
{
    std::string id;
    Stage stage = Stage::Overlay;
    /** Lower first within a stage; ties by id. */
    int order = 0;
    std::function<void(Context&)> run;
};

LILA_UI_API Registry<Pass>& registry();

/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const Pass& add(Pass pass);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const Pass* add(ext::Context& ctx, Pass pass);
LILA_UI_API bool remove(std::string_view id);

/** Runs stage's passes in order. One that throws is logged and skipped. */
LILA_UI_API void run(Stage stage, Context& ctx);
} // namespace lila::ui::renderPasses
