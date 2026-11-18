#pragma once

#include "core/Export.h"

#include "render/RenderMesh.h"

#include <QImage>

#include <array>
#include <functional>
#include <string>

namespace lila::ui::textures
{
/**
 * Texture images for the viewport, read on worker threads: EXR, TIFF and PSD through image::read, .hdr through the
 * renderer's loader, the rest with QImage. 8-bit files arrive as RGBA8; files with more bits (16-bit PNG/TIFF/PSD, EXR,
 * HDR) keep their precision as linear RGBA16F. Main thread only. Every getter returns null while its file loads (the
 * viewport redraws once it arrives) or when it can't be read, and reloads a file that changes on disk.
 *
 * A path with a <UDIM> or <UVTILE> token loads the tile set on disk. With udimTiles (material maps), so does a tile's own
 * name (name.1001.png) when sibling tiles exist; image planes leave it off, since frame numbers look the same.
 */
LILA_UI_API render::TexturePtr get(const std::string& path, bool srgb, bool udimTiles = false);

/**
 * A colour texture in an OCIO colour space (ocioSpace, of config; "" for the built-in config), converted to Lila's
 * scene-linear Rec.709 on load and kept linear. Falls back to get(path, srgb) with an error() when the conversion fails.
 */
LILA_UI_API render::TexturePtr getOcio(const std::string& path, const std::string& config, const std::string& ocioSpace, bool srgb);

/**
 * One linear image with occlusion in R, roughness in G and metallic in B: the packed map when given, its channels
 * picked by `channels` (the source channel of occlusion, roughness and metallic), else the red channels of the
 * separate maps, white where one is missing. Bits 0 to 3 of srgbMask mark orm, occlusion, roughness and metallic as
 * sRGB-encoded, decoded to linear as they are packed.
 */
LILA_UI_API render::TexturePtr packed(const std::string& orm, const std::string& occlusion, const std::string& roughness,
                          const std::string& metallic, std::array<int, 3> channels = {0, 1, 2}, uint32_t srgbMask = 0);

/**
 * The normal a material rebuilds from its height map (bake/HeightNormal.h combineNormals): the height's own normals
 * with the normal map laid on them as detail, at the normal map's size; the height's alone when normal is empty. slope
 * is the height's depth over the world size of the UV square, per axis. Encoded as the normal map is (greenUp).
 */
LILA_UI_API render::TexturePtr heightNormal(const std::string& normal, const std::string& height, double slopeU, double slopeV, bool greenUp);

/** Square preview of an image for the material editor, null while loading. */
LILA_UI_API QImage thumbnail(const std::string& path, int size);
/** Why thumbnail(path, size) failed, empty when it loaded or is still loading. */
LILA_UI_API std::string thumbnailError(const std::string& path, int size);

/** Drops a get() image from the cache, e.g. a frame of an image sequence that is no longer shown. */
LILA_UI_API void release(const std::string& path, bool srgb);

/** Why a path failed to load, empty when it loaded or is still loading. */
LILA_UI_API std::string error(const std::string& path);

/** Called on the main thread whenever an image finishes loading. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/**
 * Blocks until every file being read has arrived (or failed), running the main queue meanwhile; false on timeout.
 * Main thread only. Offscreen captures call it so a texture asked for this frame is in the picture.
 */
LILA_UI_API bool waitForLoads(double timeoutSeconds);
} // namespace lila::ui::textures
