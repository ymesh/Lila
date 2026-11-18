#include "tex/TextImage.h"

#include "core/ImageIO.h"
#include "core/Paths.h"
#include "curve/Spline.h"
#include "curve/Text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace lila::tex
{
namespace
{
struct Edge
{
    glm::dvec2 a, b;
};

double segmentDistance(const glm::dvec2& p, const Edge& e)
{
    const glm::dvec2 d = e.b - e.a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 0.0 ? std::clamp(glm::dot(p - e.a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(p - (e.a + d * t));
}

/** Even-odd: the glyph outlines come merged, so each filled area is crossed once. */
bool inside(const glm::dvec2& p, const std::vector<Edge>& edges)
{
    bool in = false;
    for (const Edge& e : edges)
        if ((e.a.y > p.y) != (e.b.y > p.y) && p.x < e.a.x + (p.y - e.a.y) / (e.b.y - e.a.y) * (e.b.x - e.a.x))
            in = !in;
    return in;
}
} // namespace

TextImage renderText(const TextImageSpec& spec)
{
    TextImage out;
    if (spec.text.empty())
    {
        out.error = "the text is empty";
        return out;
    }
    curve::TextOptions o;
    o.text = spec.text;
    o.font = spec.font;
    o.size = 1.0;
    o.letterSpacing = spec.tracking;
    o.anchor = curve::TextAnchor::Baseline;
    const curve::TextResult t = curve::textCurves(o);
    if (t.curves.empty())
    {
        out.error = t.warnings.empty() ? std::string("no glyphs for that text and font") : t.warnings.front();
        return out;
    }
    std::vector<Edge> edges;
    glm::dvec2 lo(1e30), hi(-1e30);
    for (const curve::Spline& s : t.curves)
        for (const curve::Piece& piece : curve::pieces(s))
        {
            glm::dvec2 prev(piece.start());
            for (int k = 1; k <= 12; ++k)
            {
                const glm::dvec2 q(curve::evaluate(piece, k / 12.0).p);
                edges.push_back({prev, q});
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
                prev = q;
            }
        }
    const double margin = spec.outline + 0.05;
    lo -= margin;
    hi += margin;
    const int h = std::clamp(spec.resolution, 16, 2048);
    const double perPx = (hi.y - lo.y) / h;
    const int w = std::clamp(int(std::ceil((hi.x - lo.x) / perPx)), 1, 8192);
    out.aspect = double(w) / double(h);

    const nlohmann::json key = {spec.text, t.fontPath, spec.tracking, spec.outline, spec.color.r, spec.color.g, spec.color.b,
                                spec.outlineColor.r, spec.outlineColor.g, spec.outlineColor.b, h, 1};
    const std::filesystem::path dir = paths::userConfigDir() / "cache" / "text";
    const std::filesystem::path file = dir / (std::to_string(std::hash<std::string>{}(key.dump())) + ".exr");
    out.file = paths::toUtf8(file);
    std::error_code ec;
    if (std::filesystem::exists(file, ec))
        return out;

    std::vector<float> rgba(size_t(w) * size_t(h) * 4, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            // Rows top first; text y grows up.
            const glm::dvec2 p(lo.x + (x + 0.5) * perPx, hi.y - (y + 0.5) * perPx);
            double d = 1e30;
            for (const Edge& e : edges)
                d = std::min(d, segmentDistance(p, e));
            const double signedPx = (inside(p, edges) ? -d : d) / perPx;
            const float fill = float(std::clamp(0.5 - signedPx, 0.0, 1.0));
            const float ring = spec.outline > 0.0 ? float(std::clamp(0.5 - (signedPx - spec.outline / perPx), 0.0, 1.0)) - fill : 0.0f;
            const float a = std::clamp(fill + ring, 0.0f, 1.0f);
            float* px = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            const glm::vec3 c = a > 0.0f ? (spec.color * fill + spec.outlineColor * ring) / a : spec.color;
            px[0] = c.r, px[1] = c.g, px[2] = c.b, px[3] = a;
        }
    std::filesystem::create_directories(dir, ec);
    try
    {
        image::writeExr(file, w, h, rgba, 16);
    }
    catch (const std::exception& e)
    {
        out.file.clear();
        out.error = e.what();
    }
    return out;
}
} // namespace lila::tex
