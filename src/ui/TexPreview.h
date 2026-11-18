#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include <QImage>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace lila::tex
{
struct Plan;
struct EvalOptions;
} // namespace lila::tex

namespace lila::ui::texPreview
{
/** One texture-graph output to show: a node's output socket, or a texture set channel when channel is set. */
struct Request
{
    std::string set;
    /** Node name; ignored for a channel. */
    std::string node;
    /** Output id; empty takes the node's first output. */
    std::string socket;
    /** A channel of the set (baseColor, roughness...), read from its Material Output. */
    std::string channel;
    int size = 128;
    /** Read the set's paint layers from their buffers (a GPU readback each), and evaluate again when the paint changes. */
    bool paint = false;
};

struct Result
{
    /** Null while the first render runs or when it failed. */
    std::shared_ptr<const image::Image> image;
    /** The port type shown: color, gray, float, vector, sdf2d (material shows its base colour, as color). */
    std::string type;
    /** The image is an older render; a newer one is on its way. */
    bool stale = false;
    std::string error;
};

/**
 * Renders a texture-graph output, cached by everything it depends on (the upstream nodes, their values and links, the
 * size), so value edits re-render only what changed. Main thread. Returns at once: the cached image, else the last one
 * for the same output marked stale (or none) while a worker renders, after which ready runs on the main thread.
 */
LILA_UI_API Result get(const Request& request, std::function<void()> ready = {});

/** Renders now, on the calling thread (tests, batch). */
LILA_UI_API Result render(const Request& request);

/**
 * What evaluates a plan: the GPU evaluator when the renderer can run, else the CPU one. A replacement runs on a worker thread and
 * returns the named output as RGBA floats, rows top first, or throws.
 */
using Renderer = std::function<image::Image(const tex::Plan& plan, const std::string& output, const tex::EvalOptions& options)>;
LILA_UI_API void setRenderer(Renderer renderer);

/** Drops every cached image (a node library reload, a changed image file). */
LILA_UI_API void clear();

/** The folder relative image paths resolve against first: the workspace, else the scene's folder. */
LILA_UI_API std::filesystem::path imageRoot();

/** For display: colour outputs encoded to sRGB, data as it is; channel -1 for all, 0 to 3 to solo one as gray. */
LILA_UI_API QImage toQImage(const image::Image& img, const std::string& type, bool srgb, int channel = -1);
/** Whether a type (and output or channel name) reads as colour, so the view encodes it to sRGB. */
LILA_UI_API bool isColor(const std::string& type, const std::string& name);
} // namespace lila::ui::texPreview
