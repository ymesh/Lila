#pragma once

#include "core/Export.h"

#include "scene/Graph.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Paint layer data on a texture set: per paint buffer, the strokes painted into it as records (strokes are data) and
 * optionally a flattened baseline of pixels the strokes paint over. The pixels themselves live on the GPU as 128x128
 * pages (render/PaintGpu.h), rebuilt from this data whenever they are missing or out of date, so the data is the truth
 * and the pages are a cache: undo, scene files and re-rasterising at another resolution all go through it.
 *
 * Paint buffers are named by id ("paint1"). The texture graph reads a buffer channel through an image parameter whose
 * value is imageRef(id, channel), "external:paint/<id>/<channel>": an External buffer the live viewport and texture
 * exports supply. Each channel image holds the painted value in rgb (gray channels in r, g and b) and its coverage in a.
 * The channel "mask" is the layer's paint mask, written like any other channel.
 *
 * Until the layer stack lands, buffers are created by texPaintStroke and texPaintLayer and stored here; a layer stack
 * keeps this storage and refers to buffers by id.
 */
namespace lila::tex::paint
{
using json = nlohmann::json;

/** Page edge in texels. Pages are allocated lazily on the GPU; an untouched page costs nothing. */
inline constexpr int kPageSize = 128;
/** Texels of UV padding around islands that strokes also paint (by the nearest edge's surface position). */
inline constexpr int kDefaultPadding = 8;
inline constexpr const char* kMaskChannel = "mask";
inline constexpr const char* kDataKind = "texPaint";
/** The texture set attr holding PaintData. */
inline constexpr const char* kPaintAttr = "paint";

/** How a stroke changes a channel: Normal lays the value over, Erase removes coverage, the rest change the value. */
enum class Blend : uint8_t
{
    Normal,
    Erase,
    Add,
    Subtract,
    Multiply,
};

LILA_SCENE_API const char* blendName(Blend b);
LILA_SCENE_API std::optional<Blend> blendFromName(std::string_view name);

/**
 * What a stroke writes: one value per enabled channel, all in one pass. Kinds:
 *
 *   paint      the channel values (or a texture graph's, see source)
 *   direction  the stroke direction as a tangent-space normal tilt into directionChannel; into any other channel as a
 *              flow vector (x, y) * 0.5 + 0.5
 *   smudge     drags what the channels held along the stroke by amount radii
 *   blur       blurs what they held over filterRadius texels; sharpen pushes away from that blur by amount
 *   clone      copies what they held at sourceOffset (uv) from each texel; heal does the same keeping the target's
 *              low frequencies (a healing brush)
 *   fill       the values over whole triangles (triangles), no dabs
 *   project    a screen-space image (image, a projection paint buffer) through the stroke's view: rgb into colorChannel,
 *              coverage from its alpha
 *   normal     sculpts directionChannel's normal with RNM: "bump" from the tip's slope, or "stamp" (stampImage, a
 *              tangent-space normal map) per dab
 *
 * Filters, fills and projections read the channels as they were before the stroke, so the result never depends on how
 * the dabs were batched. Channels named "normal" stay unit length whatever the kind.
 */
struct LILA_SCENE_API Payload
{
    std::string kind = "paint";
    /** Channel name -> value: linear rgb for colour channels, x for gray ones, encoded (0.5, 0.5, 1) normals. */
    std::map<std::string, glm::vec3> channels;
    Blend blend = Blend::Normal;
    /** Also paint the layer's mask (coverage, value 1) unless channels set "mask" themselves. */
    bool mask = true;
    /** direction: how far the normal tilts along the stroke, 0 to 1. */
    float tilt = 0.5f;
    std::string directionChannel = "normal";

    /** blur and heal: the kernel radius in texels. */
    float filterRadius = 4.0f;
    /** smudge: reach in brush radii; sharpen: strength. */
    float amount = 1.0f;
    /** clone and heal: the source, as a uv offset from each texel. */
    glm::vec2 sourceOffset{0.0f};
    /** Per symmetry copy (Dab::copy), its own source offset; copies past the end use sourceOffset. */
    std::vector<glm::vec2> sourceOffsets;
    /** fill: triangles of the stroke mesh's triangulation, ascending, and how they were picked (informational). */
    std::vector<uint32_t> triangles;
    std::string fillBy;
    /** project: the image's PaintData::images key, the channel its rgb paints, and a fade toward grazing angles (0 off, 1 full). */
    std::string image;
    std::string colorChannel = "baseColor";
    float facingFade = 0.0f;
    /**
     * paint and fill: channel values from a texture graph sampled at each texel's uv instead of the constants (the
     * material brush): {"library": id} | {"graph": material graph JSON} | {"node": type, "values": {...}}, plus
     * "resolution" (default 512) and "scale" (tiles per uv unit, default 1). Channels it has no output for keep their
     * constant.
     */
    json source;
    /** A tip from a texture graph (its first output's luminance) instead of the brush's alpha: {"node" | "graph", "values", "resolution"}. */
    json tip;
    /** normal: "bump" or "stamp"; stampImage is the stamp's file; intensity scales either (negative digs). */
    std::string normalMode = "bump";
    std::string stampImage;
    float intensity = 1.0f;
    /** Triangles of the stroke mesh's triangulation the stroke leaves alone (locked faces), ascending. */
    std::vector<uint32_t> locked;
    /** A PaintData::images key: a uv mask (its alpha, tile 1001) every texel's amount is multiplied by (the quick mask). */
    std::string limit;
    /**
     * fill: a gradient over the filled triangles instead of flat values: {type linear|radial|reflected|angle, space screen|uv,
     * start [x, y], end [x, y] (pixels of the stroke's view, or uv), stops [{position 0..1, amount 0..1, channels {name: value}}],
     * dither}. Channels a stop leaves out take the payload's value; amount scales how much paints.
     */
    json gradient;
    /**
     * Brush limits, as asked: {cavity|convexity|curvature|ao|thickness|height: [lo, hi], id: {color [r, g, b], tolerance},
     * group: name, facing: 0..1}. The command turns the map ranges into limit (a uv mask) and the group into locked, which
     * is what replays; this stays for presets and to show what was asked.
     */
    json limits;

    /** The channels the stroke writes, the mask last. */
    std::vector<std::string> written() const;
    /** Whether the kind reads the channels' prior pixels around each texel (smudge, blur, sharpen, clone, heal). */
    bool filters() const;
    json toJson() const;
    /** Throws std::invalid_argument on a bad value. */
    static Payload fromJson(const json& j);
    bool operator==(const Payload&) const = default;
};

/** Every payload kind, in the order of Payload's documentation. */
LILA_SCENE_API const std::vector<std::string>& payloadKinds();

/** One brush stamp on the surface: a point of a triangle of the stroke mesh's triangulation (geo::triangulate). */
struct Dab
{
    uint32_t triangle = 0;
    /** Barycentric weights of the triangle's corners 1 and 2 (corner 0 takes 1 - u - v). */
    float u = 0.0f, v = 0.0f;
    /** Object units. */
    float radius = 1.0f;
    float pressure = 1.0f;
    /** Per dab amount: pressure curves, flow, jitter. */
    float opacity = 1.0f;
    /** Tip angle in radians about the surface normal, from tipFrame's tangent. */
    float angle = 0.0f;
    /** 0 for the dab, k for its k-th symmetry copy: direction strokes follow each copy separately. */
    uint32_t copy = 0;
    /** Pen tilt, 0 upright to 1: the brush's tilt roundness narrows the tip by it. Size, opacity and angle already hold their tilt. */
    float tilt = 0.0f;

    bool operator==(const Dab&) const = default;
};

/** The view a stroke was painted from, in the stroke mesh's object space: for occlusion and screen-space input. */
struct StrokeView
{
    /** Object space to Vulkan clip space (reversed Z, y down). */
    glm::mat4 objectToClip{1.0f};
    glm::vec3 eye{0.0f};
    /** Into the screen. */
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    int width = 1;
    int height = 1;

    bool operator==(const StrokeView&) const = default;
};

struct PaintImage;

/**
 * A picture pinned to the screen that a stroke paints through (needs the stroke's view). At scale 1 it spans the view's
 * height, centred; offset (view pixels), scale and rotation (radians) place it. mask: its luminance times alpha scales
 * the stroke's amount. color: its rgb is the value painted into colorChannel and its alpha the amount.
 */
struct LILA_SCENE_API StrokeStencil
{
    /** PaintData::images key. */
    std::string image;
    glm::vec2 offset{0.0f};
    float scale = 1.0f;
    float rotation = 0.0f;
    bool invert = false;
    bool tile = false;
    std::string mode = "mask";
    std::string colorChannel = "baseColor";

    json toJson() const;
    /** Throws std::invalid_argument on a bad value. */
    static StrokeStencil fromJson(const json& j);
    bool operator==(const StrokeStencil&) const = default;
};

/**
 * Images strokes are about to refer to that no texture set holds yet (a stencil picked in the paint tool), so a live
 * stroke can read them before its command stores them. Keeps the most recent few. Any thread.
 */
LILA_SCENE_API void holdImage(std::shared_ptr<const PaintImage> image);
LILA_SCENE_API std::shared_ptr<const PaintImage> heldImage(const std::string& key);

struct LILA_SCENE_API Stroke
{
    /** Unique within the buffer; the tool and the command agree on it. */
    uint64_t id = 0;
    /** The painted mesh's node path. */
    std::string mesh;
    /** The brush preset as it was (brush::BrushSettings JSON): falloff, roundness, alpha, strength... */
    json brush = json::object();
    Payload payload;
    uint32_t seed = 0;
    /** Hide texels the view does not see (needs view). */
    bool occlusion = true;
    std::optional<StrokeView> view;
    std::vector<Dab> dabs;
    /** The particle brush that added to the dabs (PaintParticles.h), kept for reference: the dabs already hold its particles. */
    json particles;
    /**
     * The symmetry the stroke was painted with (PaintSymmetry JSON): the dabs already hold every copy, numbered by
     * Dab::copy; replay reads it for mirrored tips and stamps, and a projection projects once per copy.
     */
    json symmetry;
    std::optional<StrokeStencil> stencil;
    /**
     * A path stroke's Bezier, in the stroke mesh's object space: {points: [{position, in, out}], curve?: the scene curve it
     * came from}. The dabs are already sampled from it; editing it (texEditPath) samples them again.
     */
    json path;
    /**
     * Where each dab was, in the stroke mesh's object space, and the mesh's signature (PaintMesh::signature) then: when the
     * mesh's triangles or uvs change, replay finds each dab again at the closest surface point.
     */
    std::vector<glm::vec3> anchors;
    uint64_t meshSignature = 0;

    /** The PaintData::images keys the stroke reads (a projection's picture, its stencil). */
    std::vector<std::string> images() const;
    /** packed stores the dabs as binary arrays (for binary scene files); plain JSON otherwise. */
    json toJson(bool packed = true) const;
    /**
     * Dabs as packed arrays, rows [triangle, copy] and [u, v, radius, pressure, opacity, angle, tilt] (tilt optional), or objects
     * {triangle, bary: [u, v], radius, pressure?, opacity?, angle?, copy?, tilt?}. Throws std::invalid_argument.
     */
    static Stroke fromJson(const json& j);
    bool operator==(const Stroke&) const;
};

using StrokePtr = std::shared_ptr<const Stroke>;

/** Pixels a flatten kept, per channel and UDIM tile, as compressed RGBA16F pages. */
struct LILA_SCENE_API Baseline
{
    struct LILA_SCENE_API Tile
    {
        std::string channel;
        int udim = 1001;
        /** The tile's size in texels when it was captured. */
        int width = 0;
        int height = 0;
        /** Page indices (py * pagesX + px), ascending. */
        std::vector<uint32_t> pages;
        /** zstd of the pages' RGBA16F texels, kPageSize^2 * 8 bytes each, in pages order. Empty when they are in a page store. */
        std::vector<uint8_t> data;

        /** A chunk of the pages in the scene's page store (tex/PageStore.h): its file and its pages, in pages order. */
        struct Chunk
        {
            std::string file;
            std::vector<uint32_t> pages;
        };
        /**
         * Where the pages are when not in data: chunk files of the store named store (beside the scene), last seen at
         * storeAbs. A save fills these in (the pixels are unchanged), so the next save keeps the same files.
         */
        mutable std::vector<Chunk> chunks;
        mutable std::string store;
        mutable std::string storeAbs;

        /** The pages' RGBA16F texels in pages order, from data or the store. Throws std::runtime_error. */
        std::vector<uint8_t> raw() const;
    };
    std::vector<Tile> tiles;

    size_t bytes() const;
    /**
     * Inside a scene save (tex::store::activeStore()), the pages go to the store as chunks of layer (the buffer id) and
     * the JSON refers to them; otherwise they are inline.
     */
    json toJson(const std::string& layer = {}) const;
    /** Reads both forms. */
    static std::shared_ptr<const Baseline> fromJson(const json& j);
};

/** Packs pages (kPageSize^2 RGBA16F texels each, concatenated) for a Baseline tile. */
LILA_SCENE_API std::vector<uint8_t> compressPages(const std::vector<uint8_t>& raw);
/** Throws std::runtime_error on corrupt data. */
LILA_SCENE_API std::vector<uint8_t> decompressPages(const std::vector<uint8_t>& packed, size_t rawBytes);

struct LILA_SCENE_API Buffer
{
    std::vector<StrokePtr> strokes;
    std::shared_ptr<const Baseline> baseline;
    /** Texels per side the buffer rasterises at; 0 follows the texture set's tiles. */
    int width = 0;
    int height = 0;
    /** Bumped by texClearLayer, so a clear undoes and redoes as its own state. */
    uint64_t clears = 0;
};

/** An image strokes refer to by key (a projection's buffer): RGBA16F texels, rows top first, zstd packed. */
struct LILA_SCENE_API PaintImage
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;

    /** Packs RGBA floats (rounded to half). */
    static std::shared_ptr<const PaintImage> fromRgba(int width, int height, std::span<const float> rgba);
    /** The texels as floats, exactly the halves stored. Throws std::runtime_error on corrupt data. */
    std::vector<float> rgba() const;
    /** "img" and a hash of the texels: equal images, equal keys. */
    std::string key() const;
    json toJson() const;
    static std::shared_ptr<const PaintImage> fromJson(const json& j);
};

LILA_SCENE_API uint16_t floatToHalf(float f);
LILA_SCENE_API float halfToFloat(uint16_t h);

/** Every paint buffer of a texture set, immutable and shared like meshes (an edit makes a new one). */
struct LILA_SCENE_API PaintData final : scene::Data
{
    std::map<std::string, Buffer> buffers;
    /** The buffer painting tools write by default. */
    std::string active;
    /** Images strokes refer to (Payload::image), by key; dropped once no stroke refers to them. */
    std::map<std::string, std::shared_ptr<const PaintImage>> images;

    /** Drops images no stroke refers to. */
    void pruneImages();

    const char* kind() const override { return kDataKind; }
    json toJson() const override;
    size_t bytes() const override;
    static scene::DataPtr fromJson(const json& j);

    const Buffer* buffer(std::string_view id) const;
    /** A free buffer id: paint1, paint2, ... */
    std::string uniqueId() const;
};

using PaintPtr = std::shared_ptr<const PaintData>;

/** Registers the data kind. Called by tex::registerTypes. */
LILA_SCENE_API void registerDataKind();

/** The set's paint data; an empty PaintData when it has none. */
LILA_SCENE_API PaintPtr readPaint(const scene::Node& set);
/** Undoable (inside a command). */
LILA_SCENE_API void writePaint(scene::Graph& g, scene::Node& set, PaintPtr data);

/** The image parameter value that reads a buffer channel: "external:paint/<id>/<channel>". */
LILA_SCENE_API std::string imageRef(std::string_view buffer, std::string_view channel);
/** The External buffer name of a buffer channel: "paint/<id>/<channel>". */
LILA_SCENE_API std::string externalName(std::string_view buffer, std::string_view channel);
/** Splits an External name made by externalName; false for anything else. */
LILA_SCENE_API bool parseExternalName(std::string_view name, std::string* buffer, std::string* channel);

/** The tip's reference tangent on a surface with this normal: angle 0 points along it. Deterministic. */
LILA_SCENE_API glm::vec3 tipFrame(const glm::vec3& normal);
} // namespace lila::tex::paint
