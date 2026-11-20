#include "Truss.h"

#include <lila/Boolean.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weathering::truss
{
using gen::unit;

const char* bracingName(Bracing b) { return b == Bracing::X ? "x" : (b == Bracing::Zigzag ? "zigzag" : "none"); }

bool parseBracing(const std::string& s, Bracing* out)
{
    for (Bracing b : {Bracing::X, Bracing::Zigzag, Bracing::None})
        if (s == bracingName(b))
        {
            *out = b;
            return true;
        }
    return false;
}

namespace
{
struct Level
{
    glm::dvec3 p, t;
    double along = 0.0;
};

std::vector<Level> levelsOf(const std::vector<glm::dvec3>& line, double segment)
{
    std::vector<double> acc{0.0};
    for (size_t k = 1; k < line.size(); ++k)
        acc.push_back(acc.back() + glm::length(line[k] - line[k - 1]));
    const double total = acc.back();
    std::vector<Level> out;
    if (total < 1e-6)
        return out;
    const int n = std::max(1, int(std::ceil(total / std::max(1.0, segment))));
    size_t seg = 0;
    for (int i = 0; i <= n; ++i)
    {
        const double s = total * i / n;
        while (seg + 2 < line.size() && acc[seg + 1] < s)
            ++seg;
        const double len = acc[seg + 1] - acc[seg];
        const double t = len > 1e-12 ? std::clamp((s - acc[seg]) / len, 0.0, 1.0) : 0.0;
        Level l;
        l.p = glm::mix(line[seg], line[seg + 1], t);
        l.t = unit(line[seg + 1] - line[seg]);
        l.along = s / total;
        out.push_back(l);
    }
    return out;
}
} // namespace

Result build(std::span<const std::vector<glm::dvec3>> lines, const Options& o)
{
    LILA_ZONE();

    Result r;
    const int chords = std::max(3, o.chords);
    const double m = std::max(0.05, o.member);
    std::vector<Mesh> members;
    auto beam = [&](const glm::dvec3& a, const glm::dvec3& b) {
        if (glm::length(b - a) < 1e-6)
            return;
        Mesh mm;
        gen::addBeam(mm, a, b, m, m * 0.5);
        members.push_back(std::move(mm));
    };
    for (const std::vector<glm::dvec3>& line : lines)
    {
        if (line.size() < 2)
            continue;
        const std::vector<Level> levels = levelsOf(line, o.segment);
        if (levels.size() < 2)
            continue;
        std::vector<std::vector<glm::dvec3>> ring(levels.size());
        glm::dvec3 b1 = std::abs(levels[0].t.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
        for (size_t i = 0; i < levels.size(); ++i)
        {
            const glm::dvec3 t = levels[i].t;
            b1 = unit(b1 - t * glm::dot(b1, t), gen::perpendicular(t));
            const glm::dvec3 b2 = glm::cross(t, b1);
            const double end = o.endWidth >= 0.0 ? o.endWidth : o.width;
            const double side = std::max(m, glm::mix(o.width, end, levels[i].along));
            const double radius = side / (2.0 * std::sin(std::numbers::pi / chords));
            for (int k = 0; k < chords; ++k)
            {
                const double a = 2.0 * std::numbers::pi * k / chords + std::numbers::pi / chords;
                ring[i].push_back(levels[i].p + (b1 * std::cos(a) + b2 * std::sin(a)) * radius);
            }
        }
        for (size_t i = 0; i < levels.size(); ++i)
            for (int k = 0; k < chords; ++k)
            {
                const int k2 = (k + 1) % chords;
                if (o.rings || i == 0 || i + 1 == levels.size())
                    beam(ring[i][size_t(k)], ring[i][size_t(k2)]);
                if (i + 1 == levels.size())
                    continue;
                beam(ring[i][size_t(k)], ring[i + 1][size_t(k)]);
                if (o.bracing == Bracing::X)
                {
                    beam(ring[i][size_t(k)], ring[i + 1][size_t(k2)]);
                    beam(ring[i][size_t(k2)], ring[i + 1][size_t(k)]);
                }
                else if (o.bracing == Bracing::Zigzag)
                {
                    if (i % 2 == 0)
                        beam(ring[i][size_t(k)], ring[i + 1][size_t(k2)]);
                    else
                        beam(ring[i][size_t(k2)], ring[i + 1][size_t(k)]);
                }
            }
    }
    r.members = int(members.size());
    if (members.empty())
        return r;
    if (o.unite)
    {
        std::vector<lila::scene::BooleanOperand> ops;
        for (const Mesh& mm : members)
            ops.push_back({&mm, glm::dmat4(1.0)});
        lila::scene::BooleanOptions bo;
        bo.op = lila::scene::BooleanOp::Union;
        bo.cleanup = false;
        bo.splitNgons = false;
        bo.hardAngle = 30.0;
        lila::scene::BooleanResult u = lila::scene::meshBoolean(ops, glm::dmat4(1.0), bo);
        if (u.ok())
        {
            r.mesh = std::move(u.mesh);
            r.mesh.compact();
            gen::boxUvs(r.mesh);
            return r;
        }
        r.warning = "members left overlapping, not united: " + u.error;
    }
    for (const Mesh& mm : members)
        r.mesh.append(mm);
    gen::boxUvs(r.mesh);
    return r;
}
} // namespace weathering::truss
