#include <doctest/doctest.h>

#include "core/ImageIO.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace lila;

namespace
{
const fs::path kData = fs::path(LILA_TEST_IMAGE_DATA);

/** Red left half, blue right half, within tolerance (JPEG is lossy). */
void checkHalves(const image::Image& img, float tolerance)
{
    REQUIRE(img.width == 16);
    REQUIRE(img.height == 8);
    auto at = [&](int x, int y, int c) { return img.rgba[(size_t(y) * 16 + size_t(x)) * 4 + size_t(c)]; };
    CHECK(std::abs(at(2, 4, 0) - 1.0f) <= tolerance);
    CHECK(std::abs(at(2, 4, 2) - 0.0f) <= tolerance);
    CHECK(std::abs(at(13, 4, 0) - 0.0f) <= tolerance);
    CHECK(std::abs(at(13, 4, 2) - 1.0f) <= tolerance);
    CHECK(at(13, 4, 3) == 1.0f);
}
} // namespace

TEST_CASE("image::read takes JPEG")
{
    std::string err;
    checkHalves(image::read(kData / "halves.jpg", &err), 0.05f);
    CHECK(err.empty());
}

TEST_CASE("image::read takes paletted and interlaced PNG")
{
    std::string err;
    checkHalves(image::read(kData / "halves_palette.png", &err), 0.0f);
    checkHalves(image::read(kData / "halves_interlaced.png", &err), 0.0f);
    CHECK(err.empty());
}

TEST_CASE("image::read keeps 16-bit PNG precision")
{
    const image::Image img = image::read(kData / "gray16.png");
    REQUIRE(img.width == 4);
    CHECK(img.rgba[0] == doctest::Approx(32768.0f / 65535.0f));
}

TEST_CASE("image::read round-trips written PNG")
{
    const fs::path p = fs::temp_directory_path() / "lila_image_read_roundtrip.png";
    image::Image src{3, 1, {0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.25f, 0.2f, 0.4f, 0.6f, 1.0f}};
    image::write(p, src, 16);
    const image::Image back = image::read(p);
    REQUIRE(back.width == 3);
    for (size_t i = 0; i < src.rgba.size(); ++i)
        CHECK(back.rgba[i] == doctest::Approx(src.rgba[i]).epsilon(1e-4));
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("image::read reports a missing file")
{
    std::string err;
    const image::Image img = image::read(kData / "missing.png", &err);
    CHECK(img.width == 0);
    CHECK_FALSE(err.empty());
}

namespace
{
void be32Put(std::vector<unsigned char>& o, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8)
        o.push_back(static_cast<unsigned char>(v >> s));
}

void pngChunk(std::vector<unsigned char>& o, const char* tag, const std::vector<unsigned char>& body)
{
    be32Put(o, uint32_t(body.size()));
    o.insert(o.end(), tag, tag + 4);
    o.insert(o.end(), body.begin(), body.end());
    be32Put(o, 0); // The reader doesn't check CRCs.
}

/** Applies PNG filter `f` to each raw row (stride rowBytes, bpp filter distance) and prefixes the filter byte. */
std::vector<unsigned char> filterRows(const std::vector<unsigned char>& raw, size_t rowBytes, size_t bpp, int f)
{
    std::vector<unsigned char> out;
    const size_t rows = raw.size() / rowBytes;
    for (size_t y = 0; y < rows; ++y)
    {
        out.push_back(static_cast<unsigned char>(f));
        for (size_t i = 0; i < rowBytes; ++i)
        {
            const int x = raw[y * rowBytes + i];
            const int a = i >= bpp ? raw[y * rowBytes + i - bpp] : 0, b = y ? raw[(y - 1) * rowBytes + i] : 0,
                      c = i >= bpp && y ? raw[(y - 1) * rowBytes + i - bpp] : 0;
            int pred = 0;
            if (f == 1)
                pred = a;
            else if (f == 2)
                pred = b;
            else if (f == 3)
                pred = (a + b) / 2;
            else if (f == 4)
            {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            out.push_back(static_cast<unsigned char>(x - pred));
        }
    }
    return out;
}

/** A PNG with `data` in stored (uncompressed) deflate blocks, split over two IDAT chunks. */
fs::path writeTestPng(const std::string& name, int w, int h, int depth, int type, const std::vector<unsigned char>& data,
                      const std::vector<std::pair<const char*, std::vector<unsigned char>>>& extra = {})
{
    std::vector<unsigned char> z = {0x78, 0x01};
    for (size_t at = 0; at < data.size() || at == 0;)
    {
        const size_t n = std::min<size_t>(data.size() - at, 7);
        z.push_back(at + n >= data.size() ? 1 : 0);
        z.insert(z.end(), {static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8), static_cast<unsigned char>(~n),
                           static_cast<unsigned char>(~n >> 8)});
        z.insert(z.end(), data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + n));
        at += n;
        if (n == 0)
            break;
    }
    uint32_t s1 = 1, s2 = 0;
    for (unsigned char c : data)
    {
        s1 = (s1 + c) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    be32Put(z, s2 << 16 | s1);

    std::vector<unsigned char> o = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<unsigned char> ihdr;
    be32Put(ihdr, uint32_t(w));
    be32Put(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {static_cast<unsigned char>(depth), static_cast<unsigned char>(type), 0, 0, 0});
    pngChunk(o, "IHDR", ihdr);
    for (const auto& [tag, body] : extra)
        pngChunk(o, tag, body);
    const size_t half = z.size() / 2;
    pngChunk(o, "IDAT", {z.begin(), z.begin() + std::ptrdiff_t(half)});
    pngChunk(o, "IDAT", {z.begin() + std::ptrdiff_t(half), z.end()});
    pngChunk(o, "IEND", {});
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size()));
    return p;
}
} // namespace

TEST_CASE("image::read undoes every PNG filter")
{
    const int w = 5, h = 4;
    std::vector<unsigned char> raw(size_t(w) * h * 3);
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<unsigned char>((i * 37 + i / 7 * 11) & 255);
    for (int f = 0; f <= 4; ++f)
    {
        CAPTURE(f);
        const fs::path p = writeTestPng("lila_png_filter.png", w, h, 8, 2, filterRows(raw, size_t(w) * 3, 3, f));
        std::string err;
        const image::Image img = image::read(p, &err);
        REQUIRE(img.width == w);
        CHECK(err.empty());
        for (size_t px = 0; px < size_t(w) * h; ++px)
        {
            for (size_t c = 0; c < 3; ++c)
                CHECK(img.rgba[px * 4 + c] == doctest::Approx(raw[px * 3 + c] / 255.0f));
            CHECK(img.rgba[px * 4 + 3] == 1.0f);
        }
        fs::remove(p);
    }
}

TEST_CASE("image::read takes sub-byte gray, 16-bit RGBA and transparency keys")
{
    SUBCASE("2-bit gray")
    {
        // Row of 5 samples 0,1,2,3,1 packs into two bytes.
        const fs::path p = writeTestPng("lila_png_gray2.png", 5, 1, 2, 0, filterRows({0b00011011, 0b01000000}, 2, 1, 1));
        const image::Image img = image::read(p);
        REQUIRE(img.width == 5);
        const float want[] = {0.0f, 1.0f / 3, 2.0f / 3, 1.0f, 1.0f / 3};
        for (size_t x = 0; x < 5; ++x)
            CHECK(img.rgba[x * 4] == doctest::Approx(want[x]));
        fs::remove(p);
    }
    SUBCASE("16-bit RGBA")
    {
        const std::vector<unsigned char> raw = {0x80, 0x00, 0xff, 0xff, 0x00, 0x01, 0x40, 0x00, 0x12, 0x34, 0x00, 0x00, 0xff, 0xff, 0x80, 0x00};
        const fs::path p = writeTestPng("lila_png_rgba16.png", 2, 1, 16, 6, filterRows(raw, 16, 8, 4));
        const image::Image img = image::read(p);
        REQUIRE(img.width == 2);
        CHECK(img.rgba[0] == doctest::Approx(32768.0f / 65535.0f));
        CHECK(img.rgba[2] == doctest::Approx(1.0f / 65535.0f));
        CHECK(img.rgba[4] == doctest::Approx(0x1234 / 65535.0f));
        CHECK(img.rgba[7] == doctest::Approx(32768.0f / 65535.0f));
        fs::remove(p);
    }
    SUBCASE("RGB colour key")
    {
        const fs::path p = writeTestPng("lila_png_key.png", 2, 1, 8, 2, filterRows({10, 20, 30, 40, 50, 60}, 6, 3, 0),
                                        {{"tRNS", {0, 10, 0, 20, 0, 30}}});
        const image::Image img = image::read(p);
        REQUIRE(img.width == 2);
        CHECK(img.rgba[3] == 0.0f);
        CHECK(img.rgba[7] == 1.0f);
        fs::remove(p);
    }
}

TEST_CASE("image::read reports truncated PNG data")
{
    const fs::path p = writeTestPng("lila_png_short.png", 4, 4, 8, 0, filterRows(std::vector<unsigned char>(8, 7), 4, 1, 0));
    std::string err;
    const image::Image img = image::read(p, &err);
    CHECK(img.width == 0);
    CHECK(err.find("truncated") != std::string::npos);
    fs::remove(p);
}


TEST_CASE("image::downsampled averages the texels each output covers")
{
    image::Image src{4, 2, {}};
    for (int i = 0; i < 8; ++i)
        src.rgba.insert(src.rgba.end(), {float(i), 1.0f, 0.5f, 1.0f});
    const image::Image half = image::downsampled(src, 2, 1);
    REQUIRE(half.width == 2);
    REQUIRE(half.height == 1);
    CHECK(half.rgba[0] == doctest::Approx((0 + 1 + 4 + 5) / 4.0f));
    CHECK(half.rgba[4] == doctest::Approx((2 + 3 + 6 + 7) / 4.0f));
    CHECK(half.rgba[1] == doctest::Approx(1.0f));

    // Fractional coverage keeps a flat image flat.
    const image::Image odd = image::downsampled(src, 3, 2);
    REQUIRE(odd.width == 3);
    for (size_t i = 1; i < odd.rgba.size(); i += 4)
        CHECK(odd.rgba[i] == doctest::Approx(1.0f));
    CHECK(odd.rgba[0] == doctest::Approx((0.0f + 1.0f / 3.0f) * 0.75f));

    CHECK(image::downsampled(src, 8, 8).width == 4);
}

namespace
{
/** One uncompressed TIFF image, enough to exercise the reader's sample layouts. */
struct TestTiff
{
    int width = 0, height = 0, spp = 1, bits = 8, format = 1, photometric = 1, planar = 1, predictor = 1;
    bool bigEndian = false;
    int tile = 0; // 0 writes one strip per row
    /** width * height * spp values, row-major, chunky order. */
    std::vector<double> values;
};

void putSample(std::vector<unsigned char>& out, const TestTiff& t, double v)
{
    uint64_t raw = 0;
    if (t.format == 3)
        raw = t.bits == 32 ? std::bit_cast<uint32_t>(float(v)) : std::bit_cast<uint64_t>(v);
    else
        raw = uint64_t(int64_t(std::llround(v)));
    const int bytes = t.bits / 8;
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<unsigned char>(raw >> (8 * (t.bigEndian ? bytes - 1 - i : i))));
}

/** Chunk bytes for one tile or strip row band, with the predictor applied. */
std::vector<unsigned char> chunkBytes(const TestTiff& t, int x0, int y0, int cw, int ch, int plane)
{
    const int chunkSpp = t.planar == 2 ? 1 : t.spp, bytes = t.bits / 8;
    std::vector<unsigned char> out;
    for (int y = y0; y < y0 + ch; ++y)
    {
        std::vector<double> row;
        for (int x = x0; x < x0 + cw; ++x)
            for (int s = 0; s < chunkSpp; ++s)
            {
                const int sample = t.planar == 2 ? plane : s;
                row.push_back(x < t.width && y < t.height ? t.values[(size_t(y) * t.width + x) * t.spp + sample] : 0.0);
            }
        std::vector<unsigned char> b;
        if (t.predictor == 2)
        {
            const uint64_t mask = t.bits >= 32 ? 0xffffffffull : (1ull << t.bits) - 1;
            std::vector<double> diff(row.size());
            for (size_t i = 0; i < row.size(); ++i)
            {
                const uint64_t cur = uint64_t(int64_t(std::llround(row[i]))) & mask;
                const uint64_t prev = i >= size_t(chunkSpp) ? uint64_t(int64_t(std::llround(row[i - chunkSpp]))) & mask : 0;
                diff[i] = double(i >= size_t(chunkSpp) ? (cur - prev) & mask : cur);
            }
            for (double v : diff)
                putSample(b, t, v);
        }
        else if (t.predictor == 3)
        {
            // Bytes regrouped most significant first, then differenced.
            const size_t samples = row.size();
            std::vector<unsigned char> planes(samples * size_t(bytes));
            for (size_t s = 0; s < samples; ++s)
            {
                const uint32_t f = std::bit_cast<uint32_t>(float(row[s]));
                for (int k = 0; k < bytes; ++k)
                    planes[size_t(k) * samples + s] = static_cast<unsigned char>(f >> (8 * (bytes - 1 - k)));
            }
            for (size_t i = planes.size() - 1; i >= size_t(chunkSpp); --i)
                planes[i] = static_cast<unsigned char>(planes[i] - planes[i - chunkSpp]);
            b = planes;
        }
        else
            for (double v : row)
                putSample(b, t, v);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

fs::path writeTestTiff(const std::string& name, const TestTiff& t)
{
    std::vector<unsigned char> out = {static_cast<unsigned char>(t.bigEndian ? 'M' : 'I'), static_cast<unsigned char>(t.bigEndian ? 'M' : 'I')};
    auto put = [&](std::vector<unsigned char>& o, uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            o.push_back(static_cast<unsigned char>(v >> (8 * (t.bigEndian ? n - 1 - i : i))));
    };
    put(out, 42, 2);
    put(out, 0, 4); // IFD offset, patched below
    const int cw = t.tile ? t.tile : t.width, ch = t.tile ? t.tile : 1;
    const int across = (t.width + cw - 1) / cw, down = (t.height + ch - 1) / ch, planes = t.planar == 2 ? t.spp : 1;
    std::vector<uint32_t> offsets, counts;
    for (int p = 0; p < planes; ++p)
        for (int ty = 0; ty < down; ++ty)
            for (int tx = 0; tx < across; ++tx)
            {
                const std::vector<unsigned char> c = chunkBytes(t, tx * cw, ty * ch, cw, t.tile ? ch : 1, p);
                offsets.push_back(uint32_t(out.size()));
                counts.push_back(uint32_t(c.size()));
                out.insert(out.end(), c.begin(), c.end());
            }
    auto array = [&](const std::vector<uint32_t>& v) {
        const uint32_t at = uint32_t(out.size());
        for (uint32_t x : v)
            put(out, x, 4);
        return at;
    };
    const uint32_t offAt = array(offsets), countAt = array(counts);
    std::vector<uint32_t> bitsPer(size_t(t.spp), uint32_t(t.bits)), formats(size_t(t.spp), uint32_t(t.format));
    const uint32_t bitsAt = array(bitsPer), formatAt = array(formats);
    const bool many = offsets.size() > 1;
    // tag, type (3 short, 4 long), count, value or offset
    std::vector<std::array<uint32_t, 4>> e = {{256, 4, 1, uint32_t(t.width)},
                                             {257, 4, 1, uint32_t(t.height)},
                                             {258, 4, uint32_t(t.spp), t.spp == 1 ? uint32_t(t.bits) : bitsAt},
                                             {259, 3, 1, 1},
                                             {262, 3, 1, uint32_t(t.photometric)},
                                             {277, 3, 1, uint32_t(t.spp)},
                                             {284, 3, 1, uint32_t(t.planar)},
                                             {317, 3, 1, uint32_t(t.predictor)},
                                             {339, 4, uint32_t(t.spp), t.spp == 1 ? uint32_t(t.format) : formatAt}};
    if (t.tile)
    {
        e.push_back({322, 4, 1, uint32_t(t.tile)});
        e.push_back({323, 4, 1, uint32_t(t.tile)});
        e.push_back({324, 4, uint32_t(offsets.size()), many ? offAt : offsets[0]});
        e.push_back({325, 4, uint32_t(counts.size()), many ? countAt : counts[0]});
    }
    else
    {
        e.push_back({273, 4, uint32_t(offsets.size()), many ? offAt : offsets[0]});
        e.push_back({278, 4, 1, 1});
        e.push_back({279, 4, uint32_t(counts.size()), many ? countAt : counts[0]});
    }
    std::sort(e.begin(), e.end());
    const uint32_t ifd = uint32_t(out.size());
    put(out, e.size(), 2);
    for (const auto& [tag, type, count, value] : e)
    {
        put(out, tag, 2);
        put(out, type, 2);
        put(out, count, 4);
        if (type == 3 && count == 1)
        {
            put(out, value, 2);
            put(out, 0, 2);
        }
        else
            put(out, value, 4);
    }
    put(out, 0, 4);
    for (int i = 0; i < 4; ++i)
        out[4 + size_t(i)] = static_cast<unsigned char>(ifd >> (8 * (t.bigEndian ? 3 - i : i)));
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return p;
}

float texel(const image::Image& img, int x, int y, int c) { return img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4 + size_t(c)]; }
} // namespace

TEST_CASE("image::read keeps signed 16-bit grey TIFF values as stored")
{
    TestTiff t{.width = 3, .height = 2, .bits = 16, .format = 2};
    t.values = {-11000, 0, 8848, -1, 1, 32767};
    const fs::path p = writeTestTiff("lila_tiff_int16.tif", t);
    std::string err;
    const image::Image img = image::read(p, &err);
    REQUIRE_MESSAGE(img.width == 3, err);
    CHECK(texel(img, 0, 0, 0) == -11000.0f);
    CHECK(texel(img, 0, 0, 2) == -11000.0f);
    CHECK(texel(img, 2, 0, 1) == 8848.0f);
    CHECK(texel(img, 2, 1, 0) == 32767.0f);
    CHECK(texel(img, 0, 0, 3) == 1.0f);
    fs::remove(p);
}

TEST_CASE("image::read takes tiled big-endian float TIFF, edge tiles included")
{
    TestTiff t{.width = 20, .height = 10, .bits = 32, .format = 3, .bigEndian = true, .tile = 16};
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x)
            t.values.push_back(x + y * 100 - 500.25);
    const fs::path p = writeTestTiff("lila_tiff_tiled_float.tif", t);
    std::string err;
    const image::Image img = image::read(p, &err);
    REQUIRE_MESSAGE(img.width == 20, err);
    REQUIRE(img.height == 10);
    CHECK(texel(img, 0, 0, 0) == -500.25f);
    CHECK(texel(img, 19, 9, 0) == float(19 + 900 - 500.25));
    CHECK(texel(img, 17, 3, 1) == float(17 + 300 - 500.25));
    fs::remove(p);
}

TEST_CASE("image::read undoes TIFF predictors on grey + alpha and float samples")
{
    TestTiff ga{.width = 5, .height = 2, .spp = 2, .bits = 8, .predictor = 2};
    for (int i = 0; i < 10; ++i)
        ga.values.insert(ga.values.end(), {double(i * 25), double(255 - i * 20)});
    fs::path p = writeTestTiff("lila_tiff_pred2.tif", ga);
    std::string err;
    image::Image img = image::read(p, &err);
    REQUIRE_MESSAGE(img.width == 5, err);
    CHECK(texel(img, 3, 1, 0) == doctest::Approx(200.0f / 255.0f));
    CHECK(texel(img, 3, 1, 2) == doctest::Approx(200.0f / 255.0f));
    CHECK(texel(img, 3, 1, 3) == doctest::Approx(95.0f / 255.0f));
    fs::remove(p);

    TestTiff fp{.width = 6, .height = 2, .bits = 32, .format = 3, .predictor = 3};
    for (int i = 0; i < 12; ++i)
        fp.values.push_back(i * 1.5 - 4.0);
    p = writeTestTiff("lila_tiff_pred3.tif", fp);
    img = image::read(p, &err);
    REQUIRE_MESSAGE(img.width == 6, err);
    for (int i = 0; i < 12; ++i)
        CHECK(texel(img, i % 6, i / 6, 0) == float(i * 1.5 - 4.0));
    fs::remove(p);
}

TEST_CASE("image::read takes planar 16-bit RGB TIFF as 0..1")
{
    TestTiff t{.width = 2, .height = 2, .spp = 3, .bits = 16, .photometric = 2, .planar = 2};
    t.values = {65535, 0, 0, 0, 65535, 0, 0, 0, 65535, 32768, 32768, 32768};
    const fs::path p = writeTestTiff("lila_tiff_planar.tif", t);
    std::string err;
    const image::Image img = image::read(p, &err);
    REQUIRE_MESSAGE(img.width == 2, err);
    CHECK(texel(img, 0, 0, 0) == 1.0f);
    CHECK(texel(img, 1, 0, 1) == 1.0f);
    CHECK(texel(img, 0, 1, 2) == 1.0f);
    CHECK(texel(img, 1, 1, 0) == doctest::Approx(32768.0f / 65535.0f));
    CHECK(texel(img, 1, 1, 3) == 1.0f);
    fs::remove(p);
}

TEST_CASE("image::read names what a TIFF uses that it can't read")
{
    TestTiff t{.width = 2, .height = 1, .bits = 16, .format = 3};
    t.values = {0, 0};
    const fs::path p = writeTestTiff("lila_tiff_half.tif", t);
    std::string err;
    CHECK(image::read(p, &err).width == 0);
    CHECK(err.find("16-bit float samples") != std::string::npos);
    fs::remove(p);
}
