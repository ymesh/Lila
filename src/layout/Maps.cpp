#include "layout/Maps.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Triangulate.h"
#include "layout/Lines.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::layout
{
namespace
{

image::Image blank(int res)
{
    image::Image img;
    img.width = img.height = res;
    img.rgba.assign(size_t(res) * res * 4, 0.0f);
    return img;
}

/** Fills empty texels from filled neighbours, ring by ring, so filtering at island edges never reaches the background. */
void pad(image::Image& img, std::vector<char> filled, int rings)
{
    const int w = img.width, h = img.height;
    for (int ring = 0; ring < rings; ++ring)
    {
        std::vector<char> next = filled;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t i = size_t(y) * w + x;
                if (filled[i])
                    continue;
                for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const size_t j = size_t(ny) * w + nx;
                    if (!filled[j])
                        continue;
                    std::copy_n(&img.rgba[j * 4], 4, &img.rgba[i * 4]);
                    next[i] = 1;
                    break;
                }
            }
        filled = std::move(next);
    }
}
} // namespace

glm::vec3 panelColor(uint64_t key)
{
    uint64_t x = key;
    x ^= x >> 31;
    x *= 0x7fb5d329728ea185ULL;
    x ^= x >> 27;
    x *= 0x81dadef4bc2dd44dULL;
    x ^= x >> 33;
    return {float(x & 0xff) / 255.0f, float((x >> 8) & 0xff) / 255.0f, float((x >> 16) & 0xff) / 255.0f};
}

LayoutMaps bakeMaps(const Resolved& r, const MapOptions& o)
{
    LILA_ZONE();
    LayoutMaps out;
    if (!r.mesh())
        throw std::runtime_error("the layout has no mesh");
    const geo::Mesh& m = *r.mesh();
    const std::string uvName = geo::attr::uvName(o.uvSet);
    const auto* uvAttr = m.cornerAttrs().find(uvName);
    if (!uvAttr)
        throw std::runtime_error("the mesh has no UV set " + std::to_string(o.uvSet) + "; unwrap it first");
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvName);
    const int res = std::clamp(o.resolution, 16, 16384);
    out.height = blank(res);
    out.panelId = blank(res);
    out.lines = blank(res);

    // Which triangle covers each texel centre, and where in it.
    const geo::Triangulation tris = geo::triangulate(m);
    std::vector<int32_t> owner(size_t(res) * res, -1);
    std::vector<glm::vec3> bary(size_t(res) * res);
    bool outside = false;
    for (geo::Index t = 0; t < tris.triangleCount(); ++t)
    {
        glm::dvec2 p[3];
        for (int k = 0; k < 3; ++k)
        {
            const glm::vec2 q = uv[tris.corners[size_t(t) * 3 + k]];
            outside |= q.x < -1e-4f || q.y < -1e-4f || q.x > 1.0001f || q.y > 1.0001f;
            p[k] = glm::dvec2(q.x * res, (1.0 - q.y) * res);
        }
        const double area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (std::abs(area) < 1e-12)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x})))), x1 = std::min(res - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
        const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y})))), y1 = std::min(res - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const glm::dvec2 c(x + 0.5, y + 0.5);
                const double w0 = ((p[1].x - c.x) * (p[2].y - c.y) - (p[2].x - c.x) * (p[1].y - c.y)) / area;
                const double w1 = ((p[2].x - c.x) * (p[0].y - c.y) - (p[0].x - c.x) * (p[2].y - c.y)) / area;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6)
                    continue;
                const size_t i = size_t(y) * res + x;
                owner[i] = int32_t(t);
                bary[i] = glm::vec3(float(w0), float(w1), float(w2));
            }
    }
    if (outside)
        out.warnings.push_back("UVs outside 0 to 1 were left out of the maps; one tile is baked");

    double deepest = 0.0;
    for (const ResolvedRun& run : r.runs())
        if (o.heightOutputs & outputBit(run.output))
            deepest = std::max(deepest, std::abs(run.depth));
    const double reach = std::max(r.reach(), 1e-6);
    tbb::parallel_for(0, res, [&](int y) {
        for (int x = 0; x < res; ++x)
        {
            const size_t i = size_t(y) * res + x;
            if (owner[i] < 0)
                continue;
            const geo::Index t = geo::Index(owner[i]);
            glm::dvec3 pos(0.0);
            for (int k = 0; k < 3; ++k)
                pos += m.position(m.cornerPoint(tris.corners[size_t(t) * 3 + k])) * double(bary[i][k]);
            float* hgt = &out.height.rgba[i * 4];
            const double depth = deepest > 0.0 ? grooveDepth(r, pos, o.heightOutputs) / deepest : 0.0;
            hgt[0] = hgt[1] = hgt[2] = float(1.0 - std::clamp(depth, 0.0, 1.0));
            hgt[3] = 1.0f;
            float* id = &out.panelId.rgba[i * 4];
            const LineSample line = r.lineAt(pos, reach);
            const bool onLine = line.run >= 0 && line.distance < r.runs()[size_t(line.run)].width * 0.5;
            if (!onLine)
                if (const PanelSample panel = r.panelAt(pos); panel.panel >= 0)
                {
                    const glm::vec3 c = panelColor(panel.key);
                    id[0] = c.r;
                    id[1] = c.g;
                    id[2] = c.b;
                }
            id[3] = 1.0f;
            float* ln = &out.lines.rgba[i * 4];
            float v = 0.0f;
            if (line.run >= 0)
            {
                const double hw = std::max(1e-9, r.runs()[size_t(line.run)].width * 0.5);
                v = float(std::clamp(1.0 - line.distance / hw, 0.0, 1.0));
            }
            ln[0] = ln[1] = ln[2] = v;
            ln[3] = 1.0f;
        }
    });
    std::vector<char> filled(owner.size());
    for (size_t i = 0; i < owner.size(); ++i)
        filled[i] = owner[i] >= 0;
    for (image::Image* img : {&out.height, &out.panelId, &out.lines})
        pad(*img, filled, std::max(0, o.padding));
    return out;
}
} // namespace lila::layout
