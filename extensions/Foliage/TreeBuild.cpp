#include "TreeBuild.h"

#include <lila/MeshOps.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace treegen
{
namespace geo = lila::geo;

namespace
{
constexpr double kPi = 3.14159265358979323846;
const glm::dvec3 kWorldY{0.0, 1.0, 0.0};

double radians(double d) { return d * kPi / 180.0; }

glm::dvec3 normalized(const glm::dvec3& v)
{
    const double l = glm::length(v);
    return l < kEps ? v : v / l;
}

/** The shortest rotation taking a onto b. */
glm::dquat fromTo(const glm::dvec3& a, const glm::dvec3& b) { return glm::rotation(normalized(a), normalized(b)); }

glm::dvec3 axisVector(int index, int sign)
{
    glm::dvec3 v(0.0);
    v[index] = double(sign);
    return v;
}

/** "auto" keeps the measured axis; "X", "-Z" and so on pick one. */
std::pair<int, int> pickAxis(const std::string& choice, int index, int sign)
{
    if (choice.empty() || choice == "auto" || choice == "Auto")
        return {index, sign};
    std::string text = choice;
    const bool negative = text[0] == '-';
    while (!text.empty() && (text[0] == '-' || text[0] == '+'))
        text.erase(0, 1);
    if (text.size() != 1)
        throw std::runtime_error("Unknown axis '" + choice + "'.");
    const char c = char(std::tolower(static_cast<unsigned char>(text[0])));
    if (c < 'x' || c > 'z')
        throw std::runtime_error("Unknown axis '" + choice + "'.");
    return {c - 'x', negative ? -1 : 1};
}

struct Key3
{
    int64_t x, y, z;
    bool operator==(const Key3&) const = default;
};
struct Key3Hash
{
    size_t operator()(const Key3& k) const
    {
        uint64_t h = 1469598103934665603ull;
        for (int64_t v : {k.x, k.y, k.z})
            h = (h ^ uint64_t(v)) * 1099511628211ull;
        return size_t(h);
    }
};
using Grid = std::unordered_map<Key3, std::vector<glm::dvec3>, Key3Hash>;

/** Takes the point when nothing within minDistance holds a cell around it. */
bool reserve(Grid& grid, const glm::dvec3& point, double cell, double minDistance)
{
    const Key3 key{int64_t(std::floor(point.x / cell)), int64_t(std::floor(point.y / cell)), int64_t(std::floor(point.z / cell))};
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
                if (auto it = grid.find({key.x + dx, key.y + dy, key.z + dz}); it != grid.end())
                    for (const glm::dvec3& other : it->second)
                        if (glm::length(other - point) < minDistance)
                            return false;
    grid[key].push_back(point);
    return true;
}

Key3 roundKey(const glm::dvec3& p) { return {std::llround(p.x * 1e6), std::llround(p.y * 1e6), std::llround(p.z * 1e6)}; }

double num(const json& j, const char* k, double fallback)
{
    auto it = j.find(k);
    return it == j.end() || !it->is_number() ? fallback : it->get<double>();
}
int integer(const json& j, const char* k, int fallback)
{
    auto it = j.find(k);
    return it == j.end() || !it->is_number() ? fallback : int(std::llround(it->get<double>()));
}
std::string text(const json& j, const char* k, const char* fallback)
{
    auto it = j.find(k);
    return it == j.end() || !it->is_string() ? fallback : it->get<std::string>();
}
} // namespace

Span spanOf(const json& v)
{
    if (v.is_array() && v.size() == 2)
        return {v[0].get<double>(), v[1].get<double>()};
    if (v.is_number())
        return {v.get<double>(), v.get<double>()};
    return {};
}

double between(Rng& rng, const Span& s)
{
    if (std::abs(s.high - s.low) < kEps)
        return s.low;
    return rng.uniform(std::min(s.low, s.high), std::max(s.low, s.high));
}

glm::dvec3 rotateAbout(const glm::dvec3& v, const glm::dvec3& axis, double r) { return glm::angleAxis(r, normalized(axis)) * v; }

glm::dvec3 perpendicular(const glm::dvec3& d)
{
    const glm::dvec3 ref = std::abs(glm::dot(d, kWorldY)) < 0.95 ? kWorldY : glm::dvec3(1.0, 0.0, 0.0);
    return normalized(ref - d * glm::dot(ref, d));
}

glm::dvec3 spun(const glm::dvec3& d, double degrees) { return rotateAbout(perpendicular(d), d, radians(degrees)); }

glm::dvec3 jitter(const glm::dvec3& d, double degrees, Rng& rng)
{
    if (degrees <= kEps)
        return d;
    const glm::dvec3 axis = spun(d, rng.uniform(0.0, 360.0));
    return normalized(rotateAbout(d, axis, radians(rng.uniform(-degrees, degrees))));
}

// ------------------------------------------------------------------------------------------------ trunk

TrunkSpec trunkSpec(const json& t, const glm::dvec3& up)
{
    TrunkSpec s;
    s.lean = num(t, "lean", s.lean);
    s.taper = num(t, "taper", s.taper);
    s.branchStart = num(t, "branch_start", s.branchStart);
    s.branchAngle = num(t, "branch_angle", s.branchAngle);
    s.angleVariance = num(t, "angle_variance", s.angleVariance);
    s.lengthRatio = num(t, "length_ratio", s.lengthRatio);
    s.lengthFalloff = num(t, "length_falloff", s.lengthFalloff);
    s.radiusRatio = num(t, "radius_ratio", s.radiusRatio);
    s.gnarl = num(t, "gnarl", s.gnarl);
    s.upPull = num(t, "up_pull", s.upPull);
    const double h = num(t, "height", 0.0), r = num(t, "radius", 0.0);
    s.height = h != 0.0 ? h : 600.0;
    s.radius = r != 0.0 ? r : 22.0;
    s.sections = integer(t, "sections", s.sections);
    s.levels = integer(t, "levels", s.levels);
    s.branches = integer(t, "branches", s.branches);
    s.up = up;
    if (s.height <= kEps || s.radius <= kEps)
        throw std::runtime_error("Height and radius have to be positive.");
    return s;
}

namespace
{
void grow(const glm::dvec3& start, const glm::dvec3& direction, double length, double rootRadius, double tipRadius, int sections, double gnarl,
          double upPull, const glm::dvec3& up, Rng& rng, Limb& out, glm::dvec3& heading)
{
    out.points = {start};
    out.radii = {rootRadius};
    heading = normalized(direction);
    const double step = length / std::max(sections, 1);
    const glm::dvec3 target = upPull >= 0.0 ? up : -up;
    const double pull = std::min(0.95, std::abs(upPull)) / std::max(sections, 1);
    for (int i = 0; i < sections; ++i)
    {
        heading = jitter(heading, gnarl, rng);
        if (pull > kEps)
            heading = normalized(heading * (1.0 - pull) + target * pull);
        out.points.push_back(out.points.back() + heading * step);
        out.radii.push_back(rootRadius + (tipRadius - rootRadius) * ((i + 1.0) / sections));
    }
}

void along(const Limb& l, double t, glm::dvec3& point, glm::dvec3& heading, double& radius)
{
    const int steps = int(l.points.size()) - 1;
    const double position = std::clamp(t, 0.0, 0.999) * steps;
    const int index = std::min(int(position), steps - 1);
    const double blend = position - index;
    point = l.points[index] * (1.0 - blend) + l.points[index + 1] * blend;
    heading = normalized(l.points[index + 1] - l.points[index]);
    radius = l.radii[index] * (1.0 - blend) + l.radii[index + 1] * blend;
}

void recurse(const TrunkSpec& spec, Rng& rng, Skeleton& out, const glm::dvec3& start, const glm::dvec3& direction, double length, double rootRadius,
             int level)
{
    const int sections = std::max(2, spec.sections - level);
    const double tipRadius = rootRadius * spec.taper;
    const double pull = level ? spec.upPull : 0.0;
    Limb limb;
    glm::dvec3 heading;
    grow(start, direction, length, rootRadius, tipRadius, sections, spec.gnarl, pull, spec.up, rng, limb, heading);
    out.limbs.push_back(limb);
    if (level >= spec.levels)
    {
        out.tips.push_back({limb.points.back(), heading, limb.radii.back()});
        return;
    }
    const int count = std::max(1, spec.branches);
    for (int i = 0; i < count; ++i)
    {
        const double t = spec.branchStart + (1.0 - spec.branchStart) * ((i + 0.5) / count);
        glm::dvec3 point, headingAt;
        double radius;
        along(limb, t, point, headingAt, radius);
        const double angle = spec.branchAngle + rng.uniform(-1.0, 1.0) * spec.angleVariance;
        const double spin = (i + level * 0.37) * kPhyllotaxis + rng.uniform(-1.0, 1.0) * 18.0;
        const glm::dvec3 hinge = spun(headingAt, spin);
        const glm::dvec3 child = normalized(rotateAbout(headingAt, hinge, radians(angle)));
        const double shorten = std::max(0.1, 1.0 - spec.lengthFalloff * t);
        const double childLength = length * spec.lengthRatio * shorten * (1.0 + rng.uniform(-0.2, 0.2));
        recurse(spec, rng, out, point, child, childLength, radius * spec.radiusRatio, level + 1);
    }
}
} // namespace

Skeleton skeleton(const TrunkSpec& spec, Rng& rng)
{
    Skeleton out;
    const glm::dvec3 lean = spun(spec.up, rng.uniform(0.0, 360.0));
    const glm::dvec3 heading = normalized(rotateAbout(spec.up, lean, radians(spec.lean)));
    recurse(spec, rng, out, glm::dvec3(0.0), heading, spec.height, spec.radius, 0);
    return out;
}

void Arrays::append(const Arrays& o)
{
    const Index base = Index(points.size());
    const Index uvBase = Index(us.size());
    const bool uvs = !o.uvIds.empty();
    points.insert(points.end(), o.points.begin(), o.points.end());
    counts.insert(counts.end(), o.counts.begin(), o.counts.end());
    for (Index i : o.connects)
        connects.push_back(i + base);
    if (uvs)
    {
        us.insert(us.end(), o.us.begin(), o.us.end());
        vs.insert(vs.end(), o.vs.begin(), o.vs.end());
        for (Index i : o.uvIds)
            uvIds.push_back(i + uvBase);
        for (Island is : o.islands)
        {
            is.start += uvBase;
            is.end += uvBase;
            islands.push_back(is);
        }
    }
}

namespace
{
void ringUvs(double radius, int sides, std::vector<double>& us, std::vector<double>& vs)
{
    for (int step = 0; step < sides; ++step)
    {
        const double angle = 2.0 * kPi * step / sides;
        us.push_back(radius + std::cos(angle) * radius);
        vs.push_back(radius + std::sin(angle) * radius);
    }
}
} // namespace

Arrays tube(const std::vector<glm::dvec3>& points, const std::vector<double>& radii, int sides, double pointBelow)
{
    Arrays a;
    const size_t count = points.size();
    size_t rings = count;
    if (radii.back() < pointBelow)
        for (rings = count - 1; rings > 1 && radii[rings - 1] < pointBelow;)
            --rings;
    const int stride = sides + 1;
    glm::dvec3 reference(0.0), previous(0.0);
    bool first = true;
    double travelled = 0.0, widest = 0.0;
    for (size_t i = 0; i < rings; ++i)
    {
        glm::dvec3 heading;
        if (i == 0)
            heading = normalized(points[1] - points[0]);
        else if (i == count - 1)
            heading = normalized(points[count - 1] - points[count - 2]);
        else
            heading = normalized(points[i + 1] - points[i - 1]);
        if (first)
        {
            reference = perpendicular(heading);
            first = false;
        }
        else
        {
            travelled += glm::length(points[i] - points[i - 1]);
            reference = fromTo(previous, heading) * reference;
        }
        reference = normalized(reference - heading * glm::dot(reference, heading));
        const glm::dvec3 binormal = glm::cross(heading, reference);
        previous = heading;

        const double radius = radii[i];
        for (int step = 0; step < sides; ++step)
        {
            const double angle = 2.0 * kPi * step / sides;
            a.points.push_back(points[i] + reference * (std::cos(angle) * radius) + binormal * (std::sin(angle) * radius));
        }
        const double circumference = 2.0 * kPi * radius;
        widest = std::max(widest, circumference);
        for (int step = 0; step < stride; ++step)
        {
            a.us.push_back(circumference * step / sides);
            a.vs.push_back(travelled);
        }
    }
    a.islands.push_back({0, a.us.size(), widest, travelled});

    for (size_t i = 0; i + 1 < rings; ++i)
        for (int step = 0; step < sides; ++step)
        {
            const int next = (step + 1) % sides;
            a.counts.push_back(4);
            for (Index v : {Index(i * sides + step), Index(i * sides + next), Index((i + 1) * sides + next), Index((i + 1) * sides + step)})
                a.connects.push_back(v);
            for (Index v : {Index(i * stride + step), Index(i * stride + step + 1), Index((i + 1) * stride + step + 1), Index((i + 1) * stride + step)})
                a.uvIds.push_back(v);
        }

    const Index apex = Index(a.points.size());
    a.points.push_back(points.back());
    size_t block = a.us.size();
    const double tipRadius = std::max(kEps, radii[rings - 1]);
    ringUvs(tipRadius, sides, a.us, a.vs);
    a.us.push_back(tipRadius);
    a.vs.push_back(tipRadius);
    a.islands.push_back({block, a.us.size(), tipRadius * 2.0, tipRadius * 2.0});
    const Index last = Index((rings - 1) * sides);
    for (int step = 0; step < sides; ++step)
    {
        const int next = (step + 1) % sides;
        a.counts.push_back(3);
        for (Index v : {last + step, last + next, apex})
            a.connects.push_back(v);
        for (Index v : {Index(block + step), Index(block + next), Index(block + sides)})
            a.uvIds.push_back(v);
    }

    block = a.us.size();
    const double baseRadius = std::max(kEps, radii.front());
    ringUvs(baseRadius, sides, a.us, a.vs);
    a.islands.push_back({block, a.us.size(), baseRadius * 2.0, baseRadius * 2.0});
    a.counts.push_back(Index(sides));
    for (int s = sides - 1; s >= 0; --s)
    {
        a.connects.push_back(Index(s));
        a.uvIds.push_back(Index(block + s));
    }
    return a;
}

namespace
{
using Box = std::pair<double, double>;

/** Rows of scaled boxes in the unit square; empty when they do not fit. */
std::vector<std::pair<double, double>> shelf(const std::vector<Box>& boxes, double scale, double padding)
{
    std::vector<std::pair<double, double>> offsets(boxes.size());
    std::vector<size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return boxes[a].second > boxes[b].second; });
    double x = 0.0, y = 0.0, row = 0.0;
    for (size_t i : order)
    {
        const double w = boxes[i].first * scale + padding, h = boxes[i].second * scale + padding;
        if (w > 1.0 || h > 1.0)
            return {};
        if (x + w > 1.0)
        {
            y += row;
            x = 0.0;
            row = 0.0;
        }
        if (y + h > 1.0)
            return {};
        offsets[i] = {x + padding * 0.5, y + padding * 0.5};
        x += w;
        row = std::max(row, h);
    }
    return offsets;
}

double fit(const std::vector<Box>& boxes, double padding)
{
    double biggest = 0.0;
    for (const Box& b : boxes)
        biggest = std::max({biggest, b.first, b.second});
    double low = 0.0, high = 1.0 / std::max(kEps, biggest);
    for (int i = 0; i < 40; ++i)
    {
        const double middle = (low + high) * 0.5;
        if (shelf(boxes, middle, padding).empty())
            high = middle;
        else
            low = middle;
    }
    return low;
}
} // namespace

double layoutUvs(std::vector<Arrays>& pieces, const std::string& mode, double tile, double density, int mapSize, double baseRadius, std::string* warning)
{
    if (mode == "tiled")
    {
        const double span = std::max(kEps, tile > kEps ? tile : 2.0 * kPi * baseRadius);
        for (Arrays& p : pieces)
        {
            for (double& u : p.us)
                u /= span;
            for (double& v : p.vs)
                v /= span;
        }
        return 0.0;
    }
    std::vector<Box> boxes;
    for (const Arrays& p : pieces)
        for (const Arrays::Island& is : p.islands)
            boxes.emplace_back(is.width, is.height);
    const double map = std::max(64.0, double(mapSize));
    const double padding = 8.0 / map;
    double scale = -1.0;
    if (density > kEps)
    {
        scale = density / map / 100.0;
        if (shelf(boxes, scale, padding).empty())
        {
            if (warning)
                *warning = std::to_string(int(density)) + " px/m does not fit a " + std::to_string(mapSize) + " map, packing to fit.";
            scale = -1.0;
        }
    }
    if (scale < 0.0)
        scale = fit(boxes, padding);
    const auto offsets = shelf(boxes, scale, padding);
    if (offsets.empty())
        return 0.0;
    size_t index = 0;
    for (Arrays& p : pieces)
        for (const Arrays::Island& is : p.islands)
        {
            const auto [ou, ov] = offsets[index++];
            for (size_t uv = is.start; uv < is.end; ++uv)
            {
                p.us[uv] = p.us[uv] * scale + ou;
                p.vs[uv] = p.vs[uv] * scale + ov;
            }
        }
    return scale * map * 100.0;
}

Mesh toMesh(const Arrays& a)
{
    Mesh m;
    for (const glm::dvec3& p : a.points)
        m.addPoint(p);
    struct Built
    {
        Index face;
        size_t start, count;
    };
    std::vector<Built> built;
    std::vector<Index> facePoints;
    size_t start = 0;
    for (Index count : a.counts)
    {
        facePoints.assign(a.connects.begin() + start, a.connects.begin() + start + count);
        const Index f = m.addFace(std::span<const Index>(facePoints));
        if (f != geo::kInvalid)
            built.push_back({f, start, count});
        start += count;
    }
    if (!a.uvIds.empty())
    {
        std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (const Built& b : built)
            m.forEachFaceCorner(b.face, [&](Index c) {
                const Index p = m.cornerPoint(c);
                for (size_t k = 0; k < b.count; ++k)
                    if (a.connects[b.start + k] == p)
                    {
                        const Index id = a.uvIds[b.start + k];
                        uv[c] = glm::vec2(float(a.us[id]), float(a.vs[id]));
                        break;
                    }
            });
    }
    return m;
}

Trunk buildTrunk(const json& trunk, const glm::dvec3& up)
{
    const TrunkSpec spec = trunkSpec(trunk, up);
    Rng rng(uint64_t(int64_t(num(trunk, "seed", 0.0))));
    Skeleton sk = skeleton(spec, rng);
    const int sides = std::max(3, integer(trunk, "sides", 8));
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Limb& l : sk.limbs)
        for (size_t i = 0; i < l.points.size(); ++i)
        {
            lo = glm::min(lo, l.points[i] - l.radii[i]);
            hi = glm::max(hi, l.points[i] + l.radii[i]);
        }
    // A cap triangle is r^2 sin(2pi/sides) / 2; meshCheck calls a face zero-area under 1e-8 of the diagonal squared, so caps keep twice that.
    const double pointBelow = glm::length(hi - lo) * std::sqrt(4e-8 / std::sin(2.0 * kPi / sides));
    std::vector<Arrays> pieces(sk.limbs.size());
    tbb::parallel_for(size_t(0), sk.limbs.size(), [&](size_t i) { pieces[i] = tube(sk.limbs[i].points, sk.limbs[i].radii, sides, pointBelow); });
    Trunk out;
    out.density = layoutUvs(pieces, text(trunk, "uv_mode", "packed"), num(trunk, "uv_tile", 0.0), num(trunk, "texel_density", 0.0),
                            integer(trunk, "uv_map_size", 2048), spec.radius, &out.warning);
    Arrays all;
    for (const Arrays& p : pieces)
        all.append(p);
    out.mesh = toMesh(all);
    geo::setHardEdgesByAngle(out.mesh, 50.0);
    out.tips = std::move(sk.tips);
    return out;
}

// ------------------------------------------------------------------------------------------------ blobs

namespace
{
void icosphere(int detail, Arrays& a)
{
    const double g = 1.6180339887;
    const double v[12][3] = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g}, {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
    for (const auto& p : v)
        a.points.push_back(normalized(glm::dvec3(p[0], p[1], p[2])));
    std::vector<std::array<Index, 3>> faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                               {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    std::map<std::pair<Index, Index>, Index> cache;
    auto middle = [&](Index x, Index y) {
        const auto key = std::minmax(x, y);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
        a.points.push_back(normalized((a.points[x] + a.points[y]) * 0.5));
        return cache[key] = Index(a.points.size() - 1);
    };
    for (int d = 0; d < std::max(0, detail); ++d)
    {
        cache.clear();
        std::vector<std::array<Index, 3>> split;
        for (const auto& [x, y, z] : faces)
        {
            const Index xy = middle(x, y), yz = middle(y, z), zx = middle(z, x);
            split.push_back({x, xy, zx});
            split.push_back({y, yz, xy});
            split.push_back({z, zx, yz});
            split.push_back({xy, yz, zx});
        }
        faces = std::move(split);
    }
    for (const auto& f : faces)
    {
        a.counts.push_back(3);
        a.connects.insert(a.connects.end(), f.begin(), f.end());
    }
}

void quadsphere(int segments, Arrays& a)
{
    const int steps = std::max(1, segments);
    std::unordered_map<Key3, Index, Key3Hash> lookup;
    auto vertex = [&](double x, double y, double z) {
        const Key3 key = roundKey({x, y, z});
        auto it = lookup.find(key);
        if (it != lookup.end())
            return it->second;
        const double xx = x * x, yy = y * y, zz = z * z;
        a.points.push_back({x * std::sqrt(1.0 - (yy + zz) * 0.5 + yy * zz / 3.0), y * std::sqrt(1.0 - (zz + xx) * 0.5 + zz * xx / 3.0),
                            z * std::sqrt(1.0 - (xx + yy) * 0.5 + xx * yy / 3.0)});
        return lookup[key] = Index(a.points.size() - 1);
    };
    // Origin corner plus the two full span edge vectors, ordered so du x dv faces outward.
    const int faces[6][3][3] = {{{1, -1, -1}, {0, 2, 0}, {0, 0, 2}},  {{-1, -1, -1}, {0, 0, 2}, {0, 2, 0}}, {{-1, 1, -1}, {0, 0, 2}, {2, 0, 0}},
                                {{-1, -1, -1}, {2, 0, 0}, {0, 0, 2}}, {{-1, -1, 1}, {2, 0, 0}, {0, 2, 0}},  {{-1, -1, -1}, {0, 2, 0}, {2, 0, 0}}};
    for (const auto& f : faces)
    {
        std::vector<std::vector<Index>> grid(steps + 1, std::vector<Index>(steps + 1));
        for (int i = 0; i <= steps; ++i)
            for (int j = 0; j <= steps; ++j)
            {
                double p[3];
                for (int ax = 0; ax < 3; ++ax)
                    p[ax] = f[0][ax] + f[1][ax] * double(i) / steps + f[2][ax] * double(j) / steps;
                grid[i][j] = vertex(p[0], p[1], p[2]);
            }
        for (int i = 0; i < steps; ++i)
            for (int j = 0; j < steps; ++j)
            {
                a.counts.push_back(4);
                for (Index v : {grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]})
                    a.connects.push_back(v);
            }
    }
}

void uvsphere(int detail, Arrays& a)
{
    const int axis = std::max(6, detail * 8);
    const int rings = std::max(3, axis / 2);
    a.points.push_back({0.0, 1.0, 0.0});
    for (int ring = 1; ring < rings; ++ring)
    {
        const double theta = kPi * ring / rings;
        for (int step = 0; step < axis; ++step)
        {
            const double phi = 2.0 * kPi * step / axis;
            a.points.push_back({std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
        }
    }
    a.points.push_back({0.0, -1.0, 0.0});
    const Index south = Index(a.points.size() - 1);
    for (int step = 0; step < axis; ++step)
    {
        const int next = (step + 1) % axis;
        a.counts.push_back(3);
        for (Index v : {Index(0), Index(1 + next), Index(1 + step)})
            a.connects.push_back(v);
    }
    for (int ring = 0; ring < rings - 2; ++ring)
    {
        const int base = 1 + ring * axis;
        for (int step = 0; step < axis; ++step)
        {
            const int next = (step + 1) % axis;
            a.counts.push_back(4);
            for (int v : {base + step, base + next, base + axis + next, base + axis + step})
                a.connects.push_back(Index(v));
        }
    }
    const int base = 1 + (rings - 2) * axis;
    for (int step = 0; step < axis; ++step)
    {
        const int next = (step + 1) % axis;
        a.counts.push_back(3);
        for (Index v : {Index(base + step), Index(base + next), south})
            a.connects.push_back(v);
    }
}

/** Cuts the unit blob off below a height on its Y, leaving the shell open underneath. */
Arrays clip(const Arrays& in, double height)
{
    Arrays out;
    std::unordered_map<Key3, Index, Key3Hash> lookup;
    auto indexOf = [&](const glm::dvec3& p) {
        const Key3 key = roundKey(p);
        auto it = lookup.find(key);
        if (it != lookup.end())
            return it->second;
        out.points.push_back(p);
        return lookup[key] = Index(out.points.size() - 1);
    };
    size_t start = 0;
    for (Index count : in.counts)
    {
        std::vector<glm::dvec3> face;
        for (size_t k = 0; k < count; ++k)
            face.push_back(in.points[in.connects[start + k]]);
        start += count;
        std::vector<glm::dvec3> edge;
        for (size_t k = 0; k < face.size(); ++k)
        {
            const glm::dvec3& cur = face[k];
            const glm::dvec3& next = face[(k + 1) % face.size()];
            const bool inside = cur.y >= height;
            if (inside)
                edge.push_back(cur);
            if (inside != (next.y >= height))
                edge.push_back(cur + (next - cur) * ((height - cur.y) / (next.y - cur.y)));
        }
        std::vector<glm::dvec3> trimmed;
        for (size_t k = 0; k < edge.size(); ++k)
            if (glm::length(edge[k] - edge[(k + edge.size() - 1) % edge.size()]) > 1e-6)
                trimmed.push_back(edge[k]);
        if (trimmed.size() < 3)
            continue;
        out.counts.push_back(Index(trimmed.size()));
        for (const glm::dvec3& p : trimmed)
            out.connects.push_back(indexOf(p));
    }
    if (out.counts.empty())
        throw std::runtime_error("Blob Clip removed the whole blob.");
    return out;
}
} // namespace

Arrays blobArrays(const std::string& kind, int detail, double clipAmount, const glm::dvec3& up, double spin)
{
    Arrays a;
    if (kind == "quadsphere")
        quadsphere(std::max(2, detail * 4), a);
    else if (kind == "icosphere")
        icosphere(detail, a);
    else if (kind == "uvsphere")
        uvsphere(detail, a);
    else
        throw std::runtime_error("Unknown blob type '" + kind + "'.");
    clipAmount = std::clamp(clipAmount, 0.0, 0.95);
    if (clipAmount > kEps)
        a = clip(a, clipAmount * 2.0 - 1.0);
    const glm::dvec3 direction = glm::length(up) > kEps ? normalized(up) : kWorldY;
    glm::dquat turn = fromTo(kWorldY, direction);
    if (std::abs(spin) > kEps)
        turn = glm::angleAxis(radians(spin), direction) * turn;
    for (glm::dvec3& p : a.points)
        p = turn * p;
    return a;
}

std::vector<BlobPlacement> placeBlobs(const std::vector<Tip>& tips, double radius, double variation, double taper, double offset, double align,
                                      double tilt, const glm::dvec3& up, int64_t seed)
{
    if (tips.empty())
        throw std::runtime_error("No branch tips to sit a canopy on.");
    if (radius <= kEps)
        radius = 600.0 * 0.28;
    double average = 0.0;
    for (const Tip& t : tips)
        average += t.radius;
    average /= double(tips.size());
    Rng rng(uint64_t(seed + 977));
    std::vector<BlobPlacement> out;
    for (const Tip& t : tips)
    {
        const double weight = average < kEps ? 1.0 : t.radius / average;
        double size = radius * (1.0 + (weight - 1.0) * taper);
        size *= 1.0 + rng.uniform(-1.0, 1.0) * variation;
        size = std::max(radius * 0.05, size);
        glm::dvec3 direction = up * (1.0 - align) + t.heading * align;
        direction = glm::length(direction) > kEps ? normalized(direction) : up;
        direction = jitter(direction, tilt, rng);
        BlobPlacement b;
        b.radius = size;
        b.centre = t.point + direction * (size * offset);
        b.up = direction;
        b.spin = rng.uniform(0.0, 360.0);
        out.push_back(b);
    }
    return out;
}

std::vector<Mesh> buildBlobs(const std::vector<BlobPlacement>& placements, const std::string& kind, int detail, double clipAmount)
{
    blobArrays(kind, detail, clipAmount, kWorldY, 0.0); // throws for a bad kind or a clip that removes everything
    std::vector<Mesh> out(placements.size());
    tbb::parallel_for(size_t(0), placements.size(), [&](size_t i) {
        const BlobPlacement& b = placements[i];
        Arrays a = blobArrays(kind, detail, clipAmount, b.up, b.spin);
        for (glm::dvec3& p : a.points)
            p = p * b.radius + b.centre;
        out[i] = toMesh(a);
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ canopy

Canopy::Canopy(const std::vector<CanopyInput>& inputs)
{
    if (inputs.empty())
        throw std::runtime_error("Pick at least one canopy mesh.");
    double total = 0.0;
    for (const CanopyInput& in : inputs)
    {
        const Mesh& m = *in.mesh;
        const int owner = int(mCenters.size());
        const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(glm::dmat3(in.world)));
        const std::vector<glm::vec3> pn = geo::pointNormals(m);
        glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        std::vector<glm::dvec3> world(m.pointCapacity());
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                world[p] = glm::dvec3(in.world * glm::dvec4(m.position(p), 1.0));
                lo = glm::min(lo, world[p]);
                hi = glm::max(hi, world[p]);
            }
        const geo::Triangulation tris = geo::triangulate(m);
        for (size_t t = 0; t < tris.faces.size(); ++t)
        {
            Tri tri;
            for (int k = 0; k < 3; ++k)
            {
                const Index p = m.cornerPoint(tris.corners[t * 3 + k]);
                tri.p[k] = world[p];
                tri.n[k] = normalMatrix * glm::dvec3(pn[p]);
                const double l = glm::length(tri.n[k]);
                if (l > kEps)
                    tri.n[k] /= l;
            }
            const double area = glm::length(glm::cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0])) * 0.5;
            if (area < kEps)
                continue;
            tri.owner = owner;
            total += area;
            mTris.push_back(tri);
            mCumulative.push_back(total);
        }
        if (m.numPoints() == 0)
            lo = hi = glm::dvec3(0.0);
        const glm::dvec3 center = (lo + hi) * 0.5;
        const glm::dvec3 spans = hi - lo;
        int widest = 0;
        for (int k = 1; k < 3; ++k)
            if (spans[k] > spans[widest])
                widest = k;
        glm::dvec3 heading = axisVector(widest, 1);
        glm::dvec3 anchor = center;
        if (in.hasAnchor)
        {
            anchor = in.anchor;
            heading = normalized(in.anchorHeading);
        }
        mCenters.push_back(center);
        mAnchors.push_back(anchor);
        mHeadings.push_back(heading);
    }
    if (mTris.empty())
        throw std::runtime_error("The canopy has no usable faces.");
    mArea = total;
}

Sample Canopy::point(Rng& rng) const
{
    const double target = rng.random() * mCumulative.back();
    size_t index = size_t(std::lower_bound(mCumulative.begin(), mCumulative.end(), target) - mCumulative.begin());
    const Tri& t = mTris[std::min(index, mTris.size() - 1)];
    double u = rng.random(), v = rng.random();
    if (u + v > 1.0)
    {
        u = 1.0 - u;
        v = 1.0 - v;
    }
    const double w = 1.0 - u - v;
    Sample s;
    s.point = t.p[0] * w + t.p[1] * u + t.p[2] * v;
    glm::dvec3 n = t.n[0] * w + t.n[1] * u + t.n[2] * v;
    if (glm::length(n) < kEps)
        n = glm::cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
    s.normal = normalized(n);
    s.center = mCenters[t.owner];
    s.anchor = mAnchors[t.owner];
    s.heading = mHeadings[t.owner];
    s.owner = t.owner;
    return s;
}

std::vector<Sample> Canopy::sample(int count, Rng& rng, double minDistance, double minNormalUp, const glm::dvec3& up, int clumps, double clumpRadius,
                                   double clumpFalloff) const
{
    constexpr double kPiLocal = kPi;
    std::vector<glm::dvec3> centres;
    if (clumps > 0)
    {
        if (clumpRadius <= kEps)
            clumpRadius = std::sqrt(mArea / (clumps * kPiLocal)) * 0.75;
        Grid grid;
        const double spacing = std::max(clumpRadius * 1.4, kEps);
        for (int i = 0; i < std::max(clumps * 60, 600) && int(centres.size()) < clumps; ++i)
        {
            const Sample s = point(rng);
            if (glm::dot(s.normal, up) < minNormalUp)
                continue;
            if (!reserve(grid, s.point, spacing, spacing))
                continue;
            centres.push_back(s.point);
        }
    }
    Grid grid;
    const bool spaced = minDistance > kEps;
    const double cell = std::max(minDistance, kEps);
    std::vector<Sample> placed;
    const int attempts = std::max(count * (centres.empty() ? 40 : 80), 800);
    for (int i = 0; i < attempts && int(placed.size()) < count; ++i)
    {
        Sample s = point(rng);
        if (glm::dot(s.normal, up) < minNormalUp)
            continue;
        if (!centres.empty())
        {
            double near = std::numeric_limits<double>::max();
            int clump = 0;
            for (size_t c = 0; c < centres.size(); ++c)
                if (const double d = glm::length(centres[c] - s.point); d < near)
                {
                    near = d;
                    clump = int(c);
                }
            if (near > clumpRadius)
                continue;
            const double density = std::pow(1.0 - near / clumpRadius, std::max(0.0, clumpFalloff));
            if (rng.random() > density)
                continue;
            s.clump = clump;
        }
        if (spaced && !reserve(grid, s.point, cell, minDistance))
            continue;
        placed.push_back(s);
    }
    return placed;
}

// ------------------------------------------------------------------------------------------------ leaf

namespace
{
Mesh compacted(const Mesh& source)
{
    Mesh m = source;
    if (m.hasDeleted())
        m.compact();
    return m;
}
} // namespace

Leaf makeLeaf(const Mesh& source, const std::string& upAxis, const std::string& forwardAxis, const std::string& pivot)
{
    Leaf leaf;
    leaf.mesh = compacted(source);
    if (leaf.mesh.numPoints() == 0)
        throw std::runtime_error("The leaf mesh has no vertices.");
    std::vector<glm::dvec3> points(leaf.mesh.positions().begin(), leaf.mesh.positions().end());
    glm::dvec3 lo = points[0], hi = points[0];
    for (const glm::dvec3& p : points)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::dvec3 span = hi - lo;
    int thin = 0, wide = 0;
    for (int k = 1; k < 3; ++k)
    {
        if (span[k] < span[thin])
            thin = k;
        if (span[k] > span[wide])
            wide = k;
    }
    if (thin == wide)
    {
        thin = 1;
        wide = 2;
    }
    const int forwardSign = std::abs(hi[wide]) >= std::abs(lo[wide]) ? 1 : -1;
    const auto [upIndex, upSign] = pickAxis(upAxis, thin, 1);
    const auto [forwardIndex, forwardDir] = pickAxis(forwardAxis, wide, forwardSign);
    if (upIndex == forwardIndex)
        throw std::runtime_error("The up and forward axes have to differ.");
    leaf.up = axisVector(upIndex, upSign);
    leaf.forward = axisVector(forwardIndex, forwardDir);
    leaf.side = glm::cross(leaf.up, leaf.forward);

    leaf.shift = glm::dvec3(0.0);
    if (pivot == "center")
        leaf.shift = (lo + hi) * 0.5;
    else if (pivot == "base")
    {
        const double base = forwardDir > 0 ? lo[forwardIndex] : hi[forwardIndex];
        const int sideIndex = 3 - upIndex - forwardIndex;
        leaf.shift = axisVector(forwardIndex, 1) * base + axisVector(upIndex, 1) * ((lo[upIndex] + hi[upIndex]) * 0.5) +
                     axisVector(sideIndex, 1) * ((lo[sideIndex] + hi[sideIndex]) * 0.5);
    }
    leaf.length = kEps;
    for (glm::dvec3& p : points)
    {
        p -= leaf.shift;
        leaf.length = std::max(leaf.length, glm::dot(p, leaf.forward));
    }
    leaf.points = std::move(points);
    return leaf;
}

Leaf makeLeaf(const Mesh& source, const Leaf& reference)
{
    Leaf leaf;
    leaf.mesh = compacted(source);
    if (leaf.mesh.numPoints() == 0)
        throw std::runtime_error("The leaf mesh has no vertices.");
    leaf.up = reference.up;
    leaf.forward = reference.forward;
    leaf.side = reference.side;
    leaf.shift = reference.shift;
    leaf.length = reference.length;
    for (const glm::dvec3& p : leaf.mesh.positions())
        leaf.points.push_back(p - leaf.shift);
    return leaf;
}

ScatterSettings scatterSettings(const json& l)
{
    ScatterSettings s;
    s.count = integer(l, "count", s.count);
    s.seed = int64_t(num(l, "seed", 0.0));
    s.size = num(l, "size", 0.0);
    auto span = [&](const char* k, Span& out) {
        if (l.contains(k))
            out = spanOf(l[k]);
    };
    span("scale", s.scale);
    span("push", s.push);
    span("spin", s.spin);
    span("tilt", s.tilt);
    span("curl", s.curl);
    span("wrap", s.wrap);
    span("gather", s.gather);
    span("align", s.align);
    s.wrapRadius = num(l, "wrap_radius", 0.0);
    s.clumps = integer(l, "clumps", 0);
    s.clumpRadius = num(l, "clump_radius", 0.0);
    s.clumpFalloff = num(l, "clump_falloff", 1.0);
    s.shadeBias = num(l, "shade_bias", 0.0);
    s.minDistance = num(l, "min_distance", 0.0);
    s.minNormalUp = num(l, "min_normal_up", -1.0);
    s.normalBlend = num(l, "normal_blend", 1.0);
    s.colorSet = text(l, "color_set", "leafShade");
    s.pivot = text(l, "pivot", "base");
    s.leafUp = text(l, "leaf_up", "auto");
    s.leafForward = text(l, "leaf_forward", "auto");
    if (auto it = l.find("combine"); it != l.end() && it->is_boolean())
        s.combine = it->get<bool>();
    return s;
}

namespace
{
/** The leaf's world axes for a sample: up along the normal, spun, tilted and swung toward the branch heading. */
void orient(const glm::dvec3& normal, double spin, double tilt, double tiltSpin, const glm::dvec3& heading, double align, LeafPlan& plan)
{
    glm::dvec3 axisUp = normal;
    glm::dvec3 axisForward = spun(axisUp, spin);
    if (std::abs(tilt) > kEps)
    {
        const glm::dvec3 hinge = spun(axisUp, tiltSpin + 90.0);
        axisUp = rotateAbout(axisUp, hinge, radians(tilt));
        axisForward = rotateAbout(axisForward, hinge, radians(tilt));
    }
    if (align > kEps)
    {
        glm::dvec3 target = heading - axisUp * glm::dot(heading, axisUp);
        if (glm::length(target) > kEps)
        {
            if (glm::dot(target, axisForward) < 0.0)
                target = -target;
            const glm::dvec3 blended = axisForward * (1.0 - align) + normalized(target) * align;
            if (glm::length(blended) > kEps)
                axisForward = blended;
        }
    }
    axisForward = normalized(axisForward - axisUp * glm::dot(axisForward, axisUp));
    plan.axisUp = axisUp;
    plan.axisForward = axisForward;
    plan.axisSide = glm::cross(axisUp, axisForward);
}
} // namespace

std::vector<LeafPlan> planLeaves(const Canopy& canopy, const Leaf& primary, const ScatterSettings& s, const glm::dvec3& up)
{
    Rng rng(uint64_t(s.seed));
    const std::vector<Sample> samples = canopy.sample(s.count, rng, s.minDistance, s.minNormalUp, up, s.clumps, s.clumpRadius, s.clumpFalloff);
    if (samples.empty())
        throw std::runtime_error("No sample points survived the filters.");
    const double base = s.size > kEps ? s.size / primary.length : 1.0;
    std::map<int, double> tints;
    std::vector<LeafPlan> plans;
    plans.reserve(samples.size());
    for (const Sample& sample : samples)
    {
        LeafPlan plan;
        plan.factor = between(rng, s.scale) * base;
        const double curlAngle = between(rng, s.curl);
        glm::dvec3 origin = sample.point + sample.normal * between(rng, s.push);
        const double pull = between(rng, s.gather);
        if (std::abs(pull) > kEps)
            origin = origin + (sample.anchor - origin) * pull;
        const int bunch = sample.clump >= 0 ? sample.clump : -1 - sample.owner;
        if (!tints.contains(bunch))
            tints[bunch] = rng.random();
        const double lit = 0.5 * glm::dot(sample.normal, up) + 0.5;
        const double exposure = lit * (1.0 - std::clamp(pull, 0.0, 1.0)) + s.shadeBias;
        plan.curl = std::abs(curlAngle) > 0.01 ? -primary.length * plan.factor / radians(curlAngle) : 0.0;
        plan.radius = s.wrapRadius > kEps ? s.wrapRadius : glm::length(sample.point - sample.center);
        const double spin = between(rng, s.spin);
        const double tilt = between(rng, s.tilt);
        const double tiltSpin = rng.uniform(0.0, 360.0);
        const double align = std::clamp(between(rng, s.align), 0.0, 1.0);
        orient(sample.normal, spin, tilt, tiltSpin, sample.heading, align, plan);
        plan.origin = origin;
        plan.wrap = std::clamp(between(rng, s.wrap), 0.0, 1.0);
        plan.normal = sample.normal;
        plan.shade = glm::vec3(float(std::clamp(exposure, 0.0, 1.0)), float(rng.random()), float(tints[bunch]));
        plans.push_back(plan);
    }
    return plans;
}

glm::dvec3 placeLeafPoint(const glm::dvec3& point, const Leaf& leaf, const LeafPlan& plan)
{
    const glm::dvec3 p = point * plan.factor;
    double height = glm::dot(p, leaf.up), reach = glm::dot(p, leaf.forward);
    const double lateral = glm::dot(p, leaf.side);
    if (plan.curl != 0.0)
    {
        const double angle = reach / plan.curl;
        const double r = plan.curl + height;
        reach = r * std::sin(angle);
        height = r * std::cos(angle) - plan.curl;
    }
    glm::dvec3 bent = leaf.up * height + leaf.forward * reach + leaf.side * lateral;
    if (plan.wrap > kEps && plan.radius > kEps)
    {
        const double h = glm::dot(bent, leaf.up);
        const glm::dvec3 planar = bent - leaf.up * h;
        const double radius = glm::length(planar);
        if (radius >= kEps)
        {
            const double angle = radius / plan.radius;
            const glm::dvec3 direction = planar / radius;
            const glm::dvec3 wrapped = (leaf.up * std::cos(angle) + direction * std::sin(angle)) * (plan.radius + h) - leaf.up * plan.radius;
            bent = bent * (1.0 - plan.wrap) + wrapped * plan.wrap;
        }
    }
    // Leaf frame coordinates onto the plan's world axes.
    const double lu = glm::dot(bent, leaf.up), lf = glm::dot(bent, leaf.forward), ls = glm::dot(bent, leaf.side);
    return plan.axisUp * lu + plan.axisForward * lf + plan.axisSide * ls + plan.origin;
}

namespace
{
/** The leaf topology once: face point lists, their uv0 and material ids. */
struct LeafTopology
{
    std::vector<std::vector<Index>> faces;
    std::vector<std::vector<glm::vec2>> uvs;
    std::vector<int32_t> materials;
    bool hasUvs = false, hasMaterials = false;
};

LeafTopology topology(const Mesh& m)
{
    LeafTopology t;
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::span<const int32_t> mat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    t.hasUvs = !uv.empty();
    t.hasMaterials = !mat.empty();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> pts;
        std::vector<glm::vec2> fuv;
        m.forEachFaceCorner(f, [&](Index c) {
            pts.push_back(m.cornerPoint(c));
            fuv.push_back(t.hasUvs ? uv[c] : glm::vec2(0.0f));
        });
        t.faces.push_back(std::move(pts));
        t.uvs.push_back(std::move(fuv));
        t.materials.push_back(t.hasMaterials ? mat[f] : 0);
    }
    return t;
}

Mesh assemble(const Leaf& leaf, const LeafTopology& topo, const std::vector<std::vector<glm::dvec3>>& placed, std::span<const LeafPlan> plans,
              size_t first, size_t count, double normalBlend, const std::string& colorSet)
{
    Mesh m;
    const size_t perLeaf = leaf.points.size();
    for (size_t i = first; i < first + count; ++i)
        for (const glm::dvec3& p : placed[i])
            m.addPoint(p);
    struct Built
    {
        Index face;
        size_t topoFace;
        Index base;
    };
    std::vector<Built> built;
    std::vector<Index> pts;
    for (size_t i = 0; i < count; ++i)
    {
        const Index base = Index(i * perLeaf);
        for (size_t f = 0; f < topo.faces.size(); ++f)
        {
            pts.clear();
            for (Index p : topo.faces[f])
                pts.push_back(p + base);
            const Index id = m.addFace(std::span<const Index>(pts));
            if (id != geo::kInvalid)
                built.push_back({id, f, base});
        }
    }
    if (topo.hasUvs)
    {
        std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (const Built& b : built)
        {
            const auto& fp = topo.faces[b.topoFace];
            m.forEachFaceCorner(b.face, [&](Index c) {
                const Index local = m.cornerPoint(c) - b.base;
                for (size_t k = 0; k < fp.size(); ++k)
                    if (fp[k] == local)
                    {
                        uv[c] = topo.uvs[b.topoFace][k];
                        break;
                    }
            });
        }
    }
    if (topo.hasMaterials)
    {
        m.materialNames() = leaf.mesh.materialNames();
        std::span<int32_t> mat = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        for (const Built& b : built)
            mat[b.face] = topo.materials[b.topoFace];
    }

    if (normalBlend > kEps)
    {
        const std::vector<glm::vec3> current = geo::pointNormals(m);
        std::vector<glm::vec3> mixed(m.pointCapacity());
        for (size_t i = 0; i < count; ++i)
        {
            const glm::dvec3 target = plans[first + i].normal;
            for (size_t k = 0; k < perLeaf; ++k)
            {
                const Index p = Index(i * perLeaf + k);
                const glm::dvec3 v = glm::dvec3(current[p]) * (1.0 - normalBlend) + target * normalBlend;
                mixed[p] = glm::length(v) < kEps ? current[p] : glm::vec3(glm::normalize(v));
            }
        }
        std::span<glm::vec3> cn = m.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        for (Index c = 0; c < m.cornerCapacity(); ++c)
            if (m.cornerValid(c))
                cn[c] = mixed[m.cornerPoint(c)];
    }

    if (!colorSet.empty())
    {
        std::vector<glm::vec4> perPoint(m.pointCapacity(), glm::vec4(1.0f));
        for (size_t i = 0; i < count; ++i)
            for (size_t k = 0; k < perLeaf; ++k)
                perPoint[i * perLeaf + k] = glm::vec4(plans[first + i].shade, 1.0f);
        geo::colors::add(m, colorSet, glm::vec4(1.0f));
        geo::colors::setFromPoints(m, colorSet, perPoint);
        geo::colors::setCurrent(m, colorSet);
    }
    return m;
}
} // namespace

std::vector<Mesh> emitLeaves(const Leaf& leaf, const std::vector<LeafPlan>& plans, bool combine, double normalBlend, const std::string& colorSet)
{
    const LeafTopology topo = topology(leaf.mesh);
    std::vector<std::vector<glm::dvec3>> placed(plans.size());
    tbb::parallel_for(size_t(0), plans.size(), [&](size_t i) {
        placed[i].reserve(leaf.points.size());
        for (const glm::dvec3& p : leaf.points)
            placed[i].push_back(placeLeafPoint(p, leaf, plans[i]));
    });
    std::vector<Mesh> out;
    if (combine)
    {
        out.push_back(assemble(leaf, topo, placed, plans, 0, plans.size(), normalBlend, colorSet));
        return out;
    }
    out.resize(plans.size());
    tbb::parallel_for(size_t(0), plans.size(), [&](size_t i) { out[i] = assemble(leaf, topo, placed, plans, i, 1, normalBlend, colorSet); });
    return out;
}

// ------------------------------------------------------------------------------------------------ leaf plane

Arrays leafPlaneArrays(double length, double width, int segments, int cross, double taper, bool doubleSided)
{
    length = length > kEps ? length : 12.0;
    width = width > kEps ? width : 5.0;
    segments = std::max(1, segments);
    cross = std::max(1, cross);
    taper = std::clamp(taper, 0.0, 1.0);
    Arrays a;
    for (int row = 0; row <= segments; ++row)
    {
        const double alongV = double(row) / segments;
        // Widest around the middle, narrowing to nothing at the tip.
        const double shape = std::sin(kPi * (0.25 + 0.75 * alongV));
        const double span = width * ((1.0 - taper) + taper * shape);
        for (int column = 0; column <= cross; ++column)
        {
            const double across = double(column) / cross;
            a.points.push_back({(across - 0.5) * span, 0.0, alongV * length});
            a.us.push_back(across);
            a.vs.push_back(alongV);
        }
    }
    const Index stride = Index(cross + 1);
    auto quad = [&](Index o, int row, int column, bool flip) {
        Index q[4] = {o + row * stride + column, o + (row + 1) * stride + column, o + (row + 1) * stride + column + 1, o + row * stride + column + 1};
        if (flip)
            std::swap(q[0], q[3]), std::swap(q[1], q[2]);
        a.counts.push_back(4);
        for (Index v : q)
        {
            a.connects.push_back(v);
            a.uvIds.push_back(v);
        }
    };
    for (int row = 0; row < segments; ++row)
        for (int column = 0; column < cross; ++column)
            quad(0, row, column, false);
    if (doubleSided)
    {
        const Index flipped = Index(a.points.size());
        for (Index i = 0; i < flipped; ++i)
        {
            a.points.push_back(a.points[i]);
            a.us.push_back(a.us[i]);
            a.vs.push_back(a.vs[i]);
        }
        for (int row = 0; row < segments; ++row)
            for (int column = 0; column < cross; ++column)
                quad(flipped, row, column, true);
    }
    return a;
}

// ------------------------------------------------------------------------------------------------ maps

namespace
{
using P2 = std::pair<double, double>;

std::array<P2, 3> bleed(std::array<P2, 3> tri, double padding)
{
    if (padding <= 0.0)
        return tri;
    const double mu = (tri[0].first + tri[1].first + tri[2].first) / 3.0;
    const double mv = (tri[0].second + tri[1].second + tri[2].second) / 3.0;
    for (P2& p : tri)
    {
        const double du = p.first - mu, dv = p.second - mv;
        const double span = std::sqrt(du * du + dv * dv);
        if (span < kEps)
            continue;
        const double step = (span + padding) / span;
        p = {mu + du * step, mv + dv * step};
    }
    return tri;
}

void scanline(std::vector<uint8_t>& cover, int size, std::array<P2, 3> tri)
{
    std::stable_sort(tri.begin(), tri.end(), [](const P2& a, const P2& b) { return a.second < b.second; });
    const auto [ax, ay] = tri[0];
    const auto [bx, by] = tri[1];
    const auto [cx, cy] = tri[2];
    if (cy - ay < kEps)
        return;
    const int first = std::max(0, int(std::floor(ay)));
    const int last = std::min(size - 1, int(std::ceil(cy)));
    for (int y = first; y <= last; ++y)
    {
        const double middle = y + 0.5;
        if (middle < ay || middle > cy)
            continue;
        const double longEdge = ax + (cx - ax) * (middle - ay) / (cy - ay);
        double shortEdge;
        if (middle < by && by - ay > kEps)
            shortEdge = ax + (bx - ax) * (middle - ay) / (by - ay);
        else if (cy - by > kEps)
            shortEdge = bx + (cx - bx) * (middle - by) / (cy - by);
        else
            shortEdge = bx;
        const double left = std::min(longEdge, shortEdge), right = std::max(longEdge, shortEdge);
        const int start = std::max(0, int(std::floor(left)));
        const int end = std::min(size - 1, int(std::ceil(right)));
        if (end < start)
            continue;
        std::fill(cover.begin() + size_t(y) * size + start, cover.begin() + size_t(y) * size + end + 1, uint8_t(255));
    }
}
} // namespace

std::vector<uint8_t> opacityMask(const Mesh& m, int size, double padding)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    if (uv.empty())
        throw std::runtime_error("The leaf mesh has no UVs to rasterise.");
    size = std::max(16, size);
    if (padding <= 0.0)
        padding = std::max(2.0, size / 256.0);
    std::vector<uint8_t> cover(size_t(size) * size, 0);
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<P2> pts;
        m.forEachFaceCorner(f, [&](Index c) { pts.emplace_back(uv[c].x * size, (1.0 - uv[c].y) * size); });
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            scanline(cover, size, bleed({pts[0], pts[k], pts[k + 1]}, padding));
    }
    return cover;
}

json spread(std::vector<double> d)
{
    std::sort(d.begin(), d.end());
    const size_t count = d.size();
    if (!count)
        return {{"count", 0}, {"out", 0.0}, {"in", 0.0}, {"out99", 0.0}, {"in99", 0.0}};
    return {{"count", count},
            {"out", std::max(0.0, d.back())},
            {"in", std::max(0.0, -d.front())},
            {"out99", std::max(0.0, d[std::min(count - 1, size_t(double(count) * 0.99))])},
            {"in99", std::max(0.0, -d[size_t(double(count) * 0.01)])}};
}
} // namespace treegen
