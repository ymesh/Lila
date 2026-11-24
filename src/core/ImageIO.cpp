#include "core/ImageIO.h"

#include "core/Dds.h"
#include "core/Profile.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
// nanosvg's unreachable-code warning comes from the optimiser, after the push below has been popped.
#pragma warning(disable : 4702)
#pragma warning(push, 0)
#pragma warning(disable : 4996)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "curve/third_party/nanosvg/nanosvg.h"
#include "curve/third_party/nanosvg/nanosvgrast.h"
#pragma warning(pop)
#include <miniz.h>
#include <webp/decode.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <glm/gtc/packing.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

namespace lila::image
{
namespace
{
void putBe32(std::vector<unsigned char>& out, uint32_t v)
{
    out.insert(out.end(), {static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8),
                           static_cast<unsigned char>(v)});
}

uint32_t be32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

void chunk(std::vector<unsigned char>& out, const char* type, const unsigned char* data, size_t size)
{
    putBe32(out, uint32_t(size));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size)
        out.insert(out.end(), data, data + size);
    putBe32(out, uint32_t(mz_crc32(MZ_CRC32_INIT, out.data() + start, size + 4)));
}

/** adler32 of a ++ b from adler32(a), adler32(b) and b's length (zlib's adler32_combine). */
uint32_t adlerCombine(uint32_t a1, uint32_t a2, size_t len2)
{
    constexpr uint32_t base = 65521;
    const uint32_t rem = uint32_t(len2 % base);
    uint32_t sum1 = a1 & 0xffff;
    uint32_t sum2 = uint32_t((uint64_t(rem) * sum1) % base);
    sum1 += (a2 & 0xffff) + base - 1;
    sum2 += ((a1 >> 16) & 0xffff) + ((a2 >> 16) & 0xffff) + base - rem;
    if (sum1 >= base)
        sum1 -= base;
    if (sum1 >= base)
        sum1 -= base;
    if (sum2 >= (base << 1))
        sum2 -= (base << 1);
    if (sum2 >= base)
        sum2 -= base;
    return sum1 | (sum2 << 16);
}

/**
 * A zlib stream of data, its chunks deflated in parallel (pigz style): every chunk but the last ends on a sync flush, so
 * the raw deflate pieces simply concatenate, and the chunks' adler32s combine into the stream's.
 */
std::vector<unsigned char> deflateParallel(const unsigned char* data, size_t size, int level)
{
    LILA_ZONE();

    constexpr size_t kChunk = size_t(1) << 20;
    const size_t chunks = std::max<size_t>(1, (size + kChunk - 1) / kChunk);
    std::vector<std::vector<unsigned char>> parts(chunks);
    std::vector<uint32_t> adler(chunks);
    const mz_uint flags = tdefl_create_comp_flags_from_zip_params(std::clamp(level, 1, 9), -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    tbb::parallel_for(size_t(0), chunks, [&](size_t c) {
        const size_t at = c * kChunk, n = std::min(kChunk, size - std::min(size, at));
        auto comp = std::make_unique<tdefl_compressor>();
        std::vector<unsigned char>& out = parts[c];
        out.reserve(n / 2 + 64);
        auto put = [](const void* buf, int len, void* user) -> mz_bool {
            auto* v = static_cast<std::vector<unsigned char>*>(user);
            v->insert(v->end(), static_cast<const unsigned char*>(buf), static_cast<const unsigned char*>(buf) + len);
            return MZ_TRUE;
        };
        tdefl_init(comp.get(), put, &out, int(flags));
        const tdefl_status st = tdefl_compress_buffer(comp.get(), data + at, n, c + 1 == chunks ? TDEFL_FINISH : TDEFL_SYNC_FLUSH);
        if (st != (c + 1 == chunks ? TDEFL_STATUS_DONE : TDEFL_STATUS_OKAY))
            throw std::runtime_error("deflate failed");
        adler[c] = uint32_t(mz_adler32(MZ_ADLER32_INIT, data + at, n));
    });
    size_t total = 6;
    for (const auto& p : parts)
        total += p.size();
    std::vector<unsigned char> out;
    out.reserve(total);
    // CMF 0x78 (deflate, 32K window), FLG 0x01: no dictionary, header checksum valid.
    out.insert(out.end(), {0x78, 0x01});
    uint32_t a = adler[0];
    for (size_t c = 0; c < chunks; ++c)
    {
        out.insert(out.end(), parts[c].begin(), parts[c].end());
        if (c)
            a = adlerCombine(a, adler[c], std::min(kChunk, size - c * kChunk));
    }
    putBe32(out, a);
    return out;
}

void writePngImpl(const fs::path& path, int w, int h, std::span<const float> rgba, int bits, bool dither, int level)
{
    LILA_ZONE();

    const size_t bytes = size_t(bits / 8);
    const size_t stride = 1 + size_t(w) * 4 * bytes;
    std::vector<unsigned char> raw(stride * size_t(h)), filtered(raw.size());
    tbb::parallel_for(0, h, [&](int y) {
        unsigned char* row = raw.data() + stride * size_t(y);
        row[0] = 0;
        for (size_t i = 0; i < size_t(w) * 4; ++i)
        {
            const float v = std::clamp(rgba[size_t(y) * size_t(w) * 4 + i], 0.0f, 1.0f);
            if (bits == 16)
            {
                const auto q = static_cast<uint16_t>(std::lround(v * 65535.0f));
                row[1 + 2 * i] = static_cast<unsigned char>(q >> 8);
                row[2 + 2 * i] = static_cast<unsigned char>(q & 0xff);
            }
            else
                row[1 + i] = quantize8(v, int(i / 4), y, int(i % 4), dither);
        }
    });
    // Filter Up: smooth maps become runs of small deltas, which deflate far better and faster at low levels.
    tbb::parallel_for(0, h, [&](int y) {
        const unsigned char* row = raw.data() + stride * size_t(y);
        unsigned char* out = filtered.data() + stride * size_t(y);
        out[0] = y ? 2 : 0;
        if (!y)
        {
            std::memcpy(out + 1, row + 1, stride - 1);
            return;
        }
        const unsigned char* up = row - stride;
        for (size_t i = 1; i < stride; ++i)
            out[i] = static_cast<unsigned char>(row[i] - up[i]);
    });
    raw = {};
    const std::vector<unsigned char> packed = deflateParallel(filtered.data(), filtered.size(), level);
    const mz_ulong packedSize = mz_ulong(packed.size());

    std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<unsigned char> ihdr;
    putBe32(ihdr, uint32_t(w));
    putBe32(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {static_cast<unsigned char>(bits), 6, 0, 0, 0});
    chunk(out, "IHDR", ihdr.data(), ihdr.size());
    chunk(out, "IDAT", packed.data(), packedSize);
    chunk(out, "IEND", nullptr, 0);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!f)
        throw std::runtime_error("can't write " + path.string());
}

/** The whole file in one read. */
std::vector<unsigned char> readAll(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const std::streamoff size = f.tellg();
    std::vector<unsigned char> d(size > 0 ? size_t(size) : 0);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
    if (!f)
        d.resize(size_t(std::max<std::streamsize>(f.gcount(), 0)));
    return d;
}

/** The PNG's interlace byte, or -1 when the data isn't a PNG. */
int pngInterlace(const std::vector<unsigned char>& d)
{
    static constexpr unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (d.size() < 33 || std::memcmp(d.data(), kSig, 8) != 0 || std::memcmp(&d[12], "IHDR", 4) != 0)
        return -1;
    return d[28];
}

/**
 * A non-interlaced PNG of any colour type and bit depth, inflated row by row so neither the image size nor the file
 * size is bounded by 32-bit counts (stb_image stops near 2 GB of decoded data).
 */
Image readPngStreamed(const std::vector<unsigned char>& d, std::string* err)
{
    LILA_ZONE();
    auto fail = [&](const std::string& why) {
        if (err)
            *err = why;
        return Image{};
    };
    const uint32_t w32 = be32(&d[16]), h32 = be32(&d[20]);
    const int depth = d[24], type = d[25];
    if (w32 == 0 || h32 == 0 || w32 > uint32_t(INT32_MAX) || h32 > uint32_t(INT32_MAX) || d[26] != 0 || d[27] != 0)
        return fail("bad PNG header");
    const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    const bool depthOk = type == 0 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                         : type == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                                     : (depth == 8 || depth == 16);
    if (!channels || !depthOk)
        return fail("bad PNG colour type or bit depth");
    const int w = int(w32), h = int(h32);

    std::array<std::array<float, 4>, 256> palette{};
    for (auto& p : palette)
        p = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<uint32_t, 3> key{};
    bool hasKey = false;
    std::vector<std::pair<const unsigned char*, size_t>> idat;
    for (size_t at = 8; at + 12 <= d.size();)
    {
        const size_t len = be32(&d[at]);
        if (at + 12 + len > d.size())
            break;
        const unsigned char* tag = &d[at + 4];
        const unsigned char* body = &d[at + 8];
        if (std::memcmp(tag, "IDAT", 4) == 0)
            idat.emplace_back(body, len);
        else if (std::memcmp(tag, "PLTE", 4) == 0)
            for (size_t i = 0; i < std::min<size_t>(len / 3, 256); ++i)
                palette[i] = {body[i * 3] / 255.0f, body[i * 3 + 1] / 255.0f, body[i * 3 + 2] / 255.0f, 1.0f};
        else if (std::memcmp(tag, "tRNS", 4) == 0)
        {
            if (type == 3)
                for (size_t i = 0; i < std::min<size_t>(len, 256); ++i)
                    palette[i][3] = body[i] / 255.0f;
            else if ((type == 0 && len >= 2) || (type == 2 && len >= 6))
            {
                hasKey = true;
                for (size_t c = 0; c < size_t(type == 0 ? 1 : 3); ++c)
                    key[c] = uint32_t(body[c * 2]) << 8 | body[c * 2 + 1];
            }
        }
        else if (std::memcmp(tag, "IEND", 4) == 0)
            break;
        at += 12 + len;
    }
    if (idat.empty())
        return fail("PNG has no image data");

    Image img;
    try
    {
        img = Image{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
    }
    catch (const std::bad_alloc&)
    {
        return fail("not enough memory for a " + std::to_string(w) + " x " + std::to_string(h) + " image");
    }

    const size_t rowBytes = (size_t(w) * size_t(channels) * size_t(depth) + 7) / 8;
    const size_t bpp = std::max<size_t>(1, size_t(channels) * size_t(depth) / 8);
    const float maxValue = float((1u << depth) - 1);
    auto sample = [&](const unsigned char* row, size_t i) -> uint32_t {
        if (depth == 8)
            return row[i];
        if (depth == 16)
            return uint32_t(row[i * 2]) << 8 | row[i * 2 + 1];
        const size_t bit = i * size_t(depth);
        return (row[bit / 8] >> (8 - depth - int(bit % 8))) & ((1u << depth) - 1);
    };
    auto convert = [&](const unsigned char* row, float* out) {
        for (size_t x = 0; x < size_t(w); ++x, out += 4)
        {
            const size_t s = x * size_t(channels);
            if (type == 3)
            {
                std::memcpy(out, palette[sample(row, x) & 255].data(), sizeof(float) * 4);
                continue;
            }
            if (type == 0 || type == 4)
            {
                const uint32_t g = sample(row, s);
                out[0] = out[1] = out[2] = float(g) / maxValue;
                out[3] = type == 4 ? float(sample(row, s + 1)) / maxValue : hasKey && g == key[0] ? 0.0f : 1.0f;
                continue;
            }
            const uint32_t r = sample(row, s), g = sample(row, s + 1), b = sample(row, s + 2);
            out[0] = float(r) / maxValue;
            out[1] = float(g) / maxValue;
            out[2] = float(b) / maxValue;
            out[3] = type == 6 ? float(sample(row, s + 3)) / maxValue : hasKey && r == key[0] && g == key[1] && b == key[2] ? 0.0f : 1.0f;
        }
    };

    mz_stream z{};
    if (mz_inflateInit(&z) != MZ_OK)
        return fail("inflate failed");
    std::unique_ptr<mz_stream, int (*)(mz_stream*)> closer(&z, mz_inflateEnd);
    size_t chunk = 0;
    // Rows unfilter in order, then convert to floats in parallel a batch at a time.
    constexpr size_t kBatch = 64;
    std::vector<unsigned char> rows((rowBytes + 1) * kBatch);
    std::vector<unsigned char> prev(rowBytes, 0);
    for (int y0 = 0; y0 < h; y0 += int(kBatch))
    {
        const int count = std::min(int(kBatch), h - y0);
        for (int r = 0; r < count; ++r)
        {
            unsigned char* line = &rows[size_t(r) * (rowBytes + 1)];
            size_t filled = 0;
            while (filled < rowBytes + 1)
            {
                if (z.avail_in == 0 && chunk < idat.size())
                {
                    z.next_in = idat[chunk].first;
                    z.avail_in = unsigned(idat[chunk].second);
                    ++chunk;
                }
                z.next_out = line + filled;
                z.avail_out = unsigned(rowBytes + 1 - filled);
                const int status = mz_inflate(&z, MZ_SYNC_FLUSH);
                const size_t before = filled;
                filled = rowBytes + 1 - z.avail_out;
                if (filled < rowBytes + 1 && (status == MZ_STREAM_END || (filled == before && z.avail_in == 0 && chunk == idat.size())))
                    return fail("truncated PNG data");
                if (status != MZ_OK && status != MZ_STREAM_END && !(status == MZ_BUF_ERROR && z.avail_in == 0))
                    return fail("corrupt PNG data");
            }
            const unsigned char filter = line[0];
            unsigned char* px = line + 1;
            for (size_t i = 0; i < rowBytes; ++i)
            {
                const int a = i >= bpp ? px[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                int add = 0;
                if (filter == 1)
                    add = a;
                else if (filter == 2)
                    add = b;
                else if (filter == 3)
                    add = (a + b) / 2;
                else if (filter == 4)
                {
                    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                }
                else if (filter != 0)
                    return fail("bad PNG filter");
                px[i] = static_cast<unsigned char>(px[i] + add);
            }
            std::memcpy(prev.data(), px, rowBytes);
        }
        tbb::parallel_for(0, count, [&](int r) {
            convert(&rows[size_t(r) * (rowBytes + 1) + 1], img.rgba.data() + size_t(y0 + r) * size_t(w) * 4);
        });
    }
    return img;
}

/** PNG (any colour type, bit depth or interlacing) and JPEG through stb_image, as RGBA floats. */
Image readStbImpl(const fs::path& path, std::string* err)
{
    LILA_ZONE();
    auto fail = [&](const std::string& why) {
        if (err)
            *err = path.string() + ": " + why;
        return Image{};
    };
    const std::vector<unsigned char> data = readAll(path);
    if (data.empty())
        return fail("can't read the file");
    if (pngInterlace(data) == 0)
    {
        std::string why;
        Image img = readPngStreamed(data, &why);
        if (img.width == 0)
            return fail(why);
        return img;
    }
    const int size = int(std::min<size_t>(data.size(), size_t(INT32_MAX)));
    int w = 0, h = 0, channels = 0;
    const bool wide = stbi_is_16_bit_from_memory(data.data(), size);
    void* pixels = wide ? static_cast<void*>(stbi_load_16_from_memory(data.data(), size, &w, &h, &channels, 4))
                        : static_cast<void*>(stbi_load_from_memory(data.data(), size, &w, &h, &channels, 4));
    if (!pixels)
        return fail(stbi_failure_reason() ? stbi_failure_reason() : "unreadable image");
    std::unique_ptr<void, void (*)(void*)> owned(pixels, stbi_image_free);
    Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
    const size_t rowValues = size_t(w) * 4;
    if (wide)
    {
        const auto* src = static_cast<const uint16_t*>(pixels);
        tbb::parallel_for(0, h, [&](int y) {
            const size_t at = size_t(y) * rowValues;
            for (size_t i = 0; i < rowValues; ++i)
                img.rgba[at + i] = float(src[at + i]) * (1.0f / 65535.0f);
        });
    }
    else
    {
        static const std::array<float, 256> lut = [] {
            std::array<float, 256> t{};
            for (int i = 0; i < 256; ++i)
                t[size_t(i)] = float(i) / 255.0f;
            return t;
        }();
        const auto* src = static_cast<const uint8_t*>(pixels);
        tbb::parallel_for(0, h, [&](int y) {
            const size_t at = size_t(y) * rowValues;
            for (size_t i = 0; i < rowValues; ++i)
                img.rgba[at + i] = lut[src[at + i]];
        });
    }
    return img;
}

// EXR -----------------------------------------------------------------------------------------------------------
// Single-part scanline files: half RGBA (channels stored A, B, G, R as the format sorts them), ZIP blocks of 16 lines.

constexpr int kExrZipLines = 16;
constexpr uint32_t kExrHalf = 1;
constexpr uint32_t kExrFloat = 2;

void putLe32(std::vector<unsigned char>& out, uint32_t v)
{
    out.insert(out.end(), {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
                           static_cast<unsigned char>(v >> 24)});
}

uint32_t le32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void exrAttr(std::vector<unsigned char>& out, const char* name, const char* type, const std::vector<unsigned char>& value)
{
    out.insert(out.end(), name, name + std::strlen(name) + 1);
    out.insert(out.end(), type, type + std::strlen(type) + 1);
    putLe32(out, uint32_t(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

/** OpenEXR's ZIP preprocessing: bytes split into even and odd halves, then delta coded. */
std::vector<unsigned char> exrPredict(const std::vector<unsigned char>& raw)
{
    std::vector<unsigned char> t(raw.size());
    const size_t half = (raw.size() + 1) / 2;
    for (size_t i = 0; i < raw.size(); ++i)
        t[i % 2 ? half + i / 2 : i / 2] = raw[i];
    for (size_t i = t.size(); i-- > 1;)
        t[i] = static_cast<unsigned char>(int(t[i]) - int(t[i - 1]) + 128 + 256);
    return t;
}

std::vector<unsigned char> exrUnpredict(std::vector<unsigned char> t)
{
    for (size_t i = 1; i < t.size(); ++i)
        t[i] = static_cast<unsigned char>(int(t[i - 1]) + int(t[i]) - 128);
    std::vector<unsigned char> raw(t.size());
    const size_t half = (t.size() + 1) / 2;
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = t[i % 2 ? half + i / 2 : i / 2];
    return raw;
}

void writeExrChannelsImpl(const fs::path& path, int w, int h, std::vector<ExrChannel> channels, const std::vector<ExrAttribute>& extra, int level)
{
    LILA_ZONE();

    // Readers expect the channel list, and each line's channel data, sorted by name.
    std::ranges::sort(channels, [](const ExrChannel& a, const ExrChannel& b) { return a.name < b.name; });
    std::vector<unsigned char> out = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
    if (std::ranges::any_of(channels, [](const ExrChannel& c) { return c.name.size() > 31; }))
        out[5] |= 0x04; // long names
    std::vector<unsigned char> chlist;
    for (const ExrChannel& c : channels)
    {
        chlist.insert(chlist.end(), c.name.begin(), c.name.end());
        chlist.push_back(0);
        putLe32(chlist, c.full ? kExrFloat : kExrHalf);
        chlist.insert(chlist.end(), {0, 0, 0, 0});
        putLe32(chlist, 1);
        putLe32(chlist, 1);
    }
    chlist.push_back(0);
    exrAttr(out, "channels", "chlist", chlist);
    exrAttr(out, "compression", "compression", {3});
    std::vector<unsigned char> box;
    for (int v : {0, 0, w - 1, h - 1})
        putLe32(box, uint32_t(v));
    exrAttr(out, "dataWindow", "box2i", box);
    exrAttr(out, "displayWindow", "box2i", box);
    exrAttr(out, "lineOrder", "lineOrder", {0});
    std::vector<unsigned char> one;
    putLe32(one, std::bit_cast<uint32_t>(1.0f));
    exrAttr(out, "pixelAspectRatio", "float", one);
    exrAttr(out, "screenWindowCenter", "v2f", {0, 0, 0, 0, 0, 0, 0, 0});
    exrAttr(out, "screenWindowWidth", "float", one);
    for (const ExrAttribute& a : extra)
        exrAttr(out, a.name.c_str(), a.type.c_str(), a.value);
    out.push_back(0);

    size_t lineBytes = 0;
    for (const ExrChannel& c : channels)
        lineBytes += size_t(w) * (c.full ? 4 : 2);
    const int blocks = (h + kExrZipLines - 1) / kExrZipLines;
    const size_t tableAt = out.size();
    out.resize(out.size() + size_t(blocks) * 8);
    struct Block
    {
        std::vector<unsigned char> raw, packed;
        mz_ulong size = 0;
    };
    std::vector<Block> zipped(static_cast<size_t>(blocks));
    tbb::parallel_for(0, blocks, [&](int b) {
        const int y0 = b * kExrZipLines, y1 = std::min(h, y0 + kExrZipLines);
        std::vector<unsigned char> raw;
        raw.reserve(size_t(y1 - y0) * lineBytes);
        for (int y = y0; y < y1; ++y)
            for (const ExrChannel& c : channels)
                for (int x = 0; x < w; ++x)
                {
                    const float f = c.data[size_t(y) * size_t(w) + size_t(x)];
                    if (c.full)
                        putLe32(raw, std::bit_cast<uint32_t>(f));
                    else
                    {
                        const uint16_t v = uint16_t(glm::packHalf1x16(f));
                        raw.insert(raw.end(), {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)});
                    }
                }
        const std::vector<unsigned char> pre = exrPredict(raw);
        mz_ulong size = mz_compressBound(mz_ulong(pre.size()));
        std::vector<unsigned char> packed(size);
        if (mz_compress2(packed.data(), &size, pre.data(), mz_ulong(pre.size()), std::clamp(level, 1, 9)) != MZ_OK)
            throw std::runtime_error("can't compress " + path.string());
        zipped[size_t(b)] = {std::move(raw), std::move(packed), size};
    });
    for (int b = 0; b < blocks; ++b)
    {
        const int y0 = b * kExrZipLines;
        const std::vector<unsigned char>& raw = zipped[size_t(b)].raw;
        const std::vector<unsigned char>& packed = zipped[size_t(b)].packed;
        const mz_ulong size = zipped[size_t(b)].size;
        // A block that does not shrink is stored raw, which readers tell apart by its size.
        const bool stored = size >= raw.size();
        const uint64_t at = out.size();
        for (int i = 0; i < 8; ++i)
            out[tableAt + size_t(b) * 8 + size_t(i)] = static_cast<unsigned char>(at >> (8 * i));
        putLe32(out, uint32_t(y0));
        putLe32(out, uint32_t(stored ? raw.size() : size));
        if (stored)
            out.insert(out.end(), raw.begin(), raw.end());
        else
            out.insert(out.end(), packed.begin(), packed.begin() + std::ptrdiff_t(size));
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!f)
        throw std::runtime_error("can't write " + path.string());
}

void writeExrImpl(const fs::path& path, int w, int h, std::span<const float> rgba, bool full, int level)
{
    std::vector<ExrChannel> channels(4);
    const size_t count = size_t(w) * size_t(h);
    for (int c = 0; c < 4; ++c)
    {
        channels[size_t(c)].name = std::string(1, "RGBA"[c]);
        channels[size_t(c)].full = full;
        channels[size_t(c)].data.resize(count);
        for (size_t i = 0; i < count; ++i)
            channels[size_t(c)].data[i] = rgba[i * 4 + size_t(c)];
    }
    writeExrChannelsImpl(path, w, h, std::move(channels), {exrChromaticitiesRec709()}, level);
}

Image readExrImpl(const fs::path& path, std::string* err)
{
    auto fail = [&](const std::string& why) {
        if (err)
            *err = path.string() + ": " + why;
        return Image{};
    };
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 8 || le32(d.data()) != 20000630u)
        return fail("not an EXR");
    size_t at = 8;
    int w = 0, h = 0, compression = -1;
    std::vector<std::string> channels;
    std::vector<uint32_t> types;
    auto cstr = [&]() {
        std::string s;
        while (at < d.size() && d[at])
            s += char(d[at++]);
        ++at;
        return s;
    };
    for (;;)
    {
        if (at >= d.size())
            return fail("truncated header");
        if (d[at] == 0)
        {
            ++at;
            break;
        }
        const std::string name = cstr(), type = cstr();
        if (at + 4 > d.size())
            return fail("truncated header");
        const uint32_t size = le32(&d[at]);
        at += 4;
        if (at + size > d.size())
            return fail("truncated header");
        const unsigned char* v = &d[at];
        if (name == "channels")
        {
            for (size_t p = 0; p < size && v[p];)
            {
                std::string c;
                while (p < size && v[p])
                    c += char(v[p++]);
                ++p;
                const uint32_t pixelType = le32(v + p);
                if (pixelType != kExrHalf && pixelType != kExrFloat)
                    return fail("only half or float channels");
                channels.push_back(c);
                types.push_back(pixelType);
                p += 16;
            }
        }
        else if (name == "compression")
            compression = v[0];
        else if (name == "dataWindow")
        {
            w = int(le32(v + 8)) - int(le32(v)) + 1;
            h = int(le32(v + 12)) - int(le32(v + 4)) + 1;
        }
        at += size;
    }
    if (w <= 0 || h <= 0 || (compression != 0 && compression != 3))
        return fail("only uncompressed or ZIP scanline images");
    const int lines = compression == 3 ? kExrZipLines : 1;
    const int blocks = (h + lines - 1) / lines;
    Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 0.0f)};
    if (std::ranges::find(channels, std::string("A")) == channels.end())
        for (size_t i = 3; i < img.rgba.size(); i += 4)
            img.rgba[i] = 1.0f;
    for (int b = 0; b < blocks; ++b)
    {
        const size_t entry = at + size_t(b) * 8;
        if (entry + 8 > d.size())
            return fail("truncated offsets");
        uint64_t off = 0;
        for (int i = 0; i < 8; ++i)
            off |= uint64_t(d[entry + size_t(i)]) << (8 * i);
        if (off + 8 > d.size())
            return fail("bad offset");
        const int y0 = int(le32(&d[off]));
        const uint32_t size = le32(&d[off + 4]);
        const int y1 = std::min(h, y0 + lines);
        size_t pixelBytes = 0;
        for (uint32_t t : types)
            pixelBytes += t == kExrFloat ? 4 : 2;
        const size_t rawSize = size_t(y1 - y0) * size_t(w) * pixelBytes;
        if (off + 8 + size > d.size())
            return fail("truncated block");
        std::vector<unsigned char> raw(d.begin() + std::ptrdiff_t(off + 8), d.begin() + std::ptrdiff_t(off + 8 + size));
        if (size < rawSize)
        {
            std::vector<unsigned char> pre(rawSize);
            mz_ulong got = mz_ulong(rawSize);
            if (mz_uncompress(pre.data(), &got, raw.data(), mz_ulong(raw.size())) != MZ_OK || got != rawSize)
                return fail("bad block data");
            raw = exrUnpredict(std::move(pre));
        }
        if (raw.size() < rawSize)
            return fail("short block");
        size_t p = 0;
        for (int y = y0; y < y1; ++y)
            for (size_t ci = 0; ci < channels.size(); ++ci)
            {
                const std::string& c = channels[ci];
                const int slot = c == "R" ? 0 : c == "G" ? 1 : c == "B" ? 2 : c == "A" ? 3 : -1;
                const bool full = types[ci] == kExrFloat;
                for (int x = 0; x < w; ++x, p += full ? 4 : 2)
                    if (slot >= 0)
                        img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(slot)] =
                            full ? std::bit_cast<float>(le32(&raw[p])) : glm::unpackHalf1x16(uint16_t(raw[p] | raw[p + 1] << 8));
            }
    }
    return img;
}


// TIFF ----------------------------------------------------------------------------------------------------------
// Little-endian baseline files: one IFD, chunky RGBA strips of about 1 MB.

constexpr uint16_t kTiffShort = 3;
constexpr uint16_t kTiffLong = 4;

void putLe16(std::vector<unsigned char>& out, uint16_t v) { out.insert(out.end(), {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)}); }

void putBe16(std::vector<unsigned char>& out, uint16_t v) { out.insert(out.end(), {static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)}); }

uint16_t be16(const unsigned char* p) { return uint16_t(p[0] << 8 | p[1]); }

uint16_t quantize16(float v) { return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); }

void writeFile(const fs::path& path, const std::vector<unsigned char>& out)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!f)
        throw std::runtime_error("can't write " + path.string());
}

void writeTiffImpl(const fs::path& path, int w, int h, std::span<const float> rgba, int bits, bool deflate, bool dither, int level)
{
    LILA_ZONE();

    const size_t bytes = size_t(bits / 8);
    const size_t rowBytes = size_t(w) * 4 * bytes;
    const int rowsPerStrip = std::clamp(int((size_t(1) << 20) / rowBytes), 1, h);
    const int strips = (h + rowsPerStrip - 1) / rowsPerStrip;
    const bool predict = deflate && bits != 32;

    std::vector<unsigned char> out = {'I', 'I', 42, 0, 0, 0, 0, 0};
    std::vector<uint32_t> offsets, counts;
    std::vector<unsigned char> raw;
    std::vector<uint32_t> row(size_t(w) * 4);
    for (int s = 0; s < strips; ++s)
    {
        const int y0 = s * rowsPerStrip, y1 = std::min(h, y0 + rowsPerStrip);
        raw.clear();
        raw.reserve(size_t(y1 - y0) * rowBytes);
        for (int y = y0; y < y1; ++y)
        {
            const float* src = rgba.data() + size_t(y) * size_t(w) * 4;
            if (bits == 32)
            {
                for (size_t i = 0; i < row.size(); ++i)
                    putLe32(raw, std::bit_cast<uint32_t>(src[i]));
                continue;
            }
            for (size_t i = 0; i < row.size(); ++i)
                row[i] = bits == 8 ? quantize8(src[i], int(i / 4), y, int(i % 4), dither) : quantize16(src[i]);
            // Horizontal differencing per sample (TIFF predictor 2), modulo the sample size.
            if (predict)
                for (size_t i = row.size(); i-- > 4;)
                    row[i] = (row[i] - row[i - 4]) & (bits == 8 ? 0xffu : 0xffffu);
            for (uint32_t v : row)
                if (bits == 8)
                    raw.push_back(static_cast<unsigned char>(v));
                else
                    putLe16(raw, uint16_t(v));
        }
        offsets.push_back(uint32_t(out.size()));
        if (deflate)
        {
            mz_ulong size = mz_compressBound(mz_ulong(raw.size()));
            std::vector<unsigned char> packed(size);
            if (mz_compress2(packed.data(), &size, raw.data(), mz_ulong(raw.size()), std::clamp(level, 1, 9)) != MZ_OK)
                throw std::runtime_error("can't compress " + path.string());
            out.insert(out.end(), packed.begin(), packed.begin() + std::ptrdiff_t(size));
            counts.push_back(uint32_t(size));
        }
        else
        {
            out.insert(out.end(), raw.begin(), raw.end());
            counts.push_back(uint32_t(raw.size()));
        }
        if (out.size() & 1)
            out.push_back(0);
        if (out.size() > 0xf0000000u)
            throw std::runtime_error("too large for a TIFF: " + path.string());
    }

    struct Entry
    {
        uint16_t tag, type;
        std::vector<uint32_t> values;
    };
    const uint16_t format = bits == 32 ? 3 : 1;
    std::vector<Entry> entries = {
        {256, kTiffLong, {uint32_t(w)}},
        {257, kTiffLong, {uint32_t(h)}},
        {258, kTiffShort, {uint32_t(bits), uint32_t(bits), uint32_t(bits), uint32_t(bits)}},
        {259, kTiffShort, {deflate ? 8u : 1u}},
        {262, kTiffShort, {2}},
        {273, kTiffLong, offsets},
        {277, kTiffShort, {4}},
        {278, kTiffLong, {uint32_t(rowsPerStrip)}},
        {279, kTiffLong, counts},
        {284, kTiffShort, {1}},
        {317, kTiffShort, {predict ? 2u : 1u}},
        {338, kTiffShort, {2}},
        {339, kTiffShort, {format, format, format, format}},
    };
    // Values over 4 bytes go before the IFD, which then starts on a word boundary.
    std::vector<uint32_t> at(entries.size(), 0);
    for (size_t e = 0; e < entries.size(); ++e)
    {
        const Entry& en = entries[e];
        if (en.values.size() * (en.type == kTiffShort ? 2 : 4) <= 4)
            continue;
        at[e] = uint32_t(out.size());
        for (uint32_t v : en.values)
            en.type == kTiffShort ? putLe16(out, uint16_t(v)) : putLe32(out, v);
        if (out.size() & 1)
            out.push_back(0);
    }
    const uint32_t ifd = uint32_t(out.size());
    putLe16(out, uint16_t(entries.size()));
    for (size_t e = 0; e < entries.size(); ++e)
    {
        const Entry& en = entries[e];
        putLe16(out, en.tag);
        putLe16(out, en.type);
        putLe32(out, uint32_t(en.values.size()));
        if (at[e])
            putLe32(out, at[e]);
        else if (en.type == kTiffShort)
        {
            putLe16(out, uint16_t(en.values[0]));
            putLe16(out, en.values.size() > 1 ? uint16_t(en.values[1]) : uint16_t(0));
        }
        else
            putLe32(out, en.values[0]);
    }
    putLe32(out, 0);
    for (int i = 0; i < 4; ++i)
        out[4 + size_t(i)] = static_cast<unsigned char>(ifd >> (8 * i));
    writeFile(path, out);
}

/** TIFF LZW (compression 5): MSB-first codes, 256 clear, 257 end, width grows one code early. False on corrupt data. */
bool decodeTiffLzw(const unsigned char* src, size_t n, std::vector<unsigned char>& out)
{
    std::vector<uint32_t> prefix(4096);
    std::vector<unsigned char> suffix(4096), first(4096), stack;
    stack.reserve(4096);
    for (uint32_t i = 0; i < 256; ++i)
        suffix[i] = first[i] = static_cast<unsigned char>(i);
    size_t o = 0, bit = 0;
    uint32_t next = 258, width = 9;
    int prev = -1;
    auto emit = [&](uint32_t code) {
        stack.clear();
        for (; code >= 258; code = prefix[code])
            stack.push_back(suffix[code]);
        stack.push_back(static_cast<unsigned char>(code));
        for (auto it = stack.rbegin(); it != stack.rend() && o < out.size(); ++it)
            out[o++] = *it;
    };
    while (o < out.size() && bit + width <= n * 8)
    {
        uint32_t code = 0;
        for (uint32_t i = 0; i < width; ++i, ++bit)
            code = code << 1 | ((src[bit >> 3] >> (7 - (bit & 7))) & 1u);
        if (code == 257)
            break;
        if (code == 256)
        {
            next = 258;
            width = 9;
            prev = -1;
            continue;
        }
        if (prev < 0)
        {
            if (code > 255)
                return false;
            emit(code);
        }
        else
        {
            if (code > next)
                return false;
            const unsigned char head = code == next ? first[uint32_t(prev)] : first[code];
            if (next < 4096)
            {
                prefix[next] = uint32_t(prev);
                suffix[next] = head;
                first[next] = first[uint32_t(prev)];
                ++next;
            }
            emit(code);
        }
        prev = int(code);
        if (next + 1 >= (1u << width) && width < 12)
            ++width;
    }
    return o == out.size();
}

/** PackBits (compression 32773). False on corrupt data. */
bool decodePackBits(const unsigned char* src, size_t n, std::vector<unsigned char>& out)
{
    size_t i = 0, o = 0;
    while (o < out.size() && i < n)
    {
        const int c = static_cast<signed char>(src[i++]);
        if (c >= 0)
        {
            const size_t len = size_t(c) + 1;
            if (i + len > n || o + len > out.size())
                return false;
            std::memcpy(&out[o], &src[i], len);
            i += len;
            o += len;
        }
        else if (c != -128)
        {
            const size_t len = size_t(1 - c);
            if (i >= n || o + len > out.size())
                return false;
            std::memset(&out[o], src[i++], len);
            o += len;
        }
    }
    return o == out.size();
}

/** Undoes TIFF predictor 3 on one row: byte differencing, then each sample's bytes regathered from their planes. */
void undoFloatPredictor(unsigned char* row, size_t samples, size_t stride, size_t bytes, std::vector<unsigned char>& tmp)
{
    const size_t n = samples * bytes;
    for (size_t i = stride; i < n; ++i)
        row[i] = static_cast<unsigned char>(row[i] + row[i - stride]);
    tmp.assign(row, row + n);
    // The planes run most significant byte first; the samples come out little-endian.
    for (size_t s = 0; s < samples; ++s)
        for (size_t b = 0; b < bytes; ++b)
            row[s * bytes + b] = tmp[(bytes - 1 - b) * samples + s];
}

/**
 * Baseline TIFF and BigTIFF, strips or tiles, chunky or planar, 1 to 4 samples (grey, grey + alpha, RGB, RGBA; extra
 * samples past four are skipped). 8 and 16-bit unsigned samples read as 0..1; signed integers, 32-bit unsigned and
 * floats read as stored, so heights and other data keep their units.
 */
Image readTiffImpl(const fs::path& path, std::string* err)
{
    auto fail = [&](const std::string& why) {
        if (err)
            *err = path.string() + ": " + why;
        return Image{};
    };
    const std::vector<unsigned char> d = readAll(path);
    if (d.size() < 16 || !((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')))
        return fail("not a TIFF");
    const bool le = d[0] == 'I';
    auto num = [&](size_t at, int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i)
            v |= uint64_t(d[at + size_t(le ? i : n - 1 - i)]) << (8 * i);
        return v;
    };
    const uint64_t version = num(2, 2);
    if (version != 42 && version != 43)
        return fail("not a TIFF");
    const bool big = version == 43;
    const size_t ifd = size_t(big ? num(8, 8) : num(4, 4));
    const size_t countSize = big ? 8 : 2, entrySize = big ? 20 : 12, inlineSize = big ? 8 : 4;
    if (ifd + countSize > d.size())
        return fail("truncated");
    const size_t n = size_t(num(ifd, int(countSize)));
    if (ifd + countSize + n * entrySize > d.size())
        return fail("truncated IFD");
    std::map<uint16_t, std::vector<uint64_t>> tags;
    std::vector<unsigned char> icc;
    for (size_t e = 0; e < n; ++e)
    {
        const size_t p = ifd + countSize + e * entrySize;
        const uint16_t tag = uint16_t(num(p, 2)), type = uint16_t(num(p + 2, 2));
        const uint64_t count = big ? num(p + 4, 8) : num(p + 4, 4);
        const size_t value = p + (big ? 12 : 8);
        if (tag == 34675 && (type == 7 || type == 1))
        {
            const uint64_t base = count <= inlineSize ? value : num(value, int(inlineSize));
            if (base + count <= d.size())
                icc.assign(d.begin() + std::ptrdiff_t(base), d.begin() + std::ptrdiff_t(base + count));
            continue;
        }
        // BYTE, SHORT, LONG and LONG8 carry every tag read here.
        const int size = type == 1 ? 1 : type == kTiffShort ? 2 : type == kTiffLong ? 4 : type == 16 ? 8 : 0;
        if (!size || count > (uint64_t(1) << 28))
            continue;
        const uint64_t base = uint64_t(size) * count <= inlineSize ? value : num(value, int(inlineSize));
        if (base + uint64_t(size) * count > d.size())
            return fail("bad tag offset");
        std::vector<uint64_t> v(static_cast<size_t>(count));
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = num(size_t(base) + i * size_t(size), size);
        tags[tag] = std::move(v);
    }
    auto one = [&](uint16_t tag, uint64_t fallback) {
        auto it = tags.find(tag);
        return it != tags.end() && !it->second.empty() ? it->second[0] : fallback;
    };
    const int w = int(one(256, 0)), h = int(one(257, 0));
    const uint64_t spp = one(277, 1), compression = one(259, 1), predictor = one(317, 1), bits = one(258, 1);
    const uint64_t format = one(339, 1), photometric = one(262, spp >= 3 ? 2 : 1), planar = one(284, 1);
    if (w <= 0 || h <= 0)
        return fail("no image size");
    if (spp < 1 || spp > 16 || photometric > 2 || (photometric == 2 && spp < 3) || (photometric < 2 && spp > 2))
        return fail("only grey, grey + alpha, RGB or RGBA (no palette, CMYK or YCbCr)");
    const bool sampleOk = format == 3 ? (bits == 32 || bits == 64) : (format == 1 || format == 2) && (bits == 8 || bits == 16 || bits == 32);
    if (!sampleOk)
        return fail(std::to_string(bits) + "-bit " + (format == 2 ? "signed" : format == 3 ? "float" : "unsigned") + " samples");
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32773 && compression != 32946)
        return fail("only uncompressed, LZW, PackBits or deflate");
    if (predictor != 1 && predictor != 2 && predictor != 3)
        return fail("predictor " + std::to_string(predictor));

    const bool tiled = tags.contains(322);
    const int cw = tiled ? int(one(322, 0)) : w;
    const int ch = tiled ? int(one(323, 0)) : int(std::clamp<uint64_t>(one(278, uint64_t(h)), 1, uint64_t(h)));
    const std::vector<uint64_t>& offsets = tags[tiled ? uint16_t(324) : uint16_t(273)];
    const std::vector<uint64_t>& counts = tags[tiled ? uint16_t(325) : uint16_t(279)];
    if (cw <= 0 || ch <= 0)
        return fail("bad tile size");
    const size_t across = size_t((w + cw - 1) / cw), down = size_t((h + ch - 1) / ch);
    const size_t chunkSpp = planar == 2 ? 1 : size_t(spp), planes = planar == 2 ? size_t(spp) : 1;
    if (offsets.size() < across * down * planes || counts.size() < offsets.size())
        return fail("missing strips or tiles");

    const size_t bytes = size_t(bits / 8);
    // 8 and 16-bit unsigned are colour; anything wider or signed is data, kept as stored.
    const bool normalised = format == 1 && bits <= 16;
    const float scale = bits == 8 ? 1.0f / 255.0f : 1.0f / 65535.0f;
    auto sampleAt = [&](const unsigned char* q, bool littleEndian) -> float {
        uint64_t v = 0;
        for (size_t i = 0; i < bytes; ++i)
            v |= uint64_t(q[littleEndian ? i : bytes - 1 - i]) << (8 * i);
        if (format == 3)
            return bits == 32 ? std::bit_cast<float>(uint32_t(v)) : float(std::bit_cast<double>(v));
        if (format == 2)
            return bits == 8 ? float(int8_t(v)) : bits == 16 ? float(int16_t(v)) : float(int32_t(v));
        if (normalised)
        {
            const float f = float(v) * scale;
            return photometric == 0 ? 1.0f - f : f;
        }
        return float(uint32_t(v));
    };
    const uint64_t mask = bits >= 32 ? 0xffffffffull : (1ull << bits) - 1;
    auto getInt = [&](const unsigned char* row, size_t i) {
        uint64_t v = 0;
        for (size_t b = 0; b < bytes; ++b)
            v |= uint64_t(row[i * bytes + (le ? b : bytes - 1 - b)]) << (8 * b);
        return v;
    };
    auto putInt = [&](unsigned char* row, size_t i, uint64_t v) {
        for (size_t b = 0; b < bytes; ++b)
            row[i * bytes + (le ? b : bytes - 1 - b)] = static_cast<unsigned char>(v >> (8 * b));
    };

    Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 1.0f)};
    const size_t rowSamples = size_t(cw) * chunkSpp, rowBytes = rowSamples * bytes;
    std::vector<unsigned char> raw, tmp;
    for (size_t plane = 0; plane < planes; ++plane)
        for (size_t ty = 0; ty < down; ++ty)
            for (size_t tx = 0; tx < across; ++tx)
            {
                const size_t c = (plane * down + ty) * across + tx;
                const int y0 = int(ty) * ch, x0 = int(tx) * cw;
                // Strips stop at the image's last row; tiles are always whole.
                const int rows = tiled ? ch : std::min(ch, h - y0);
                if (offsets[c] + counts[c] > d.size())
                    return fail("truncated strip or tile");
                const unsigned char* src = &d[size_t(offsets[c])];
                raw.assign(size_t(rows) * rowBytes, 0);
                bool ok = true;
                if (compression == 1)
                {
                    if (counts[c] < raw.size())
                        return fail("short strip or tile");
                    std::memcpy(raw.data(), src, raw.size());
                }
                else if (compression == 5)
                    ok = decodeTiffLzw(src, size_t(counts[c]), raw);
                else if (compression == 32773)
                    ok = decodePackBits(src, size_t(counts[c]), raw);
                else
                {
                    mz_ulong got = mz_ulong(raw.size());
                    ok = mz_uncompress(raw.data(), &got, src, mz_ulong(counts[c])) == MZ_OK && got == raw.size();
                }
                if (!ok)
                    return fail("bad strip or tile data");
                for (int r = 0; r < rows && y0 + r < h; ++r)
                {
                    unsigned char* row = raw.data() + size_t(r) * rowBytes;
                    bool rowLe = le;
                    if (predictor == 3 && format == 3)
                    {
                        undoFloatPredictor(row, rowSamples, chunkSpp, bytes, tmp);
                        rowLe = true;
                    }
                    else if (predictor == 2 && format != 3)
                        for (size_t i = chunkSpp; i < rowSamples; ++i)
                            putInt(row, i, (getInt(row, i) + getInt(row, i - chunkSpp)) & mask);
                    const int y = y0 + r;
                    for (int x = 0; x < cw && x0 + x < w; ++x)
                        for (size_t s = 0; s < chunkSpp; ++s)
                        {
                            const size_t sample = planar == 2 ? plane : s;
                            if (sample >= 4 || (spp <= 2 && sample >= 2))
                                continue;
                            const float v = sampleAt(row + (size_t(x) * chunkSpp + s) * bytes, rowLe);
                            float* px = &img.rgba[(size_t(y) * size_t(w) + size_t(x0 + x)) * 4];
                            if (spp <= 2 && sample == 0)
                                px[0] = px[1] = px[2] = v;
                            else
                                px[spp <= 2 ? 3 : sample] = v;
                        }
                }
            }
    img.icc = std::move(icc);
    return img;
}

// PSD -----------------------------------------------------------------------------------------------------------
// Version 1 RGB documents, raw (uncompressed) channel data, big-endian.

constexpr int kPsdMaxSize = 30000;
constexpr int16_t kPsdChannels[4] = {-1, 0, 1, 2};

void writePsdImpl(const fs::path& path, int w, int h, std::span<const Layer> layers, int bits, bool dither)
{
    LILA_ZONE();

    const size_t bytes = size_t(bits / 8);
    const size_t plane = size_t(w) * size_t(h) * bytes;
    auto channel = [&](std::vector<unsigned char>& out, std::span<const float> rgba, int c) {
        const size_t start = out.size();
        out.resize(start + plane);
        unsigned char* p = out.data() + start;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const float v = rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)];
                if (bytes == 1)
                    *p++ = quantize8(v, x, y, c, dither);
                else
                {
                    const uint16_t q = quantize16(v);
                    *p++ = static_cast<unsigned char>(q >> 8);
                    *p++ = static_cast<unsigned char>(q);
                }
            }
    };

    // Layer records bottom first, so the first layer given ends up on top.
    std::vector<unsigned char> info;
    putBe16(info, uint16_t(layers.size()));
    for (size_t k = layers.size(); k-- > 0;)
    {
        const Layer& l = layers[k];
        putBe32(info, 0);
        putBe32(info, 0);
        putBe32(info, uint32_t(h));
        putBe32(info, uint32_t(w));
        putBe16(info, 4);
        for (int16_t id : kPsdChannels)
        {
            putBe16(info, uint16_t(id));
            putBe32(info, uint32_t(2 + plane));
        }
        info.insert(info.end(), {'8', 'B', 'I', 'M', 'n', 'o', 'r', 'm', 255, 0, 0, 0});
        std::string name = l.name.substr(0, 255);
        std::vector<unsigned char> pascal = {static_cast<unsigned char>(name.size())};
        pascal.insert(pascal.end(), name.begin(), name.end());
        while (pascal.size() % 4)
            pascal.push_back(0);
        putBe32(info, uint32_t(8 + pascal.size()));
        putBe32(info, 0); // layer mask data
        putBe32(info, 0); // blending ranges
        info.insert(info.end(), pascal.begin(), pascal.end());
    }
    for (size_t k = layers.size(); k-- > 0;)
        for (int16_t id : kPsdChannels)
        {
            putBe16(info, 0);
            channel(info, layers[k].rgba, id < 0 ? 3 : id);
        }

    std::vector<unsigned char> section;
    if (bits == 8)
    {
        if (info.size() & 1)
            info.push_back(0);
        putBe32(section, uint32_t(info.size()));
        section.insert(section.end(), info.begin(), info.end());
        putBe32(section, 0); // global layer mask
    }
    else
    {
        while (info.size() % 4)
            info.push_back(0);
        putBe32(section, 0);
        putBe32(section, 0);
        section.insert(section.end(), {'8', 'B', 'I', 'M', 'L', 'r', '1', '6'});
        putBe32(section, uint32_t(info.size()));
        section.insert(section.end(), info.begin(), info.end());
    }
    if (section.size() + plane * 4 * (layers.size() + 1) > 0xf0000000u)
        throw std::runtime_error("too large for a PSD: " + path.string());

    std::vector<unsigned char> out = {'8', 'B', 'P', 'S', 0, 1, 0, 0, 0, 0, 0, 0};
    putBe16(out, 4);
    putBe32(out, uint32_t(h));
    putBe32(out, uint32_t(w));
    putBe16(out, uint16_t(bits));
    putBe16(out, 3);
    putBe32(out, 0); // colour mode data
    putBe32(out, 0); // image resources
    putBe32(out, uint32_t(section.size()));
    out.insert(out.end(), section.begin(), section.end());
    putBe16(out, 0);
    for (int c = 0; c < 4; ++c)
        channel(out, layers.front().rgba, c);
    writeFile(path, out);
}

struct PsdFile
{
    int width = 0;
    int height = 0;
    int bits = 0;
    std::vector<std::pair<std::string, Image>> layers;
    Image composite;
};

bool parsePsd(const fs::path& path, PsdFile& out, std::string* err, bool wantLayers)
{
    auto fail = [&](const std::string& why) {
        if (err)
            *err = path.string() + ": " + why;
        return false;
    };
    const std::vector<unsigned char> d = readAll(path);
    if (d.size() < 26 || std::memcmp(d.data(), "8BPS", 4) != 0 || be16(&d[4]) != 1)
        return fail("not a PSD");
    const int channels = be16(&d[12]);
    const int h = int(be32(&d[14])), w = int(be32(&d[18])), bits = be16(&d[22]);
    if (be16(&d[24]) != 3 || (bits != 8 && bits != 16) || channels < 3 || w <= 0 || h <= 0)
        return fail("only 8/16 bit RGB");
    out.width = w;
    out.height = h;
    out.bits = bits;
    const size_t bytes = size_t(bits / 8);
    size_t at = 26;
    std::vector<unsigned char> icc;
    for (int skip = 0; skip < 2; ++skip)
    {
        if (at + 4 > d.size())
            return fail("truncated");
        const size_t end = at + 4 + be32(&d[at]);
        // Image resources: 8BIM blocks of id, padded Pascal name, size and padded data; 1039 is the ICC profile.
        for (size_t r = at + 4; skip == 1 && r + 12 <= std::min(end, d.size());)
        {
            if (std::memcmp(&d[r], "8BIM", 4) != 0)
                break;
            const uint16_t id = be16(&d[r + 4]);
            size_t p = r + 6;
            p += (size_t(d[p]) + 2) & ~size_t(1);
            if (p + 4 > d.size())
                break;
            const size_t size = be32(&d[p]);
            p += 4;
            if (p + size > d.size())
                break;
            if (id == 1039)
                icc.assign(d.begin() + std::ptrdiff_t(p), d.begin() + std::ptrdiff_t(p + size));
            r = p + ((size + 1) & ~size_t(1));
        }
        at = end;
    }
    if (at + 4 > d.size())
        return fail("truncated");
    const size_t sectionEnd = at + 4 + be32(&d[at]);
    if (sectionEnd > d.size())
        return fail("truncated layers");

    auto sample = [&](const unsigned char* p) { return bytes == 1 ? float(*p) / 255.0f : float(be16(p)) / 65535.0f; };
    auto parseLayers = [&](size_t p, size_t end) -> bool {
        if (p + 2 > end)
            return false;
        const int count = std::abs(int16_t(be16(&d[p])));
        p += 2;
        struct Rec
        {
            std::string name;
            int top, left, bottom, right;
            std::vector<std::pair<int16_t, uint32_t>> channels;
        };
        std::vector<Rec> recs;
        for (int i = 0; i < count; ++i)
        {
            if (p + 18 > end)
                return false;
            Rec r;
            r.top = int(be32(&d[p]));
            r.left = int(be32(&d[p + 4]));
            r.bottom = int(be32(&d[p + 8]));
            r.right = int(be32(&d[p + 12]));
            const int nc = be16(&d[p + 16]);
            p += 18;
            for (int c = 0; c < nc; ++c, p += 6)
            {
                if (p + 6 > end)
                    return false;
                r.channels.emplace_back(int16_t(be16(&d[p])), be32(&d[p + 2]));
            }
            if (p + 16 > end)
                return false;
            const size_t extra = be32(&d[p + 12]);
            const size_t ex = p + 16;
            p = ex + extra;
            if (p > end)
                return false;
            size_t q = ex;
            q += 4 + be32(&d[q]);
            q += 4 + be32(&d[q]);
            if (q < p)
                r.name.assign(reinterpret_cast<const char*>(&d[q + 1]), std::min<size_t>(d[q], p - q - 1));
            recs.push_back(std::move(r));
        }
        for (const Rec& r : recs)
        {
            const int lw = r.right - r.left, lh = r.bottom - r.top;
            Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 0.0f)};
            bool hasAlpha = false;
            for (const auto& [id, len] : r.channels)
            {
                if (p + len > end || len < 2)
                    return false;
                if (be16(&d[p]) != 0 || size_t(len) < 2 + size_t(lw) * size_t(lh) * bytes)
                    return false;
                const int slot = id == -1 ? 3 : id;
                hasAlpha |= id == -1;
                if (slot >= 0 && slot < 4)
                    for (int y = 0; y < lh; ++y)
                        for (int x = 0; x < lw; ++x)
                        {
                            const int X = x + r.left, Y = y + r.top;
                            if (X >= 0 && X < w && Y >= 0 && Y < h)
                                img.rgba[(size_t(Y) * size_t(w) + size_t(X)) * 4 + size_t(slot)] =
                                    sample(&d[p + 2 + (size_t(y) * size_t(lw) + size_t(x)) * bytes]);
                        }
                p += len;
            }
            if (!hasAlpha)
                for (size_t i = 3; i < img.rgba.size(); i += 4)
                    img.rgba[i] = 1.0f;
            out.layers.emplace_back(r.name, std::move(img));
        }
        return true;
    };

    if (wantLayers)
    {
        size_t p = at + 4;
        const size_t infoLen = be32(&d[p]);
        if (infoLen && !parseLayers(p + 4, p + 4 + infoLen))
            return fail("bad layer data (only uncompressed layers)");
        p += 4 + infoLen;
        if (p + 4 <= sectionEnd)
            p += 4 + be32(&d[p]);
        while (p + 12 <= sectionEnd)
        {
            const size_t len = be32(&d[p + 8]);
            if (std::memcmp(&d[p + 4], "Lr16", 4) == 0 && !parseLayers(p + 12, std::min(sectionEnd, p + 12 + len)))
                return fail("bad 16-bit layer data (only uncompressed layers)");
            p += 12 + len;
        }
    }

    at = sectionEnd;
    const size_t plane = size_t(w) * size_t(h) * bytes;
    if (at + 2 > d.size() || be16(&d[at]) != 0 || at + 2 + plane * size_t(channels) > d.size())
        return fail("only an uncompressed flattened image");
    at += 2;
    out.composite = Image{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 1.0f)};
    for (int c = 0; c < std::min(channels, 4); ++c)
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
            out.composite.rgba[i * 4 + size_t(c)] = sample(&d[at + size_t(c) * plane + i * bytes]);
    out.composite.icc = std::move(icc);
    return true;
}

void checkSize(const fs::path& path, int w, int h, std::span<const float> rgba)
{
    if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4)
        throw std::runtime_error("bad image size for " + path.string());
}

std::string lowerExt(const fs::path& p)
{
    std::string e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}
} // namespace

uint8_t quantize8(float v, int x, int y, int c, bool dither)
{
    const float s = std::clamp(v, 0.0f, 1.0f) * 255.0f;
    const float r = std::round(s);
    if (!dither || std::abs(s - r) < 1e-3f)
        return static_cast<uint8_t>(r);
    // Two uniform values from a per-texel hash; their sum minus one is triangular on (-1, 1).
    auto hash = [](uint32_t h) {
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    };
    const uint32_t seed = hash(uint32_t(x) * 0x9e3779b1u ^ hash(uint32_t(y) * 0x85ebca6bu ^ uint32_t(c)));
    const float a = float(seed & 0xffff) / 65536.0f, b = float(seed >> 16) / 65536.0f;
    return static_cast<uint8_t>(std::clamp(std::round(s + a + b - 1.0f), 0.0f, 255.0f));
}

void writePng(const fs::path& path, int width, int height, std::span<const float> rgba, int bits, bool dither, int level)
{
    checkSize(path, width, height, rgba);
    if (bits != 8 && bits != 16)
        throw std::runtime_error("PNG bit depth must be 8 or 16");
    writePngImpl(path, width, height, rgba, bits, dither, level);
}

ExrAttribute exrString(const std::string& name, const std::string& value)
{
    return {name, "string", std::vector<unsigned char>(value.begin(), value.end())};
}

ExrAttribute exrChromaticitiesRec709()
{
    std::vector<unsigned char> v;
    for (float f : {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f})
        putLe32(v, std::bit_cast<uint32_t>(f));
    return {"chromaticities", "chromaticities", v};
}

void writeExrChannels(const fs::path& path, int width, int height, std::vector<ExrChannel> channels, const std::vector<ExrAttribute>& attributes,
                      int level)
{
    if (width <= 0 || height <= 0)
        throw std::runtime_error("EXR needs a size");
    for (const ExrChannel& c : channels)
        if (c.data.size() != size_t(width) * size_t(height) || c.name.empty())
            throw std::runtime_error("EXR channel " + c.name + " has the wrong size");
    writeExrChannelsImpl(path, width, height, std::move(channels), attributes, level);
}

void writeExr(const fs::path& path, int width, int height, std::span<const float> rgba, int bits, int level)
{
    checkSize(path, width, height, rgba);
    if (bits != 16 && bits != 32)
        throw std::runtime_error("EXR bit depth must be 16 (half) or 32 (float)");
    writeExrImpl(path, width, height, rgba, bits == 32, level);
}

namespace
{
void appendTgaRle(std::vector<unsigned char>& out, const uint32_t* px, int count)
{
    for (int i = 0; i < count;)
    {
        int run = 1;
        while (i + run < count && run < 128 && px[i + run] == px[i])
            ++run;
        if (run > 1)
        {
            out.push_back(static_cast<unsigned char>(0x80 | (run - 1)));
            out.insert(out.end(), reinterpret_cast<const unsigned char*>(&px[i]), reinterpret_cast<const unsigned char*>(&px[i]) + 4);
            i += run;
            continue;
        }
        // A raw packet ends where the next run of two or more begins.
        int raw = 1;
        while (i + raw < count && raw < 128 && !(i + raw + 1 < count && px[i + raw] == px[i + raw + 1]))
            ++raw;
        out.push_back(static_cast<unsigned char>(raw - 1));
        out.insert(out.end(), reinterpret_cast<const unsigned char*>(&px[i]), reinterpret_cast<const unsigned char*>(&px[i + raw]));
        i += raw;
    }
}

} // namespace

void writeTga(const fs::path& path, int width, int height, std::span<const float> rgba, bool dither, bool rle)
{
    LILA_ZONE();

    checkSize(path, width, height, rgba);
    std::vector<uint32_t> bgra(size_t(width) * size_t(height));
    tbb::parallel_for(0, height, [&](int y) {
        for (int x = 0; x < width; ++x)
        {
            const size_t i = size_t(y) * size_t(width) + size_t(x);
            auto q = [&](int c) { return uint32_t(quantize8(rgba[i * 4 + size_t(c)], x, y, c, dither)); };
            bgra[i] = q(2) | q(1) << 8 | q(0) << 16 | q(3) << 24;
        }
    });
    std::vector<unsigned char> out(18);
    out[2] = rle ? 10 : 2;
    out[12] = static_cast<unsigned char>(width & 0xff);
    out[13] = static_cast<unsigned char>(width >> 8);
    out[14] = static_cast<unsigned char>(height & 0xff);
    out[15] = static_cast<unsigned char>(height >> 8);
    out[16] = 32;
    out[17] = 0x28; // 8 alpha bits, top-left origin
    if (!rle)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(bgra.data());
        out.insert(out.end(), bytes, bytes + bgra.size() * 4);
        writeFile(path, out);
        return;
    }
    // Packets stop at each scanline: some readers decode TGA a row at a time.
    std::vector<std::vector<unsigned char>> rows(static_cast<size_t>(height));
    tbb::parallel_for(0, height, [&](int y) { appendTgaRle(rows[size_t(y)], bgra.data() + size_t(y) * size_t(width), width); });
    for (const auto& row : rows)
        out.insert(out.end(), row.begin(), row.end());
    writeFile(path, out);
}

void writeTiff(const fs::path& path, int width, int height, std::span<const float> rgba, int bits, bool deflate, bool dither, int level)
{
    checkSize(path, width, height, rgba);
    if (bits != 8 && bits != 16 && bits != 32)
        throw std::runtime_error("TIFF bit depth must be 8, 16 or 32 (float)");
    writeTiffImpl(path, width, height, rgba, bits, deflate, dither, level);
}

void writePsd(const fs::path& path, int width, int height, std::span<const Layer> layers, int bits, bool dither)
{
    if (layers.empty())
        throw std::runtime_error("a PSD needs at least one layer: " + path.string());
    for (const Layer& l : layers)
        checkSize(path, width, height, l.rgba);
    if (bits != 8 && bits != 16)
        throw std::runtime_error("PSD bit depth must be 8 or 16");
    if (width > kPsdMaxSize || height > kPsdMaxSize)
        throw std::runtime_error("PSD documents are at most 30000 x 30000: " + path.string());
    writePsdImpl(path, width, height, layers, bits, dither);
}

void write(const fs::path& path, const Image& image, const WriteOptions& o)
{
    const std::string ext = lowerExt(path);
    if (ext == ".png")
        writePng(path, image.width, image.height, image.rgba, o.bits >= 16 ? 16 : 8, o.dither, o.level);
    else if (ext == ".exr")
        writeExr(path, image.width, image.height, image.rgba, o.bits >= 32 ? 32 : 16, o.level);
    else if (ext == ".tga")
        writeTga(path, image.width, image.height, image.rgba, o.dither, o.compress);
    else if (ext == ".tif" || ext == ".tiff")
        writeTiff(path, image.width, image.height, image.rgba, o.bits >= 32 ? 32 : o.bits >= 16 ? 16 : 8, o.compress, o.dither, o.level);
    else if (ext == ".psd")
    {
        const Layer layer{path.stem().string(), image.rgba};
        writePsd(path, image.width, image.height, std::span(&layer, 1), o.bits >= 16 ? 16 : 8, o.dither);
    }
    else
        throw std::runtime_error("can't write " + ext + " images: " + path.string());
}

void write(const fs::path& path, const Image& image, int bits) { write(path, image, WriteOptions{.bits = bits}); }

Image readPng(const fs::path& path, std::string* err) { return readStbImpl(path, err); }

Image readExr(const fs::path& path, std::string* err) { return readExrImpl(path, err); }

Image readTga(const fs::path& path, std::string* err)
{
    auto fail = [&](const std::string& why) {
        if (err)
            *err = path.string() + ": " + why;
        return Image{};
    };
    const std::vector<unsigned char> d = readAll(path);
    if (d.size() < 18 || (d[2] != 2 && d[2] != 10) || d[1] != 0)
        return fail("only true colour TGA, uncompressed or RLE");
    const int w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, bpp = d[16] / 8;
    const bool topDown = (d[17] & 0x20) != 0;
    size_t at = 18 + size_t(d[0]);
    const size_t count = size_t(w) * size_t(h);
    if ((bpp != 3 && bpp != 4) || w <= 0 || h <= 0)
        return fail("unsupported TGA");
    std::vector<unsigned char> px;
    const unsigned char* src = nullptr;
    if (d[2] == 2)
    {
        if (d.size() < at + count * size_t(bpp))
            return fail("truncated TGA");
        src = &d[at];
    }
    else
    {
        px.resize(count * size_t(bpp));
        for (size_t n = 0; n < count;)
        {
            if (at >= d.size())
                return fail("truncated TGA");
            const unsigned char head = d[at++];
            const size_t len = std::min(size_t(head & 0x7f) + 1, count - n);
            const size_t need = (head & 0x80) ? size_t(bpp) : len * size_t(bpp);
            if (at + need > d.size())
                return fail("truncated TGA");
            for (size_t k = 0; k < len; ++k)
                std::memcpy(&px[(n + k) * size_t(bpp)], &d[at + ((head & 0x80) ? 0 : k * size_t(bpp))], size_t(bpp));
            at += need;
            n += len;
        }
        src = px.data();
    }
    Image img{w, h, std::vector<float>(count * 4, 1.0f)};
    for (int y = 0; y < h; ++y)
    {
        const int row = topDown ? y : h - 1 - y;
        for (int x = 0; x < w; ++x)
        {
            const unsigned char* p = src + (size_t(row) * size_t(w) + size_t(x)) * size_t(bpp);
            float* o = &img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            o[0] = float(p[2]) / 255.0f;
            o[1] = float(p[1]) / 255.0f;
            o[2] = float(p[0]) / 255.0f;
            if (bpp == 4)
                o[3] = float(p[3]) / 255.0f;
        }
    }
    return img;
}

Image readTiff(const fs::path& path, std::string* err) { return readTiffImpl(path, err); }

Image readPsd(const fs::path& path, std::string* err)
{
    PsdFile f;
    return parsePsd(path, f, err, false) ? std::move(f.composite) : Image{};
}

std::vector<std::pair<std::string, Image>> readPsdLayers(const fs::path& path, std::string* err)
{
    PsdFile f;
    return parsePsd(path, f, err, true) ? std::move(f.layers) : std::vector<std::pair<std::string, Image>>{};
}

Image readWebp(const fs::path& path, std::string* err)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        if (err)
            *err = "cannot open file";
        return {};
    }
    return decodeWebp(bytes.data(), bytes.size(), err);
}

Image decodeWebp(const uint8_t* data, size_t size, std::string* err)
{
    int w = 0, h = 0;
    uint8_t* px = data && size ? WebPDecodeRGBA(data, size, &w, &h) : nullptr;
    if (!px)
    {
        if (err)
            *err = "not a readable WebP image";
        return {};
    }
    Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
    for (size_t i = 0; i < img.rgba.size(); ++i)
        img.rgba[i] = px[i] / 255.0f;
    WebPFree(px);
    return img;
}

Image readSvg(const fs::path& path, int longestSide, std::string* err)
{
    LILA_ZONE();
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto fail = [&](const char* why) {
        if (err)
            *err = why;
        return Image{};
    };
    if (text.empty())
        return fail("can't read the SVG file");
    std::unique_ptr<NSVGimage, void (*)(NSVGimage*)> svg(nsvgParse(text.data(), "px", 96.0f), nsvgDelete);
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f)
        return fail("not a readable SVG, or it has no size");
    const float scale = float(std::clamp(longestSide, 16, 16384)) / std::max(svg->width, svg->height);
    const int w = std::max(1, int(std::lround(svg->width * scale))), h = std::max(1, int(std::lround(svg->height * scale)));
    std::unique_ptr<NSVGrasterizer, void (*)(NSVGrasterizer*)> raster(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    std::vector<unsigned char> px(size_t(w) * size_t(h) * 4);
    nsvgRasterize(raster.get(), svg.get(), 0.0f, 0.0f, scale, px.data(), w, h, w * 4);
    Image img{w, h, std::vector<float>(px.size())};
    // Straight-alpha sRGB bytes, read as an 8-bit PNG would be.
    for (size_t i = 0; i < px.size(); ++i)
        img.rgba[i] = px[i] / 255.0f;
    return img;
}

Image read(const fs::path& path, std::string* err)
{
    const std::string ext = lowerExt(path);
    if (ext == ".svg")
        return readSvg(path, 2048, err);
    if (ext == ".exr")
        return readExr(path, err);
    if (ext == ".tga")
        return readTga(path, err);
    if (ext == ".tif" || ext == ".tiff")
        return readTiff(path, err);
    if (ext == ".psd")
        return readPsd(path, err);
    if (ext == ".webp")
        return readWebp(path, err);
    if (ext == ".dds")
    {
        dds::File f;
        if (!dds::read(path, f, err))
            return {};
        return Image{f.width, f.height, dds::decodeFloat(f)};
    }
    return readPng(path, err);
}

Image downsampled(const Image& src, int width, int height)
{
    LILA_ZONE();
    const int w = std::clamp(width, 1, std::max(src.width, 1)), h = std::clamp(height, 1, std::max(src.height, 1));
    if (w == src.width && h == src.height)
        return src;
    struct Span
    {
        int first = 0;
        std::vector<float> weights;
    };
    // Each output texel covers [i * scale, (i + 1) * scale) of the source; a source texel weighs its overlap with that.
    auto spans = [](int from, int to) {
        std::vector<Span> out(static_cast<size_t>(to));
        const double scale = double(from) / double(to);
        for (int i = 0; i < to; ++i)
        {
            const double a = i * scale, b = (i + 1) * scale;
            Span& s = out[size_t(i)];
            s.first = int(a);
            for (int j = s.first; j < std::min(from, int(std::ceil(b))); ++j)
                s.weights.push_back(float((std::min(b, double(j + 1)) - std::max(a, double(j))) / scale));
        }
        return out;
    };
    const std::vector<Span> xs = spans(src.width, w), ys = spans(src.height, h);
    Image out{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 0.0f), src.icc};
    tbb::parallel_for(0, h, [&](int y) {
        float* row = &out.rgba[size_t(y) * size_t(w) * 4];
        const Span& sy = ys[size_t(y)];
        for (size_t k = 0; k < sy.weights.size(); ++k)
        {
            const float* in = &src.rgba[size_t(sy.first + int(k)) * size_t(src.width) * 4];
            const float wy = sy.weights[k];
            for (int x = 0; x < w; ++x)
            {
                const Span& sx = xs[size_t(x)];
                for (size_t m = 0; m < sx.weights.size(); ++m)
                {
                    const float wt = wy * sx.weights[m];
                    const float* t = in + size_t(sx.first + int(m)) * 4;
                    for (int c = 0; c < 4; ++c)
                        row[size_t(x) * 4 + size_t(c)] += t[c] * wt;
                }
            }
        }
    });
    return out;
}

const std::vector<std::string>& writableFormats()
{
    static const std::vector<std::string> f = {"png", "exr", "tga", "tif", "tiff", "psd"};
    return f;
}

std::string udimLabel(int udim, std::string_view style)
{
    const int u = (udim - 1001) % 10, v = (udim - 1001) / 10;
    if (style == "uv")
        return "u" + std::to_string(u + 1) + "_v" + std::to_string(v + 1);
    if (style == "uv0")
        return "u" + std::to_string(u) + "_v" + std::to_string(v);
    return std::to_string(udim);
}

int udimFromLabel(std::string_view label, std::string_view style)
{
    auto number = [](std::string_view s, int& out) {
        if (s.empty() || s.size() > 6)
            return false;
        out = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int u = 0, v = 0;
    if (style != "uv" && style != "uv0")
        return number(label, u) && u >= 1001 && u <= 1999 ? u : 0;
    const size_t sep = label.find("_v");
    if (label.size() < 4 || label[0] != 'u' || sep == std::string_view::npos || !number(label.substr(1, sep - 1), u) || !number(label.substr(sep + 2), v))
        return 0;
    if (style == "uv")
    {
        --u;
        --v;
    }
    return u >= 0 && u < 10 && v >= 0 && v < 100 ? 1001 + u + 10 * v : 0;
}

std::optional<UdimPattern> udimPattern(std::string_view path, bool tileNumbers)
{
    auto findToken = [&](std::string_view token) -> size_t {
        for (size_t i = 0; i + token.size() <= path.size(); ++i)
            if (std::equal(token.begin(), token.end(), path.begin() + std::ptrdiff_t(i),
                           [](char t, char c) { return std::toupper(static_cast<unsigned char>(c)) == t; }))
                return i;
        return std::string_view::npos;
    };
    for (auto [token, style] : {std::pair<std::string_view, const char*>{"<UDIM>", "1001"}, {"<UVTILE>", "uv"}})
        if (size_t at = findToken(token); at != std::string_view::npos)
            return UdimPattern{std::string(path.substr(0, at)), std::string(path.substr(at + token.size())), style};
    if (!tileNumbers)
        return std::nullopt;
    const size_t slash = path.find_last_of("/\\");
    const size_t nameStart = slash == std::string_view::npos ? 0 : slash + 1;
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || dot < nameStart + 5)
        return std::nullopt;
    const size_t label = dot - 4;
    const char before = path[label - 1];
    if ((before != '.' && before != '_') || label - 1 < nameStart || udimFromLabel(path.substr(label, 4), "1001") == 0)
        return std::nullopt;
    return UdimPattern{std::string(path.substr(0, label)), std::string(path.substr(dot)), "1001"};
}

std::string udimTilePath(const UdimPattern& pattern, int udim)
{
    return pattern.prefix + udimLabel(udim, pattern.style) + pattern.suffix;
}

std::vector<std::pair<int, std::string>> findUdimTiles(const UdimPattern& pattern)
{
    std::vector<std::pair<int, std::string>> tiles;
    const fs::path prefix(reinterpret_cast<const char8_t*>(pattern.prefix.c_str()));
    // A prefix ending in a separator names the directory itself, with an empty file name before the label.
    const bool bare = !pattern.prefix.empty() && (pattern.prefix.back() == '/' || pattern.prefix.back() == '\\');
    const fs::path dir = bare ? prefix : prefix.parent_path();
    const std::u8string head = bare ? std::u8string() : prefix.filename().u8string();
    const std::u8string tail(reinterpret_cast<const char8_t*>(pattern.suffix.c_str()));
    std::error_code ec;
    for (fs::directory_iterator it(dir.empty() ? fs::path(".") : dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::u8string name = it->path().filename().u8string();
        if (name.size() <= head.size() + tail.size() || !name.starts_with(head) || !name.ends_with(tail))
            continue;
        const std::u8string mid = name.substr(head.size(), name.size() - head.size() - tail.size());
        const int udim = udimFromLabel(std::string_view(reinterpret_cast<const char*>(mid.data()), mid.size()), pattern.style);
        if (udim && it->is_regular_file(ec))
            tiles.emplace_back(udim, udimTilePath(pattern, udim));
    }
    std::ranges::sort(tiles);
    return tiles;
}

fs::path sidecarPath(const fs::path& image)
{
    fs::path p = image;
    p += ".json";
    return p;
}

void writeSidecar(const fs::path& image, const nlohmann::json& meta)
{
    std::ofstream f(sidecarPath(image));
    f << meta.dump(2);
    if (!f)
        throw std::runtime_error("can't write " + sidecarPath(image).string());
}

nlohmann::json readSidecar(const fs::path& image)
{
    std::ifstream f(sidecarPath(image));
    if (!f)
        return nullptr;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    return j.is_discarded() ? nlohmann::json() : j;
}
} // namespace lila::image
