#include "deform/BendBones.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::deform::bendbones
{
namespace
{
glm::dvec3 perpendicular(const glm::dvec3& v, const glm::dvec3& t)
{
    const glm::dvec3 p = v - glm::dot(v, t) * t;
    const double l = glm::length(p);
    if (l > 1e-12)
        return p / l;
    const glm::dvec3 any = std::abs(t.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
    return glm::normalize(glm::cross(t, glm::cross(any, t)));
}

/** Centripetal Catmull-Rom between p1 and p2 at u in [0, 1]. */
glm::dvec3 catmullRom(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, const glm::dvec3& p3, double u)
{
    auto knot = [](double t, const glm::dvec3& a, const glm::dvec3& b) { return t + std::max(std::sqrt(glm::length(b - a)), 1e-6); };
    const double t0 = 0.0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
    const double t = t1 + (t2 - t1) * u;
    const glm::dvec3 a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1;
    const glm::dvec3 a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2;
    const glm::dvec3 a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3;
    const glm::dvec3 b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
    const glm::dvec3 b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
    return (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2;
}

glm::dmat3 turnAbout(const glm::dvec3& axis, double angle) { return glm::dmat3(glm::mat3_cast(glm::angleAxis(angle, axis))); }

/** The sample before arc length s and how far past it, clamped to the curve. */
std::pair<size_t, double> sampleAt(const Curve& c, double s)
{
    const auto it = std::upper_bound(c.length.begin(), c.length.end(), s);
    size_t i = it == c.length.begin() ? 0 : size_t(it - c.length.begin()) - 1;
    i = std::min(i, c.length.size() - 2);
    const double span = c.length[i + 1] - c.length[i];
    return {i, span > 1e-12 ? std::clamp((s - c.length[i]) / span, 0.0, 1.0) : 0.0};
}

glm::dvec3 pointAt(const Curve& c, double s)
{
    const auto [i, f] = sampleAt(c, s);
    return glm::mix(c.point[i], c.point[i + 1], f);
}

glm::dmat3 frameAt(const Curve& c, double s)
{
    const auto [i, f] = sampleAt(c, s);
    const glm::dquat a = glm::quat_cast(c.frame[i]);
    glm::dquat b = glm::quat_cast(c.frame[i + 1]);
    if (glm::dot(a, b) < 0.0)
        b = -b;
    return glm::dmat3(glm::mat3_cast(glm::normalize(glm::slerp(a, b, f))));
}

/** The arc length where the strand at offset (normal, binormal) has covered a fraction of its length over a segment. */
double strandLength(const Curve& c, int k, const glm::dvec2& offset, double fraction)
{
    const int from = c.knot[size_t(k)], to = c.knot[size_t(k) + 1];
    std::vector<double> run{0.0};
    glm::dvec3 prev = c.point[size_t(from)] + c.frame[size_t(from)] * glm::dvec3(0.0, offset.x, offset.y);
    for (int i = from + 1; i <= to; ++i)
    {
        const glm::dvec3 q = c.point[size_t(i)] + c.frame[size_t(i)] * glm::dvec3(0.0, offset.x, offset.y);
        run.push_back(run.back() + glm::length(q - prev));
        prev = q;
    }
    const double want = std::clamp(fraction, 0.0, 1.0) * run.back();
    const auto it = std::lower_bound(run.begin(), run.end(), want);
    if (it == run.begin())
        return c.length[size_t(from)];
    const size_t j = size_t(it - run.begin());
    const double span = run[j] - run[j - 1];
    const double f = span > 1e-12 ? (want - run[j - 1]) / span : 0.0;
    return glm::mix(c.length[size_t(from) + j - 1], c.length[size_t(from) + j], f);
}

/** The fraction of the segment's strand length at arc length s, for the strand at offset. */
double strandFraction(const Curve& c, int k, const glm::dvec2& offset, double s)
{
    const int from = c.knot[size_t(k)], to = c.knot[size_t(k) + 1];
    double total = 0.0, upTo = 0.0;
    glm::dvec3 prev = c.point[size_t(from)] + c.frame[size_t(from)] * glm::dvec3(0.0, offset.x, offset.y);
    for (int i = from + 1; i <= to; ++i)
    {
        const glm::dvec3 q = c.point[size_t(i)] + c.frame[size_t(i)] * glm::dvec3(0.0, offset.x, offset.y);
        const double d = glm::length(q - prev);
        if (c.length[size_t(i)] <= s)
            upTo += d;
        else if (c.length[size_t(i) - 1] < s)
            upTo += d * (s - c.length[size_t(i) - 1]) / std::max(c.length[size_t(i)] - c.length[size_t(i) - 1], 1e-12);
        total += d;
        prev = q;
    }
    return total > 1e-12 ? upTo / total : 0.0;
}
} // namespace

Curve build(const Chain& chain, const std::vector<glm::dvec3>& normals, int samplesPerSpan)
{
    LILA_ZONE();
    Curve c;
    const size_t joints = chain.position.size();
    if (joints < 2)
        return c;
    // The control points: each joint, then its bone's CVs.
    std::vector<glm::dvec3> control;
    std::vector<int> jointControl;
    for (size_t j = 0; j < joints; ++j)
    {
        jointControl.push_back(int(control.size()));
        control.push_back(chain.position[j]);
        if (j + 1 < joints && j < chain.cvs.size())
            control.insert(control.end(), chain.cvs[j].begin(), chain.cvs[j].end());
    }
    const size_t spans = control.size() - 1;
    auto at = [&](long i) {
        if (i < 0)
            return 2.0 * control[0] - control[1];
        if (size_t(i) >= control.size())
            return 2.0 * control.back() - control[control.size() - 2];
        return control[size_t(i)];
    };
    for (size_t sp = 0; sp < spans; ++sp)
        for (int k = 0; k < samplesPerSpan; ++k)
            c.point.push_back(catmullRom(at(long(sp) - 1), at(long(sp)), at(long(sp) + 1), at(long(sp) + 2), double(k) / samplesPerSpan));
    c.point.push_back(control.back());
    for (int jc : jointControl)
        c.knot.push_back(jc * samplesPerSpan);

    const size_t n = c.point.size();
    c.length.assign(n, 0.0);
    for (size_t i = 1; i < n; ++i)
        c.length[i] = c.length[i - 1] + glm::length(c.point[i] - c.point[i - 1]);
    std::vector<glm::dvec3> tangent(n);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 d = c.point[std::min(i + 1, n - 1)] - c.point[i == 0 ? 0 : i - 1];
        tangent[i] = glm::length(d) > 1e-12 ? glm::normalize(d) : (i ? tangent[i - 1] : glm::dvec3(1.0, 0.0, 0.0));
    }

    // Rotation-minimising frames by double reflection, from the first joint's normal.
    auto jointNormal = [&](size_t j, const glm::dvec3& t) {
        if (j < normals.size())
            return perpendicular(chain.rotation[j] * normals[j], t);
        const glm::dmat3& r = chain.rotation[j];
        int least = 0;
        for (int a = 1; a < 3; ++a)
            if (std::abs(glm::dot(r[a], t)) < std::abs(glm::dot(r[least], t)))
                least = a;
        return perpendicular(r[least], t);
    };
    std::vector<glm::dvec3> normal(n);
    normal[0] = jointNormal(0, tangent[0]);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const glm::dvec3 v1 = c.point[i + 1] - c.point[i];
        const double c1 = glm::dot(v1, v1);
        if (c1 < 1e-24)
        {
            normal[i + 1] = perpendicular(normal[i], tangent[i + 1]);
            continue;
        }
        const glm::dvec3 rL = normal[i] - (2.0 / c1) * glm::dot(v1, normal[i]) * v1;
        const glm::dvec3 tL = tangent[i] - (2.0 / c1) * glm::dot(v1, tangent[i]) * v1;
        const glm::dvec3 v2 = tangent[i + 1] - tL;
        const double c2 = glm::dot(v2, v2);
        normal[i + 1] = perpendicular(c2 < 1e-24 ? rL : rL - (2.0 / c2) * glm::dot(v2, rL) * v2, tangent[i + 1]);
    }

    // Twist: at each joint, the turn from the carried normal to the joint's own, ramped along the arc in between.
    std::vector<double> twist(joints, 0.0);
    for (size_t j = 0; j < joints && !normals.empty(); ++j)
    {
        const size_t i = size_t(c.knot[j]);
        const glm::dvec3 want = jointNormal(j, tangent[i]);
        twist[j] = std::atan2(glm::dot(glm::cross(normal[i], want), tangent[i]), glm::dot(normal[i], want));
        if (j > 0)
            while (twist[j] - twist[j - 1] > glm::pi<double>())
                twist[j] -= 2.0 * glm::pi<double>();
        if (j > 0)
            while (twist[j] - twist[j - 1] < -glm::pi<double>())
                twist[j] += 2.0 * glm::pi<double>();
    }
    c.frame.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        size_t j = 0;
        while (j + 2 < joints && size_t(c.knot[j + 1]) <= i)
            ++j;
        const double a = c.length[size_t(c.knot[j])], b = c.length[size_t(c.knot[j + 1])];
        const double f = b > a ? std::clamp((c.length[i] - a) / (b - a), 0.0, 1.0) : 0.0;
        const double angle = glm::mix(twist[j], twist[j + 1], f);
        const glm::dvec3 nn = turnAbout(tangent[i], angle) * normal[i];
        c.frame[i] = glm::dmat3(tangent[i], nn, glm::cross(tangent[i], nn));
    }
    return c;
}

std::vector<glm::dvec3> restNormals(const Chain& chain, const Curve& rest)
{
    std::vector<glm::dvec3> out;
    for (size_t j = 0; j < chain.position.size(); ++j)
        out.push_back(glm::transpose(chain.rotation[j]) * rest.frame[size_t(rest.knot[j])][1]);
    return out;
}

Place locate(const Curve& rest, const glm::dvec3& p)
{
    double best = std::numeric_limits<double>::max(), s = 0.0;
    for (size_t i = 0; i + 1 < rest.point.size(); ++i)
    {
        const glm::dvec3 a = rest.point[i], d = rest.point[i + 1] - a;
        const double len2 = glm::dot(d, d);
        const double f = len2 > 1e-24 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
        if (const double dist = glm::length(p - (a + d * f)); dist < best)
        {
            best = dist;
            s = glm::mix(rest.length[i], rest.length[i + 1], f);
        }
    }
    Place out;
    out.distance = best;
    const size_t segments = rest.knot.size() - 1;
    while (out.segment + 1 < int(segments) && rest.length[size_t(rest.knot[size_t(out.segment) + 1])] < s)
        ++out.segment;
    const double a = rest.length[size_t(rest.knot[size_t(out.segment)])], b = rest.length[size_t(rest.knot[size_t(out.segment) + 1])];
    out.fraction = b > a ? (s - a) / (b - a) : 0.0;
    out.offset = glm::transpose(frameAt(rest, s)) * (p - pointAt(rest, s));
    return out;
}

glm::dvec3 deformPoint(const Curve& rest, const Curve& posed, const Place& place, const Options& o)
{
    const int k = place.segment;
    const double ra = rest.length[size_t(rest.knot[size_t(k)])], rb = rest.length[size_t(rest.knot[size_t(k) + 1])];
    const double pa = posed.length[size_t(posed.knot[size_t(k)])], pb = posed.length[size_t(posed.knot[size_t(k) + 1])];
    const double stretch = rb > ra ? (pb - pa) / (rb - ra) : 1.0;
    const double thin = glm::mix(1.0, 1.0 / std::sqrt(std::max(stretch, 1e-3)), std::clamp(o.volume, 0.0, 1.0));
    const glm::dvec2 across(place.offset.y * thin, place.offset.z * thin);

    double s = pa + place.fraction * (pb - pa);
    if (o.evenBend > 0.0 && glm::length(across) > 1e-9)
    {
        const double fr = strandFraction(rest, k, glm::dvec2(place.offset.y, place.offset.z), ra + place.fraction * (rb - ra));
        s = glm::mix(s, strandLength(posed, k, across, fr), std::clamp(o.evenBend, 0.0, 1.0));
    }
    const glm::dmat3 f = frameAt(posed, s);
    glm::dvec3 d = f * glm::dvec3(0.0, across.x, across.y);
    // Held short of the bend's centre: an offset reaching past it would fold the inside through itself.
    const double h = std::max(1e-3, (posed.length.back() - posed.length.front()) / double(posed.point.size() - 1));
    const glm::dvec3 bendRate = (frameAt(posed, s + h)[0] - frameAt(posed, s - h)[0]) / (2.0 * h);
    if (const double curvature = glm::length(bendRate); curvature > 1e-9)
    {
        const glm::dvec3 toCentre = bendRate / curvature;
        const double reach = glm::dot(d, toCentre), limit = 0.85 / curvature;
        if (reach > limit)
            d -= (reach - limit) * toCentre;
    }
    return pointAt(posed, s) + f[0] * place.offset.x + d;
}

Binding bindPoints(std::span<const Curve> rest, std::span<const glm::dvec3> points, double blend)
{
    LILA_ZONE();
    Binding b;
    for (const glm::dvec3& p : points)
    {
        std::vector<Place> places;
        double nearest = std::numeric_limits<double>::max();
        for (const Curve& c : rest)
        {
            places.push_back(c.point.size() >= 2 ? locate(c, p) : Place{.distance = std::numeric_limits<double>::max()});
            nearest = std::min(nearest, places.back().distance);
        }
        const double band = std::max(blend * nearest, 1e-6);
        std::vector<std::pair<double, uint32_t>> ws;
        for (size_t i = 0; i < places.size(); ++i)
            if (const double over = places[i].distance - nearest; over < band)
            {
                const double x = 1.0 - over / band;
                ws.push_back({x * x, uint32_t(i)});
            }
        std::sort(ws.begin(), ws.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        if (ws.size() > 2)
            ws.resize(2);
        double total = 0.0;
        for (const auto& w : ws)
            total += w.first;
        for (const auto& [w, c] : ws)
        {
            b.chain.push_back(c);
            b.weight.push_back(w / total);
            b.place.push_back(places[c]);
        }
        b.offsets.push_back(uint32_t(b.chain.size()));
    }
    return b;
}

void deformPoints(std::span<const Curve> rest, std::span<const Curve> posed, const Binding& b, const Options& o, std::span<glm::dvec3> points)
{
    LILA_ZONE();
    const size_t n = std::min(points.size(), b.offsets.size() - 1);
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec3 sum(0.0);
        double total = 0.0;
        for (uint32_t e = b.offsets[i]; e < b.offsets[i + 1]; ++e)
        {
            const uint32_t c = b.chain[e];
            if (c >= rest.size() || c >= posed.size() || rest[c].knot.size() != posed[c].knot.size() || rest[c].point.size() != posed[c].point.size())
                continue;
            sum += b.weight[e] * deformPoint(rest[c], posed[c], b.place[e], o);
            total += b.weight[e];
        }
        if (total > 1e-12)
            points[i] = sum / total;
    }
}

glm::dmat4 matrixAt(const Curve& c, double s)
{
    glm::dmat4 m(frameAt(c, s));
    m[3] = glm::dvec4(pointAt(c, s), 1.0);
    return m;
}

std::vector<double> segmentLengths(const Curve& c, int perBone)
{
    std::vector<double> out;
    perBone = std::max(1, perBone);
    for (size_t k = 0; k + 1 < c.knot.size(); ++k)
    {
        const double a = c.length[size_t(c.knot[k])], b = c.length[size_t(c.knot[k + 1])];
        for (int i = 0; i < perBone; ++i)
            out.push_back(a + (b - a) * double(i) / double(perBone));
    }
    if (!c.knot.empty())
        out.push_back(c.length[size_t(c.knot.back())]);
    return out;
}
} // namespace lila::deform::bendbones
