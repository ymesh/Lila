#include "WireLayout.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace weathering::wires
{
using gen::unit;

namespace
{
/** The main horizontal direction through points (their centroid and principal axis in XZ). */
std::pair<glm::dvec3, glm::dvec3> mainLine(std::span<const glm::dvec3> pts)
{
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : pts)
        c += p;
    if (pts.empty())
        return {c, {1, 0, 0}};
    c /= double(pts.size());
    double xx = 0, xz = 0, zz = 0;
    for (const glm::dvec3& p : pts)
    {
        const glm::dvec3 d = p - c;
        xx += d.x * d.x;
        xz += d.x * d.z;
        zz += d.z * d.z;
    }
    if (xx + zz < 1e-9)
        return {c, {1, 0, 0}};
    const double angle = 0.5 * std::atan2(2.0 * xz, xx - zz);
    return {c, {std::cos(angle), 0.0, std::sin(angle)}};
}

bool isPole(const glm::dvec3& s) { return s.y > 2.5 * std::max(s.x, s.z) && std::max(s.x, s.z) < 100.0; }
bool isBracket(const glm::dvec3& s) { return std::max({s.x, s.y, s.z}) <= 60.0; }
} // namespace

std::vector<Anchor> anchorsOn(std::span<const Bounds> hosts, const Options& o)
{
    std::vector<glm::dvec3> centers;
    for (const Bounds& b : hosts)
        centers.push_back(b.center());
    const auto [lc, ld] = mainLine(centers);
    const glm::dvec3 side(-ld.z, 0.0, ld.x);
    std::vector<Anchor> out;
    for (size_t k = 0; k < hosts.size(); ++k)
    {
        const Bounds& b = hosts[k];
        const glm::dvec3 s = b.size(), c = b.center();
        Anchor a;
        a.host = int(k);
        if (isPole(s))
        {
            a.kind = Kind::Pole;
            a.at = glm::dvec3(c.x, b.max.y + o.insulator, c.z);
            out.push_back(a);
        }
        else if (isBracket(s))
        {
            a.kind = Kind::Bracket;
            a.at = c;
            glm::dvec3 away(0.0);
            double best = 1e300;
            for (size_t j = 0; j < hosts.size(); ++j)
                if (j != k && !isBracket(hosts[j].size()) && glm::length(hosts[j].center() - c) < best)
                {
                    best = glm::length(hosts[j].center() - c);
                    away = c - hosts[j].center();
                }
            away.y = 0.0;
            a.out = unit(away, glm::dvec3(1, 0, 0));
            out.push_back(a);
        }
        else
        {
            struct Corner
            {
                glm::dvec3 p;
                double score;
            };
            std::vector<Corner> corners;
            for (int q = 0; q < 4; ++q)
            {
                const glm::dvec3 p((q & 1) ? b.max.x : b.min.x, b.min.y + s.y * std::clamp(o.cornerHeight, 0.0, 1.0), (q & 2) ? b.max.z : b.min.z);
                const double perp = glm::dot(p - lc, side);
                corners.push_back({p, std::abs(perp) + (perp < 0.0 ? 1e-3 : 0.0)});
            }
            std::ranges::sort(corners, [](const Corner& x, const Corner& y) { return x.score < y.score; });
            for (int q = 0; q < 2; ++q)
            {
                Anchor ca = a;
                ca.kind = Kind::Corner;
                ca.out = unit(glm::dvec3(corners[size_t(q)].p.x - c.x, 0.0, corners[size_t(q)].p.z - c.z), glm::dvec3(1, 0, 0));
                ca.at = corners[size_t(q)].p + ca.out * o.standoff;
                out.push_back(ca);
            }
        }
    }
    return out;
}

std::vector<Anchor> anchorsAlong(std::span<const glm::dvec3> line, double spacing, double height)
{
    std::vector<Anchor> out;
    if (line.size() < 2)
        return out;
    double total = 0.0;
    for (size_t k = 1; k < line.size(); ++k)
        total += glm::length(line[k] - line[k - 1]);
    const int n = std::max(1, int(std::round(total / std::max(100.0, spacing))));
    size_t seg = 0;
    double segStart = 0.0;
    for (int i = 0; i <= n; ++i)
    {
        const double s = total * i / n;
        while (seg + 2 < line.size() && segStart + glm::length(line[seg + 1] - line[seg]) < s)
        {
            segStart += glm::length(line[seg + 1] - line[seg]);
            ++seg;
        }
        const double len = glm::length(line[seg + 1] - line[seg]);
        const glm::dvec3 p = glm::mix(line[seg], line[seg + 1], len > 1e-9 ? std::clamp((s - segStart) / len, 0.0, 1.0) : 0.0);
        Anchor a;
        a.kind = Kind::Pole;
        a.at = p + glm::dvec3(0, height, 0);
        a.poleBase = p.y;
        out.push_back(a);
    }
    return out;
}

std::vector<std::vector<int>> chain(std::span<const Anchor> anchors, double maxSpan, bool keepOrder)
{
    std::vector<int> order(anchors.size());
    std::iota(order.begin(), order.end(), 0);
    if (!keepOrder)
    {
        std::vector<glm::dvec3> pts;
        for (const Anchor& a : anchors)
            pts.push_back(a.at);
        const auto [c, d] = mainLine(pts);
        std::ranges::stable_sort(order, [&](int x, int y) { return glm::dot(anchors[size_t(x)].at - c, d) < glm::dot(anchors[size_t(y)].at - c, d); });
    }
    std::vector<std::vector<int>> out;
    std::vector<int> run;
    for (int i : order)
    {
        if (!run.empty() && glm::length(anchors[size_t(i)].at - anchors[size_t(run.back())].at) > maxSpan)
        {
            if (run.size() >= 2)
                out.push_back(run);
            run.clear();
        }
        run.push_back(i);
    }
    if (run.size() >= 2)
        out.push_back(run);
    return out;
}
} // namespace weathering::wires
