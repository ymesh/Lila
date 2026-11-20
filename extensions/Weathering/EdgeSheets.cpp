#include "EdgeSheets.h"

#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace weathering::edges
{
using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace lila;

namespace
{
constexpr int kSheetVersion = 3;
constexpr int kWidth = 1024;
constexpr int kRows = 8;
constexpr int kRowPx = 64;
constexpr int kHeight = kRows * kRowPx;
/** 64 px across a strip is 10 cm, so a half-width strip is 80 cm long. */
constexpr double kPxPerM = 640.0;

enum class Look
{
    Chips,
    Worn,
    Crack,
    Dirt,
    Moss,
};

struct Row
{
    const char* name;
    Look look;
    const char* tag;
    /** Feature size: above 1 is finer. */
    double scale = 1.0;
};

constexpr std::array<Row, kRows> kLayout = {{
    {"ChippedPaint", Look::Chips, "chips"},
    {"SmallChips", Look::Chips, "chips", 1.8},
    {"WornCorner", Look::Worn, "worn"},
    {"Crack", Look::Crack, "crack"},
    {"CornerDirt", Look::Dirt, "dirt"},
    {"Grime", Look::Dirt, "dirt"},
    {"MossSeam", Look::Moss, "moss"},
    {"MossClumps", Look::Moss, "moss", 0.7},
}};

fs::path sheetDir() { return PrefStore::get().userDir() / "weathering-decals"; }

double hashCell(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 0x8DA6B343u ^ uint32_t(y) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return double(h & 0xFFFFFF) / double(0x1000000);
}

double valueNoise(double x, double y, uint32_t seed)
{
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    const double ux = fx * fx * (3.0 - 2.0 * fx), uy = fy * fy * (3.0 - 2.0 * fy);
    const double a = hashCell(ix, iy, seed), b = hashCell(ix + 1, iy, seed), c = hashCell(ix, iy + 1, seed), d = hashCell(ix + 1, iy + 1, seed);
    return glm::mix(glm::mix(a, b, ux), glm::mix(c, d, ux), uy);
}

double fbm(double x, double y, uint32_t seed, int octaves = 4)
{
    double sum = 0.0, amp = 0.5, norm = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += valueNoise(x, y, seed + uint32_t(o) * 101u) * amp;
        norm += amp;
        x *= 2.03;
        y *= 2.03;
        amp *= 0.5;
    }
    return sum / norm;
}

double smoothstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** One texel of a strip: colour, opacity and a height for the normal. */
struct Texel
{
    glm::dvec3 color{0.5};
    double alpha = 0.0;
    double height = 0.0;
};

/**
 * A strip texel at (x, y) cm, x along the strip's length, y across it with the edge at the middle; d is 0 on the
 * edge and 1 at the strip's sides.
 */
Texel paint(Look look, bool toon, double x, double y, double d, uint32_t seed, double scale)
{
    Texel t;
    // Toon shapes are bigger and cut hard; realistic ones are fine and soft.
    const double grain = (toon ? 0.35 : 1.0) * scale;
    switch (look)
    {
    case Look::Chips:
    {
        const double n = fbm(x * 0.32 * grain, y * 0.32 * grain, seed);
        const double edge = 0.42 + 0.35 * d;
        if (toon)
        {
            const double core = n > edge + 0.04 ? 1.0 : 0.0, rim = n > edge ? 1.0 : 0.0;
            t.alpha = rim;
            t.color = core > 0.0 ? glm::dvec3(0.78, 0.74, 0.66) : glm::dvec3(0.22, 0.18, 0.16);
            t.height = -core;
        }
        else
        {
            const double core = smoothstep(edge + 0.02, edge + 0.06, n), rim = smoothstep(edge - 0.02, edge + 0.01, n);
            t.alpha = rim;
            const double speck = fbm(x * 2.0, y * 2.0, seed + 7);
            t.color = glm::mix(glm::dvec3(0.12, 0.11, 0.1), glm::dvec3(0.46, 0.44, 0.41) * (0.85 + 0.3 * speck), core);
            t.height = -core;
        }
        break;
    }
    case Look::Worn:
    {
        const double n = fbm(x * 0.2 * grain, y * 0.6 * grain, seed);
        const double w = std::pow(std::max(0.0, 1.0 - d), 1.6) * (0.45 + 0.75 * n);
        if (toon)
        {
            t.alpha = w > 0.45 ? 1.0 : 0.0;
            t.color = glm::dvec3(0.85, 0.82, 0.74);
        }
        else
        {
            t.alpha = smoothstep(0.15, 0.7, w);
            t.color = glm::dvec3(0.62, 0.6, 0.56);
        }
        t.height = -0.3 * t.alpha;
        break;
    }
    case Look::Crack:
    {
        // A crack wanders along the strip near the edge; toon cracks zigzag.
        double centre;
        if (toon)
        {
            const double period = 9.0, ph = x / period;
            const double tri = std::abs(2.0 * (ph - std::floor(ph + 0.5)));
            centre = (tri - 0.5) * 3.0 * (0.6 + 0.8 * hashCell(int(std::floor(x / period)), 3, seed));
        }
        else
            centre = (fbm(x * 0.08, 1.7, seed) - 0.5) * 6.0 + (fbm(x * 0.6, 4.1, seed + 3) - 0.5) * 1.2;
        const double off = std::abs((y - 5.0) - centre);
        const double width = toon ? 0.45 : 0.18 + 0.25 * fbm(x * 0.4, 9.0, seed + 5);
        const double branch = toon ? 0.0 : smoothstep(0.82, 0.9, 1.0 - std::abs(fbm(x * 0.5, y * 0.5, seed + 9) * 2.0 - 1.0)) * (1.0 - d);
        const double line = toon ? (off < width ? 1.0 : 0.0) : std::max(1.0 - smoothstep(width * 0.5, width, off), branch * 0.8);
        t.alpha = line * (toon ? 1.0 : 1.0 - 0.6 * d);
        t.color = toon ? glm::dvec3(0.1, 0.08, 0.1) : glm::dvec3(0.05, 0.045, 0.04);
        t.height = -line;
        break;
    }
    case Look::Dirt:
    {
        const double n = fbm(x * 0.25 * grain, y * 0.5 * grain, seed);
        const double w = std::pow(std::max(0.0, 1.0 - d), 1.3) * (0.55 + 0.6 * n);
        if (toon)
        {
            t.alpha = w > 0.5 ? 0.9 : w > 0.3 ? 0.55 : 0.0;
            t.color = w > 0.5 ? glm::dvec3(0.32, 0.24, 0.18) : glm::dvec3(0.45, 0.36, 0.27);
        }
        else
        {
            t.alpha = smoothstep(0.1, 0.8, w) * 0.85;
            t.color = glm::mix(glm::dvec3(0.2, 0.16, 0.11), glm::dvec3(0.1, 0.08, 0.06), n);
        }
        break;
    }
    case Look::Moss:
    {
        const double n = fbm(x * 0.4 * grain, y * 0.4 * grain, seed);
        const double edge = 0.35 + 0.45 * d;
        if (toon)
        {
            const double core = n > edge + 0.05 ? 1.0 : 0.0, rim = n > edge ? 1.0 : 0.0;
            t.alpha = rim;
            t.color = core > 0.0 ? glm::dvec3(0.42, 0.62, 0.22) : glm::dvec3(0.2, 0.34, 0.12);
            t.height = rim * 0.6 + core * 0.4;
        }
        else
        {
            const double cover = smoothstep(edge, edge + 0.1, n);
            const double tuft = fbm(x * 3.0, y * 3.0, seed + 11);
            t.alpha = cover;
            t.color = glm::mix(glm::dvec3(0.1, 0.17, 0.05), glm::dvec3(0.27, 0.36, 0.1), tuft);
            t.height = cover * (0.5 + 0.5 * tuft);
        }
        break;
    }
    }
    return t;
}

json writeSheet(const fs::path& dir, const std::string& name, bool toon)
{
    LILA_ZONE();
    const fs::path file = dir / (name + ".lilatrim");
    const fs::path textures = dir / (name + "_Textures");
    std::error_code ec;
    fs::create_directories(textures, ec);

    std::vector<double> height(size_t(kWidth) * kHeight, 0.0);
    image::Image color, opacity, normal;
    for (image::Image* img : {&color, &opacity, &normal})
    {
        img->width = kWidth;
        img->height = kHeight;
        img->rgba.assign(size_t(kWidth) * kHeight * 4, 0.0f);
    }
    const double cmPerPx = 100.0 / kPxPerM;
    json trims = json::array();
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < 2; ++col)
        {
            const Row& r = kLayout[size_t(row)];
            const int x0 = col * (kWidth / 2), x1 = x0 + kWidth / 2, y0 = row * kRowPx, y1 = y0 + kRowPx;
            const uint32_t seed = uint32_t(row * 2 + col + 1) * (toon ? 7919u : 104729u);
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px)
                {
                    const double x = (px - x0 + 0.5) * cmPerPx, y = (py - y0 + 0.5) * cmPerPx;
                    const double across = double(py - y0 + 0.5) / kRowPx;
                    const double d = std::abs(across - 0.5) * 2.0;
                    // Shapes thin out toward the strip's ends as they do toward its sides, so no piece shows a box edge.
                    const double along = double(px - x0 + 0.5) / (x1 - x0);
                    const double ends = smoothstep(0.0, 0.15, along) * smoothstep(0.0, 0.15, 1.0 - along);
                    Texel t = paint(r.look, toon, x, y, std::max(d, 1.0 - ends), seed, r.scale);
                    const double border = smoothstep(0.0, 0.06, along) * smoothstep(0.0, 0.06, 1.0 - along) * smoothstep(0.0, 0.12, 1.0 - d);
                    t.alpha = std::clamp(t.alpha * (toon ? (border > 0.5 ? 1.0 : 0.0) : border), 0.0, 1.0);
                    const size_t i = size_t(py) * kWidth + size_t(px);
                    height[i] = t.height * t.alpha;
                    for (int c = 0; c < 3; ++c)
                        color.rgba[i * 4 + size_t(c)] = float(t.color[c]);
                    color.rgba[i * 4 + 3] = float(t.alpha);
                    for (int c = 0; c < 3; ++c)
                        opacity.rgba[i * 4 + size_t(c)] = float(t.alpha);
                    opacity.rgba[i * 4 + 3] = 1.0f;
                }
            json tags = json::array({"edge", "weathering", r.tag, toon ? "toon" : "realistic"});
            trims.push_back({{"name", std::string(r.name) + std::to_string(col + 1)},
                             {"min", {x0, y0}},
                             {"max", {x1, y1}},
                             {"kind", "rect"},
                             {"tags", tags},
                             {"allowRotate", false},
                             {"profile", {{"style", "flat"}}}});
        }

    // Normals from the height: toon sheets keep only a hint of relief.
    const double strength = toon ? 0.6 : 2.5;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            auto h = [&](int px, int py) { return height[size_t(std::clamp(py, 0, kHeight - 1)) * kWidth + size_t(std::clamp(px, 0, kWidth - 1))]; };
            const double dx = (h(x + 1, y) - h(x - 1, y)) * strength, dy = (h(x, y + 1) - h(x, y - 1)) * strength;
            const glm::dvec3 n = glm::normalize(glm::dvec3(-dx, dy, 1.0));
            const size_t i = size_t(y) * kWidth + size_t(x);
            normal.rgba[i * 4 + 0] = float(n.x * 0.5 + 0.5);
            normal.rgba[i * 4 + 1] = float(n.y * 0.5 + 0.5);
            normal.rgba[i * 4 + 2] = float(n.z * 0.5 + 0.5);
            normal.rgba[i * 4 + 3] = 1.0f;
        }

    json outputs = json::array();
    for (const auto& [map, suffix, img, space] : {std::tuple{"baseColor", "basecolor", &color, "srgb"}, std::tuple{"opacity", "opacity", &opacity, "linear"},
                                                  std::tuple{"normal", "normal", &normal, "linear"}})
    {
        const fs::path out = textures / (name + "_" + suffix + ".png");
        image::write(out, *img, 8);
        outputs.push_back({{"map", map}, {"file", out.generic_string()}, {"udim", 1001}, {"width", kWidth}, {"height", kHeight},
                           {"format", "png"}, {"bits", 8}, {"colorSpace", space}});
    }
    std::ofstream(textures / (name + "_bake.json")) << json{{"outputs", outputs}, {"folder", textures.generic_string()}}.dump(2);
    const json sheet = {{"lilatrim", 1},
                        {"name", name},
                        {"width", kWidth},
                        {"height", kHeight},
                        {"pxPerM", kPxPerM},
                        {"padding", 2},
                        {"mipLevel", 0},
                        {"maps", {"baseColor", "normal", "opacity"}},
                        {"folder", textures.generic_string()},
                        {"decal", true},
                        {"trims", trims},
                        {"scalars", {{"Roughness", toon ? 0.85 : 0.75}}},
                        {"vectors", json::object()}};
    std::ofstream(file) << sheet.dump(2);
    return file.generic_string();
}

json marker() { return {{"version", kSheetVersion}}; }
} // namespace

json writeEdgeSheets(bool force)
{
    const fs::path dir = sheetDir();
    const fs::path realistic = dir / "T_WeatheringEdgesRealistic.lilatrim", toon = dir / "T_WeatheringEdgesToon.lilatrim";
    bool current = false;
    try
    {
        std::ifstream in(dir / "edge-decals.json");
        current = in && json::parse(in) == marker() && fs::exists(realistic) && fs::exists(toon);
    }
    catch (const std::exception&)
    {
    }
    if (current && !force)
        return {{"dir", dir.generic_string()}, {"built", false}, {"realistic", realistic.generic_string()}, {"toon", toon.generic_string()}};
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeSheet(dir, "T_WeatheringEdgesRealistic", false);
    writeSheet(dir, "T_WeatheringEdgesToon", true);
    std::ofstream(dir / "edge-decals.json") << marker().dump();
    log::info("Weathering: edge decal sheets written to {}", dir.generic_string());
    return {{"dir", dir.generic_string()}, {"built", true}, {"realistic", realistic.generic_string()}, {"toon", toon.generic_string()}};
}

fs::path edgeSheet(const std::string& style)
{
    const json r = writeEdgeSheets(false);
    return fs::path(r[style == "toon" ? "toon" : "realistic"].get<std::string>());
}
} // namespace weathering::edges
