#include <doctest/doctest.h>

#include "core/ImageIO.h"

#include <algorithm>
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
