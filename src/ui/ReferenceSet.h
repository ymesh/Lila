#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * The reference images an artist works from, shared by the Reference windows (tabs beside the viewports) and the
 * reference overlays (an image glued to a view's screen, in front of or behind the model). Adding, removing and
 * replacing images are undo steps; view tweaks (pan, zoom, overlay placement and opacity) are not, like camera moves.
 * The set saves with the scene, or in the project folder or the user's config, per the storage preference.
 */
namespace lila::ui::references
{
struct Image
{
    std::string id;
    std::string name; // tab label
    /** As stored (paths::storedPath); empty when embedded only. */
    std::string path;
    /** The file's bytes, base64, when embedded in the set; empty when linked. */
    std::string data;
    std::string ext; // of the embedded file, without the dot
    bool flipH = false;
    bool flipV = false;
    bool grayscale = false;
    int quarterTurns = 0; // clockwise, 0..3
    double brightness = 0.0;
    double contrast = 1.0;
    /** The Reference window it is a tab of; empty when it only shows as an overlay. */
    std::string window;
    /** Video: frames added to the one shown, and the rate it plays at (0 for the file's own). */
    int videoOffset = 0;
    double videoFps = 0.0;
    /** Video: holds pausedFrame instead of following the scene time. */
    bool paused = false;
    int64_t pausedFrame = 0;
};

enum class Anchor
{
    Center,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

enum class Depth
{
    Behind,
    Front,
};

enum class Mode
{
    Normal,
    Grayscale,
    Outline,
};

/** The camera a reference photo was lined up from, so the view can return to it. */
struct BoundView
{
    bool set = false;
    glm::dvec3 target{0.0};
    double distance = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
    double focalLength = 35.0;
    bool ortho = false;
    double orthoWidth = 0.0;
    glm::dvec3 forward{0.0, 0.0, -1.0}; // world view direction when bound
};

struct Overlay
{
    std::string id;
    std::string image;
    std::string view = "persp";
    Anchor anchor = Anchor::Center;
    /** Image centre from the anchor point, in view heights, y down. */
    glm::dvec2 offset{0.0};
    /** Shown height of the cropped image in view heights. */
    double scale = 0.6;
    /** The part of the image shown, (u0, v0, u1, v1) of the unflipped image, v down. */
    glm::dvec4 crop{0.0, 0.0, 1.0, 1.0};
    double rotation = 0.0; // degrees clockwise on screen
    double opacity = 0.3;
    Depth depth = Depth::Front;
    Mode mode = Mode::Normal;
    bool visible = true;
    /** Draws the scene's meshes see-through while this overlay shows, for working over a photo behind the model. */
    bool ghost = false;
    glm::vec3 outlineColor{0.25f, 0.85f, 1.0f};
    BoundView bound;
    /** Degrees the view may turn away from the bound view before the overlay has faded out. */
    double fadeAngle = 15.0;
    /** The lens the photo was taken with, offered when binding. */
    double focalLength = 105.0;
};

struct Window
{
    std::string id;
    std::string current; // image id of the shown tab
    double opacity = 1.0;
    bool onTop = false;
    bool clickThrough = false;
};

struct LILA_UI_API Set
{
    std::vector<Image> images;
    std::vector<Overlay> overlays;
    std::vector<Window> windows;

    nlohmann::json toJson() const;
    static Set fromJson(const nlohmann::json& j);

    Image* image(const std::string& id);
    const Image* image(const std::string& id) const;
    Overlay* overlay(const std::string& id);
    const Overlay* overlay(const std::string& id) const;
    Window* window(const std::string& id);
    const Window* window(const std::string& id) const;
    /** Images that are tabs of the window, in tab order. */
    std::vector<const Image*> tabs(const std::string& window) const;
    std::vector<const Overlay*> overlaysIn(const std::string& view) const;
    std::string newImageId() const;
    std::string newOverlayId() const;
    std::string newWindowId() const;
};

/** The first Reference window, always present in the set's window list. */
inline constexpr const char* kMainWindow = "main";
/** The scene file section ("ui"/key) the set saves under. */
inline constexpr const char* kSection = "referenceImages";
inline constexpr const char* kStoragePref = "/references/storage";
inline constexpr const char* kFileName = "references.lilaref";

enum class Storage
{
    Scene,
    Project, // <project>/references.lilaref
    User,    // <user config>/references.lilaref
};
LILA_UI_API Storage storage();
/** Moves the current set to the new storage: it is written there and the old place stops receiving it. */
LILA_UI_API void setStorage(Storage s);
LILA_UI_API std::string storageName(Storage s);
LILA_UI_API std::optional<Storage> storageFromName(const std::string& name);
/** The file a Project or User set lives in; empty for Scene or with no project open. */
LILA_UI_API std::filesystem::path storageFile(Storage s);

LILA_UI_API const Set& current();
/**
 * Replaces the set. label non-empty records it as an undo step under that name (inside the running command's chunk);
 * empty is a view tweak that only marks the set changed.
 */
LILA_UI_API void apply(Set next, const std::string& label = {});
/** Edits the set in place as a view tweak: no undo step. */
LILA_UI_API void tweak(const std::function<void(Set&)>& edit);
/** Called after every change, undo and load. Returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** The file to read an image from: its linked path resolved, or its embedded bytes written to the reference cache. */
LILA_UI_API std::string fileOf(const Image& img);
/** Reads the file into img.data / img.ext. False with a log message when it can't be read or is a video (videos stay linked). */
LILA_UI_API bool embed(Image& img);
/** Whether the image is a video file. */
LILA_UI_API bool isVideo(const Image& img);
/** The frame a video reference shows at a scene time: its pause, offset and rate applied, looping over its length. */
LILA_UI_API int64_t videoFrame(const Image& img, double sceneSeconds);
/** Image and video files a set accepts. */
LILA_UI_API bool isImageFile(const std::string& path);

/** Writes the set to a .lilaref file. Throws std::runtime_error. */
LILA_UI_API void saveFile(const Set& set, const std::filesystem::path& file);
/** Throws std::runtime_error. */
LILA_UI_API Set loadFile(const std::filesystem::path& file);

/** Dropping files on a Reference window: new tabs, or with Shift the current tab's image replaced. */
enum class DropAction
{
    AddTabs,
    Replace,
};
LILA_UI_API DropAction dropAction(bool shift, bool hasCurrentTab);

/** An overlay's screen corners in view pixels, top left, top right, bottom right, bottom left of the image as shown. */
LILA_UI_API std::array<glm::dvec2, 4> overlayCorners(const Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize);
LILA_UI_API bool overlayContains(const Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize, glm::dvec2 px);
/** The image's (u0, v0, u1, v1) for the corners overlayCorners gives: the crop, with flips swapping the ends. */
LILA_UI_API glm::dvec4 overlayUv(const Overlay& o, const Image* img);
/** The image (u, v) under view pixel px, outside 0..1 when px is off the image. */
LILA_UI_API glm::dvec2 overlayImageUv(const Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize, glm::dvec2 px);
/** Changes the crop keeping the image where it is on screen: only the shown part grows or shrinks. */
LILA_UI_API void setCrop(Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize, glm::dvec4 crop);
/** How much of the overlay shows with the view looking along forward: 1 unbound or at the bound view, 0 past fadeAngle. */
LILA_UI_API double boundFade(const Overlay& o, const glm::dvec3& forward);
/** Lays the view's overlays out down its right edge, opaque: Spotlight's tile, for keeping references beside the model. */
LILA_UI_API void park(Set& set, const std::string& view, const std::function<double(const Image&)>& imageAspect, glm::dvec2 viewSize);
/** Moves an overlay to anchor without moving it on screen. */
LILA_UI_API void reanchor(Overlay& o, Anchor anchor, glm::dvec2 viewSize);

/** referenceImage, referenceOverlay and referenceSet commands, the scene section and storage handling. Idempotent. */
LILA_UI_API void install();
} // namespace lila::ui::references
