#include "Patterns.h"

#include <lila/MeshOps.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace plating
{
using namespace lila;
using geo::Index;

namespace
{
constexpr double kAa = 0.03;

double frac(double x) { return x - std::floor(x); }

/** 1 inside (d below the edge), 0 outside, with a soft edge kAa wide. */
double inside(double d, double edge)
{
    const double t = std::clamp((edge + kAa - d) / (2.0 * kAa), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double hexEdgeDistance(double u, double v)
{
    // Two offset lattices of hexagon centres; the nearer centre owns the point.
    const double rx = 1.0, ry = std::sqrt(3.0);
    auto wrap = [](double x, double m) { return x - m * std::floor(x / m); };
    const glm::dvec2 a(wrap(u, rx) - rx * 0.5, wrap(v, ry) - ry * 0.5);
    const glm::dvec2 b(wrap(u - rx * 0.5, rx) - rx * 0.5, wrap(v - ry * 0.5, ry) - ry * 0.5);
    const glm::dvec2 g = glm::dot(a, a) < glm::dot(b, b) ? a : b;
    const glm::dvec2 q = glm::abs(g);
    const double hexDist = std::max(glm::dot(q, glm::normalize(glm::dvec2(1.0, std::sqrt(3.0)))), q.x);
    return 0.5 - hexDist;
}
} // namespace

double patternHeight(PatternKind kind, double u, double v, double width)
{
    width = std::clamp(width, 0.01, 0.9);
    switch (kind)
    {
    case PatternKind::Squares:
    {
        const double fu = frac(u), fv = frac(v);
        return inside(std::min({fu, 1.0 - fu, fv, 1.0 - fv}), width * 0.5);
    }
    case PatternKind::Grille:
    {
        const double fv = frac(v);
        return inside(std::min(fv, 1.0 - fv), width * 0.5);
    }
    case PatternKind::Hex: return inside(hexEdgeDistance(u, v), width * 0.5);
    case PatternKind::DiamondPlate:
    {
        // Each cell holds one long diamond, turned the other way in neighbouring cells, like tread plate.
        const double x = frac(u) - 0.5, y = frac(v) - 0.5;
        const double sign = (int(std::floor(u)) + int(std::floor(v))) % 2 == 0 ? 1.0 : -1.0;
        const double c = std::sqrt(0.5);
        const double a = (x + sign * y) * c, b = (sign * x - y) * c;
        return inside(std::abs(a) / 0.42 + std::abs(b) / std::max(0.05, width), 1.0);
    }
    case PatternKind::Slots:
    {
        const double x = frac(u) - 0.5, y = frac(v) - 0.5;
        const double along = std::max(std::abs(x) - 0.3, 0.0);
        return inside(std::hypot(along, y), width);
    }
    case PatternKind::Dots: return inside(std::hypot(frac(u) - 0.5, frac(v) - 0.5), width * 1.5);
    }
    return 0.0;
}

std::vector<PanelAxes> panelAxes(const layout::Resolved& r)
{
    const std::vector<layout::PanelInfo>& panels = r.panels();
    std::vector<PanelAxes> out(panels.size());
    if (!r.mesh())
        return out;
    const geo::Mesh& m = *r.mesh();
    const geo::Triangulation tris = geo::triangulate(m);
    const std::vector<int> label = r.trianglePanels();
    struct Acc
    {
        double area = 0.0;
        glm::dvec3 sum{0.0}, normal{0.0};
        glm::dmat3 second{0.0};
    };
    std::vector<Acc> acc(panels.size());
    for (size_t t = 0; t < label.size() && t < tris.triangleCount(); ++t)
    {
        if (label[t] < 0)
            continue;
        const glm::dvec3 a = m.position(m.cornerPoint(tris.corners[t * 3])), b = m.position(m.cornerPoint(tris.corners[t * 3 + 1])),
                         c = m.position(m.cornerPoint(tris.corners[t * 3 + 2]));
        const glm::dvec3 n = glm::cross(b - a, c - a);
        const double area = 0.5 * glm::length(n);
        const glm::dvec3 centre = (a + b + c) / 3.0;
        Acc& x = acc[size_t(label[t])];
        x.area += area;
        x.sum += centre * area;
        x.normal += n;
        x.second += glm::outerProduct(centre, centre) * area;
    }
    for (size_t k = 0; k < acc.size(); ++k)
    {
        const Acc& x = acc[k];
        if (x.area <= 0.0)
            continue;
        PanelAxes& p = out[k];
        p.origin = x.sum / x.area;
        const glm::dvec3 n = glm::length(x.normal) > 1e-12 ? glm::normalize(x.normal) : glm::dvec3(0, 1, 0);
        const glm::dmat3 cov = x.second / x.area - glm::outerProduct(p.origin, p.origin);
        // The panel's longest direction in its own plane, by power iteration on the spread with the normal taken out.
        const glm::dmat3 flat = (glm::dmat3(1.0) - glm::outerProduct(n, n)) * cov * (glm::dmat3(1.0) - glm::outerProduct(n, n));
        glm::dvec3 u = glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
        for (int it = 0; it < 32; ++it)
        {
            const glm::dvec3 next = flat * u;
            if (glm::length(next) < 1e-15)
                break;
            u = glm::normalize(next);
        }
        u = glm::normalize(u - n * glm::dot(u, n));
        p.u = u;
        p.v = glm::cross(n, u);
    }
    return out;
}

PatternResult applyPattern(const layout::Resolved& r, const geo::Mesh& mesh, const PatternOptions& o)
{
    LILA_ZONE();
    PatternResult out;
    out.mesh = mesh;
    geo::Mesh& m = out.mesh;
    if (!r.mesh() || r.panels().empty() || o.depth == 0.0 || o.scale <= 0.0)
        return out;
    const std::vector<PanelAxes> axes = o.projection == Projection::Panel ? panelAxes(r) : std::vector<PanelAxes>{};
    std::vector<char> picked(r.panels().size(), o.pickedOnly ? 0 : 1);
    if (o.pickedOnly)
        for (size_t k = 0; k < r.panels().size(); ++k)
        {
            const int ov = r.panels()[k].override;
            if (ov >= 0 && size_t(ov) < r.layout().panels.size())
                picked[k] = r.layout().panels[size_t(ov)].values.value("pattern", false) ? 1 : 0;
        }
    if (o.pickedOnly && std::none_of(picked.begin(), picked.end(), [](char c) { return c != 0; }))
    {
        out.warnings.push_back("No panels picked for the pattern: click panels in the pencil's Patterns mode");
        return out;
    }
    const std::vector<glm::vec3> normals = geo::pointNormals(m);
    const double angle = o.rotation * 3.14159265358979 / 180.0, ca = std::cos(angle), sa = std::sin(angle);
    std::vector<glm::dvec3> moved(m.pointCapacity());
    std::vector<char> hit(m.pointCapacity(), 0);
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index i) {
        if (!m.pointValid(i) || i >= normals.size())
            return;
        const glm::dvec3 p = m.position(i), n(normals[i]);
        const layout::PanelSample panel = r.panelAt(p);
        if (panel.panel < 0 || !picked[size_t(panel.panel)])
            return;
        double fade = 1.0;
        if (o.margin > 0.0)
            if (const layout::LineSample line = r.lineAt(p, o.margin); line.run >= 0)
            {
                const double t = std::clamp(line.distance / o.margin, 0.0, 1.0);
                fade = t * t * (3.0 - 2.0 * t);
            }
        if (fade <= 0.0)
            return;
        double u = 0.0, v = 0.0;
        if (o.projection == Projection::Panel && size_t(panel.panel) < axes.size())
        {
            const PanelAxes& a = axes[size_t(panel.panel)];
            u = glm::dot(p - a.origin, a.u) / o.scale;
            v = glm::dot(p - a.origin, a.v) / o.scale;
        }
        else
        {
            const glm::dvec3 an = glm::abs(n);
            const int axis = an.x > an.y && an.x > an.z ? 0 : an.y > an.z ? 1 : 2;
            u = p[(axis + 1) % 3] / o.scale;
            v = p[(axis + 2) % 3] / o.scale;
        }
        const double ru = u * ca - v * sa, rv = u * sa + v * ca;
        const double h = patternHeight(o.kind, ru, rv, o.width) * fade;
        if (h <= 0.0)
            return;
        moved[i] = p - n * (o.depth * h);
        hit[i] = 1;
    });
    for (Index i = 0; i < m.pointCapacity(); ++i)
        if (hit[i])
        {
            m.setPosition(i, moved[i]);
            ++out.moved;
        }
    double edges = 0.0;
    size_t count = 0;
    for (Index e = 0; e < m.edgeCapacity() && count < 20000; ++e)
        if (m.edgeValid(e))
        {
            edges += m.edgeLength(e);
            ++count;
        }
    if (count && edges / double(count) > o.scale * 0.25)
        out.warnings.push_back(std::format("The mesh is too coarse to show a {:.3g} cm pattern (edges about {:.3g} cm): add sculpt levels or "
                                           "subdivide under the pattern, or raise its scale",
                                           o.scale, edges / double(count)));
    return out;
}
} // namespace plating
