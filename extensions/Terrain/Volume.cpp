// Terrain volume: caves, tunnels, overhangs, arches and mesh stamps as a brick-sparse signed distance field over the
// heightfield (solid below the ground, the edits adding and carving), and the split that leaves the heightfield what it
// can hold and turns the rest into mesh pieces with holes under them and seams on the ground.

#include "Volume.h"

#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/SurfaceNets.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <mutex>
#include <numeric>
#include <set>
#include <unordered_map>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
constexpr int kVolBrick = 8;
constexpr double kVolPi = 3.14159265358979323846;
constexpr double kVolHuge = 1e30;

// ---------------------------------------------------------------------------------------------------- noise

uint64_t volMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct VolumeRng
{
    uint64_t state;
    double next()
    {
        state = volMix64(state);
        return double(state >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
};

double volLattice(int x, int y, int z, uint32_t seed)
{
    const uint64_t h = volMix64(uint64_t(uint32_t(x)) + volMix64(uint64_t(uint32_t(y)) + volMix64(uint64_t(uint32_t(z)) + volMix64(seed))));
    return double(h >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

double volFade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

/** Smooth value noise in -1 to 1. */
double volValueNoise(const glm::dvec3& p, uint32_t seed)
{
    const glm::dvec3 f = glm::floor(p);
    const glm::ivec3 i(f);
    const glm::dvec3 t = p - f;
    const double u = volFade(t.x), v = volFade(t.y), w = volFade(t.z);
    auto at = [&](int dx, int dy, int dz) { return volLattice(i.x + dx, i.y + dy, i.z + dz, seed); };
    const double x00 = glm::mix(at(0, 0, 0), at(1, 0, 0), u), x10 = glm::mix(at(0, 1, 0), at(1, 1, 0), u);
    const double x01 = glm::mix(at(0, 0, 1), at(1, 0, 1), u), x11 = glm::mix(at(0, 1, 1), at(1, 1, 1), u);
    return glm::mix(glm::mix(x00, x10, v), glm::mix(x01, x11, v), w);
}

double volFbm(const glm::dvec3& p, uint32_t seed)
{
    double sum = 0.0, amp = 0.5, total = 0.0, freq = 1.0;
    for (uint32_t o = 0; o < 3; ++o)
    {
        sum += amp * volValueNoise(p * freq + glm::dvec3(17.3 * o), seed + 101u * o);
        total += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return sum / total;
}

double volNoise1(double s, uint32_t seed) { return volValueNoise({s, 0.37, 0.71}, seed); }

double volSmin(double a, double b, double k)
{
    if (k <= 0.0)
        return std::min(a, b);
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

double volSmax(double a, double b, double k) { return -volSmin(-a, -b, k); }

double volSmoothstep(double e0, double e1, double x)
{
    if (e0 == e1)
        return x < e0 ? 0.0 : 1.0;
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ---------------------------------------------------------------------------------------------------- shapes

/** One signed distance term of an op, combined into the field in order. */
struct VolShape
{
    geo::Bounds box;
    std::function<double(const glm::dvec3&)> sdf;
    bool subtract = false;
    double blend = 0.0;
    uint8_t material = 0;
    size_t op = 0;
};

/** A chain of spheres swept along points: the distance to the nearest segment less its radius there. */
struct VolTube
{
    std::vector<glm::dvec3> p;
    std::vector<double> r;

    struct Near
    {
        double d = kVolHuge;
        size_t seg = 0;
        double t = 0.0;
    };

    Near nearest(const glm::dvec3& q) const
    {
        Near best;
        if (p.size() == 1)
        {
            best.d = glm::length(q - p[0]) - r[0];
            return best;
        }
        for (size_t s = 0; s + 1 < p.size(); ++s)
        {
            const glm::dvec3 ab = p[s + 1] - p[s];
            const double len2 = glm::dot(ab, ab);
            const double t = len2 > 0.0 ? std::clamp(glm::dot(q - p[s], ab) / len2, 0.0, 1.0) : 0.0;
            const double d = glm::length(q - (p[s] + ab * t)) - glm::mix(r[s], r[s + 1], t);
            if (d < best.d)
                best = {d, s, t};
        }
        return best;
    }

    geo::Bounds bounds() const
    {
        geo::Bounds b;
        for (size_t k = 0; k < p.size(); ++k)
        {
            b.extend(p[k] - glm::dvec3(r[k]));
            b.extend(p[k] + glm::dvec3(r[k]));
        }
        return b;
    }
};

std::vector<glm::dvec3> volResample(const std::vector<glm::dvec3>& line, double step)
{
    std::vector<glm::dvec3> out;
    if (line.empty())
        return out;
    out.push_back(line[0]);
    for (size_t s = 0; s + 1 < line.size(); ++s)
    {
        const double len = glm::length(line[s + 1] - line[s]);
        const int n = std::max(1, int(std::ceil(len / std::max(step, 1e-6))));
        for (int k = 1; k <= n; ++k)
            out.push_back(glm::mix(line[s], line[s + 1], double(k) / n));
    }
    return out;
}

glm::dvec3 volHorizontalSide(const glm::dvec3& tangent)
{
    glm::dvec3 side = glm::cross(tangent, glm::dvec3(0.0, 1.0, 0.0));
    const double l = glm::length(side);
    return l > 1e-9 ? side / l : glm::dvec3(1.0, 0.0, 0.0);
}

glm::dvec3 volRotateY(const glm::dvec3& v, double a) { return {v.x * std::cos(a) + v.z * std::sin(a), v.y, -v.x * std::sin(a) + v.z * std::cos(a)}; }

double volFeatureSize(const VolumeOp& op, const std::map<std::string, std::shared_ptr<const geo::Mesh>>& meshes)
{
    if (op.kind == "cave" || op.kind == "tunnel" || op.kind == "capsule" || op.kind == "sphere")
        return op.radius;
    if (op.kind == "overhang")
        return std::min(op.depth, op.strata);
    if (op.kind == "arch")
        return std::min(op.thickness, op.width);
    if (op.kind == "box")
        return std::min({op.size.x, op.size.y, op.size.z}) * 0.5;
    if (op.kind == "mesh")
        if (auto it = meshes.find(op.object); it != meshes.end() && it->second)
        {
            const glm::dvec3 e = it->second->bounds().size();
            return std::max({e.x, e.y, e.z}) / 32.0;
        }
    return kVolHuge;
}

/** The terms an op adds to the field, in order. h gives ground heights for ops that follow the ground. */
std::vector<VolShape> volShapesOf(const VolumeOp& op, size_t index, uint8_t material, const Heightfield& h, double voxel,
                            const std::map<std::string, std::shared_ptr<const geo::Mesh>>& meshes, std::string& message)
{
    std::vector<VolShape> out;
    const uint32_t seed = op.seed * 2654435761u + uint32_t(index);
    auto add = [&](geo::Bounds box, std::function<double(const glm::dvec3&)> f, bool subtract) {
        VolShape s;
        s.box = box;
        s.sdf = std::move(f);
        s.subtract = subtract;
        s.blend = op.blend;
        s.material = material;
        s.op = index;
        out.push_back(std::move(s));
    };
    const double noiseAmp = std::max(0.0, op.noise);
    const double noiseSize = std::max(op.noiseSize, voxel);

    if (op.kind == "cave")
    {
        if (op.path.size() < 2)
        {
            message = "a cave needs a path of two or more points";
            return out;
        }
        const double r0 = std::max(op.radius, voxel);
        const double step = std::max(r0 * 0.5, voxel);
        std::vector<glm::dvec3> main = volResample(op.path, step);
        std::vector<double> along(main.size(), 0.0);
        for (size_t k = 1; k < main.size(); ++k)
            along[k] = along[k - 1] + glm::length(main[k] - main[k - 1]);
        const double total = std::max(along.back(), 1e-6);
        const double wave = std::max(noiseSize * 4.0, r0 * 4.0);
        // Wander and swell along the way, pinned at both ends so the entrances stay where they were put.
        VolTube tube;
        for (size_t k = 0; k < main.size(); ++k)
        {
            const glm::dvec3 tan = glm::normalize(main[std::min(k + 1, main.size() - 1)] - main[k > 0 ? k - 1 : 0] + glm::dvec3(1e-9, 0, 0));
            const double pin = volSmoothstep(0.0, 2.0 * r0, along[k]) * volSmoothstep(0.0, 2.0 * r0, total - along[k]);
            const glm::dvec3 off = (volHorizontalSide(tan) * volNoise1(along[k] / wave, seed) + glm::dvec3(0.0, 0.5, 0.0) * volNoise1(along[k] / wave, seed + 7u)) *
                                   r0 * noiseAmp * 1.5 * pin;
            tube.p.push_back(main[k] + off);
            tube.r.push_back(r0 * std::max(0.4, 1.0 + 0.6 * noiseAmp * volNoise1(along[k] / (wave * 0.5), seed + 3u)));
        }
        std::vector<VolTube> tubes{tube};
        struct Chamber
        {
            glm::dvec3 c;
            double r;
        };
        std::vector<Chamber> chambers;
        VolumeRng rng{volMix64(seed + 0xB5u)};
        std::vector<glm::dvec3> ends{tube.p.back()};
        for (int b = 0; b < std::max(0, op.branches); ++b)
        {
            const size_t at = std::clamp(size_t(rng.range(0.15, 0.85) * double(tube.p.size() - 1)), size_t(1), tube.p.size() - 2);
            const glm::dvec3 tan = tube.p[at + 1] - tube.p[at - 1];
            glm::dvec3 dir = glm::normalize(glm::dvec3(tan.x, 0.0, tan.z) + glm::dvec3(1e-9, 0, 0));
            dir = volRotateY(dir, (rng.next() < 0.5 ? -1.0 : 1.0) * rng.range(35.0, 75.0) * kVolPi / 180.0);
            double pitch = rng.range(-12.0, 12.0) * kVolPi / 180.0;
            const double len = std::max(op.branchLength, r0) * rng.range(0.6, 1.2);
            const double br = r0 * rng.range(0.5, 0.8);
            VolTube t;
            glm::dvec3 p = tube.p[at];
            const int steps = std::max(2, int(len / step));
            for (int s = 0; s <= steps; ++s)
            {
                t.p.push_back(p);
                t.r.push_back(br * glm::mix(1.0, 0.6, double(s) / steps));
                dir = volRotateY(dir, rng.range(-15.0, 15.0) * kVolPi / 180.0);
                pitch *= 0.8;
                p += glm::dvec3(dir.x * std::cos(pitch), std::sin(pitch), dir.z * std::cos(pitch)) * (len / steps);
            }
            ends.push_back(t.p.back());
            tubes.push_back(std::move(t));
        }
        for (int c = 0; c < std::max(0, op.chambers); ++c)
        {
            const double cr = std::max(op.chamberRadius, voxel) * rng.range(0.8, 1.2);
            glm::dvec3 at = size_t(c) < ends.size() ? ends[size_t(c)] : tube.p[size_t(rng.range(0.2, 0.8) * double(tube.p.size() - 1))];
            chambers.push_back({at + glm::dvec3(0.0, cr * 0.2, 0.0), cr});
        }
        geo::Bounds box;
        for (const VolTube& t : tubes)
            box.extend(t.bounds());
        for (const Chamber& c : chambers)
        {
            box.extend(c.c - glm::dvec3(c.r));
            box.extend(c.c + glm::dvec3(c.r));
        }
        const double pad = r0 * noiseAmp + op.blend + 2.0 * voxel;
        box.min -= glm::dvec3(pad);
        box.max += glm::dvec3(pad);
        const double k = r0 * 0.6;
        add(box,
            [tubes, chambers, k, r0, noiseAmp, noiseSize, seed](const glm::dvec3& q) {
                double d = kVolHuge;
                for (const VolTube& t : tubes)
                    d = volSmin(d, t.nearest(q).d, k);
                for (const Chamber& c : chambers)
                {
                    const glm::dvec3 e = (q - c.c) / glm::dvec3(c.r, c.r * 0.65, c.r);
                    d = volSmin(d, (glm::length(e) - 1.0) * c.r * 0.65, k);
                }
                return noiseAmp > 0.0 ? d + noiseAmp * r0 * volFbm(q / noiseSize, seed) : d;
            },
            op.subtract);
        return out;
    }

    if (op.kind == "tunnel")
    {
        if (op.path.size() < 2)
        {
            message = "a tunnel needs a path of two or more points";
            return out;
        }
        const double r = std::max(op.radius, voxel);
        // Extended past both ends so the portals open fully; the centre rides half a radius over the road.
        std::vector<glm::dvec3> road = op.path;
        auto extend = [&](glm::dvec3 a, glm::dvec3 b) {
            glm::dvec3 d(a.x - b.x, 0.0, a.z - b.z);
            const double l = glm::length(d);
            return l > 1e-9 ? a + d / l * r : a;
        };
        road.insert(road.begin(), extend(road[0], road[1]));
        road.push_back(extend(road.back(), road[road.size() - 2]));
        road = volResample(road, std::max(r * 0.5, voxel));
        VolTube centre;
        for (const glm::dvec3& p : road)
        {
            centre.p.push_back(p + glm::dvec3(0.0, 0.5 * r, 0.0));
            centre.r.push_back(r);
        }
        geo::Bounds box = centre.bounds();
        box.min -= glm::dvec3(2.0 * voxel + op.blend);
        box.max += glm::dvec3(2.0 * voxel + op.blend);
        add(box,
            [centre, road](const glm::dvec3& q) {
                const VolTube::Near n = centre.nearest(q);
                const double floorY = glm::mix(road[n.seg].y, road[std::min(n.seg + 1, road.size() - 1)].y, n.t);
                return std::max(n.d, floorY - q.y);
            },
            op.subtract);
        return out;
    }

    if (op.kind == "overhang")
    {
        if (op.path.size() < 2)
        {
            message = "an overhang needs a cliff line of two or more points";
            return out;
        }
        std::vector<glm::dvec3> line = volResample(op.path, std::max(h.cell, voxel));
        for (glm::dvec3& p : line)
            p.y = h.heightAt(p.x, p.z);
        // The side the hill rises on is the side it cuts into.
        double rise = 0.0;
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            const glm::dvec3 side = volHorizontalSide(line[k + 1] - line[k]);
            const glm::dvec3 n = h.normalAt(line[k].x, line[k].z);
            rise -= side.x * n.x + side.z * n.z;
        }
        const double sign = rise >= 0.0 ? 1.0 : -1.0;
        std::vector<double> along(line.size(), 0.0);
        for (size_t k = 1; k < line.size(); ++k)
            along[k] = along[k - 1] + glm::length(glm::dvec2(line[k].x - line[k - 1].x, line[k].z - line[k - 1].z));
        const double depth = std::max(op.depth, voxel), height = std::max(op.height, voxel), strata = std::max(op.strata, voxel);
        const double hard = std::clamp(op.hardness, 0.0, 1.0), outside = depth + 2.0 * strata;
        struct Where
        {
            double into, rel, s, fromEnd;
        };
        auto where = [line, along, sign](const glm::dvec3& q) {
            double best = kVolHuge;
            Where w{0, 0, 0, 0};
            for (size_t k = 0; k + 1 < line.size(); ++k)
            {
                const glm::dvec2 a(line[k].x, line[k].z), b(line[k + 1].x, line[k + 1].z), p(q.x, q.z);
                const glm::dvec2 ab = b - a;
                const double len2 = glm::dot(ab, ab);
                const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
                const double d = glm::length(p - (a + ab * t));
                if (d < best)
                {
                    best = d;
                    const glm::dvec3 side = volHorizontalSide(glm::dvec3(ab.x, 0.0, ab.y));
                    const double s = along[k] + t * std::sqrt(len2);
                    const glm::dvec2 off = p - (a + ab * t);
                    w = {sign * -(off.x * side.x + off.y * side.z), q.y - glm::mix(line[k].y, line[k + 1].y, t), s, std::min(s, along.back() - s)};
                }
            }
            return w;
        };
        geo::Bounds box;
        for (const glm::dvec3& p : line)
        {
            box.extend(p - glm::dvec3(outside + depth, strata, outside + depth));
            box.extend(p + glm::dvec3(outside + depth, height + op.lip + 2.0 * strata, outside + depth));
        }
        box.min -= glm::dvec3(2.0 * voxel + op.blend);
        box.max += glm::dvec3(2.0 * voxel + op.blend);
        add(box,
            [where, depth, height, strata, hard, outside, noiseAmp, noiseSize, seed](const glm::dvec3& q) {
                const Where w = where(q);
                // Soft bands between thin hard ones cut deepest; the cut closes toward its top and its ends.
                const double band = 0.5 - 0.5 * std::cos(2.0 * kVolPi * std::max(w.rel, 0.0) / strata);
                double reach = depth * (hard + (1.0 - hard) * band) * (1.0 + noiseAmp * volFbm(glm::dvec3(w.s, w.rel, 0.0) / noiseSize, seed));
                reach *= volSmoothstep(height, height - strata, w.rel) * volSmoothstep(0.0, 1.5 * depth, w.fromEnd);
                return std::max({w.into - reach, -w.into - outside, -(w.rel + 0.5 * strata), w.rel - height});
            },
            op.subtract);
        if (op.lip > 0.0)
        {
            const double thick = std::max(strata, 100.0), lip = op.lip;
            add(box,
                [where, lip, depth, height, thick](const glm::dvec3& q) {
                    const Where w = where(q);
                    return std::max({-w.into - lip, w.into - 0.5 * depth, (height - 0.25 * thick) - w.rel, w.rel - (height + thick), 0.5 * depth - w.fromEnd});
                },
                false);
        }
        return out;
    }

    if (op.kind == "arch")
    {
        if (op.path.size() < 2)
        {
            message = "an arch needs path=[foot, foot]";
            return out;
        }
        glm::dvec3 a = op.path.front(), b = op.path.back();
        a.y = h.heightAt(a.x, a.z);
        b.y = h.heightAt(b.x, b.z);
        const glm::dvec3 flat(b.x - a.x, 0.0, b.z - a.z);
        const double span = glm::length(flat);
        if (span < voxel)
        {
            message = "an arch's feet are too close";
            return out;
        }
        const glm::dvec3 u = flat / span, w = volHorizontalSide(u);
        const double thick = std::max(op.thickness, voxel), wide = std::max(op.width, voxel), rise = std::max(op.height, thick);
        const double squash = thick / wide;
        auto curveY = [a, b, rise](double t) { return glm::mix(a.y, b.y, t) + rise * std::sin(kVolPi * t); };
        // In a frame along the span: u across it, y up, w squashed so the round tube reads as an elliptical one.
        VolTube tube;
        for (int k = 0; k <= 32; ++k)
        {
            const double t = double(k) / 32.0, sine = std::sin(kVolPi * t);
            tube.p.push_back({t * span, curveY(t) - thick * (1.0 - sine), 0.0});
            tube.r.push_back(thick * (1.0 + 0.6 * (1.0 - sine)));
        }
        auto local = [a, u, w, squash](const glm::dvec3& q) {
            const glm::dvec3 d = q - a;
            return glm::dvec3(d.x * u.x + d.z * u.z, q.y, (d.x * w.x + d.z * w.z) * squash);
        };
        geo::Bounds box;
        for (double t : {0.0, 1.0})
            for (double s : {-1.0, 1.0})
            {
                const glm::dvec3 base = a + u * (t * span) + w * (s * (wide + thick));
                box.extend(base - glm::dvec3(2.0 * thick, 4.0 * thick, 2.0 * thick));
                box.extend(base + glm::dvec3(2.0 * thick, rise + 3.0 * thick, 2.0 * thick));
            }
        box.min -= glm::dvec3(2.0 * voxel + op.blend);
        box.max += glm::dvec3(2.0 * voxel + op.blend);
        if (op.carve)
            add(box,
                [local, span, curveY, thick, wide, squash](const glm::dvec3& q) {
                    const glm::dvec3 l = local(q);
                    const double t = std::clamp(l.x / span, 0.0, 1.0);
                    const double inner = curveY(t) - thick * (1.0 + 0.6 * (1.0 - std::sin(kVolPi * t)));
                    const double ground = glm::mix(curveY(0.0), curveY(1.0), t);
                    return std::max({ground - l.y, l.y - inner, std::abs(l.z / squash) - (0.5 * wide + thick), -l.x + thick, l.x - span + thick});
                },
                true);
        add(box,
            [tube, local, squash, noiseAmp, noiseSize, thick, seed](const glm::dvec3& q) {
                const double d = tube.nearest(local(q)).d * std::min(1.0, 1.0 / squash);
                return noiseAmp > 0.0 ? d + noiseAmp * thick * volFbm(q / noiseSize, seed) : d;
            },
            op.subtract);
        return out;
    }

    if (op.kind == "sphere" || op.kind == "capsule")
    {
        VolTube t;
        if (op.kind == "sphere")
            t.p = {op.center};
        else if (op.path.size() >= 2)
            t.p = op.path;
        else
        {
            message = "a capsule needs a path of two or more points";
            return out;
        }
        t.r.assign(t.p.size(), std::max(op.radius, voxel));
        geo::Bounds box = t.bounds();
        const double pad = std::max(op.radius, voxel) * noiseAmp + op.blend + 2.0 * voxel;
        box.min -= glm::dvec3(pad);
        box.max += glm::dvec3(pad);
        const double r = op.radius;
        add(box,
            [t, noiseAmp, noiseSize, r, seed](const glm::dvec3& q) {
                const double d = t.nearest(q).d;
                return noiseAmp > 0.0 ? d + noiseAmp * r * volFbm(q / noiseSize, seed) : d;
            },
            op.subtract);
        return out;
    }

    if (op.kind == "box")
    {
        const glm::dvec3 half = glm::max(op.size * 0.5, glm::dvec3(voxel * 0.5)), c = op.center;
        geo::Bounds box;
        box.extend(c - half - glm::dvec3(2.0 * voxel + op.blend));
        box.extend(c + half + glm::dvec3(2.0 * voxel + op.blend));
        add(box,
            [c, half](const glm::dvec3& q) {
                const glm::dvec3 d = glm::abs(q - c) - half;
                return glm::length(glm::max(d, glm::dvec3(0.0))) + std::min(std::max({d.x, d.y, d.z}), 0.0);
            },
            op.subtract);
        return out;
    }

    if (op.kind == "mesh")
    {
        auto it = meshes.find(op.object);
        if (it == meshes.end() || !it->second || it->second->numFaces() == 0)
        {
            message = "no mesh for " + (op.object.empty() ? std::string("this stamp") : op.object);
            return out;
        }
        auto sdf = std::make_shared<const gn::Volume>(gn::meshToSdf(*it->second, voxel, 3));
        geo::Bounds box = it->second->bounds();
        box.min -= glm::dvec3(3.0 * voxel + op.blend);
        box.max += glm::dvec3(3.0 * voxel + op.blend);
        add(box, [sdf](const glm::dvec3& q) { return sdf->sample(q); }, op.subtract);
        return out;
    }
    message = "unknown kind " + op.kind;
    return out;
}

// ---------------------------------------------------------------------------------------------------- the field

int volFloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int64_t volPackKey(int a, int b, int c)
{
    constexpr int64_t kOff = int64_t(1) << 20;
    return ((int64_t(a) + kOff) << 42) | ((int64_t(b) + kOff) << 21) | (int64_t(c) + kOff);
}

int64_t volPackKey(const glm::ivec3& v) { return volPackKey(v.x, v.y, v.z); }

/** Column facts from the top down: the first ground crossing, the rock under it before air again, and sign changes. */
struct VolColumn
{
    float top = 0.0f;
    float roof = float(kVolHuge);
    uint16_t changes = 1;
};

/**
 * Voxels over the heightfield: index (i, j, k) sits at x = origin.x + i * voxel, y = (j + 0.5) * voxel, z = origin.z + k *
 * voxel, so every heightfield sample has a voxel column through it. Bricks of 8 cubed hold the field where edits reach;
 * everywhere else it is the ground's: height difference scaled by the normal's up component.
 */
struct VolField
{
    const Heightfield* h = nullptr;
    double voxel = 1.0;
    int per = 1;
    int maxI = 0, maxK = 0;
    std::vector<glm::ivec3> keys;
    std::unordered_map<int64_t, int> brickOf;
    std::vector<float> sdf;
    std::vector<uint8_t> mat;
    std::unordered_map<int64_t, std::array<VolColumn, kVolBrick * kVolBrick>> columns;

    double x(int i) const { return h->origin.x + double(i) * voxel; }
    double y(int j) const { return (double(j) + 0.5) * voxel; }
    double z(int k) const { return h->origin.y + double(k) * voxel; }
    glm::dvec3 world(const glm::ivec3& v) const { return {x(v.x), y(v.y), z(v.z)}; }
    int jOf(double wy) const { return int(std::floor(wy / voxel - 0.5)); }

    float ground(int i, int j, int k) const
    {
        const double wx = x(i), wz = z(k);
        return float((y(j) - h->heightAt(wx, wz)) * h->normalAt(wx, wz).y);
    }
    int brick(const glm::ivec3& v) const
    {
        auto it = brickOf.find(volPackKey(volFloorDiv(v.x, kVolBrick), volFloorDiv(v.y, kVolBrick), volFloorDiv(v.z, kVolBrick)));
        return it == brickOf.end() ? -1 : it->second;
    }
    static size_t local(const glm::ivec3& v)
    {
        const int a = v.x & (kVolBrick - 1), b = v.y & (kVolBrick - 1), c = v.z & (kVolBrick - 1);
        return size_t(a) + kVolBrick * (size_t(b) + kVolBrick * size_t(c));
    }
    float value(const glm::ivec3& v) const
    {
        const int b = brick(v);
        return b < 0 ? ground(v.x, v.y, v.z) : sdf[size_t(b) * 512 + local(v)];
    }
    uint8_t material(const glm::ivec3& v) const
    {
        const int b = brick(v);
        return b < 0 ? 0 : mat[size_t(b) * 512 + local(v)];
    }
    const VolColumn* column(int i, int k) const
    {
        auto it = columns.find(volPackKey(volFloorDiv(i, kVolBrick), 0, volFloorDiv(k, kVolBrick)));
        return it == columns.end() ? nullptr : &it->second[size_t(i & (kVolBrick - 1)) + kVolBrick * size_t(k & (kVolBrick - 1))];
    }
};

void volEvaluateBricks(VolField& f, const std::vector<VolShape>& shapes)
{
    LILA_ZONE();

    f.sdf.assign(f.keys.size() * 512, 0.0f);
    f.mat.assign(f.keys.size() * 512, 0);
    tbb::parallel_for(size_t(0), f.keys.size(), [&](size_t b) {
        const glm::ivec3 base = f.keys[b] * kVolBrick;
        geo::Bounds bb;
        bb.extend(f.world(base));
        bb.extend(f.world(base + glm::ivec3(kVolBrick - 1)));
        std::vector<const VolShape*> here;
        for (const VolShape& s : shapes)
            if (s.box.min.x <= bb.max.x && s.box.max.x >= bb.min.x && s.box.min.y <= bb.max.y && s.box.max.y >= bb.min.y && s.box.min.z <= bb.max.z &&
                s.box.max.z >= bb.min.z)
                here.push_back(&s);
        for (int c = 0; c < kVolBrick; ++c)
            for (int a = 0; a < kVolBrick; ++a)
            {
                const int i = base.x + a, k = base.z + c;
                const double wx = f.x(i), wz = f.z(k);
                const double gh = f.h->heightAt(wx, wz), ny = f.h->normalAt(wx, wz).y;
                for (int bj = 0; bj < kVolBrick; ++bj)
                {
                    const glm::dvec3 p(wx, f.y(base.y + bj), wz);
                    double d = (p.y - gh) * ny;
                    uint8_t m = 0;
                    for (const VolShape* s : here)
                    {
                        if (p.x < s->box.min.x || p.x > s->box.max.x || p.y < s->box.min.y || p.y > s->box.max.y || p.z < s->box.min.z || p.z > s->box.max.z)
                            continue;
                        const double v = s->sdf(p);
                        if (s->subtract)
                        {
                            if (-v > d)
                                m = s->material;
                            d = volSmax(d, -v, s->blend);
                        }
                        else
                        {
                            if (v < d)
                                m = s->material;
                            d = volSmin(d, v, s->blend);
                        }
                    }
                    const size_t at = b * 512 + size_t(a) + kVolBrick * (size_t(bj) + kVolBrick * size_t(c));
                    f.sdf[at] = float(d);
                    f.mat[at] = m;
                }
            }
    });
}

void volScanColumns(VolField& f)
{
    LILA_ZONE();

    std::map<int64_t, std::pair<int, int>> spans;
    std::map<int64_t, glm::ivec2> at;
    for (const glm::ivec3& k : f.keys)
    {
        const int64_t c = volPackKey(k.x, 0, k.z);
        auto it = spans.find(c);
        if (it == spans.end())
        {
            spans[c] = {k.y, k.y};
            at[c] = {k.x, k.z};
        }
        else
        {
            it->second.first = std::min(it->second.first, k.y);
            it->second.second = std::max(it->second.second, k.y);
        }
    }
    std::vector<int64_t> list;
    for (const auto& [c, s] : spans)
    {
        list.push_back(c);
        f.columns[c];
    }
    tbb::parallel_for(size_t(0), list.size(), [&](size_t n) {
        const int64_t c = list[n];
        const auto [lo, hi] = spans.at(c);
        const glm::ivec2 bc = at.at(c);
        auto& cols = f.columns.at(c);
        for (int lk = 0; lk < kVolBrick; ++lk)
            for (int li = 0; li < kVolBrick; ++li)
            {
                const int i = bc.x * kVolBrick + li, k = bc.y * kVolBrick + lk;
                const double gh = f.h->heightAt(f.x(i), f.z(k));
                const int top = std::max(hi * kVolBrick + kVolBrick, f.jOf(gh) + 2), bottom = std::min(lo * kVolBrick - 1, f.jOf(gh) - 2);
                VolColumn col;
                col.changes = 0;
                bool foundTop = false, roofDone = false;
                float prev = f.value({i, top, k});
                for (int j = top - 1; j >= bottom; --j)
                {
                    const float v = f.value({i, j, k});
                    if ((prev < 0.0f) != (v < 0.0f))
                    {
                        const double yc = f.y(j + 1) - double(prev) / (double(prev) - double(v)) * f.voxel;
                        ++col.changes;
                        if (!foundTop && v < 0.0f)
                        {
                            col.top = float(yc);
                            foundTop = true;
                        }
                        else if (foundTop && !roofDone && v >= 0.0f)
                        {
                            col.roof = float(double(col.top) - yc);
                            roofDone = true;
                        }
                    }
                    prev = v;
                }
                if (!foundTop)
                    col.top = float(gh);
                cols[size_t(li) + kVolBrick * size_t(lk)] = col;
            }
    });
}

// ---------------------------------------------------------------------------------------------------- the split

/** Where a split mesh vertex comes from, so the same point made twice is one vertex. */
struct VolVKey
{
    uint8_t kind = 0; // 0 surface nets point, 1 edge on a grid line, 2 grid corner in a triangle, 3 skirt under a vertex
    int a = 0, b = 0, c = 0, d = 0;
    auto operator<=>(const VolVKey&) const = default;
};

struct VolVert
{
    glm::dvec3 p;
    bool snapped = false;
    /** On ground the volume left alone: placed on the landscape's surface, with its normal. */
    bool ground = false;
    /** Grid segments on the hole outline it sits on: (axis, line, cell) packed; -1 when none. */
    int64_t seg[4] = {-1, -1, -1, -1};
};

struct VolPoly
{
    std::vector<int> v;
    uint8_t material = 0;
    bool skirt = false;
    bool top = false;
};

/** Surface nets of the whole field at a stride, as global points and quads. */
struct VolSurface
{
    std::vector<glm::dvec3> pos;
    std::vector<std::array<int, 4>> quads;
    std::vector<uint8_t> material;
};

VolSurface volSurfaceAt(const VolField& f, const std::vector<glm::ivec3>& coarseBricks, int stride)
{
    LILA_ZONE();

    struct Part
    {
        std::vector<glm::ivec3> cells;
        std::vector<glm::dvec3> offsets;
        std::vector<int> quads;
        std::vector<uint8_t> mats;
    };
    std::vector<Part> parts(coarseBricks.size());
    tbb::parallel_for(size_t(0), coarseBricks.size(), [&](size_t b) {
        const glm::ivec3 base = coarseBricks[b] * kVolBrick - 1;
        constexpr int n = kVolBrick + 2;
        std::vector<float> values(size_t(n) * n * n);
        for (int c = 0; c < n; ++c)
            for (int bj = 0; bj < n; ++bj)
                for (int a = 0; a < n; ++a)
                    values[size_t(a) + n * (size_t(bj) + n * size_t(c))] = f.value((base + glm::ivec3(a, bj, c)) * stride);
        geo::SurfaceNetsOptions o;
        o.faceMin = glm::ivec3(1);
        o.faceMax = glm::ivec3(kVolBrick + 1);
        const geo::SurfaceNets nets = geo::surfaceNets({glm::ivec3(n), values}, o);
        Part& p = parts[b];
        for (size_t k = 0; k < nets.cells.size(); ++k)
        {
            p.cells.push_back(nets.cells[k] + base);
            p.offsets.push_back(nets.offsets[k]);
        }
        p.quads.assign(nets.quads.begin(), nets.quads.end());
        for (const glm::ivec4& e : nets.edges)
        {
            glm::ivec3 lo = glm::ivec3(e) + base, hi = lo;
            hi[e.w] += 1;
            const glm::ivec3 solidEnd = f.value(lo * stride) < 0.0f ? lo : hi;
            p.mats.push_back(f.material(solidEnd * stride));
        }
    });
    VolSurface s;
    std::unordered_map<int64_t, int> index;
    const double sv = f.voxel * stride;
    for (const Part& p : parts)
    {
        std::vector<int> local(p.cells.size());
        for (size_t k = 0; k < p.cells.size(); ++k)
        {
            auto [it, fresh] = index.try_emplace(volPackKey(p.cells[k]), int(s.pos.size()));
            if (fresh)
            {
                const glm::dvec3 c = glm::dvec3(p.cells[k]) + p.offsets[k];
                s.pos.push_back({f.h->origin.x + c.x * sv, (c.y * stride + 0.5) * f.voxel, f.h->origin.y + c.z * sv});
            }
            local[k] = it->second;
        }
        for (size_t q = 0; q * 4 + 3 < p.quads.size(); ++q)
        {
            s.quads.push_back({local[size_t(p.quads[q * 4])], local[size_t(p.quads[q * 4 + 1])], local[size_t(p.quads[q * 4 + 2])], local[size_t(p.quads[q * 4 + 3])]});
            s.material.push_back(p.mats[q]);
        }
    }
    return s;
}

/** Everything the split decides on the heightfield's grid, shared by every level of detail. */
struct VolPlan
{
    const Heightfield* h = nullptr;
    /** The heights after the split (the landscape), for snapping and the top-surface test. */
    const Heightfield* land = nullptr;
    int cx = 0, cz = 0;
    std::vector<uint8_t> hole;     // per cell
    std::vector<uint8_t> nrCell;   // per cell: a column the heightfield can't hold lies in or next to it
    double roofHalf = 0.0;
    double skirt = 0.0;

    double lineX(int i) const { return h->origin.x + double(i) * h->cell; }
    double lineZ(int j) const { return h->origin.y + double(j) * h->cell; }
    bool holeAt(int x, int z) const { return x >= 0 && z >= 0 && x < cx && z < cz && hole[size_t(z) * cx + x]; }
    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < cx && z < cz; }
    int cellX(double wx) const { return std::clamp(int(std::floor((wx - h->origin.x) / h->cell)), 0, cx - 1); }
    int cellZ(double wz) const { return std::clamp(int(std::floor((wz - h->origin.y) / h->cell)), 0, cz - 1); }
    /** The segment of grid line x = i between rows j and j + 1 is on the outline: a hole on exactly one side. */
    bool outlineX(int i, int j) const { return inside(i - 1, j) && inside(i, j) && holeAt(i - 1, j) != holeAt(i, j); }
    bool outlineZ(int i, int j) const { return inside(i, j - 1) && inside(i, j) && holeAt(i, j - 1) != holeAt(i, j); }
};

struct VolMesher
{
    const VolPlan& plan;
    const VolSurface& surf;
    std::map<VolVKey, int> ids;
    std::vector<VolVert> verts;
    std::vector<VolPoly> polys;
    std::map<std::pair<int, int>, std::vector<std::pair<double, int>>> crossings;

    int vertex(const VolVKey& k, const glm::dvec3& p)
    {
        auto [it, fresh] = ids.try_emplace(k, int(verts.size()));
        if (fresh)
            verts.push_back({p});
        return it->second;
    }

    /** Where the original edge a-b meets grid line `line` of axis (0: x = X, 1: z = Z), computed the same from either side. */
    int edgeCross(int a, int b, int axis, int line)
    {
        if (a > b)
            std::swap(a, b);
        const glm::dvec3 pa = surf.pos[size_t(a)], pb = surf.pos[size_t(b)];
        const double L = axis == 0 ? plan.lineX(line) : plan.lineZ(line);
        const double ca = axis == 0 ? pa.x : pa.z, cb = axis == 0 ? pb.x : pb.z;
        glm::dvec3 p = pa + (pb - pa) * ((L - ca) / (cb - ca));
        (axis == 0 ? p.x : p.z) = L;
        return vertex({1, a, b, axis, line}, p);
    }

    /** All grid line crossings strictly inside edge a-b, ordered from a to b. */
    std::vector<int> crossingsOf(int a, int b)
    {
        const bool flip = a > b;
        const std::pair<int, int> key = flip ? std::pair(b, a) : std::pair(a, b);
        auto it = crossings.find(key);
        if (it == crossings.end())
        {
            std::vector<std::pair<double, int>> list;
            const glm::dvec3 pa = surf.pos[size_t(key.first)], pb = surf.pos[size_t(key.second)];
            for (int axis = 0; axis < 2; ++axis)
            {
                const double ca = axis == 0 ? pa.x : pa.z, cb = axis == 0 ? pb.x : pb.z;
                const double org = axis == 0 ? plan.h->origin.x : plan.h->origin.y;
                const int n = axis == 0 ? plan.cx : plan.cz;
                const double lo = std::min(ca, cb), hi = std::max(ca, cb);
                for (int line = std::max(0, int(std::floor((lo - org) / plan.h->cell))); line <= std::min(n, int(std::ceil((hi - org) / plan.h->cell))); ++line)
                {
                    const double L = axis == 0 ? plan.lineX(line) : plan.lineZ(line);
                    if (L > lo && L < hi)
                        list.push_back({(L - ca) / (cb - ca), edgeCross(key.first, key.second, axis, line)});
                }
            }
            std::sort(list.begin(), list.end());
            it = crossings.emplace(key, std::move(list)).first;
        }
        std::vector<int> out;
        for (const auto& [t, v] : it->second)
            out.push_back(v);
        if (flip)
            std::reverse(out.begin(), out.end());
        return out;
    }

    /** A face kept whole: every grid crossing on its edges goes in, so faces cut beside it share them. */
    void wholeFace(const std::vector<int>& sn, uint8_t material, bool top)
    {
        VolPoly poly;
        poly.material = material;
        poly.top = top;
        for (size_t k = 0; k < sn.size(); ++k)
        {
            const int a = sn[k], b = sn[(k + 1) % sn.size()];
            poly.v.push_back(vertex({0, a, 0, 0, 0}, surf.pos[size_t(a)]));
            for (int c : crossingsOf(a, b))
                poly.v.push_back(c);
        }
        polys.push_back(std::move(poly));
    }

    struct PV
    {
        glm::dvec3 p;
        int id;
        int sn;        // its surface nets point, or -1
        int e0, e1;    // the original edge it lies on, or -1
        int lx, lz;    // the grid lines it lies on, or -1
    };

    /** The top triangle a, b, c cut to one hole cell. */
    void clipToCell(const std::array<int, 3>& tri, int triId, int cxi, int czi, uint8_t material)
    {
        std::vector<PV> poly;
        for (int k = 0; k < 3; ++k)
            poly.push_back({surf.pos[size_t(tri[k])], -1, tri[k], -1, -1, -1, -1});
        const glm::dvec3 pa = surf.pos[size_t(tri[0])], pb = surf.pos[size_t(tri[1])], pc = surf.pos[size_t(tri[2])];
        auto planeY = [&](double x, double z) {
            const double det = (pb.z - pc.z) * (pa.x - pc.x) + (pc.x - pb.x) * (pa.z - pc.z);
            if (std::abs(det) < 1e-18)
                return (pa.y + pb.y + pc.y) / 3.0;
            const double l1 = ((pb.z - pc.z) * (x - pc.x) + (pc.x - pb.x) * (z - pc.z)) / det;
            const double l2 = ((pc.z - pa.z) * (x - pc.x) + (pa.x - pc.x) * (z - pc.z)) / det;
            return l1 * pa.y + l2 * pb.y + (1.0 - l1 - l2) * pc.y;
        };
        auto edgeOf = [](const PV& u, const PV& v) -> std::pair<int, int> {
            auto ends = [](const PV& w) { return w.sn >= 0 ? std::pair(w.sn, -1) : std::pair(w.e0, w.e1); };
            const auto [u0, u1] = ends(u);
            const auto [v0, v1] = ends(v);
            if (u1 < 0 && v1 < 0 && u0 >= 0 && v0 >= 0)
                return {u0, v0};
            if (u1 < 0 && u0 >= 0 && v1 >= 0 && (v0 == u0 || v1 == u0))
                return {v0, v1};
            if (v1 < 0 && v0 >= 0 && u1 >= 0 && (u0 == v0 || u1 == v0))
                return {u0, u1};
            if (u1 >= 0 && std::minmax(u0, u1) == std::minmax(v0, v1))
                return {u0, u1};
            return {-1, -1};
        };
        auto clip = [&](int axis, int line, bool keepAbove) {
            const double L = axis == 0 ? plan.lineX(line) : plan.lineZ(line);
            auto side = [&](const PV& w) {
                const double c = axis == 0 ? w.p.x : w.p.z;
                return c == L ? 0 : ((c > L) == keepAbove ? 1 : -1);
            };
            std::vector<PV> out;
            for (size_t k = 0; k < poly.size(); ++k)
            {
                const PV& u = poly[k];
                const PV& v = poly[(k + 1) % poly.size()];
                const int su = side(u), sv = side(v);
                if (su >= 0)
                    out.push_back(u);
                if ((su > 0 && sv < 0) || (su < 0 && sv > 0))
                {
                    const auto [e0, e1] = edgeOf(u, v);
                    PV x{};
                    x.sn = -1;
                    x.e0 = e0;
                    x.e1 = e1;
                    x.lx = axis == 0 ? line : (u.lx >= 0 && u.lx == v.lx ? u.lx : -1);
                    x.lz = axis == 1 ? line : (u.lz >= 0 && u.lz == v.lz ? u.lz : -1);
                    if (e0 >= 0)
                    {
                        x.id = edgeCross(e0, e1, axis, line);
                        x.p = verts[size_t(x.id)].p;
                    }
                    else if ((axis == 0 ? x.lz : x.lx) >= 0)
                    {
                        // Along a grid line already cut: the crossing is a grid corner inside the triangle.
                        const int i = axis == 0 ? line : x.lx, j = axis == 1 ? line : x.lz;
                        const glm::dvec3 p(plan.lineX(i), planeY(plan.lineX(i), plan.lineZ(j)), plan.lineZ(j));
                        x.id = vertex({2, i, j, triId, 0}, p);
                        x.p = p;
                    }
                    else
                    {
                        const double cu = axis == 0 ? u.p.x : u.p.z, cv = axis == 0 ? v.p.x : v.p.z;
                        glm::dvec3 p = u.p + (v.p - u.p) * ((L - cu) / (cv - cu));
                        (axis == 0 ? p.x : p.z) = L;
                        x.id = vertex({4, triId, axis, line, int(verts.size())}, p);
                        x.p = p;
                    }
                    out.push_back(x);
                }
            }
            for (PV& w : out)
                if (side(w) == 0)
                    (axis == 0 ? w.lx : w.lz) = line;
            poly = std::move(out);
        };
        clip(0, cxi, true);
        if (poly.size() >= 3)
            clip(0, cxi + 1, false);
        if (poly.size() >= 3)
            clip(1, czi, true);
        if (poly.size() >= 3)
            clip(1, czi + 1, false);
        if (poly.size() < 3)
            return;
        VolPoly out;
        out.material = material;
        out.top = true;
        for (const PV& w : poly)
        {
            const int id = w.sn >= 0 ? vertex({0, w.sn, 0, 0, 0}, w.p) : w.id;
            if (out.v.empty() || out.v.back() != id)
                out.v.push_back(id);
        }
        while (out.v.size() > 1 && out.v.front() == out.v.back())
            out.v.pop_back();
        if (out.v.size() >= 3)
            polys.push_back(std::move(out));
    }

    void topTriangle(const std::array<int, 3>& tri, int triId, uint8_t material)
    {
        double lo[2] = {kVolHuge, kVolHuge}, hi[2] = {-kVolHuge, -kVolHuge};
        for (int v : tri)
        {
            const glm::dvec3& p = surf.pos[size_t(v)];
            lo[0] = std::min(lo[0], p.x);
            hi[0] = std::max(hi[0], p.x);
            lo[1] = std::min(lo[1], p.z);
            hi[1] = std::max(hi[1], p.z);
        }
        const int x0 = plan.cellX(lo[0]), x1 = plan.cellX(hi[0]), z0 = plan.cellZ(lo[1]), z1 = plan.cellZ(hi[1]);
        int holes = 0, cells = 0;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                ++cells;
                holes += plan.holeAt(x, z) ? 1 : 0;
            }
        if (holes == 0)
            return;
        if (holes == cells)
        {
            wholeFace({tri[0], tri[1], tri[2]}, material, true);
            return;
        }
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                if (plan.holeAt(x, z))
                    clipToCell(tri, triId, x, z, material);
    }

    /** Puts outline vertices on the ground exactly, and remembers which outline segments each lies on. */
    void snap()
    {
        const Heightfield& land = *plan.land;
        auto heightOn = [&](int axis, int line, double along) {
            // Linear along the grid line between its two samples, as the landscape draws that edge.
            const double org = axis == 0 ? land.origin.y : land.origin.x;
            const int n = axis == 0 ? land.nz : land.nx;
            const double f = (along - org) / land.cell;
            const int s = std::clamp(int(std::floor(f)), 0, n - 2);
            const double t = std::clamp(f - s, 0.0, 1.0);
            const double a = axis == 0 ? land.at(line, s) : land.at(s, line), b = axis == 0 ? land.at(line, s + 1) : land.at(s + 1, line);
            return a + (b - a) * t;
        };
        for (VolVert& v : verts)
        {
            const double fx = (v.p.x - land.origin.x) / land.cell, fz = (v.p.z - land.origin.y) / land.cell;
            const int ix = int(std::lround(fx)), iz = int(std::lround(fz));
            const bool onX = ix >= 0 && ix < land.nx && v.p.x == plan.lineX(ix);
            const bool onZ = iz >= 0 && iz < land.nz && v.p.z == plan.lineZ(iz);
            int n = 0;
            auto addSeg = [&](int64_t key) {
                if (n < 4 && std::find(v.seg, v.seg + n, key) == v.seg + n)
                    v.seg[n++] = key;
            };
            if (onX)
            {
                const int j = onZ ? iz : int(std::floor(fz));
                for (int jj : {j, onZ ? iz - 1 : j})
                    if (jj >= 0 && jj < plan.cz && plan.outlineX(ix, jj))
                        addSeg(volPackKey(0, ix, jj));
            }
            if (onZ)
            {
                const int i = onX ? ix : int(std::floor(fx));
                for (int ii : {i, onX ? ix - 1 : i})
                    if (ii >= 0 && ii < plan.cx && plan.outlineZ(ii, iz))
                        addSeg(volPackKey(1, iz, ii));
            }
            if (n == 0)
                continue;
            v.snapped = true;
            v.p.y = onX && onZ ? double(land.at(ix, iz)) : onX ? heightOn(0, ix, v.p.z) : heightOn(1, iz, v.p.x);
        }
    }

    /** Edges on the outline with one face get a skirt down into the ground. */
    void skirts()
    {
        std::map<std::pair<int, int>, int> uses;
        for (const VolPoly& p : polys)
            for (size_t k = 0; k < p.v.size(); ++k)
                ++uses[std::minmax(p.v[k], p.v[(k + 1) % p.v.size()])];
        auto sameSeg = [&](const VolVert& a, const VolVert& b) {
            for (int64_t s : a.seg)
                if (s >= 0 && std::find(std::begin(b.seg), std::end(b.seg), s) != std::end(b.seg))
                    return true;
            return false;
        };
        const size_t count = polys.size();
        for (size_t f = 0; f < count; ++f)
        {
            if (!polys[f].top)
                continue;
            const std::vector<int> ring = polys[f].v;
            const uint8_t material = polys[f].material;
            for (size_t k = 0; k < ring.size(); ++k)
            {
                const int u = ring[k], w = ring[(k + 1) % ring.size()];
                if (uses[std::minmax(u, w)] != 1 || !verts[size_t(u)].snapped || !verts[size_t(w)].snapped || !sameSeg(verts[size_t(u)], verts[size_t(w)]))
                    continue;
                const int ud = vertex({3, u, 0, 0, 0}, verts[size_t(u)].p - glm::dvec3(0.0, plan.skirt, 0.0));
                const int wd = vertex({3, w, 0, 0, 0}, verts[size_t(w)].p - glm::dvec3(0.0, plan.skirt, 0.0));
                VolPoly s;
                s.v = {w, u, ud, wd};
                s.material = material;
                s.skirt = true;
                polys.push_back(std::move(s));
            }
        }
    }
};

/** Surface nets quads become kept faces: top ones cut to the holes, the rest kept where a column the heightfield can't hold is near. */
VolMesher volMeshLevel(const VolPlan& plan, const VolSurface& surf, const std::function<bool(const glm::dvec3&)>& nearNr,
                       const std::function<bool(const glm::dvec3&)>& groundLike)
{
    LILA_ZONE();

    VolMesher m{plan, surf};
    std::vector<uint8_t> isTop(surf.quads.size(), 0);
    for (size_t q = 0; q < surf.quads.size(); ++q)
    {
        glm::dvec3 c(0.0);
        for (int v : surf.quads[q])
            c += surf.pos[size_t(v)] * 0.25;
        // Inside a cave (deep under the top, a column the heightfield can't hold nearby) a face is kept whole; anything
        // else is ground, cut to the holes.
        isTop[q] = c.y >= plan.land->heightAt(c.x, c.z) - plan.roofHalf || !nearNr(c);
    }
    // Top faces first, so every cut edge's crossings are known before the faces kept whole next to them.
    for (size_t q = 0; q < surf.quads.size(); ++q)
        if (isTop[q] == 1)
        {
            const auto& v = surf.quads[q];
            m.topTriangle({v[0], v[1], v[2]}, int(q * 2), surf.material[q]);
            m.topTriangle({v[0], v[2], v[3]}, int(q * 2 + 1), surf.material[q]);
        }
    for (size_t q = 0; q < surf.quads.size(); ++q)
        if (isTop[q] == 0)
        {
            const auto& v = surf.quads[q];
            m.wholeFace({v[0], v[1], v[2], v[3]}, surf.material[q], false);
        }
    std::vector<uint8_t> onTop(m.verts.size(), 0);
    for (const VolPoly& p : m.polys)
        if (p.top)
            for (int v : p.v)
                onTop[size_t(v)] = 1;
    for (size_t v = 0; v < m.verts.size(); ++v)
        if (onTop[v] && groundLike(m.verts[v].p))
        {
            m.verts[v].p.y = plan.land->heightAt(m.verts[v].p.x, m.verts[v].p.z);
            m.verts[v].ground = true;
        }
    m.snap();
    return m;
}

int volFindRoot(std::vector<int>& parent, int x)
{
    while (parent[size_t(x)] != x)
    {
        parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
        x = parent[size_t(x)];
    }
    return x;
}

/** The polygons' connected parts, numbered by their first polygon. */
std::vector<int> volComponents(const VolMesher& m)
{
    std::vector<int> parent(m.verts.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (const VolPoly& p : m.polys)
        for (size_t k = 1; k < p.v.size(); ++k)
        {
            const int a = volFindRoot(parent, p.v[0]), b = volFindRoot(parent, p.v[k]);
            if (a != b)
                parent[size_t(std::max(a, b))] = std::min(a, b);
        }
    std::map<int, int> number;
    std::vector<int> out(m.polys.size());
    for (size_t f = 0; f < m.polys.size(); ++f)
    {
        const int r = volFindRoot(parent, m.polys[f].v[0]);
        out[f] = number.try_emplace(r, int(number.size())).first->second;
    }
    return out;
}

glm::dvec3 volNewellNormal(const VolMesher& m, const VolPoly& p)
{
    glm::dvec3 n(0.0);
    for (size_t k = 0; k < p.v.size(); ++k)
    {
        const glm::dvec3& a = m.verts[size_t(p.v[k])].p;
        const glm::dvec3& b = m.verts[size_t(p.v[(k + 1) % p.v.size()])].p;
        n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

/** The chosen polygons as a mesh: materials, triplanar uvs, and normals that follow the ground on the seam. */
std::shared_ptr<const geo::Mesh> volBuildMesh(const VolMesher& m, const std::vector<size_t>& faces, const std::vector<glm::dvec3>& smooth, const Heightfield& land,
                                           const std::vector<std::string>& materialNames, double uvScale)
{
    geo::Mesh out;
    std::unordered_map<int, geo::Index> local;
    std::vector<geo::Index> made;
    std::vector<size_t> source;
    for (size_t f : faces)
    {
        const VolPoly& p = m.polys[f];
        std::vector<geo::Index> pts;
        for (int v : p.v)
        {
            auto [it, fresh] = local.try_emplace(v, geo::kInvalid);
            if (fresh)
                it->second = out.addPoint(m.verts[size_t(v)].p);
            pts.push_back(it->second);
        }
        const geo::Index id = out.addFace(pts);
        if (id == geo::kInvalid)
            continue;
        made.push_back(id);
        source.push_back(f);
    }
    out.materialNames() = materialNames;
    std::span<int32_t> mat = out.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (size_t k = 0; k < made.size(); ++k)
        mat[made[k]] = m.polys[source[k]].material;
    std::span<glm::vec3> nrm = out.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
    std::span<glm::vec2> uv = out.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    std::unordered_map<geo::Index, int> backTo;
    for (const auto& [v, p] : local)
        backTo[p] = v;
    for (size_t k = 0; k < made.size(); ++k)
    {
        const VolPoly& p = m.polys[source[k]];
        const glm::dvec3 fn = glm::normalize(volNewellNormal(m, p) + glm::dvec3(0.0, 1e-30, 0.0));
        const glm::dvec3 an = glm::abs(fn);
        for (geo::Index c : out.faceCorners(made[k]))
        {
            const geo::Index pt = out.cornerPoint(c);
            const int v = backTo[pt];
            const glm::dvec3 pos = out.position(pt);
            glm::dvec3 n = smooth[size_t(v)];
            if (p.skirt)
                n = fn;
            else if (m.verts[size_t(v)].snapped || m.verts[size_t(v)].ground)
                n = land.normalAt(pos.x, pos.z);
            nrm[c] = glm::vec3(n);
            glm::dvec2 t = an.y >= an.x && an.y >= an.z ? glm::dvec2(pos.x, pos.z) : an.x >= an.z ? glm::dvec2(pos.z, pos.y) : glm::dvec2(pos.x, pos.y);
            uv[c] = glm::vec2(t / uvScale);
        }
    }
    out.removeAllLoose();
    out.compact();
    return std::make_shared<const geo::Mesh>(std::move(out));
}

std::vector<glm::dvec3> volSmoothNormals(const VolMesher& m)
{
    std::vector<glm::dvec3> n(m.verts.size(), glm::dvec3(0.0));
    for (const VolPoly& p : m.polys)
    {
        if (p.skirt)
            continue;
        const glm::dvec3 fn = volNewellNormal(m, p);
        for (int v : p.v)
            n[size_t(v)] += fn;
    }
    for (glm::dvec3& v : n)
    {
        const double l = glm::length(v);
        v = l > 0.0 ? v / l : glm::dvec3(0.0, 1.0, 0.0);
    }
    return n;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- ops and settings

const std::vector<std::string>& volumeKinds()
{
    static const std::vector<std::string> k{"cave", "tunnel", "overhang", "arch", "sphere", "box", "capsule", "mesh"};
    return k;
}

namespace
{
json volPointsJson(const std::vector<glm::dvec3>& pts)
{
    json a = json::array();
    for (const glm::dvec3& p : pts)
        a.push_back({p.x, p.y, p.z});
    return a;
}

std::vector<glm::dvec3> volPointsFrom(const json& j)
{
    std::vector<glm::dvec3> out;
    if (j.is_array())
        for (const json& p : j)
            if (p.is_array() && p.size() == 3)
                out.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
    return out;
}

glm::dvec3 volVecFrom(const json& j, glm::dvec3 fallback)
{
    return j.is_array() && j.size() == 3 ? glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : fallback;
}
} // namespace

json VolumeOp::toJson() const
{
    json j = {{"id", id},         {"kind", kind},       {"mode", subtract ? "subtract" : "add"}, {"material", material},  {"blend", blend},
              {"seed", seed},     {"radius", radius},   {"noise", noise},                        {"noiseSize", noiseSize}};
    if (!path.empty())
        j["path"] = volPointsJson(path);
    if (kind == "cave")
    {
        j["branches"] = branches;
        j["branchLength"] = branchLength;
        j["chambers"] = chambers;
        j["chamberRadius"] = chamberRadius;
    }
    if (kind == "overhang")
    {
        j["depth"] = depth;
        j["height"] = height;
        j["strata"] = strata;
        j["hardness"] = hardness;
        j["lip"] = lip;
    }
    if (kind == "arch")
    {
        j["height"] = height;
        j["thickness"] = thickness;
        j["width"] = width;
        j["carve"] = carve;
    }
    if (kind == "sphere" || kind == "box")
    {
        j["center"] = {center.x, center.y, center.z};
        j["size"] = {size.x, size.y, size.z};
    }
    if (kind == "mesh")
        j["object"] = object;
    return j;
}

VolumeOp VolumeOp::fromJson(const json& j)
{
    VolumeOp o;
    o.id = j.value("id", std::string());
    o.kind = j.value("kind", std::string("cave"));
    if (std::find(volumeKinds().begin(), volumeKinds().end(), o.kind) == volumeKinds().end())
        throw std::invalid_argument("unknown volume kind '" + o.kind + "'");
    const std::string mode = j.value("mode", std::string("subtract"));
    if (mode != "add" && mode != "subtract")
        throw std::invalid_argument("mode is add or subtract, not '" + mode + "'");
    o.subtract = mode == "subtract";
    o.material = j.value("material", std::string("rock"));
    o.blend = j.value("blend", 0.0);
    o.seed = j.value("seed", 0u);
    o.path = volPointsFrom(j.value("path", json::array()));
    o.radius = j.value("radius", o.radius);
    o.noise = j.value("noise", o.noise);
    o.noiseSize = j.value("noiseSize", o.noiseSize);
    o.branches = j.value("branches", 0);
    o.branchLength = j.value("branchLength", o.branchLength);
    o.chambers = j.value("chambers", 0);
    o.chamberRadius = j.value("chamberRadius", o.chamberRadius);
    o.depth = j.value("depth", o.depth);
    o.height = j.value("height", o.height);
    o.strata = j.value("strata", o.strata);
    o.hardness = j.value("hardness", o.hardness);
    o.lip = j.value("lip", 0.0);
    o.thickness = j.value("thickness", o.thickness);
    o.width = j.value("width", o.width);
    o.carve = j.value("carve", true);
    o.center = volVecFrom(j.value("center", json()), glm::dvec3(0.0));
    o.size = volVecFrom(j.value("size", json()), glm::dvec3(100.0));
    o.object = j.value("object", std::string());
    return o;
}

json VolumeSettings::toJson() const
{
    return {{"split", split}, {"voxelSize", voxelSize}, {"minRoof", minRoof}, {"maxHeightError", maxHeightError}, {"margin", margin}, {"skirt", skirt},
            {"lods", lods},   {"chunkSize", chunkSize}, {"uvScale", uvScale}, {"maxVoxels", maxVoxels}};
}

VolumeSettings VolumeSettings::fromJson(const json& j)
{
    VolumeSettings s;
    if (!j.is_object())
        return s;
    s.split = j.value("split", s.split);
    s.voxelSize = std::max(0.0, j.value("voxelSize", s.voxelSize));
    s.minRoof = std::max(0.0, j.value("minRoof", s.minRoof));
    s.maxHeightError = std::max(0.0, j.value("maxHeightError", s.maxHeightError));
    s.margin = std::clamp(j.value("margin", s.margin), 0, 16);
    s.skirt = std::max(0.0, j.value("skirt", s.skirt));
    s.lods = std::clamp(j.value("lods", s.lods), 1, 5);
    s.chunkSize = std::max(0.0, j.value("chunkSize", s.chunkSize));
    s.uvScale = std::max(1e-3, j.value("uvScale", s.uvScale));
    s.maxVoxels = std::max(1e5, j.value("maxVoxels", s.maxVoxels));
    return s;
}

// ---------------------------------------------------------------------------------------------------- build

std::vector<uint8_t> holeCells(const Heightfield& h)
{
    std::vector<uint8_t> out(size_t(std::max(0, h.nx - 1)) * size_t(std::max(0, h.nz - 1)), 0);
    const std::vector<float>* m = h.findMask("holes");
    if (!m || m->size() != h.size())
        return out;
    for (int z = 0; z + 1 < h.nz; ++z)
        for (int x = 0; x + 1 < h.nx; ++x)
            out[size_t(z) * (h.nx - 1) + x] = (*m)[h.index(x, z)] > 0.5f || (*m)[h.index(x + 1, z)] > 0.5f || (*m)[h.index(x, z + 1)] > 0.5f ||
                                              (*m)[h.index(x + 1, z + 1)] > 0.5f;
    return out;
}

VolumeResult buildVolume(Heightfield& h, const std::vector<VolumeOp>& ops, const VolumeSettings& s,
                         const std::map<std::string, std::shared_ptr<const geo::Mesh>>& meshes)
{
    LILA_ZONE();

    VolumeResult r;
    r.settings = s;
    if (h.empty())
        return r;
    const Heightfield before = h;

    // Voxel: a whole fraction of the cell, about a third of the smallest feature unless asked.
    double feature = kVolHuge;
    for (const VolumeOp& op : ops)
        feature = std::min(feature, volFeatureSize(op, meshes));
    int per = 1;
    if (s.voxelSize > 0.0)
        per = std::clamp(int(std::lround(h.cell / s.voxelSize)), 1, 256);
    else if (feature < kVolHuge)
        per = std::clamp(int(std::ceil(h.cell / std::max(feature / 3.0, 1.0))), 1, 64);

    std::vector<std::string> materialNames{"ground"};
    std::vector<uint8_t> opMaterial(ops.size(), 0);
    for (size_t k = 0; k < ops.size(); ++k)
    {
        auto it = std::find(materialNames.begin(), materialNames.end(), ops[k].material);
        if (it == materialNames.end() && materialNames.size() < 255)
            it = materialNames.insert(materialNames.end(), ops[k].material);
        opMaterial[k] = uint8_t(std::min<ptrdiff_t>(it - materialNames.begin(), 254));
    }

    VolField f;
    f.h = &before;
    std::vector<VolShape> shapes;
    std::vector<int64_t> brickList;
    for (int attempt = 0;; ++attempt)
    {
        f.per = per;
        f.voxel = h.cell / double(per);
        f.maxI = (h.nx - 1) * per;
        f.maxK = (h.nz - 1) * per;
        shapes.clear();
        r.ops.clear();
        std::set<int64_t> bricks;
        for (size_t k = 0; k < ops.size(); ++k)
        {
            std::string message;
            std::vector<VolShape> made = volShapesOf(ops[k], k, opMaterial[k], before, f.voxel, meshes, message);
            VolumeOpResult& res = r.ops[ops[k].id.empty() ? "op" + std::to_string(k + 1) : ops[k].id];
            res.message = message;
            for (VolShape& sh : made)
            {
                res.bounds.extend(sh.box);
                const glm::ivec3 lo(int(std::floor((sh.box.min.x - h.origin.x) / f.voxel)), f.jOf(sh.box.min.y), int(std::floor((sh.box.min.z - h.origin.y) / f.voxel)));
                const glm::ivec3 hi(int(std::ceil((sh.box.max.x - h.origin.x) / f.voxel)), f.jOf(sh.box.max.y) + 1, int(std::ceil((sh.box.max.z - h.origin.y) / f.voxel)));
                const glm::ivec3 clo(std::max(lo.x, 0), lo.y, std::max(lo.z, 0)), chi(std::min(hi.x, f.maxI), hi.y, std::min(hi.z, f.maxK));
                if (clo.x > chi.x || clo.z > chi.z)
                    continue;
                res.applied = true;
                for (int bk = volFloorDiv(clo.z, kVolBrick); bk <= volFloorDiv(chi.z, kVolBrick); ++bk)
                    for (int bj = volFloorDiv(clo.y, kVolBrick); bj <= volFloorDiv(chi.y, kVolBrick); ++bj)
                        for (int bi = volFloorDiv(clo.x, kVolBrick); bi <= volFloorDiv(chi.x, kVolBrick); ++bi)
                            bricks.insert(volPackKey(bi, bj, bk));
                shapes.push_back(std::move(sh));
            }
            if (!res.applied && res.message.empty())
                res.message = "outside the terrain";
        }
        if (double(bricks.size()) * 512.0 <= s.maxVoxels || per == 1)
        {
            brickList.assign(bricks.begin(), bricks.end());
            break;
        }
        const int coarser = std::max(1, per / 2);
        r.warnings.push_back(std::format("voxel {:.1f} cm would need {:.1f} million voxels (limit {:.1f}); used {:.1f} cm", h.cell / per,
                                         double(bricks.size()) * 512e-6, s.maxVoxels * 1e-6, h.cell / coarser));
        per = coarser;
    }
    r.voxel = f.voxel;
    r.voxelsPerCell = per;
    for (int64_t key : brickList)
    {
        constexpr int64_t kOff = int64_t(1) << 20, kMask = (int64_t(1) << 21) - 1;
        f.brickOf[key] = int(f.keys.size());
        f.keys.push_back({int(((key >> 42) & kMask) - kOff), int(((key >> 21) & kMask) - kOff), int((key & kMask) - kOff)});
    }
    r.bricks = f.keys.size();
    r.voxels = f.keys.size() * 512;
    for (const std::string& w : r.warnings)
        log::warn("terrain volume: {}", w);
    if (f.keys.empty())
    {
        h.masks.erase("holes");
        return r;
    }
    volEvaluateBricks(f, shapes);
    volScanColumns(f);

    // The landscape: every column the bricks reach takes its top crossing (split on); the rest keep their heights.
    const int cx = h.nx - 1, cz = h.nz - 1;
    Heightfield land = before;
    land.masks.clear();
    if (s.split)
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
                if (const VolColumn* c = f.column(x * per, z * per))
                {
                    const float t = c->top;
                    if (std::abs(t - land.heights[land.index(x, z)]) > 1e-3f)
                        ++r.heightChanged;
                    land.heights[land.index(x, z)] = t;
                }

    // Cells: open where a column the heightfield can't hold breaks through it or its landscape would cut into air.
    std::vector<uint8_t> state(size_t(cx) * cz, 0);
    std::vector<uint8_t> nrCell(size_t(cx) * cz, 0);
    const double tol = std::max(2.0 * f.voxel, 0.25 * s.minRoof);
    std::set<int64_t> cellSet;
    for (const auto& [key, cols] : f.columns)
    {
        constexpr int64_t kOff = int64_t(1) << 20, kMask = (int64_t(1) << 21) - 1;
        const int bi = int(((key >> 42) & kMask) - kOff), bk = int((key & kMask) - kOff);
        for (int lk = 0; lk < kVolBrick; ++lk)
            for (int li = 0; li < kVolBrick; ++li)
            {
                const int i = bi * kVolBrick + li, k = bk * kVolBrick + lk;
                if (i < 0 || k < 0 || i > f.maxI || k > f.maxK)
                    continue;
                // A column on a grid line belongs to the cells either side of it.
                for (int z = (k % per == 0 ? k / per - 1 : k / per); z <= k / per; ++z)
                    for (int x = (i % per == 0 ? i / per - 1 : i / per); x <= i / per; ++x)
                        if (x >= 0 && z >= 0 && x < cx && z < cz)
                            cellSet.insert(volPackKey(x, 0, z));
            }
    }
    std::vector<glm::ivec2> activeCells;
    for (int64_t key : cellSet)
    {
        constexpr int64_t kOff = int64_t(1) << 20, kMask = (int64_t(1) << 21) - 1;
        activeCells.push_back({int(((key >> 42) & kMask) - kOff), int((key & kMask) - kOff)});
    }
    std::vector<double> cellError(activeCells.size(), 0.0);
    tbb::parallel_for(size_t(0), activeCells.size(), [&](size_t n) {
        const glm::ivec2 c = activeCells[n];
        bool open = false, nr = false;
        double err = 0.0;
        for (int k = c.y * per; k <= (c.y + 1) * per && !open; ++k)
            for (int i = c.x * per; i <= (c.x + 1) * per && !open; ++i)
            {
                const VolColumn* col = f.column(i, k);
                if (!col)
                    continue;
                const double L = land.heightAt(f.x(i), f.z(k));
                if (!s.split)
                {
                    open = col->changes > 1 || std::abs(double(col->top) - L) > 0.25 * f.voxel;
                    nr = nr || col->changes > 1;
                    continue;
                }
                if (col->changes > 1)
                {
                    nr = true;
                    open = std::abs(double(col->top) - L) > tol || double(col->roof) < s.minRoof;
                }
                else
                    err = std::max(err, std::abs(double(col->top) - L));
            }
        if (s.split && s.maxHeightError > 0.0 && err > s.maxHeightError)
            open = true;
        state[size_t(c.y) * cx + c.x] = open ? 2 : nr ? 1 : 0;
        nrCell[size_t(c.y) * cx + c.x] = nr ? 1 : 0;
        cellError[n] = open ? 0.0 : err;
    });
    for (double e : cellError)
        r.heightError = std::max(r.heightError, e);

    // Hole samples: corners of open cells, grown by the margin; a cell with any hole corner is a hole.
    std::vector<uint8_t> holeSample(h.size(), 0);
    for (int z = 0; z < cz; ++z)
        for (int x = 0; x < cx; ++x)
            if (state[size_t(z) * cx + x] == 2)
            {
                ++r.openCells;
                for (int dz = -s.margin; dz <= 1 + s.margin; ++dz)
                    for (int dx = -s.margin; dx <= 1 + s.margin; ++dx)
                    {
                        const int sx = x + dx, sz = z + dz;
                        if (sx >= 0 && sz >= 0 && sx < h.nx && sz < h.nz)
                            holeSample[h.index(sx, sz)] = 1;
                    }
            }
            else if (state[size_t(z) * cx + x] == 1)
                ++r.buriedCells;
    VolPlan plan;
    plan.h = &before;
    plan.land = &land;
    plan.cx = cx;
    plan.cz = cz;
    plan.hole.assign(size_t(cx) * cz, 0);
    for (int z = 0; z < cz; ++z)
        for (int x = 0; x < cx; ++x)
            plan.hole[size_t(z) * cx + x] =
                holeSample[h.index(x, z)] || holeSample[h.index(x + 1, z)] || holeSample[h.index(x, z + 1)] || holeSample[h.index(x + 1, z + 1)];
    plan.nrCell = nrCell;
    plan.skirt = s.skirt;
    for (uint8_t v : holeSample)
        r.holeSamples += v;
    for (uint8_t v : plan.hole)
        r.holeCells += v;

    h.heights = land.heights;
    if (r.holeSamples > 0)
    {
        std::vector<float>& mask = h.mask("holes");
        for (size_t k = 0; k < mask.size(); ++k)
            mask[k] = holeSample[k] ? 1.0f : 0.0f;
    }
    else
        h.masks.erase("holes");

    // Seam outline length, for coverage.
    for (int j = 0; j < cz; ++j)
        for (int i = 0; i <= cx; ++i)
            if (plan.outlineX(i, j))
                r.seamLength += h.cell;
    for (int j = 0; j <= cz; ++j)
        for (int i = 0; i < cx; ++i)
            if (plan.outlineZ(i, j))
                r.seamLength += h.cell;

    auto nearNrAt = [&](int stride) {
        return [&f, stride](const glm::dvec3& c) {
            const int i0 = int(std::lround((c.x - f.h->origin.x) / f.voxel)), k0 = int(std::lround((c.z - f.h->origin.y) / f.voxel));
            for (int dk = -stride; dk <= stride; ++dk)
                for (int di = -stride; di <= stride; ++di)
                    if (const VolColumn* col = f.column(i0 + di, k0 + dk); col && col->changes > 1)
                        return true;
            return false;
        };
    };

    auto groundLikeAt = [&](int stride) {
        return [&f, &land, stride](const glm::dvec3& c) {
            const int i0 = int(std::lround((c.x - f.h->origin.x) / f.voxel)), k0 = int(std::lround((c.z - f.h->origin.y) / f.voxel));
            for (int dk = -stride; dk <= stride; ++dk)
                for (int di = -stride; di <= stride; ++di)
                    if (const VolColumn* col = f.column(i0 + di, k0 + dk))
                        if (std::abs(double(col->top) - land.heightAt(f.x(i0 + di), f.z(k0 + dk))) > 0.25 * f.voxel)
                            return false;
            return true;
        };
    };

    // Bricks to mesh: where the edits reach, plus the ground over every hole.
    auto bricksAt = [&](int stride) {
        std::set<int64_t> set;
        for (const glm::ivec3& k : f.keys)
            set.insert(volPackKey(volFloorDiv(k.x, stride), volFloorDiv(k.y, stride), volFloorDiv(k.z, stride)));
        const int span = kVolBrick * stride;
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
            {
                if (!plan.hole[size_t(z) * cx + x])
                    continue;
                double lo = kVolHuge, hi = -kVolHuge;
                for (int dz = 0; dz <= 1; ++dz)
                    for (int dx = 0; dx <= 1; ++dx)
                    {
                        lo = std::min({lo, double(land.at(x + dx, z + dz)), double(before.at(x + dx, z + dz))});
                        hi = std::max({hi, double(land.at(x + dx, z + dz)), double(before.at(x + dx, z + dz))});
                    }
                const int j0 = volFloorDiv(f.jOf(lo) - 2 * stride, span), j1 = volFloorDiv(f.jOf(hi) + 2 * stride, span);
                for (int bk = volFloorDiv(z * per, span); bk <= volFloorDiv((z + 1) * per, span); ++bk)
                    for (int bi = volFloorDiv(x * per, span); bi <= volFloorDiv((x + 1) * per, span); ++bi)
                        for (int bj = j0; bj <= j1; ++bj)
                            set.insert(volPackKey(bi, bj, bk));
            }
        std::vector<glm::ivec3> out;
        for (int64_t key : set)
        {
            constexpr int64_t kOff = int64_t(1) << 20, kMask = (int64_t(1) << 21) - 1;
            out.push_back({int(((key >> 42) & kMask) - kOff), int(((key >> 21) & kMask) - kOff), int((key & kMask) - kOff)});
        }
        return out;
    };

    std::map<int64_t, int> regionOfCell;
    std::map<std::pair<int, int64_t>, size_t> pieceOf;
    std::vector<geo::Mesh> joined;
    for (int level = 0; level < s.lods; ++level)
    {
        const int stride = 1 << level;
        plan.roofHalf = std::max(0.5 * s.minRoof, 1.5 * stride * f.voxel);
        const VolSurface surf = volSurfaceAt(f, bricksAt(stride), stride);
        VolMesher m = volMeshLevel(plan, surf, nearNrAt(stride), groundLikeAt(stride));
        m.skirts();
        if (m.polys.empty())
            break;
        const std::vector<int> comp = volComponents(m);
        const std::vector<glm::dvec3> smooth = volSmoothNormals(m);
        auto tileOf = [&](const VolPoly& p) {
            glm::dvec3 c(0.0);
            for (int v : p.v)
                c += m.verts[size_t(v)].p / double(p.v.size());
            if (s.chunkSize <= 0.0)
                return int64_t(0);
            return volPackKey(int(std::floor((c.x - h.origin.x) / s.chunkSize)), 0, int(std::floor((c.z - h.origin.y) / s.chunkSize)));
        };
        // Each connected part is a region at LOD0; coarser levels find theirs through the cells LOD0 faces sit over.
        std::vector<int> regionOf(comp.size());
        if (level == 0)
        {
            for (size_t p = 0; p < m.polys.size(); ++p)
            {
                regionOf[p] = comp[p];
                glm::dvec3 c(0.0);
                for (int v : m.polys[p].v)
                    c += m.verts[size_t(v)].p / double(m.polys[p].v.size());
                regionOfCell.try_emplace(volPackKey(plan.cellX(c.x), 0, plan.cellZ(c.z)), comp[p]);
            }
        }
        else
        {
            std::map<int, int> compRegion;
            for (size_t p = 0; p < m.polys.size(); ++p)
            {
                if (compRegion.count(comp[p]))
                    continue;
                glm::dvec3 c(0.0);
                for (int v : m.polys[p].v)
                    c += m.verts[size_t(v)].p / double(m.polys[p].v.size());
                const int x = plan.cellX(c.x), z = plan.cellZ(c.z);
                for (int ring = 0; ring <= 3 && !compRegion.count(comp[p]); ++ring)
                    for (int dz = -ring; dz <= ring && !compRegion.count(comp[p]); ++dz)
                        for (int dx = -ring; dx <= ring; ++dx)
                            if (auto it = regionOfCell.find(volPackKey(x + dx, 0, z + dz)); it != regionOfCell.end())
                            {
                                compRegion[comp[p]] = it->second;
                                break;
                            }
            }
            for (size_t p = 0; p < m.polys.size(); ++p)
            {
                auto it = compRegion.find(comp[p]);
                regionOf[p] = it == compRegion.end() ? -1 : it->second;
            }
        }
        std::map<std::pair<int, int64_t>, std::vector<size_t>> groups;
        size_t lost = 0;
        for (size_t p = 0; p < m.polys.size(); ++p)
        {
            if (regionOf[p] < 0)
            {
                ++lost;
                continue;
            }
            groups[{regionOf[p], tileOf(m.polys[p])}].push_back(p);
        }
        if (lost > 0)
            r.warnings.push_back(std::format("LOD{}: {} faces matched no LOD0 piece and were left out", level, lost));
        for (const auto& [key, faces] : groups)
        {
            auto mesh = volBuildMesh(m, faces, smooth, land, materialNames, s.uvScale);
            if (mesh->numFaces() == 0)
                continue;
            if (level == 0)
            {
                VolumePiece piece;
                piece.region = key.first;
                piece.lods.push_back(mesh);
                piece.bounds = mesh->bounds();
                std::set<int64_t> holeSeen, buriedSeen;
                std::set<int32_t> used;
                const std::span<const int32_t> fm = mesh->faceAttrs().get<int32_t>(geo::attr::kMaterial);
                for (geo::Index fi = 0; fi < mesh->faceCapacity(); ++fi)
                    if (mesh->faceValid(fi))
                    {
                        const glm::dvec3 c = mesh->faceCentroid(fi);
                        const int x = plan.cellX(c.x), z = plan.cellZ(c.z);
                        (plan.holeAt(x, z) ? holeSeen : buriedSeen).insert(volPackKey(x, 0, z));
                        if (fi < fm.size())
                            used.insert(fm[fi]);
                    }
                for (int64_t c : holeSeen)
                    buriedSeen.erase(c);
                piece.holeCells = holeSeen.size();
                piece.buriedCells = buriedSeen.size();
                for (int32_t u : used)
                    if (u >= 0 && size_t(u) < materialNames.size())
                        piece.materials.push_back(materialNames[size_t(u)]);
                pieceOf[key] = r.pieces.size();
                r.pieces.push_back(std::move(piece));
                joined.push_back(*mesh);
            }
            else if (auto it = pieceOf.find(key); it != pieceOf.end() && r.pieces[it->second].lods.size() == size_t(level))
                r.pieces[it->second].lods.push_back(mesh);
        }
    }
    // Chunk numbers and names, in region order.
    std::map<int, int> chunks;
    for (VolumePiece& p : r.pieces)
    {
        p.chunk = chunks[p.region]++;
        p.name = std::format("piece{}_{}", p.region + 1, p.chunk + 1);
    }

    // Every column the heightfield can't hold must lie under or in some piece.
    std::set<int64_t> covered;
    for (const VolumePiece& p : r.pieces)
        for (geo::Index fi = 0; fi < p.lods[0]->faceCapacity(); ++fi)
            if (p.lods[0]->faceValid(fi))
            {
                const glm::dvec3 c = p.lods[0]->faceCentroid(fi);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        covered.insert(volPackKey(plan.cellX(c.x) + dx, 0, plan.cellZ(c.z) + dz));
            }
    for (const auto& [key, cols] : f.columns)
    {
        constexpr int64_t kOff = int64_t(1) << 20, kMask = (int64_t(1) << 21) - 1;
        const int bi = int(((key >> 42) & kMask) - kOff), bk = int((key & kMask) - kOff);
        for (int lk = 0; lk < kVolBrick; ++lk)
            for (int li = 0; li < kVolBrick; ++li)
            {
                const int i = bi * kVolBrick + li, k = bk * kVolBrick + lk;
                if (i < 0 || k < 0 || i > f.maxI || k > f.maxK || cols[size_t(li) + kVolBrick * size_t(lk)].changes <= 1)
                    continue;
                if (!covered.count(volPackKey(std::min(i / per, cx - 1), 0, std::min(k / per, cz - 1))))
                    ++r.unmeshedColumns;
            }
    }

    if (!r.pieces.empty())
    {
        geo::Mesh all;
        for (const geo::Mesh& m : joined)
            all.append(m);
        r.mesh = std::make_shared<const geo::Mesh>(std::move(all));
        std::vector<std::shared_ptr<const geo::Mesh>> lod0;
        for (const VolumePiece& p : r.pieces)
            lod0.push_back(p.lods[0]);
        const SeamReport seams = measureSeams(h, lod0);
        r.seamGap = seams.maxGap;
        r.seamUncovered = seams.uncovered;
    }
    if (r.unmeshedColumns > 0)
        r.warnings.push_back(std::format("{} columns the heightfield can't hold have no mesh", r.unmeshedColumns));
    return r;
}

// ---------------------------------------------------------------------------------------------------- checks

namespace
{
/** The highest mesh surface under a point, cast from above; NaN when nothing is there. */
double volTopHit(const std::vector<std::unique_ptr<geo::MeshBvh>>& bvhs, double x, double z, double from)
{
    double best = std::numeric_limits<double>::quiet_NaN();
    for (const auto& b : bvhs)
    {
        geo::BvhHit hit;
        if (b->raycast({x, from, z}, {0.0, -1.0, 0.0}, hit))
            if (std::isnan(best) || hit.point.y > best)
                best = hit.point.y;
    }
    return best;
}

std::vector<std::unique_ptr<geo::MeshBvh>> volBvhsOf(const std::vector<std::shared_ptr<const geo::Mesh>>& meshes, double* top)
{
    std::vector<std::unique_ptr<geo::MeshBvh>> out;
    for (const auto& m : meshes)
        if (m && m->numFaces() > 0)
        {
            out.push_back(std::make_unique<geo::MeshBvh>(*m));
            *top = std::max(*top, m->bounds().max.y);
        }
    return out;
}
} // namespace

SeamReport measureSeams(const Heightfield& h, const std::vector<std::shared_ptr<const geo::Mesh>>& meshes, int samplesPerEdge)
{
    LILA_ZONE();

    SeamReport r;
    if (h.empty())
        return r;
    const std::vector<uint8_t> cells = holeCells(h);
    const int cx = h.nx - 1, cz = h.nz - 1;
    auto hole = [&](int x, int z) { return x >= 0 && z >= 0 && x < cx && z < cz && cells[size_t(z) * cx + x]; };
    auto in = [&](int x, int z) { return x >= 0 && z >= 0 && x < cx && z < cz; };
    // Mesh edges lying along a grid line, by line: where along it they run and their heights there.
    struct Run
    {
        double a, b, ya, yb;
    };
    std::map<std::pair<int, int>, std::vector<Run>> runs;
    const double eps = 1e-6 * h.cell;
    for (const auto& m : meshes)
    {
        if (!m)
            continue;
        for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        {
            if (!m->edgeValid(e))
                continue;
            const glm::dvec3 p = m->position(m->edgePoint(e, 0)), q = m->position(m->edgePoint(e, 1));
            for (int axis = 0; axis < 2; ++axis)
            {
                const double cp = axis == 0 ? p.x : p.z, cq = axis == 0 ? q.x : q.z;
                const double org = axis == 0 ? h.origin.x : h.origin.y;
                const int line = int(std::lround((cp - org) / h.cell));
                const double L = org + line * h.cell;
                if (std::abs(cp - L) > eps || std::abs(cq - L) > eps)
                    continue;
                const double ap = axis == 0 ? p.z : p.x, aq = axis == 0 ? q.z : q.x;
                if (ap == aq)
                    continue;
                runs[{axis, line}].push_back(ap < aq ? Run{ap, aq, p.y, q.y} : Run{aq, ap, q.y, p.y});
            }
        }
    }
    const int n = std::max(1, samplesPerEdge);
    auto sample = [&](int axis, int line, glm::dvec2 a, glm::dvec2 b) {
        r.length += h.cell;
        bool uncovered = false;
        const auto it = runs.find({axis, line});
        for (int s = 0; s <= n; ++s)
        {
            const glm::dvec2 p = glm::mix(a, b, double(s) / n);
            const double along = axis == 0 ? p.y : p.x;
            const double ground = h.heightAt(p.x, p.y);
            double top = -kVolHuge;
            if (it != runs.end())
                for (const Run& run : it->second)
                    if (along >= run.a - eps && along <= run.b + eps)
                        top = std::max(top, glm::mix(run.ya, run.yb, std::clamp((along - run.a) / (run.b - run.a), 0.0, 1.0)));
            ++r.samples;
            if (top == -kVolHuge)
            {
                uncovered = true;
                continue;
            }
            const double gap = std::abs(top - ground);
            if (gap > r.maxGap)
            {
                r.maxGap = gap;
                r.worst = {p.x, ground, p.y};
            }
        }
        if (uncovered)
            r.uncovered += h.cell;
    };
    for (int j = 0; j < cz; ++j)
        for (int i = 1; i < cx; ++i)
            if (in(i - 1, j) && in(i, j) && hole(i - 1, j) != hole(i, j))
                sample(0, i, h.samplePos(i, j), h.samplePos(i, j + 1));
    for (int j = 1; j < cz; ++j)
        for (int i = 0; i < cx; ++i)
            if (in(i, j - 1) && in(i, j) && hole(i, j - 1) != hole(i, j))
                sample(1, j, h.samplePos(i, j), h.samplePos(i + 1, j));
    return r;
}

FootprintReport measureFootprint(const Heightfield& h, const std::vector<std::shared_ptr<const geo::Mesh>>& meshes, double tolerance)
{
    LILA_ZONE();

    FootprintReport r;
    if (h.empty())
        return r;
    const std::vector<uint8_t> cells = holeCells(h);
    const int cx = h.nx - 1, cz = h.nz - 1;
    double top = h.range().y;
    const auto bvhs = volBvhsOf(meshes, &top);
    top += 1000.0;
    geo::Bounds b;
    for (const auto& m : meshes)
        if (m)
            b.extend(m->bounds());
    int x0 = 0, x1 = cx - 1, z0 = 0, z1 = cz - 1;
    for (int z = 0; z < cz; ++z)
        for (int x = 0; x < cx; ++x)
            if (cells[size_t(z) * cx + x])
            {
                b.extend(glm::dvec3(h.samplePos(x, z).x, 0.0, h.samplePos(x, z).y));
                b.extend(glm::dvec3(h.samplePos(x + 1, z + 1).x, 0.0, h.samplePos(x + 1, z + 1).y));
            }
    if (!b.valid())
        return r;
    x0 = std::clamp(int(std::floor((b.min.x - h.origin.x) / h.cell)) - 1, 0, cx - 1);
    x1 = std::clamp(int(std::floor((b.max.x - h.origin.x) / h.cell)) + 1, 0, cx - 1);
    z0 = std::clamp(int(std::floor((b.min.z - h.origin.y) / h.cell)) - 1, 0, cz - 1);
    z1 = std::clamp(int(std::floor((b.max.z - h.origin.y) / h.cell)) + 1, 0, cz - 1);
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            for (double fz : {0.25, 0.75})
                for (double fx : {0.25, 0.75})
                {
                    const glm::dvec2 p = h.samplePos(x, z) + glm::dvec2(fx, fz) * h.cell;
                    const double hit = volTopHit(bvhs, p.x, p.y, top);
                    ++r.checked;
                    if (cells[size_t(z) * cx + x])
                        r.uncoveredInHole += std::isnan(hit) ? 1 : 0;
                    else if (!std::isnan(hit) && hit > h.heightAt(p.x, p.y) + tolerance)
                        ++r.meshOutsideHole;
                }
    return r;
}

// ---------------------------------------------------------------------------------------------------- results

namespace
{
std::mutex& volumePublishedMutex()
{
    static std::mutex m;
    return m;
}

std::map<uint64_t, std::map<std::string, std::shared_ptr<const VolumeResult>>>& volumePublished()
{
    static std::map<uint64_t, std::map<std::string, std::shared_ptr<const VolumeResult>>> p;
    return p;
}
} // namespace

void publishVolumeResult(uint64_t modifier, const std::string& node, std::shared_ptr<const VolumeResult> r)
{
    std::lock_guard lock(volumePublishedMutex());
    volumePublished()[modifier][node] = std::move(r);
}

void forgetGoneVolumeResults()
{
    const lila::scene::Graph& g = lila::scene::Graph::current();
    std::lock_guard lock(volumePublishedMutex());
    std::erase_if(volumePublished(), [&](const auto& e) { return !g.node(lila::scene::NodeId(e.first)); });
}

std::shared_ptr<const VolumeResult> volumeResult(uint64_t modifier, const std::string& node)
{
    std::lock_guard lock(volumePublishedMutex());
    auto it = volumePublished().find(modifier);
    if (it == volumePublished().end())
        return nullptr;
    auto n = it->second.find(node);
    return n == it->second.end() ? nullptr : n->second;
}

std::map<std::string, std::shared_ptr<const VolumeResult>> volumeResults(uint64_t modifier)
{
    std::lock_guard lock(volumePublishedMutex());
    auto it = volumePublished().find(modifier);
    return it == volumePublished().end() ? std::map<std::string, std::shared_ptr<const VolumeResult>>{} : it->second;
}
} // namespace terrain
