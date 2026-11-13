#pragma once

#include "core/Export.h"

#include <QImage>

#include <optional>
#include <string>
#include <vector>

namespace lila::ui::capture
{
/** What an offscreen capture shows. The scene itself is never changed, nor is any viewport's camera. */
struct Options
{
    /**
     * "active" (the focused viewport's camera, or three-quarter without one), "persp" (the same as three-quarter), "three-quarter",
     * "front", "back", "left", "right", "side" (right), "top" or "bottom". yaw/pitch (degrees) override the angle for a
     * perspective view: yaw 0 looks at the front (down -Z), 90 from the right, pitch negative looks down.
     */
    std::string view = "active";
    /** A scene camera to look through instead of view: its framing as it is, without yaw, pitch or refitting. */
    std::string lookThrough;
    std::optional<double> yaw, pitch;
    int width = 512;
    int height = 512;
    std::string display = "wireOnShaded"; // "shaded", "wireframe" or "wireOnShaded"
    bool isolate = false;        // only the selected objects (and their children)
    bool frameSelection = false; // frame the selection rather than everything shown
    bool frame = true;           // fit the subject in view; the active view refits only with isolate or frameSelection
    bool grid = true;            // ground grid, perspective views only
    bool showSelection = true;   // tint selected faces, edges and vertices
    bool label = true;           // view name in the corner (GUI only: text needs the font system)
    bool overlays = false;       // what the viewport draws over the scene: locators, net lines and knots, extension drawers (GUI only)
    bool hud = false;            // the heads-up readouts the view shows, with the cursor at the centre, and the Drone Camera banner (GUI only)
    std::optional<bool> ambientOcclusion; // overrides the viewport's ambient occlusion pref
    int samples = 1; // jittered frames averaged into the image (temporal antialiasing), for supersampled stills
};

struct Timing
{
    double gpuMs = 0.0;
    double aoMs = 0.0;
    double cpuMs = 0.0; // render thread recording
    double aoCpuMs = 0.0;
};

/**
 * Renders one view. Starts the renderer if it is not running (batch mode). Throws std::runtime_error. timing receives
 * the GPU time where the device has timestamps: the median of `repeat` draws of the same frame.
 */
LILA_UI_API QImage render(const Options& o, Timing* timing = nullptr, int repeat = 1);

/** count views around the up axis (from the front, turning toward the right) tiled into one image, row by row. */
LILA_UI_API QImage turntable(Options o, int count, double pitch, int columns, std::vector<std::string>* labels = nullptr);

/** Several named views tiled into one image, row by row (front, right, top, three-quarter by default). */
LILA_UI_API QImage sheet(Options o, const std::vector<std::string>& views, int columns);

/** PNG file contents. */
LILA_UI_API std::string png(const QImage& image);

/** captureView / captureTurntable commands (write PNG files) and the MCP image tools. */
LILA_UI_API void registerCommands();
LILA_UI_API void registerMcpTools();
} // namespace lila::ui::capture
