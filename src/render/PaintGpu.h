#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "render/GpuImage.h"
#include "scene/Graph.h"
#include "tex/Graph.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

/**
 * GPU paint buffers: each texture set paint buffer (tex/PaintStrokes.h) as RGBA16F pages of 128x128 texels per channel
 * and UDIM tile, allocated on first touch from shared page pools (an untouched page costs nothing). The pages are a cache
 * of the set's paint data: sync rasterises whatever the data has that the pages lack (a scene just opened, an undo past a
 * flatten, a new resolution) by replaying stroke records deterministically.
 *
 * A stroke copies each page it first touches (copy on write): the copy is painted, the original is kept as the stroke's
 * "before" and blended from, and undo swaps the two back, so undo memory is proportional to the pages a stroke touched and
 * restores them bit for bit. Strokes paint per texel by 3D distance to each dab over the surface position the texel's UV
 * raster gives it (and padding texels the nearest edge's), test the stroke view's depth and facing, and write all of the
 * payload's channels in one pass.
 *
 * The texture graph reads a channel through an External buffer named tex::paint::externalName(buffer, channel): its
 * image (externalImage) is kept up to date from the pages after every change.
 *
 * Everything here runs on the main thread (GPU work waits for the render thread), except externalImage.
 */
namespace lila::render::paint
{
using tex::paint::Dab;
using tex::paint::PaintMesh;
using tex::paint::Stroke;
using tex::paint::StrokePtr;

/** The painted part of a mesh node for a material, cached by the node's evaluated mesh. Null when it is not a mesh. */
LILA_UI_API std::shared_ptr<const PaintMesh> paintMesh(scene::Graph& g, const std::string& meshPath, const std::string& material);
/** The material a set's tiles bind to (its settings' material, else its name). */
LILA_UI_API std::string setMaterial(const scene::Node& set);

/** Brings a set's (or one buffer's) pages up to date with its paint data. Throws std::runtime_error on GPU failure. */
LILA_UI_API void sync(scene::Graph& g, scene::Node& set);
LILA_UI_API void syncBuffer(scene::Graph& g, scene::Node& set, const std::string& buffer);
/** Rasterises a buffer from its data from scratch, at its (or the set's) resolution. */
LILA_UI_API void rasterize(scene::Graph& g, scene::Node& set, const std::string& buffer);

// ---------------------------------------------------------------------------------------------------- live strokes

/**
 * Starts painting a stroke as it is drawn (the paint tool): header holds everything but the dabs. Pages update with each
 * liveDabs; applyStroke with the same stroke id then commits it as one undo step. A live stroke left pending is
 * cancelled by the next change to the buffer.
 */
LILA_UI_API void beginLive(scene::Graph& g, scene::Node& set, const std::string& buffer, StrokePtr header);
/** Builds the stroke pipelines on the render thread without waiting, so the first stroke doesn't stall on them. Nothing while the renderer is stopped. */
LILA_UI_API void prewarm();
LILA_UI_API void liveDabs(scene::NodeId set, const std::string& buffer, std::span<const Dab> dabs);
/** Puts the pages back as they were before the live stroke. */
LILA_UI_API void cancelLive(scene::NodeId set, const std::string& buffer);
LILA_UI_API bool liveActive(scene::NodeId set, const std::string& buffer);
/** Whether any buffer of the set has a live stroke. */
LILA_UI_API bool anyLive(scene::NodeId set);

// ---------------------------------------------------------------------------------------------------- commands' helpers

struct ApplyResult
{
    size_t dabs = 0;
    /** Pages the stroke changed, over all channels and tiles. */
    size_t pages = 0;
};

/**
 * Inside a command, after syncBuffer: paints the stroke into the buffer's pages (or commits the live stroke with its id)
 * and records the page undo. The caller writes the stroke into the paint data in the same command. A project stroke's
 * image comes from the set's paint data unless given.
 */
LILA_UI_API ApplyResult applyStroke(scene::Graph& g, scene::Node& set, const std::string& buffer, const Stroke& stroke,
                                    std::shared_ptr<const tex::paint::PaintImage> image = nullptr);
/** Inside a command: drops every page of the buffer, undoably. */
LILA_UI_API size_t clearPages(scene::NodeId set, const std::string& buffer);
/** The buffer's pixels as a baseline (texFlattenLayer). */
LILA_UI_API std::shared_ptr<const tex::paint::Baseline> capture(scene::NodeId set, const std::string& buffer);
/** After the caller wrote data that the pages already show (a flatten, a clear): marks the buffer in sync with it. */
LILA_UI_API void adopt(scene::NodeId set, const std::string& buffer, const tex::paint::Buffer& data);

// ---------------------------------------------------------------------------------------------------- reading

/** A channel tile as RGBA floats (rows top first), zero where nothing is painted; empty when the buffer has none. */
LILA_UI_API image::Image readChannel(scene::NodeId set, const std::string& buffer, const std::string& channel, int udim = 1001);
/** Every allocated page's texels by "channel/udim/page", for bitwise comparisons. */
LILA_UI_API std::map<std::string, std::vector<uint8_t>> pageBytes(scene::NodeId set, const std::string& buffer);
/** {width, height, pages, channels: {channel: {udim: pages}}, strokes applied, live}. */
LILA_UI_API nlohmann::json bufferInfo(scene::NodeId set, const std::string& buffer);
/** Pages allocated by every buffer, process wide. */
LILA_UI_API size_t pagesInUse();

/**
 * What a view of a mesh sees, per pixel (width x height, rows top first): (u, v, triangle as float bits, 1) of the
 * nearest surface, zero where there is none. view is in the mesh's object space. For screen clone sources (sampling a
 * channel through it) and projection checks.
 */
LILA_UI_API image::Image screenUvs(scene::Graph& g, const std::string& meshPath, const tex::paint::StrokeView& view, int width, int height);

/**
 * A GPU image resampled (nearest) over a uv rectangle (u0, v0, u1, v1; v up) into width x height RGBA8 pixels, rows top
 * first, transparent outside the image's 0-1 tile. colorDisplay shows linear colour as sRGB. For 2D views (the UV
 * editor's background); only the view-sized result comes back from the GPU.
 */
LILA_UI_API std::vector<uint8_t> viewImage(const GpuImageRef& image, const glm::vec4& uvRect, int width, int height, bool colorDisplay);

/** A buffer channel's tile as a GPU image (RGBA16F), the one the texture graph reads; null when the buffer has no such tile. */
LILA_UI_API GpuImageRef channelImage(scene::NodeId set, const std::string& buffer, const std::string& channel, int udim = 1001);

/**
 * The image a texture graph External buffer of a set reads: a paint buffer channel's tile (RGBA16F), created on first
 * request. Null for names that aren't paint buffers. Any thread.
 */
LILA_UI_API GpuImageRef externalImage(scene::NodeId set, const std::string& name, int udim);

// ---------------------------------------------------------------------------------------------------- pages

/** A paint buffer channel's tile as its pages: a device table of page addresses (0 where unpainted) and the tile's size. */
struct PageView
{
    VkDeviceAddress table = 0;
    int width = 0;
    int height = 0;
};

/**
 * Render thread (a task or a frame): a buffer channel tile as its last change left it. A tile with nothing painted on
 * the channel has no table; width 0 when the buffer has no such tile.
 */
LILA_UI_API PageView pageView(scene::NodeId set, const std::string& buffer, const std::string& channel, int udim);
/**
 * Render thread: records texels rect {x0, y0, x1, y1} of the view's tile at levelWidth x levelHeight (a box of samples per
 * texel when that is smaller, clamped at the edges) as float4 texels at dst, dstStride texels per row. Nothing for a
 * view with no table.
 */
LILA_UI_API void recordGather(VulkanContext& vk, VkCommandBuffer cmd, const PageView& view, int levelWidth, int levelHeight,
                              const std::array<int, 4>& rect, VkDeviceAddress dst, uint32_t dstStride);
/** Main thread: per UDIM tile, the texels {x0, y0, x1, y1} the set's paint changed since the last call (any buffer, any channel). */
LILA_UI_API std::map<int, std::array<int, 4>> takeChanges(scene::NodeId set);

/** Hears that a set's paint images changed (main thread). Returns an id. */
LILA_UI_API int addChangeListener(std::function<void(scene::NodeId set)> fn);
LILA_UI_API void removeChangeListener(int id);

/**
 * The paint layer strokes on the set go to: requested when given, else the set's active buffer, else the layer stack's
 * top paint layer, else (with create) a new paint layer in the stack, or a loose buffer when the set has no usable stack.
 */
LILA_UI_API std::string targetLayer(scene::Node& set, const std::string& requested, bool create);
/** Points a stack paint layer's channel images at the buffer (texSetLayer), so the stack composites what is painted. */
LILA_UI_API void wireLayer(scene::Node& set, const std::string& layer, const std::vector<std::string>& channels);

/**
 * The set's graph as it will be once a stroke paints layer's channels: layer wired to its buffers for them, or with an
 * empty layer, a new paint layer on top wired for them (as Texture Paint adds one). nullopt when that changes nothing or
 * the set has no usable stack. For compiling the next graph ahead of the stroke.
 */
LILA_UI_API std::optional<tex::Graph> wiredGraph(scene::Node& set, const std::string& layer, const std::vector<std::string>& channels);

/**
 * Builds in the background what the first stroke on a mesh would otherwise build as it starts: the mesh's ray tree, its
 * paint meshes (whole and for the set's material) and the surface atlases of the set's tiles. Main thread; returns at once.
 */
LILA_UI_API void prewarmTarget(scene::Graph& g, scene::Node& set, const std::string& meshPath);

/** texPaintStroke, texRasterize, texFlattenLayer, texClearLayer, texPaintInfo; and installSync. */
LILA_UI_API void registerCommands();
/** Keeps pages in line with the scene's paint data through undo, file loads and deletes. */
LILA_UI_API void installSync();
/** Frees every page and image (before the renderer stops). */
LILA_UI_API void shutdown();
} // namespace lila::render::paint
