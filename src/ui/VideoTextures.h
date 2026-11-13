#pragma once

#include "core/Export.h"

#include "media/Decode.h"
#include "render/RenderMesh.h"

#include <QImage>

#include <functional>
#include <optional>
#include <string>

/**
 * Video frames for the viewport and Qt widgets, shared by image planes and reference images. Each file decodes on a
 * thread of its own into a frame cache (media::VideoStream); a frame not decoded yet shows the nearest one that is, and
 * the listeners hear when the asked frame lands. Main thread.
 */
namespace lila::ui::videoTextures
{
/** The file's stream info, null when it can't be opened (error says why). path is resolved, UTF-8. */
LILA_UI_API const media::VideoInfo* info(const std::string& path);
LILA_UI_API std::string error(const std::string& path);

/**
 * A viewport texture showing frame index (the GPU image is updated in place, one per file); null until a frame has
 * decoded.
 */
LILA_UI_API render::TexturePtr texture(const std::string& path, int64_t index);
/** The same frame as a QImage, for widgets; null until a frame has decoded. */
LILA_UI_API QImage image(const std::string& path, int64_t index);
/**
 * The frame a reference video shows at a scene time: from time 0 at fps (0 for its own rate), offset frames added,
 * looping over its length.
 */
LILA_UI_API int64_t referenceFrame(const std::string& path, double sceneSeconds, double fps = 0.0, int64_t offset = 0);
/** Blocks until the frame has decoded; for offscreen captures and tests. */
LILA_UI_API bool waitFor(const std::string& path, int64_t index, double timeoutSeconds);

/** Called on the main thread when an asked-for frame lands. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** Drops a file's stream and GPU image. */
LILA_UI_API void release(const std::string& path);
} // namespace lila::ui::videoTextures
