#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "render/FrameState.h"
#include "scene/Graph.h"
#include "ui/ViewInfo.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Declared here rather than included: GpuImage.h pulls in Vulkan, which the SDK's extension headers don't carry.
namespace lila::render
{
class GpuImage;
using GpuImageRef = std::shared_ptr<GpuImage>;
} // namespace lila::render

/**
 * Viewport renderers (Maya's renderer override): an external renderer draws a viewport's image in place of Lila's own,
 * and Lila composites its wires, selection highlights, gizmos, tools, HUD and every other overlay on top, depth testing
 * them against the renderer's depth when it gives one. Selection and picking stay Lila's.
 *
 * Each viewport picks its renderer (kLila, Lila's own, by default). Picking one opens a Session for that viewport; Lila
 * then calls Session::render whenever what the view shows changes (camera, size, time or the scene) and shows the
 * newest frame delivered, so a renderer may answer at once, later from another thread, or several times as it refines.
 */
namespace lila::ui::viewportRenderers
{
/** Lila's own renderer: every viewport draws with it until another is picked. */
inline constexpr const char* kLila = "lila";

/** A frame for a session, as delivered; call from any thread. */
struct Frame;
using Deliver = std::function<void(Frame frame)>;

/** What a viewport wants drawn. Copyable, so a renderer can keep it for a worker thread (all but graph). */
struct LILA_UI_API Request
{
    /** The viewport, as hud prefs and the viewportRenderer command number them. */
    int panel = 0;
    /** Rises with every request of the viewport; hand it back in Frame::serial. */
    uint64_t serial = 0;
    /** Device pixels. */
    int width = 0;
    int height = 0;
    /** The camera, in logical (widget) pixels for its ray and project helpers. view.proj is Lila's: reversed-Z, Vulkan clip space. */
    ViewInfo view;
    double nearClip = 0.1;
    /** 0 for an infinite far plane. */
    double farClip = 0.0;
    /** Seconds: the scene's current time. */
    double time = 0.0;
    /** Rises with every scene change except selection; equal values mean the same scene. */
    uint64_t sceneRevision = 0;
    /** The view's exposure (a linear multiplier) and display gamma, for renderers that want to match Lila's look. */
    float exposure = 1.0f;
    float gamma = 1.0f;
    /** The scene. Main thread only: read what a worker needs before handing the request over. */
    scene::Graph* graph = nullptr;
    /** Takes frames for this request. Holds no viewport alive: frames after the viewport closed are dropped. */
    Deliver deliver;

    /** World-space ray through a point in device pixels, x right and y down from the top left; pixel (i, j)'s centre is (i + 0.5, j + 0.5). */
    void ray(double x, double y, glm::dvec3& origin, glm::dvec3& dir) const;
    /** The depth Frame::depth wants for a world point: its distance along the view axis, cm. */
    double depthOf(const glm::dvec3& world) const;
};

/**
 * An image for a request. Colour comes from the CPU (rgba8, texels or rgba32f) or already on Lila's GPU (image), with
 * straight alpha over the viewport's background unless premultiplied; depth is optional, from the CPU (depth) or the GPU
 * (depthImage). A frame whose size differs from the view is stretched over it. bottomUp, premultiplied, exposure and
 * glDepthProj let a renderer hand over its buffers as they are and leave the conversion to Lila's composite.
 */
struct Frame
{
    int width = 0;
    int height = 0;
    /** 8-bit RGBA, display-encoded (sRGB), row 0 at the top: width * height * 4 bytes. */
    std::vector<uint8_t> rgba8;
    /** Linear float RGBA, row 0 at the top: width * height * 4 floats. Shown as given; apply Request::exposure yourself. */
    std::vector<float> rgba32f;
    /** Colour texels as they are, tightly packed in texelFormat (RGBA8 or RGBA16F, both linear). Used before rgba32f. */
    std::vector<uint8_t> texels;
    render::TextureFormat texelFormat = render::TextureFormat::RGBA16F;
    /** Colour and depth rows run bottom up (Hydra's buffers) instead of top down. */
    bool bottomUp = false;
    /** Colour alpha is premultiplied. */
    bool premultiplied = false;
    /** Lila multiplies colour by this, for a renderer that leaves Request::exposure to it. */
    float exposure = 1.0f;
    /**
     * When set, depth or depthImage holds OpenGL window depth (0 near, 1 far) for this OpenGL clip-space projection
     * instead of view distances; 1, or zero alpha, is nothing hit.
     */
    std::optional<glm::dmat4> glDepthProj;
    /**
     * An image on Lila's device (render::Renderer::context()), RGBA8 sRGB or RGBA16F, written through
     * render::Renderer::runTask so it is complete before it is delivered. Wins over the CPU colour.
     */
    render::GpuImageRef image;
    /** Distance along the view axis in cm per pixel, 0 or less (or infinity) where nothing was hit: width * height floats. */
    std::vector<float> depth;
    /** The same as an R32F image on Lila's device. Wins over depth. */
    render::GpuImageRef depthImage;
    /** The request this answers. A frame for a request older than the one shown is dropped. */
    uint64_t serial = 0;
};

/** A setting a session offers (Hydra's render setting descriptors). value is its current value: a bool, number or string. */
struct Setting
{
    std::string key;
    std::string name;
    nlohmann::json value;
};

/** One viewport's use of a renderer: opened when the viewport picks it, destroyed when it picks another or closes, or the renderer goes. */
class LILA_UI_API Session
{
public:
    virtual ~Session() = default;
    /** Main thread. Draw what r describes and hand the frame to r.deliver, now or later and from any thread. */
    virtual void render(const Request& r) = 0;
    /**
     * Main thread: the scene changed, as Graph listeners hear it (one Change::Reset after a batch, Cleared for a new
     * scene). A render call follows, so renderers that rebuild from the graph need not override this; it is the feed a
     * renderer keeping its own copy of the scene (a Hydra scene index) updates from.
     */
    virtual void sceneChanged(scene::Change /*change*/, scene::NodeId /*node*/) {}
    /** Main thread: the settings this session offers, with their current values. */
    virtual std::vector<Setting> settings() const { return {}; }
    /** Main thread: changes one; false for a key it doesn't know or a value it refuses. A render call follows. */
    virtual bool setSetting(const std::string& /*key*/, const nlohmann::json& /*value*/) { return false; }
    /** Main thread: timings and counters for viewportStats, as a JSON object of any keys the renderer likes. */
    virtual nlohmann::json stats() const { return nlohmann::json::object(); }
};

struct Renderer
{
    std::string id;
    /** Shown in the viewport's renderer menu. */
    std::string name;
    std::string doc;
    /** Opens a session for a viewport panel. Null (or a throw) refuses, and the viewport stays as it was. */
    std::function<std::unique_ptr<Session>(int panel)> open;
};

LILA_UI_API Registry<Renderer>& registry();
/** Throws std::invalid_argument when the id is taken or is kLila. */
LILA_UI_API const Renderer& add(Renderer renderer);
/** For extensions: owned by ctx and removed when it unloads, which returns its viewports to Lila. Null, logged, when the id is taken. */
LILA_UI_API const Renderer* add(ext::Context& ctx, Renderer renderer);
/** Viewports drawing with it go back to Lila. */
LILA_UI_API bool remove(std::string_view id);

/** {id, name} of every renderer a viewport can pick: Lila's first, then the registered ones in the order added. */
LILA_UI_API std::vector<std::pair<std::string, std::string>> choices();

/** Makes panel draw with renderer id (kLila or empty for Lila's own). False, changing nothing, for an unknown id or a refused session. */
LILA_UI_API bool select(int panel, std::string_view id);
/** The renderer panel draws with: kLila unless another was picked. */
LILA_UI_API std::string selected(int panel);
/**
 * The settings saved for a renderer, {key: value}: each one changed through setSetting, applied to every session it opens
 * after open() returns. Sessions that need them while starting (Hydra's CreateDelegate) read them here.
 */
LILA_UI_API nlohmann::json savedSettings(std::string_view renderer);
/** Changes a setting of panel's session, then saves it and applies it to the renderer's other sessions. False when the session refuses it. */
LILA_UI_API bool setSetting(int panel, const std::string& key, const nlohmann::json& value);
/** The settings of panel's session; empty for Lila's own renderer. */
LILA_UI_API std::vector<Setting> settings(int panel);
/** What the external renderer drawing panel reports from Session::stats; null when Lila draws it. */
LILA_UI_API nlohmann::json stats(int panel);
/** Called on the main thread when a viewport picks another renderer. */
LILA_UI_API int addListener(std::function<void(int panel)> fn);
LILA_UI_API void removeListener(int id);

/** The viewport the commands act on when given no panel. */
LILA_UI_API void setActivePanel(std::function<int()> fn);
LILA_UI_API int activePanel();

// ---------------------------------------------------------------- the viewport's side

/**
 * Called by the viewport each frame with what it shows (serial, graph and deliver are filled in here). When panel draws
 * with an external renderer, asks it for a frame if r differs from the last request, then marks s to show the newest
 * frame delivered and returns true; false, leaving s alone, when Lila draws.
 */
LILA_UI_API bool apply(int panel, Request r, render::FrameState& s);
/** Runs on the main thread after a frame for panel is delivered, so the viewport redraws. */
LILA_UI_API void setRedraw(int panel, std::function<void()> fn);
/** The viewport reports its display's refresh rate, in Hz. */
LILA_UI_API void setRefreshRate(int panel, std::function<double()> fn);
/**
 * Seconds between refreshes of the display showing panel (1/60 when unknown). A renderer whose frame is not yet final
 * draws again this long after the last one. Main thread only.
 */
LILA_UI_API double refreshInterval(int panel);
/** The viewport closed: ends its session and forgets it. */
LILA_UI_API void release(int panel);

/** The viewportRenderer, viewportRenderers and viewportRendererSetting commands. */
LILA_UI_API void registerCommands();
} // namespace lila::ui::viewportRenderers
