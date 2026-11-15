#include "Blocks.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tailor
{
namespace
{
using glm::vec2;
using glm::vec3;

float smin(float a, float b, float k)
{
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.f, 1.f);
    return glm::mix(b, a, h) - k * h * (1.f - h);
}

/** Capsule from a to b whose radius runs from ra to rb. */
float cone(const vec3& p, const vec3& a, const vec3& b, float ra, float rb)
{
    const vec3 ab = b - a;
    const float t = std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.f, 1.f);
    return glm::length(p - (a + ab * t)) - glm::mix(ra, rb, t);
}

float ellipsoid(const vec3& p, const vec3& c, const vec3& r)
{
    const vec3 q = (p - c) / r;
    return (glm::length(q) - 1.f) * std::min(r.x, std::min(r.y, r.z));
}

/** Half width (x) of an elliptical section with depth = kDepth * width and the given circumference. */
constexpr float kDepth = 0.72f;
float halfWidth(float circumference) { return circumference / (3.14159265f * (1.f + kDepth)); }

struct Shape
{
    const Body& b;
    float crotch, hip, waist, chest, shoulder, neckBase, top;
    float hipA, waistA, chestA, shoulderA;

    explicit Shape(const Body& body)
        : b(body), crotch(body.crotchY()), hip(body.hipY()), waist(body.waistY()), chest(body.chestY()),
          shoulder(body.shoulderY()), neckBase(body.neckY()), top(body.height)
    {
        hipA = halfWidth(b.hip);
        waistA = halfWidth(b.waist);
        chestA = halfWidth(b.chest);
        shoulderA = std::max(chestA * 0.85f, b.shoulderWidth * 0.5f - 0.05f);
    }

    float torso(const vec3& p) const
    {
        const float y0 = crotch + 0.03f, y1 = shoulder - 0.04f;
        float a;
        if (p.y < waist)
            a = glm::mix(hipA, waistA, std::clamp((p.y - hip) / (waist - hip), 0.f, 1.f));
        else if (p.y < chest)
            a = glm::mix(waistA, chestA, (p.y - waist) / (chest - waist));
        else
            a = glm::mix(chestA, shoulderA, std::clamp((p.y - chest) / (y1 - chest), 0.f, 1.f));
        // The upper chest flattens towards the shoulders.
        const float bz = a * glm::mix(kDepth, 0.6f, std::clamp((p.y - chest) / (y1 - chest), 0.f, 1.f));
        const float section = (glm::length(vec2(p.x / a, p.z / bz)) - 1.f) * bz;
        return std::max(section, std::max(y0 - p.y, p.y - y1));
    }

    float operator()(const vec3& p) const
    {
        const float k = 0.035f;
        float d = torso(p);
        const float sx = std::max(b.shoulderWidth * 0.5f, chestA + 0.06f);
        for (float s : {1.f, -1.f})
        {
            // Shoulder cap, arm in the body's A-pose, hand.
            const vec3 joint{s * (sx - 0.045f), shoulder - 0.065f, 0.f};
            const vec3 dir{s * std::cos(b.armAngle), -std::sin(b.armAngle), 0.f};
            const vec3 elbow = joint + dir * (b.armLength * 0.48f), wrist = joint + dir * b.armLength;
            d = smin(d, cone(p, vec3(s * 0.04f, shoulder - 0.08f, -0.01f), joint, 0.048f, 0.046f), k);
            d = smin(d, cone(p, joint, elbow, 0.046f, 0.038f), 0.02f);
            d = smin(d, cone(p, elbow, wrist, 0.04f, 0.034f), 0.015f);
            d = smin(d, cone(p, wrist + dir * 0.03f, wrist + dir * 0.1f, 0.036f, 0.03f), 0.02f);

            // Leg: hip joint, knee, ankle, foot pointing +Z.
            const vec3 hipJ{s * hipA * 0.5f, crotch + 0.06f, 0.f};
            const vec3 knee{s * hipA * 0.5f, crotch * 0.55f, 0.01f};
            const vec3 ankle{s * hipA * 0.52f, 0.08f, -0.01f};
            d = smin(d, cone(p, hipJ, knee, hipA * 0.52f, 0.055f), k);
            d = smin(d, cone(p, knee, ankle, 0.052f, 0.035f), 0.02f);
            d = smin(d, cone(p, vec3(ankle.x, 0.04f, -0.04f), vec3(ankle.x, 0.035f, 0.15f), 0.04f, 0.035f), 0.03f);
        }
        const float neckR = b.neck / (2.f * 3.14159265f);
        d = smin(d, cone(p, vec3(0.f, shoulder - 0.06f, -0.01f), vec3(0.f, neckBase + 0.07f, 0.f), neckR * 1.05f, neckR * 0.92f), k);
        const float headH = top - neckBase;
        const float headR = b.head / (2.f * 3.14159265f);
        d = smin(d, ellipsoid(p, vec3(0.f, top - headH * 0.42f, 0.01f), vec3(headR * 0.85f, headH * 0.44f, headR * 1.05f)),
                 0.03f);
        return d;
    }
};
} // namespace

void mannequin(const Body& body, std::vector<Vec3>& points, std::vector<std::array<int, 3>>& tris, float cell)
{
    points.clear();
    tris.clear();
    const Shape shape(body);
    const float reach = body.shoulderWidth * 0.5f + body.armLength + 0.15f;
    const vec3 lo(-reach, -0.02f, -0.3f), hi(reach, body.height + 0.05f, 0.35f);
    const int nx = int((hi.x - lo.x) / cell) + 2, ny = int((hi.y - lo.y) / cell) + 2, nz = int((hi.z - lo.z) / cell) + 2;
    auto at = [&](int i, int j, int k) { return (size_t(k) * ny + j) * nx + i; };
    auto pos = [&](int i, int j, int k) { return lo + vec3(float(i), float(j), float(k)) * cell; };

    std::vector<float> phi(size_t(nx) * ny * nz);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                phi[at(i, j, k)] = shape(pos(i, j, k));

    // Surface nets: one vertex per cell the surface crosses, at the mean of its edge crossings.
    std::unordered_map<size_t, int> cellVertex;
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int edge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k + 1 < nz; ++k)
        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i)
            {
                float v[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c)
                {
                    v[c] = phi[at(i + corner[c][0], j + corner[c][1], k + corner[c][2])];
                    inside += v[c] < 0.f;
                }
                if (inside == 0 || inside == 8)
                    continue;
                vec3 sum(0.f);
                int count = 0;
                for (const auto& e : edge)
                {
                    const float a = v[e[0]], b = v[e[1]];
                    if ((a < 0.f) == (b < 0.f))
                        continue;
                    const float t = a / (a - b);
                    const vec3 pa(corner[e[0]][0], corner[e[0]][1], corner[e[0]][2]),
                        pb(corner[e[1]][0], corner[e[1]][1], corner[e[1]][2]);
                    sum += glm::mix(pa, pb, t);
                    ++count;
                }
                const vec3 p = pos(i, j, k) + sum / float(count) * cell;
                cellVertex[at(i, j, k)] = int(points.size());
                points.push_back({p.x, p.y, p.z});
            }

    // A quad around every grid edge with a sign change, wound so the normal points out of the body.
    auto vertexOf = [&](int i, int j, int k) {
        const auto it = cellVertex.find(at(i, j, k));
        return it == cellVertex.end() ? -1 : it->second;
    };
    auto quad = [&](int a, int b, int c, int d, bool flip) {
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return;
        if (flip)
            std::swap(b, d);
        tris.push_back({a, b, c});
        tris.push_back({a, c, d});
    };
    for (int k = 1; k + 1 < nz; ++k)
        for (int j = 1; j + 1 < ny; ++j)
            for (int i = 1; i + 1 < nx; ++i)
            {
                const bool in = phi[at(i, j, k)] < 0.f;
                if (in != (phi[at(i + 1, j, k)] < 0.f))
                    quad(vertexOf(i, j - 1, k - 1), vertexOf(i, j, k - 1), vertexOf(i, j, k), vertexOf(i, j - 1, k), !in);
                if (in != (phi[at(i, j + 1, k)] < 0.f))
                    quad(vertexOf(i - 1, j, k - 1), vertexOf(i - 1, j, k), vertexOf(i, j, k), vertexOf(i, j, k - 1), !in);
                if (in != (phi[at(i, j, k + 1)] < 0.f))
                    quad(vertexOf(i - 1, j - 1, k), vertexOf(i, j - 1, k), vertexOf(i, j, k), vertexOf(i - 1, j, k), !in);
            }
}

} // namespace tailor

namespace tailor
{
namespace
{
using XZ = glm::vec2;

/** Closed loops where the plane y = h cuts the mesh, as x-z points. */
std::vector<std::vector<XZ>> slice(const std::vector<Vec3>& pts, const std::vector<std::array<int, 3>>& tris, float h)
{
    // Every crossing sits on a mesh edge; a triangle crossed by the plane links its two crossings.
    std::unordered_map<uint64_t, int> crossing;
    std::vector<XZ> at;
    std::vector<std::vector<int>> links;
    auto cross = [&](int a, int b) -> int {
        const uint64_t key = (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
        if (const auto it = crossing.find(key); it != crossing.end())
            return it->second;
        const Vec3 &pa = pts[a], &pb = pts[b];
        const float t = (h - pa[1]) / (pb[1] - pa[1]);
        crossing[key] = int(at.size());
        at.push_back({pa[0] + (pb[0] - pa[0]) * t, pa[2] + (pb[2] - pa[2]) * t});
        links.emplace_back();
        return int(at.size()) - 1;
    };
    for (const auto& t : tris)
    {
        int found[2], count = 0;
        for (int k = 0; k < 3 && count < 2; ++k)
        {
            const int a = t[k], b = t[(k + 1) % 3];
            if ((pts[a][1] < h) != (pts[b][1] < h))
                found[count++] = cross(a, b);
        }
        if (count == 2)
        {
            links[found[0]].push_back(found[1]);
            links[found[1]].push_back(found[0]);
        }
    }
    std::vector<std::vector<XZ>> loops;
    std::vector<uint8_t> seen(at.size(), 0);
    for (size_t s = 0; s < at.size(); ++s)
    {
        if (seen[s])
            continue;
        std::vector<XZ> loop;
        std::vector<int> stack{int(s)};
        seen[s] = 1;
        while (!stack.empty())
        {
            const int c = stack.back();
            stack.pop_back();
            loop.push_back(at[c]);
            for (int nb : links[c])
                if (!seen[nb])
                {
                    seen[nb] = 1;
                    stack.push_back(nb);
                }
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

/** Perimeter of the convex hull: what a tape measure around the section reads. */
float hullPerimeter(std::vector<XZ> p)
{
    if (p.size() < 3)
        return 0.f;
    std::sort(p.begin(), p.end(), [](const XZ& a, const XZ& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    auto turn = [](const XZ& o, const XZ& a, const XZ& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    std::vector<XZ> hull(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], p[i]) <= 0.f)
            --k;
        hull[k++] = p[i];
    }
    for (size_t i = p.size() - 1, lower = k + 1; i-- > 0;)
    {
        while (k >= lower && turn(hull[k - 2], hull[k - 1], p[i]) <= 0.f)
            --k;
        hull[k++] = p[i];
    }
    hull.resize(k - 1);
    float len = 0.f;
    for (size_t i = 0; i < hull.size(); ++i)
        len += glm::length(hull[(i + 1) % hull.size()] - hull[i]);
    return len;
}

/** The girth of the section loop nearest the body's axis at height h, or 0 when the plane misses. */
float girth(const std::vector<Vec3>& pts, const std::vector<std::array<int, 3>>& tris, float h, const XZ& axis)
{
    float best = 0.f, bestDist = 1e30f;
    for (const auto& loop : slice(pts, tris, h))
    {
        XZ c(0.f);
        for (const XZ& q : loop)
            c += q;
        c /= float(loop.size());
        if (const float d = glm::length(c - axis); d < bestDist)
        {
            bestDist = d;
            best = hullPerimeter(loop);
        }
    }
    return best;
}
} // namespace

/**
 * The base of the neck, from the floor: walking down from the crown, the narrowest section after the head has widened and
 * before the shoulders widen again. 0 when the profile has no such waist.
 */
static float findNeck(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, float floor, float height, const XZ& axis)
{
    const float step = 0.005f * height;
    float headMax = 0.f, best = 1e30f, bestY = 0.f;
    bool narrowed = false;
    for (float y = height - step; y > 0.3f * height; y -= step)
    {
        const float g = girth(points, tris, floor + y, axis);
        if (g <= 0.f)
            continue;
        if (!narrowed)
        {
            headMax = std::max(headMax, g);
            if (g < 0.8f * headMax)
                narrowed = true, best = g, bestY = y;
            continue;
        }
        if (g < best)
            best = g, bestY = y;
        else if (g > 1.2f * best)
            return bestY;
    }
    return 0.f;
}

namespace
{
/** Fills body's left hand and foot from the mesh's points; `feet` is the centre of its feet. Leaves what it cannot read. */
void measureHandAndFoot(const std::vector<Vec3>& points, const vec3& feet, Body& body)
{
    const float k = body.torso() / 1.8f;
    std::vector<vec3> left;
    float reach = 0.f;
    for (const Vec3& q : points)
        if (const vec3 p = vec3(q[0], q[1], q[2]) - feet; p.x > 0.f)
        {
            left.push_back(p);
            reach = std::max(reach, p.x);
        }
    if (left.empty())
        return;

    // Foot: what stands on the floor.
    vec3 lo(1e30f), hi(-1e30f);
    for (const vec3& p : left)
        if (p.y < 0.035f * k)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    if (const vec3 size = hi - lo; size.z > 0.14f * k && size.z < 0.45f * k && size.x > 0.04f * k && size.x < 0.22f * k)
    {
        body.footX = 0.5f * (lo.x + hi.x);
        body.heelZ = lo.z;
        body.footLength = size.z;
        body.footWidth = size.x;
    }

    // Hand: the arm's far end, when the arms are held out from the body far enough for that to be the fingertips.
    vec3 tip(0.f), arm(0.f);
    int tips = 0, arms = 0;
    for (const vec3& p : left)
        if (p.x > reach - 0.015f * k)
            tip += p, ++tips;
    tip /= float(std::max(tips, 1));
    if (tip.x < 0.5f * body.shoulderWidth + 0.15f * k || tip.y > body.shoulderY() + 0.1f * k)
        return;
    for (const vec3& p : left)
        if (glm::distance(p, tip) < 0.3f * k)
            arm += p, ++arms;
    arm /= float(std::max(arms, 1));
    if (glm::distance(arm, tip) < 0.05f * k)
        return;
    const vec3 dir = glm::normalize(tip - arm);
    body.handTip = {tip.x, tip.y, tip.z};
    body.handDir = {dir.x, dir.y, dir.z};

    // A flat hand's thin axis is the way out of its back: the minor axis of the hand's points across the arm.
    const vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.z) < 0.9f ? vec3(0.f, 0.f, 1.f) : vec3(0.f, 1.f, 0.f)));
    const vec3 v = glm::cross(dir, u);
    vec3 centre(0.f);
    int count = 0;
    for (const vec3& p : left)
        if (glm::distance(p, tip) < 0.15f * k)
            centre += p, ++count;
    if (count < 8)
        return;
    centre /= float(count);
    float cuu = 0.f, cvv = 0.f, cuv = 0.f;
    for (const vec3& p : left)
        if (glm::distance(p, tip) < 0.15f * k)
        {
            const float a = glm::dot(p - centre, u), b = glm::dot(p - centre, v);
            cuu += a * a, cvv += b * b, cuv += a * b;
        }
    const float mean = 0.5f * (cuu + cvv), spread = std::hypot(0.5f * (cuu - cvv), cuv);
    if (mean - spread <= 0.f || (mean + spread) / (mean - spread) < 2.2f)
        return;
    const float theta = 0.5f * std::atan2(2.f * cuv, cuu - cvv) + 1.5707963f;
    vec3 back = u * std::cos(theta) + v * std::sin(theta);
    // Thumbs forward: of the two ways out of a flat hand, the back is the one that puts the thumb side in front.
    const float thumbZ = glm::cross(dir, back).z;
    if (thumbZ < -0.2f || (std::abs(thumbZ) <= 0.2f && back.y < 0.f))
        back = -back;
    body.handBack = {back.x, back.y, back.z};
}
} // namespace

Body measureBody(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, Vec3* offset, float neckGiven)
{
    Body body;
    if (points.empty() || tris.empty())
        return body;
    vec3 lo(1e30f), hi(-1e30f);
    for (const Vec3& p : points)
    {
        lo = glm::min(lo, vec3(p[0], p[1], p[2]));
        hi = glm::max(hi, vec3(p[0], p[1], p[2]));
    }
    // Lengths without a slice to read them from scale with height.
    const Body ref;
    const float s = (hi.y - lo.y) / ref.height;
    body.height = hi.y - lo.y;
    body.shoulderWidth = ref.shoulderWidth * s;
    body.armLength = ref.armLength * s;
    body.inseam = ref.inseam * s;

    const XZ axis(0.5f * (lo.x + hi.x), 0.5f * (lo.z + hi.z));
    // A head far out of human proportion moves the neck: everything below it is then sized from the neck, not the height.
    const float neck = neckGiven > 0.f ? neckGiven : findNeck(points, tris, lo.y, body.height, axis);
    if (neck > 0.f && (neckGiven > 0.f || std::abs(neck - ref.neckY() * s) > 0.05f * body.height))
    {
        body.neckHeight = neck;
        const float t = body.torso() / ref.height;
        body.shoulderWidth = ref.shoulderWidth * t;
        body.armLength = ref.armLength * t;
        body.inseam = ref.inseam * t;
    }
    const float st = body.torso() / ref.height;
    // Measured relative to the floor of the mesh, so slice heights are offset by lo.y.
    auto read = [&](float y, float fallback) {
        const float g = girth(points, tris, lo.y + y, axis);
        return g > 0.f ? g : fallback * st;
    };
    // Under the arms the section can take them in; the smallest of three is the chest.
    body.chest = std::min({read(body.chestY(), ref.chest), read(body.chestY() - 0.03f * st, ref.chest),
                           read(body.chestY() + 0.03f * st, ref.chest)});
    body.waist = read(body.waistY(), ref.waist);
    body.hip = read(body.hipY(), ref.hip);
    body.neck = read(body.neckY() + 0.03f * st, ref.neck);
    // The widest section above the neck, so a big head gets a hood that fits over it.
    body.head = 0.f;
    for (float y = body.neckY() + 0.05f * st; y < body.height - 0.02f * s; y += 0.01f * s)
        body.head = std::max(body.head, read(y, 0.f));
    if (body.head <= 0.f)
        body.head = read(body.height - 0.11f * s, ref.head);
    measureHandAndFoot(points, vec3(axis.x, lo.y, axis.y), body);
    if (offset)
        *offset = {axis.x, lo.y, axis.y};
    return body;
}

} // namespace tailor
