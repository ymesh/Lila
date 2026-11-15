#include <doctest/doctest.h>

#include "core/ImageIO.h"

#include <cmath>
#include <filesystem>

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
