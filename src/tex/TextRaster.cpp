#include "tex/TextRaster.h"

#include "core/Profile.h"
#include "curve/Spline.h"
#include "curve/Text.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>

namespace lila::tex
{
namespace
{
struct TextEdge
{
    glm::dvec2 a, b;
};

json textValueOr(const std::map<std::string, json>& values, const char* id, json fallback)
{
    auto it = values.find(id);
    return it != values.end() && !it->second.is_null() ? it->second : fallback;
}

double textNumber(const json& j, double fallback)
{
    if (j.is_number())
        return j.get<double>();
    if (j.is_boolean())
        return j.get<bool>() ? 1.0 : 0.0;
    return fallback;
}

curve::TextAlign textAlignOf(const json& v)
{
    const std::string s = v.is_string() ? v.get<std::string>() : std::string();
    const int i = v.is_number() ? v.get<int>() : -1;
    if (s == "right" || i == 2)
        return curve::TextAlign::Right;
    if (s == "left" || i == 0)
        return curve::TextAlign::Left;
    return curve::TextAlign::Center;
}

/** Adds coverage of the even-odd fill of edges to one row of texels, from sub-rowEdges across it. */
void textRasterRow(const std::vector<const TextEdge*>& edges, int y, int width, int subRows, std::vector<float>& row, std::vector<double>& xs)
{
    std::fill(row.begin(), row.end(), 0.0f);
    const float weight = 1.0f / float(subRows);
    for (int s = 0; s < subRows; ++s)
    {
        const double sy = y + (s + 0.5) / subRows;
        xs.clear();
        for (const TextEdge* e : edges)
            if ((e->a.y > sy) != (e->b.y > sy))
                xs.push_back(e->a.x + (sy - e->a.y) / (e->b.y - e->a.y) * (e->b.x - e->a.x));
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2)
        {
            const double x0 = std::clamp(xs[k], 0.0, double(width)), x1 = std::clamp(xs[k + 1], 0.0, double(width));
            if (x1 <= x0)
                continue;
            const int i0 = int(x0), i1 = std::min(width - 1, int(x1));
            for (int i = i0; i <= i1; ++i)
                row[size_t(i)] += weight * float(std::min(x1, double(i + 1)) - std::max(x0, double(i)));
        }
    }
}

image::Image drawTextSource(const json& spec, std::string* error)
{
    LILA_ZONE();
    const int w = std::clamp(spec.value("w", 1), 1, kMaxTextRaster), h = std::clamp(spec.value("h", 1), 1, kMaxTextRaster);
    image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4, 0.0f)};
    const std::string text = spec.value("text", std::string());
    const double size = spec.value("size", 0.2);
    if (text.empty() || size <= 0.0)
        return img;

    curve::TextOptions o;
    o.text = text;
    o.font = spec.value("font", std::string());
    o.size = 1.0;
    o.letterSpacing = spec.value("tracking", 0.0);
    o.lineSpacing = spec.value("lineSpacing", 1.0);
    o.align = textAlignOf(spec.value("align", json("center")));
    o.anchor = curve::TextAnchor::Middle;
    const curve::TextResult t = curve::textCurves(o);
    if (t.curves.empty() && !t.warnings.empty() && error)
        *error = t.warnings.front();

    // Em units to texels: the em is size of the texture's height, so letters keep their shape on a non-square texture.
    const double angle = glm::radians(spec.value("rotation", 0.0));
    const double c = std::cos(angle), s = std::sin(angle);
    const glm::dvec2 at(spec.value("x", 0.5) * w, (1.0 - spec.value("y", 0.5)) * h);
    const double scale = size * h;
    auto toTexels = [&](const glm::dvec3& p) { return at + glm::dvec2(c * p.x - s * p.y, -(s * p.x + c * p.y)) * scale; };

    std::vector<TextEdge> edges;
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const curve::Spline& sp : t.curves)
        for (const curve::Piece& piece : curve::pieces(sp))
        {
            double hull = 0.0;
            for (size_t i = 1; i < piece.cv.size(); ++i)
                hull += glm::length(toTexels(piece.point(i)) - toTexels(piece.point(i - 1)));
            const int steps = piece.degree() <= 1 ? 1 : std::clamp(int(std::ceil(hull / 2.0)), 2, 64);
            glm::dvec2 prev = toTexels(piece.start());
            for (int k = 1; k <= steps; ++k)
            {
                const glm::dvec2 q = toTexels(curve::evaluate(piece, double(k) / steps).p);
                edges.push_back({prev, q});
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
                prev = q;
            }
        }
    if (edges.empty())
        return img;

    // Tiled, letters crossing an edge come back in at the opposite one.
    std::vector<TextEdge> all;
    const bool tile = spec.value("tile", true);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            if ((dx || dy) && !tile)
                continue;
            const glm::dvec2 shift(dx * w, dy * h);
            if (hi.x + shift.x < 0.0 || lo.x + shift.x > w || hi.y + shift.y < 0.0 || lo.y + shift.y > h)
                continue;
            for (const TextEdge& e : edges)
                all.push_back({e.a + shift, e.b + shift});
        }

    std::vector<std::vector<const TextEdge*>> rowEdges(static_cast<size_t>(h));
    for (const TextEdge& e : all)
    {
        const int y0 = std::max(0, int(std::floor(std::min(e.a.y, e.b.y)))), y1 = std::min(h - 1, int(std::floor(std::max(e.a.y, e.b.y))));
        for (int y = y0; y <= y1; ++y)
            rowEdges[size_t(y)].push_back(&e);
    }
    std::vector<float> cover(size_t(w), 0.0f);
    std::vector<double> xs;
    for (int y = 0; y < h; ++y)
    {
        if (rowEdges[size_t(y)].empty())
            continue;
        textRasterRow(rowEdges[size_t(y)], y, w, 8, cover, xs);
        float* out = &img.rgba[size_t(y) * size_t(w) * 4];
        for (int x = 0; x < w; ++x)
        {
            const float v = std::min(1.0f, cover[size_t(x)]);
            out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = out[x * 4 + 3] = v;
        }
    }
    return img;
}
} // namespace

std::string textSourceName(const std::map<std::string, json>& values, int width, int height)
{
    const json spec = {
        {"text", textValueOr(values, "text", "")},
        {"font", textValueOr(values, "font", "")},
        {"size", textNumber(textValueOr(values, "size", 0.2), 0.2)},
        {"x", textNumber(textValueOr(values, "positionX", 0.5), 0.5)},
        {"y", textNumber(textValueOr(values, "positionY", 0.5), 0.5)},
        {"rotation", textNumber(textValueOr(values, "rotation", 0.0), 0.0)},
        {"align", textValueOr(values, "alignment", "center")},
        {"lineSpacing", textNumber(textValueOr(values, "lineSpacing", 1.0), 1.0)},
        {"tracking", textNumber(textValueOr(values, "tracking", 0.0), 0.0)},
        {"tile", textNumber(textValueOr(values, "tile", true), 1.0) != 0.0},
        {"w", std::clamp(width, 1, kMaxTextRaster)},
        {"h", std::clamp(height, 1, kMaxTextRaster)},
    };
    return std::string(kTextSourcePrefix) + spec.dump();
}

std::shared_ptr<const image::Image> textSourceImage(std::string_view name, std::string* error)
{
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const image::Image>>> recent;
    {
        std::scoped_lock lock(mutex);
        for (auto it = recent.begin(); it != recent.end(); ++it)
            if (it->first == name)
            {
                recent.splice(recent.begin(), recent, it);
                return recent.front().second;
            }
    }
    if (!name.starts_with(kTextSourcePrefix))
    {
        if (error)
            *error = "not a text source";
        return std::make_shared<const image::Image>();
    }
    json spec;
    try
    {
        spec = json::parse(name.substr(kTextSourcePrefix.size()));
        if (!spec.is_object())
            throw std::invalid_argument("not an object");
    }
    catch (const std::exception&)
    {
        if (error)
            *error = "unreadable text source";
        return std::make_shared<const image::Image>();
    }
    std::string why;
    std::shared_ptr<const image::Image> img;
    try
    {
        img = std::make_shared<const image::Image>(drawTextSource(spec, &why));
    }
    catch (const std::exception& e)
    {
        if (error)
            *error = e.what();
        return std::make_shared<const image::Image>();
    }
    if (error && !why.empty())
        *error = why;
    std::scoped_lock lock(mutex);
    recent.emplace_front(std::string(name), img);
    while (recent.size() > 6)
        recent.pop_back();
    return img;
}
} // namespace lila::tex
