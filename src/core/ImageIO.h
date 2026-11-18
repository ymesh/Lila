#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Small dependency-free image files for tools that write textures (VAT, the baker): PNG 8/16 bit RGBA, OpenEXR half or
 * float RGBA (ZIP, scanline), TGA 8 bit RGBA (uncompressed), TIFF 8/16 bit or 32 bit float RGBA (uncompressed or
 * deflate) and layered PSD 8/16 bit (one layer per image). Texels are RGBA floats, row 0 is the top scanline. Integer
 * formats clamp to 0 to 1, optionally with triangular dither at 8 bit; EXR and float TIFF store the values as they are.
 */
namespace lila::image
{
namespace fs = std::filesystem;

struct Image
{
    int width = 0;
    int height = 0;
    /** width * height * 4 floats. */
    std::vector<float> rgba;
    /** The file's embedded ICC colour profile (TIFF and PSD), empty when it carries none. */
    std::vector<unsigned char> icc;
};

/** How write() stores an image. */
struct WriteOptions
{
    /** PNG 8/16, EXR 16 (half) or 32 (float), TIFF 8/16/32 (float), PSD 8/16; TGA is always 8. */
    int bits = 8;
    /** Triangular (TPDF) dither when quantising to 8 bit. Values already on an 8-bit step stay exact. */
    bool dither = false;
    /** TIFF: deflate (zlib) strips. TGA: run-length packets. Else uncompressed. */
    bool compress = true;
    /** Deflate level for PNG, EXR and TIFF: 1 is fastest, 9 smallest. */
    int level = 6;
};

/** One layer of a layered PSD; rgba is width * height * 4 floats. */
struct Layer
{
    std::string name;
    std::span<const float> rgba;
};

/** bits is 8 or 16. Throws std::runtime_error when the file can't be written. */
LILA_FOUNDATION_API void writePng(const fs::path& path, int width, int height, std::span<const float> rgba, int bits, bool dither = false,
                                  int level = 6);
/** bits 16 is half float, 32 is float. Throws std::runtime_error. */
LILA_FOUNDATION_API void writeExr(const fs::path& path, int width, int height, std::span<const float> rgba, int bits = 16, int level = 6);

/** One EXR channel: a value per pixel, top row first, named as layers do ("R", "depth.Z", "CryptoObject00.r"). */
struct ExrChannel
{
    std::string name;
    std::vector<float> data;
    bool full = false; // 32-bit float; else half
};

/** A header attribute in its EXR encoding. */
struct ExrAttribute
{
    std::string name, type;
    std::vector<unsigned char> value;
};

LILA_FOUNDATION_API ExrAttribute exrString(const std::string& name, const std::string& value);
/** Rec.709 primaries with a D65 white: Lila's scene-linear working space. */
LILA_FOUNDATION_API ExrAttribute exrChromaticitiesRec709();

/** A multi-channel (multilayer) EXR, ZIP compressed, with extra header attributes. Throws std::runtime_error. */
LILA_FOUNDATION_API void writeExrChannels(const fs::path& path, int width, int height, std::vector<ExrChannel> channels,
                                          const std::vector<ExrAttribute>& attributes = {}, int level = 6);
/** 8 bit RGBA, top-left origin, run-length encoded unless rle is false. Throws std::runtime_error. */
LILA_FOUNDATION_API void writeTga(const fs::path& path, int width, int height, std::span<const float> rgba, bool dither = false,
                                  bool rle = true);
/** RGBA with unassociated alpha, 8/16 bit unsigned or 32 bit float, deflate with a horizontal predictor for integers. Throws std::runtime_error. */
LILA_FOUNDATION_API void writeTiff(const fs::path& path, int width, int height, std::span<const float> rgba, int bits, bool deflate = true,
                                   bool dither = false, int level = 6);
/**
 * An RGB document with one RGBA layer per entry, the first on top, and the first layer as the flattened image. 8 or 16
 * bit (16-bit layers go in the Lr16 block, as Photoshop writes them). At most 30000 x 30000. Throws std::runtime_error.
 */
LILA_FOUNDATION_API void writePsd(const fs::path& path, int width, int height, std::span<const Layer> layers, int bits, bool dither = false);

/** By extension (.png, .exr, .tga, .tif/.tiff, .psd as a single layer). Throws std::runtime_error, also for an unknown extension. */
LILA_FOUNDATION_API void write(const fs::path& path, const Image& image, const WriteOptions& options);
/** bits applies to PNG only. */
LILA_FOUNDATION_API void write(const fs::path& path, const Image& image, int bits = 8);

/** Reads what the writers above write (EXR half or float, TGA 24/32 bit uncompressed); readPng also takes JPEG and every PNG variant. Empty image and *err set on failure. */
LILA_FOUNDATION_API Image readPng(const fs::path& path, std::string* err = nullptr);
LILA_FOUNDATION_API Image readExr(const fs::path& path, std::string* err = nullptr);
LILA_FOUNDATION_API Image readTga(const fs::path& path, std::string* err = nullptr);
/** Strip-based RGB/RGBA, 8/16 bit unsigned or 32 bit float, uncompressed, LZW, PackBits or deflate, predictor none or horizontal. */
LILA_FOUNDATION_API Image readTiff(const fs::path& path, std::string* err = nullptr);
/** The flattened image of an uncompressed 8/16 bit RGB PSD. */
LILA_FOUNDATION_API Image readPsd(const fs::path& path, std::string* err = nullptr);
LILA_FOUNDATION_API Image readWebp(const fs::path& path, std::string* err = nullptr);
/** A WebP image held in memory. */
LILA_FOUNDATION_API Image decodeWebp(const uint8_t* data, size_t size, std::string* err = nullptr);
/** The layers of an uncompressed 8/16 bit RGB PSD, bottom first as stored, with their names. */
LILA_FOUNDATION_API std::vector<std::pair<std::string, Image>> readPsdLayers(const fs::path& path, std::string* err = nullptr);
/** By extension; anything not named below reads as PNG or JPEG. */
LILA_FOUNDATION_API Image read(const fs::path& path, std::string* err = nullptr);

/** The extensions write() takes, lowercase without the dot: png, exr, tga, tif, tiff, psd. */
LILA_FOUNDATION_API const std::vector<std::string>& writableFormats();

/** 0 to 255 for v (clamped to 0 to 1); with dither, a deterministic triangular dither per texel (x, y) and channel c. */
LILA_FOUNDATION_API uint8_t quantize8(float v, int x, int y, int c, bool dither);

/**
 * A UDIM tile's label in a file name: style "1001" (Mari, Unreal) gives "1001", "uv" (Mudbox, 1-based) gives "u1_v1",
 * "uv0" (0-based) gives "u0_v0". Unknown styles use "1001".
 */
LILA_FOUNDATION_API std::string udimLabel(int udim, std::string_view style);
/** The UDIM a udimLabel of that style names, 0 when it isn't one. */
LILA_FOUNDATION_API int udimFromLabel(std::string_view label, std::string_view style);

/** A UDIM set named by one path: what comes before and after the tile label, and the label's style ("1001" or "uv"). */
struct UdimPattern
{
    std::string prefix;
    std::string suffix;
    std::string style = "1001";
};

/**
 * The UDIM set a path names (UTF-8): a <UDIM> token (1001 style) or a <UVTILE> token (u1_v1), either case, or, with
 * tileNumbers, a tile number 1001 to 1999 after a dot or underscore right before the extension (name.1001.png).
 */
LILA_FOUNDATION_API std::optional<UdimPattern> udimPattern(std::string_view path, bool tileNumbers);
/** The path of one tile of the set. */
LILA_FOUNDATION_API std::string udimTilePath(const UdimPattern& pattern, int udim);
/** The tiles of the set that exist on disk, ascending by udim, with their paths. */
LILA_FOUNDATION_API std::vector<std::pair<int, std::string>> findUdimTiles(const UdimPattern& pattern);

/** <image>.json, the metadata sidecar of an image. */
LILA_FOUNDATION_API fs::path sidecarPath(const fs::path& image);
/** Writes meta as the image's sidecar. Throws std::runtime_error. */
LILA_FOUNDATION_API void writeSidecar(const fs::path& image, const nlohmann::json& meta);
/** The image's sidecar, null when there is none or it doesn't parse. */
LILA_FOUNDATION_API nlohmann::json readSidecar(const fs::path& image);
} // namespace lila::image
