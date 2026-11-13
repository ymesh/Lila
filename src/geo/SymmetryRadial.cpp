#include "geo/Symmetry.h"

#include "geo/Bvh.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <unordered_map>

namespace lila::geo
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kSlotBits = 256;

struct WideCellHash
{
    size_t operator()(const glm::i64vec3& c) const
    {
        return size_t(c.x * 73856093ll ^ c.y * 19349663ll ^ c.z * 83492791ll);
    }
};

/** Values this close to 0, 1/2 or 1 are those exactly, so quarter and sixth turns about a coordinate axis stay exact. */
double snapUnit(double v)
{
    for (double t : {0.0, 0.5, 1.0, -0.5, -1.0})
        if (std::abs(v - t) < 1e-12)
            return t;
    return v;
}

glm::dvec3 snapUnit(glm::dvec3 v)
{
    for (int i = 0; i < 3; ++i)
        v[i] = snapUnit(v[i]);
    return v;
}

int coordinateAxis(const glm::dvec3& v)
{
    for (int k = 0; k < 3; ++k)
        if (v[k] == 1.0 && v[(k + 1) % 3] == 0.0 && v[(k + 2) % 3] == 0.0)
            return k;
    return -1;
}

glm::dmat3 rotation(const glm::dvec3& a, double angle)
{
    const double c = snapUnit(std::cos(angle)), s = snapUnit(std::sin(angle));
    glm::dmat3 r(0.0);
    const glm::dvec3 cross[3] = {{0.0, a.z, -a.y}, {-a.z, 0.0, a.x}, {a.y, -a.x, 0.0}};
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            r[col][row] = snapUnit((col == row ? c : 0.0) + s * cross[col][row] + (1.0 - c) * a[row] * a[col]);
    return r;
}

/** The point p sits nearest within tolerance, among those in the grid (p itself included). */
struct PointGrid
{
    double cell = 1.0;
    std::unordered_map<glm::i64vec3, std::vector<Index>, WideCellHash> cells;

    glm::i64vec3 key(const glm::dvec3& p) const { return glm::i64vec3(glm::floor(p / cell)); }

    template <class Accept> Index nearest(const Mesh& mesh, const glm::dvec3& target, double tolerance, Accept accept) const
    {
        const glm::i64vec3 k = key(target);
        double best = tolerance * tolerance;
        Index found = kInvalid;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    auto it = cells.find(k + glm::i64vec3(dx, dy, dz));
                    if (it == cells.end())
                        continue;
                    for (Index q : it->second)
                    {
                        if (!accept(q))
                            continue;
                        const glm::dvec3 d = mesh.position(q) - target;
                        const double d2 = glm::dot(d, d);
                        if (d2 <= best && (d2 < best || q < found))
                        {
                            best = d2;
                            found = q;
                        }
                    }
                }
        return found;
    }
};

PointGrid makeGrid(const Mesh& mesh, double tolerance, std::span<const Index> points)
{
    PointGrid grid;
    grid.cell = std::max(tolerance * 2.0, 1e-9);
    for (Index p : points)
        grid.cells[grid.key(mesh.position(p))].push_back(p);
    return grid;
}

/** The element whose image of the orbit lies at the smallest angle: the orbit's member in the fundamental wedge. */
int canonicalElement(const Mesh& mesh, const RadialSymmetry& radial, std::span<const Index> images)
{
    int best = 0;
    double bestAngle = std::numeric_limits<double>::max();
    for (int g = 0; g < int(images.size()); ++g)
    {
        double a = radial.angle(mesh.position(images[g]));
        if (a > 2.0 * kPi - 1e-9)
            a = 0.0;
        if (a < bestAngle - 1e-12)
        {
            bestAngle = a;
            best = g;
        }
    }
    return best;
}

glm::dvec3 faceCentre(const Mesh& mesh, Index f)
{
    glm::dvec3 sum(0.0);
    Index n = 0;
    mesh.forEachFaceCorner(f, [&](Index c) {
        sum += mesh.position(mesh.cornerPoint(c));
        ++n;
    });
    return n ? sum / double(n) : sum;
}

/**
 * A face isomorphism grown outward from corner c0 landing on corner d0: walking forward round both faces, or with
 * reversing, walking the image backward (an orientation-reversing map such as a reflection).
 */
struct Isomorphism
{
    std::vector<Index> point, pointInv, face, faceInv;

    explicit Isomorphism(const Mesh& mesh)
        : point(mesh.pointCapacity(), kInvalid), pointInv(mesh.pointCapacity(), kInvalid), face(mesh.faceCapacity(), kInvalid),
          faceInv(mesh.faceCapacity(), kInvalid)
    {
    }

    bool grow(const Mesh& mesh, Index c0, Index d0, bool reversing)
    {
        auto map = [&](Index a, Index b) {
            if (point[a] == kInvalid && pointInv[b] == kInvalid)
            {
                point[a] = b;
                pointInv[b] = a;
                return true;
            }
            return point[a] == b && pointInv[b] == a;
        };
        std::deque<std::pair<Index, Index>> queue{{c0, d0}};
        while (!queue.empty())
        {
            const auto [c0f, d0f] = queue.front();
            queue.pop_front();
            const Index f = mesh.cornerFace(c0f);
            const Index g = mesh.cornerFace(d0f);
            if (face[f] != kInvalid)
            {
                if (face[f] != g)
                    return false;
                continue;
            }
            if (mesh.faceSize(f) != mesh.faceSize(g) || faceInv[g] != kInvalid)
                return false;
            face[f] = g;
            faceInv[g] = f;
            Index c = c0f, d = d0f;
            for (Index i = 0; i < mesh.faceSize(f); ++i)
            {
                if (!map(mesh.cornerPoint(c), mesh.cornerPoint(d)))
                    return false;
                const Index dn = reversing ? mesh.cornerPrev(d) : mesh.cornerNext(d);
                const Index e = mesh.cornerEdge(c);
                // The image of the edge from c to its next corner.
                const Index image = reversing ? dn : d;
                if (mesh.isManifoldEdge(e) && mesh.isManifoldEdge(mesh.cornerEdge(image)))
                {
                    const Index r = mesh.cornerRadial(c);
                    const Index rm = mesh.cornerRadial(image);
                    if (face[mesh.cornerFace(r)] == kInvalid)
                        queue.push_back({r, reversing ? mesh.cornerNext(rm) : rm});
                }
                c = mesh.cornerNext(c);
                d = dn;
            }
        }
        return true;
    }
};

/** The corner of the face nearest g's image of c0's face that best lines up with g's image of the walk from c0. */
Index imageCorner(const Mesh& mesh, const RadialSymmetry& radial, int g, Index c0, const std::vector<Index>& taken)
{
    const Index f = mesh.cornerFace(c0);
    const glm::dvec3 target = radial.apply(g, faceCentre(mesh, f));
    Index face = kInvalid;
    double best = std::numeric_limits<double>::max();
    for (Index h = 0; h < mesh.faceCapacity(); ++h)
    {
        if (!mesh.faceValid(h) || mesh.faceSize(h) != mesh.faceSize(f) || taken[h] != kInvalid)
            continue;
        const glm::dvec3 d = faceCentre(mesh, h) - target;
        if (glm::dot(d, d) < best)
        {
            best = glm::dot(d, d);
            face = h;
        }
    }
    if (face == kInvalid)
        return kInvalid;
    const bool reversing = radial.reflects(g);
    Index corner = kInvalid;
    best = std::numeric_limits<double>::max();
    mesh.forEachFaceCorner(face, [&](Index d0) {
        double err = 0.0;
        Index c = c0, d = d0;
        for (Index i = 0; i < mesh.faceSize(f); ++i)
        {
            const glm::dvec3 diff = mesh.position(mesh.cornerPoint(d)) - radial.apply(g, mesh.position(mesh.cornerPoint(c)));
            err += glm::dot(diff, diff);
            c = mesh.cornerNext(c);
            d = reversing ? mesh.cornerPrev(d) : mesh.cornerNext(d);
        }
        if (err < best)
        {
            best = err;
            corner = d0;
        }
    });
    return corner;
}

/** The point map of element g grown over every face, seeded at c0 and then at each shell's face it has not reached. */
std::vector<Index> topologicalImage(const Mesh& mesh, const RadialSymmetry& radial, int g, Index c0)
{
    Isomorphism iso(mesh);
    auto seed = [&](Index c) {
        const Index d = imageCorner(mesh, radial, g, c, iso.faceInv);
        return d != kInvalid && iso.grow(mesh, c, d, radial.reflects(g));
    };
    if (!seed(c0))
        return {};
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && iso.face[f] == kInvalid && !seed(mesh.faceCorner(f)))
            return {};
    return std::move(iso.point);
}

std::string signature(const RadialSymmetry& r)
{
    // Rounded so a reference re-derived from the same settings names the same layer.
    auto q = [](double v) { return int64_t(std::llround(v * 1e6)); };
    const glm::dvec3 a = r.axis(), f = r.reference();
    const uint64_t h = uint64_t(q(a.x) * 73856093ll ^ q(a.y) * 19349663ll ^ q(a.z) * 83492791ll ^ q(f.x) * 2654435761ll ^ q(f.y) * 40503ll ^
                                q(f.z) * 1099511628211ll);
    return std::format("{:08x}", uint32_t(h ^ (h >> 32)));
}
} // namespace

RadialSymmetry::RadialSymmetry(const glm::dvec3& origin, const glm::dvec3& axis, int count, bool mirror, const glm::dvec3& reference)
    : mOrigin(origin), mCount(std::clamp(count, 1, kMaxRadialCount)), mMirror(mirror)
{
    mAxis = glm::length(axis) > 1e-12 ? snapUnit(glm::normalize(axis)) : glm::dvec3(0.0, 1.0, 0.0);
    mAxisIndex = coordinateAxis(mAxis);
    glm::dvec3 ref = reference - mAxis * glm::dot(reference, mAxis);
    mReference = glm::length(ref) > 1e-9 ? snapUnit(glm::normalize(ref)) : radialReference(mAxis, 0.0);
    mLinear.clear();
    for (int k = 0; k < mCount; ++k)
        mLinear.push_back(rotation(mAxis, 2.0 * kPi * k / mCount));
    if (mMirror)
    {
        const glm::dvec3 m = glm::normalize(glm::cross(mAxis, mReference));
        glm::dmat3 s(1.0);
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                s[col][row] = snapUnit(s[col][row] - 2.0 * m[row] * m[col]);
        for (int k = 0; k < mCount; ++k)
        {
            glm::dmat3 r = mLinear[size_t(k)] * s;
            for (int col = 0; col < 3; ++col)
                r[col] = snapUnit(r[col]);
            mLinear.push_back(r);
        }
    }
}

int RadialSymmetry::compose(int a, int b) const
{
    const bool ra = a >= mCount, rb = b >= mCount;
    const int ka = ra ? a - mCount : a, kb = rb ? b - mCount : b;
    const int k = ((ra ? ka - kb : ka + kb) % mCount + mCount) % mCount;
    return ra != rb ? mCount + k : k;
}

int RadialSymmetry::inverse(int g) const
{
    return g >= mCount ? g : (mCount - g) % mCount;
}

glm::dvec3 RadialSymmetry::apply(int g, const glm::dvec3& p) const
{
    return g == 0 ? p : mOrigin + mLinear[size_t(g)] * (p - mOrigin);
}

double RadialSymmetry::angle(const glm::dvec3& p) const
{
    const glm::dvec3 d = p - mOrigin;
    double a = std::atan2(glm::dot(d, glm::cross(mAxis, mReference)), glm::dot(d, mReference));
    if (a < 0.0)
        a += 2.0 * kPi;
    return a >= 2.0 * kPi ? 0.0 : a;
}

double RadialSymmetry::wedgeAngle() const
{
    return (mMirror ? kPi : 2.0 * kPi) / mCount;
}

int RadialSymmetry::wedgeOf(const glm::dvec3& p) const
{
    const int sector = std::clamp(int(angle(p) / wedgeAngle()), 0, mMirror ? 2 * mCount - 1 : mCount - 1);
    if (!mMirror)
        return sector;
    // Even sectors are the fundamental wedge turned; odd ones its reflection, which runs the angle backward.
    return sector % 2 == 0 ? sector / 2 : mCount + ((sector + 1) / 2) % mCount;
}

double RadialSymmetry::axisDistance(const glm::dvec3& p) const
{
    const glm::dvec3 d = p - mOrigin;
    return glm::length(d - mAxis * glm::dot(d, mAxis));
}

glm::dvec3 RadialSymmetry::projectToAxis(const glm::dvec3& p) const
{
    if (mAxisIndex >= 0)
    {
        glm::dvec3 out = mOrigin;
        out[mAxisIndex] = p[mAxisIndex];
        return out;
    }
    return mOrigin + mAxis * glm::dot(p - mOrigin, mAxis);
}

SymmetryPlane RadialSymmetry::mirrorPlane(int g) const
{
    const int k = g >= mCount ? g - mCount : g;
    const double a = kPi * k / mCount;
    const glm::dvec3 dir = snapUnit(std::cos(a) * mReference + std::sin(a) * glm::cross(mAxis, mReference));
    SymmetryPlane plane;
    plane.normal = snapUnit(glm::normalize(glm::cross(mAxis, dir)));
    plane.offset = glm::dot(plane.normal, mOrigin);
    return plane;
}

glm::dvec3 RadialSymmetry::projectFixed(const glm::dvec3& p, std::span<const int> stabilizer) const
{
    int reflection = -1;
    for (int g : stabilizer)
    {
        if (g > 0 && g < mCount)
            return projectToAxis(p);
        if (g >= mCount)
        {
            if (reflection >= 0 && reflection != g)
                return projectToAxis(p);
            reflection = g;
        }
    }
    if (reflection < 0)
        return p;
    const SymmetryPlane plane = mirrorPlane(reflection);
    if (const int k = coordinateAxis(glm::abs(plane.normal)); k >= 0)
    {
        glm::dvec3 out = p;
        out[k] = plane.offset * plane.normal[k];
        return out;
    }
    return plane.project(p);
}

glm::dvec3 radialReference(const glm::dvec3& axis, double phaseDegrees)
{
    const glm::dvec3 a = glm::length(axis) > 1e-12 ? glm::normalize(axis) : glm::dvec3(0.0, 1.0, 0.0);
    const int k = coordinateAxis(glm::abs(snapUnit(a)));
    glm::dvec3 base(0.0);
    if (k >= 0)
        base[(k + 1) % 3] = 1.0;
    else
    {
        // The coordinate axis least aligned with a custom axis, made perpendicular to it.
        const glm::dvec3 m = glm::abs(a);
        base[m.x <= m.y && m.x <= m.z ? 0 : m.y <= m.z ? 1 : 2] = 1.0;
        base = glm::normalize(base - a * glm::dot(base, a));
    }
    const double t = phaseDegrees * kPi / 180.0;
    return snapUnit(base * snapUnit(std::cos(t)) + glm::cross(a, base) * snapUnit(std::sin(t)));
}

RadialSymmetry axisRadial(int axis, const glm::dvec3& origin, int count, bool mirror, double phaseDegrees)
{
    glm::dvec3 a(0.0);
    a[std::clamp(axis, 0, 2)] = 1.0;
    return RadialSymmetry(origin, a, count, mirror, radialReference(a, phaseDegrees));
}

std::string radialLayerName(const RadialSymmetry& radial)
{
    const int k = radial.axisIndex();
    std::string name = std::format("symRadial{}{}{}", radial.count(), radial.mirror() ? "D" : "", k >= 0 ? char('X' + k) : 'C');
    if (k < 0 || radial.reference() != radialReference(radial.axis(), 0.0))
        name += "_" + signature(radial);
    return name;
}

bool isSymmetryLayer(const std::string& name)
{
    return name.starts_with("symMirror") || name.starts_with("symRadial");
}

void removeSymmetryLayers(Mesh& mesh)
{
    for (const std::string& name : mesh.pointAttrs().names())
        if (isSymmetryLayer(name))
            mesh.pointAttrs().remove(name);
}

const RadialSymmetry& SymmetryMap::radial() const
{
    static const RadialSymmetry none;
    return mRadial ? *mRadial : none;
}

int SymmetryMap::order() const
{
    return mRadial ? mRadial->order() : 2;
}

Index SymmetryMap::image(int g, Index p) const
{
    if (!mRadial)
        return g == 0 ? p : mirrorPoint(p);
    if (p >= mOrbit.size() || mOrbit[p] == kInvalid)
        return kInvalid;
    return mOrbitPoints[size_t(mOrbit[p]) * size_t(order()) + size_t(mRadial->compose(g, mSlot[p]))];
}

Index SymmetryMap::orbitPoint(Index p, int g) const
{
    if (!mRadial)
    {
        const Index q = mirrorPoint(p);
        if (q == kInvalid)
            return g == 0 ? p : kInvalid;
        return (g == 0) == (side(p) >= 0) ? p : q;
    }
    if (p >= mOrbit.size() || mOrbit[p] == kInvalid)
        return kInvalid;
    return mOrbitPoints[size_t(mOrbit[p]) * size_t(order()) + size_t(g)];
}

Index SymmetryMap::imageEdge(const Mesh& mesh, int g, Index e) const
{
    if (!mesh.edgeValid(e))
        return kInvalid;
    const Index a = image(g, mesh.edgePoint(e, 0));
    const Index b = image(g, mesh.edgePoint(e, 1));
    return a == kInvalid || b == kInvalid ? kInvalid : mesh.findEdge(a, b);
}

Index SymmetryMap::imageFace(const Mesh& mesh, int g, Index f) const
{
    if (!mesh.faceValid(f))
        return kInvalid;
    const Index e = imageEdge(mesh, g, mesh.cornerEdge(mesh.faceCorner(f)));
    if (e == kInvalid)
        return kInvalid;
    Index found = kInvalid;
    mesh.forEachEdgeCorner(e, [&](Index r) {
        const Index h = mesh.cornerFace(r);
        if (found != kInvalid || mesh.faceSize(h) != mesh.faceSize(f))
            return;
        bool all = true;
        mesh.forEachFaceCorner(f, [&](Index fc) {
            const Index m = image(g, mesh.cornerPoint(fc));
            all &= m != kInvalid && mesh.findCorner(h, m) != kInvalid;
        });
        if (all)
            found = h;
    });
    return found;
}

glm::dvec3 SymmetryMap::transform(int g, const glm::dvec3& p) const
{
    if (mRadial)
        return mRadial->apply(g, p);
    return g == 0 ? p : reflected(p);
}

glm::dvec3 SymmetryMap::transformVector(int g, const glm::dvec3& v) const
{
    if (mRadial)
        return mRadial->applyVector(g, v);
    return g == 0 ? v : v - 2.0 * glm::dot(v, mPlane.normal) * mPlane.normal;
}

bool SymmetryMap::reflects(int g) const
{
    return mRadial ? mRadial->reflects(g) : g == 1;
}

int SymmetryMap::inverse(int g) const
{
    return mRadial ? mRadial->inverse(g) : g;
}

int SymmetryMap::compose(int a, int b) const
{
    return mRadial ? mRadial->compose(a, b) : a ^ b;
}

int SymmetryMap::wedge(Index p) const
{
    if (!mRadial)
        return side(p) > 0 ? 0 : side(p) < 0 ? 1 : -1;
    if (p >= mOrbit.size() || mOrbit[p] == kInvalid || mFixed[p])
        return -1;
    return mSlot[p];
}

int SymmetryMap::majorityWedge(std::span<const Index> points) const
{
    std::vector<int64_t> count(size_t(order()), 0);
    for (Index p : points)
        if (const int w = wedge(p); w >= 0)
            ++count[size_t(w)];
    return int(std::max_element(count.begin(), count.end()) - count.begin());
}

std::vector<std::vector<Index>> SymmetryMap::wedgeGroups(std::span<const Index> points) const
{
    if (std::ranges::any_of(points, [&](Index p) { return fixed(p); }))
        return {std::vector<Index>(points.begin(), points.end())};
    std::vector<std::vector<Index>> groups(static_cast<size_t>(order()));
    const int major = majorityWedge(points);
    for (Index p : points)
    {
        const int w = wedge(p);
        groups[size_t(w >= 0 ? w : major)].push_back(p);
    }
    std::erase_if(groups, [](const std::vector<Index>& g) { return g.empty(); });
    return groups;
}

bool SymmetryMap::drives(Index p, int w) const
{
    const Index q = orbitPoint(p, w);
    return q == p || q == kInvalid;
}

bool SymmetryMap::fixed(Index p) const
{
    if (!mRadial)
        return p < mOnPlane.size() && mOnPlane[p];
    return p < mFixed.size() && mFixed[p];
}

std::vector<int> SymmetryMap::stabilizer(Index orbit) const
{
    std::vector<int> out;
    const size_t base = size_t(orbit) * size_t(order());
    for (int g = 0; g < order(); ++g)
        if (mOrbitPoints[base + size_t(g)] == mOrbitPoints[base])
            out.push_back(g);
    return out;
}

glm::dvec3 SymmetryMap::constrain(Index p, const glm::dvec3& pos) const
{
    if (!fixed(p))
        return pos;
    if (!mRadial)
    {
        glm::dvec3 v = pos;
        if (mAxis >= 0)
            v[mAxis] = mPlane.offset;
        else
            v = mPlane.project(v);
        return v;
    }
    // The slot 0 point's stabilizer, carried to p's slot: conjugate by the element that took slot 0 to p.
    const int s = mSlot[p];
    std::vector<int> stab;
    for (int h : stabilizer(mOrbit[p]))
        stab.push_back(mRadial->compose(s, mRadial->compose(h, mRadial->inverse(s))));
    return mRadial->projectFixed(pos, stab);
}

void SymmetryMap::enforceWedge(Mesh& mesh, std::span<const Index> points, int w) const
{
    if (!mRadial)
    {
        enforce(mesh, points, w == 1 ? -1 : 1);
        return;
    }
    LILA_ZONE();

    const int n = order();
    w = std::clamp(w, 0, n - 1);
    std::vector<uint8_t> done(mOrbitPoints.size() / size_t(n), 0);
    for (Index p : points)
    {
        if (!mesh.pointValid(p) || p >= mOrbit.size() || mOrbit[p] == kInvalid || done[mOrbit[p]])
            continue;
        const Index o = mOrbit[p];
        done[o] = 1;
        const size_t base = size_t(o) * size_t(n);
        const Index driver = mOrbitPoints[base + size_t(w)];
        if (!mesh.pointValid(driver))
            continue;
        glm::dvec3 rep = mRadial->apply(mRadial->inverse(w), mesh.position(driver));
        if (mFixed[driver])
            rep = mRadial->projectFixed(rep, stabilizer(o));
        // A point filling several slots (on the axis or a mirror plane) is written once, from its first slot.
        for (int g = 0; g < n; ++g)
        {
            const Index q = mOrbitPoints[base + size_t(g)];
            if (mesh.pointValid(q) && mSlot[q] == g)
                mesh.setPosition(q, mRadial->apply(g, rep));
        }
    }
}

void SymmetryMap::symmetrizeWedge(Mesh& mesh, int w) const
{
    std::vector<Index> all;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            all.push_back(p);
    enforceWedge(mesh, all, w);
}

std::vector<Index> SymmetryMap::unmatchedPoints(const Mesh& mesh) const
{
    std::vector<Index> out;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && image(1 % order(), p) == kInvalid)
            out.push_back(p);
    return out;
}

void SymmetryMap::averageOrbits(std::span<glm::dvec3> values, bool positions) const
{
    const int n = order();
    std::vector<Index> members(static_cast<size_t>(n));
    for (Index p = 0; p < values.size(); ++p)
    {
        // Each orbit once, from its slot 0 point.
        if (orbitPoint(p, 0) != p)
            continue;
        bool ok = true;
        for (int g = 0; g < n && ok; ++g)
        {
            members[size_t(g)] = orbitPoint(p, g);
            ok = members[size_t(g)] != kInvalid && members[size_t(g)] < values.size();
        }
        if (!ok)
            continue;
        glm::dvec3 sum(0.0);
        for (int g = 0; g < n; ++g)
            sum += positions ? transform(inverse(g), values[members[size_t(g)]]) : transformVector(inverse(g), values[members[size_t(g)]]);
        glm::dvec3 avg = sum / double(n);
        if (positions)
            avg = constrain(p, avg);
        for (int g = 0; g < n; ++g)
            values[members[size_t(g)]] = positions ? transform(g, avg) : transformVector(g, avg);
    }
}

bool SymmetryMap::addOrbit(const Mesh& mesh, std::span<const Index> images, int repElement)
{
    const int n = order();
    for (Index q : images)
        if (q == kInvalid || !mesh.pointValid(q) || mOrbit[q] != kInvalid)
            return false;
    const Index orbit = Index(mOrbitPoints.size() / size_t(n));
    const size_t base = mOrbitPoints.size();
    for (int g = 0; g < n; ++g)
        mOrbitPoints.push_back(images[size_t(mRadial->compose(g, repElement))]);
    for (int g = 0; g < n; ++g)
    {
        const Index q = mOrbitPoints[base + size_t(g)];
        if (mOrbit[q] == orbit)
        {
            mFixed[q] = 1;
            continue;
        }
        mOrbit[q] = orbit;
        mSlot[q] = int16_t(g);
    }
    return true;
}

int SymmetryMap::storedSlot(Index p) const
{
    if (mSlot[p] != 0 || !mFixed[p])
        return mSlot[p];
    // The orbit's slot 0 point on a mirror plane stores the reflection that fixes it, which reading needs.
    for (int h : stabilizer(mOrbit[p]))
        if (mRadial->reflects(h))
            return h;
    return 0;
}

void SymmetryMap::countUnmatched(const Mesh& mesh)
{
    mUnmatched = 0;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && mOrbit[p] == kInvalid)
            ++mUnmatched;
}

SymmetryMap SymmetryMap::buildRadial(const Mesh& mesh, const RadialSymmetry& radial, double tolerance)
{
    LILA_ZONE();

    SymmetryMap map;
    map.mRadial = radial;
    map.mTolerance = tolerance;
    const Index pc = mesh.pointCapacity();
    const int n = radial.order();
    map.mOrbit.assign(pc, kInvalid);
    map.mSlot.assign(pc, -1);
    map.mFixed.assign(pc, 0);

    std::vector<Index> valid;
    for (Index p = 0; p < pc; ++p)
        if (mesh.pointValid(p))
            valid.push_back(p);
    const PointGrid grid = makeGrid(mesh, tolerance, valid);
    std::vector<Index> found(size_t(pc) * size_t(n), kInvalid);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, valid.size(), 2048), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            const Index p = valid[i];
            const glm::dvec3 pos = mesh.position(p);
            const bool onAxis = radial.axisDistance(pos) <= tolerance;
            for (int g = 0; g < n; ++g)
                found[size_t(p) * size_t(n) + size_t(g)] =
                    g == 0 || (onAxis && !radial.reflects(g)) ? p : grid.nearest(mesh, radial.apply(g, pos), tolerance, [](Index) { return true; });
        }
    });
    for (Index p : valid)
    {
        if (map.mOrbit[p] != kInvalid)
            continue;
        std::span<const Index> images(found.data() + size_t(p) * size_t(n), size_t(n));
        if (std::ranges::find(images, kInvalid) != images.end())
            continue;
        map.addOrbit(mesh, images, canonicalElement(mesh, radial, images));
    }
    map.countUnmatched(mesh);
    return map;
}

SymmetryMap SymmetryMap::buildRadialTopological(const Mesh& mesh, Index seedEdge, const RadialSymmetry& radial)
{
    LILA_ZONE();

    SymmetryMap map;
    map.mRadial = radial;
    if (!mesh.edgeValid(seedEdge) || mesh.edgeCorner(seedEdge) == kInvalid)
        return map;
    const Index pc = mesh.pointCapacity();
    const int count = radial.count(), n = radial.order();
    const Index seed = mesh.edgeCorner(seedEdge);
    std::vector<Index> turn, flip;
    if (count > 1 && (turn = topologicalImage(mesh, radial, 1, seed)).empty())
        return map;
    if (radial.mirror() && (flip = topologicalImage(mesh, radial, count, seed)).empty())
        return map;

    map.mOrbit.assign(pc, kInvalid);
    map.mSlot.assign(pc, -1);
    map.mFixed.assign(pc, 0);
    map.mTolerance = std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
    std::vector<Index> images(static_cast<size_t>(n));
    for (Index p = 0; p < pc; ++p)
    {
        if (!mesh.pointValid(p) || map.mOrbit[p] != kInvalid)
            continue;
        bool ok = true;
        Index q = p;
        for (int k = 0; k < count && ok; ++k)
        {
            images[size_t(k)] = q;
            q = count > 1 ? turn[q] : q;
            ok = q != kInvalid;
        }
        // The turns must close up after count steps.
        ok = ok && q == p;
        if (ok && radial.mirror())
        {
            Index r = flip[p];
            ok = r != kInvalid && flip[r] == p;
            for (int k = 0; k < count && ok; ++k)
            {
                images[size_t(count + k)] = r;
                r = count > 1 ? turn[r] : r;
                ok = r != kInvalid;
            }
        }
        if (ok)
            map.addOrbit(mesh, images, canonicalElement(mesh, radial, images));
    }
    map.countUnmatched(mesh);
    return map;
}

SymmetryMap SymmetryMap::persistent(Mesh& mesh, const RadialSymmetry& radial, double tolerance, SymmetrySync* sync)
{
    LILA_ZONE();

    SymmetryMap map;
    map.mRadial = radial;
    map.mTolerance = tolerance;
    const Index pc = mesh.pointCapacity();
    const int count = radial.count(), n = radial.order();
    map.mOrbit.assign(pc, kInvalid);
    map.mSlot.assign(pc, -1);
    map.mFixed.assign(pc, 0);
    SymmetrySync info;

    std::span<int32_t> keys = mesh.pointAttrs().getOrAdd<int32_t>(radialLayerName(radial));
    auto pos = [&](Index p) { return mesh.position(p); };

    // Ordered by stored orbit id, so the same layer always resolves the same way.
    std::map<int64_t, std::vector<std::pair<int, Index>>> groups;
    std::vector<Index> axisPoints;
    int64_t nextId = 0;
    for (Index p = 0; p < pc; ++p)
    {
        if (!mesh.pointValid(p) || keys[p] == 0)
            continue;
        if (keys[p] == kSymmetryOnPlane)
        {
            axisPoints.push_back(p);
            continue;
        }
        const int64_t k = int64_t(keys[p]) - 1;
        if (k < 0 || k % kSlotBits >= n)
            continue;
        groups[k / kSlotBits].push_back({int(k % kSlotBits), p});
        nextId = std::max(nextId, k / kSlotBits + 1);
    }
    std::vector<Index> images(static_cast<size_t>(n));
    for (Index p : axisPoints)
    {
        std::ranges::fill(images, p);
        if (map.addOrbit(mesh, images, 0))
            ++info.stored;
    }
    for (auto& [id, members] : groups)
    {
        std::ranges::fill(images, kInvalid);
        for (auto [slot, p] : members)
            if (images[size_t(slot)] == kInvalid)
                images[size_t(slot)] = p;
        // On a mirror plane: the orbit's slot 0 point stored the reflection that fixes it, the rest their turns.
        if (radial.mirror() && images[0] == kInvalid)
        {
            int plane = -1, filled = 0;
            for (int k = 0; k < count; ++k)
                if (images[size_t(count + k)] != kInvalid)
                {
                    plane = k;
                    ++filled;
                }
            if (filled == 1)
            {
                images[0] = images[size_t(count + plane)];
                for (int j = 0; j < count; ++j)
                    images[size_t(count + j)] = images[size_t(((j - plane) % count + count) % count)];
            }
        }
        if (std::ranges::find(images, kInvalid) != images.end())
            continue;
        if (map.addOrbit(mesh, images, 0))
            info.stored += Index(members.size());
    }

    std::vector<Index> fresh;
    for (Index p = 0; p < pc; ++p)
        if (mesh.pointValid(p) && map.mOrbit[p] == kInvalid)
        {
            keys[p] = 0;
            fresh.push_back(p);
        }
    const Index storedOrbits = Index(map.mOrbitPoints.size() / size_t(n));

    auto distinct = [&](std::span<const Index> imgs) {
        Index c = 0;
        for (int g = 0; g < n; ++g)
            c += std::find(imgs.begin(), imgs.begin() + g, imgs[size_t(g)]) == imgs.begin() + g;
        return c;
    };

    // A new point's image under g neighbours the images of its matched neighbours. Ops build every wedge together, so
    // this finds new points' images however far the old ones have drifted.
    auto matchTopologically = [&]() {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (Index a : fresh)
            {
                if (map.mOrbit[a] != kInvalid)
                    continue;
                std::vector<Index> anchors;
                mesh.forEachPointEdge(a, [&](Index e) {
                    const Index nb = mesh.edgeOtherPoint(e, a);
                    if (map.mOrbit[nb] != kInvalid)
                        anchors.push_back(nb);
                });
                if (anchors.empty())
                    continue;
                bool ok = true;
                images[0] = a;
                for (int g = 1; g < n && ok; ++g)
                {
                    const glm::dvec3 target = radial.apply(g, pos(a));
                    std::vector<Index> candidates{a};
                    mesh.forEachPointEdge(map.image(g, anchors[0]), [&](Index e) { candidates.push_back(mesh.edgeOtherPoint(e, map.image(g, anchors[0]))); });
                    double best = std::numeric_limits<double>::max();
                    Index found = kInvalid;
                    for (Index b : candidates)
                    {
                        if (map.mOrbit[b] != kInvalid)
                            continue;
                        bool fits = b != a || glm::length(target - pos(a)) <= map.mTolerance;
                        for (Index m : anchors)
                        {
                            const Index mi = map.image(g, m);
                            fits = fits && mi != b && mesh.findEdge(b, mi) != kInvalid;
                        }
                        const glm::dvec3 d = pos(b) - target;
                        if (fits && glm::dot(d, d) < best)
                        {
                            best = glm::dot(d, d);
                            found = b;
                        }
                    }
                    images[size_t(g)] = found;
                    ok = found != kInvalid;
                    // The same point under two elements only where both put a in the same place.
                    for (int h = 0; h < g && ok; ++h)
                        if (images[size_t(h)] == found)
                            ok = glm::length(radial.apply(h, pos(a)) - target) <= map.mTolerance;
                }
                if (!ok)
                    continue;
                // a takes the wedge of a neighbour, so a new orbit's slots line up with the old ones around it.
                int rep = -1;
                for (Index m : anchors)
                    if (!map.mFixed[m])
                    {
                        rep = radial.inverse(map.mSlot[m]);
                        break;
                    }
                if (rep < 0)
                    rep = canonicalElement(mesh, radial, images);
                if (map.addOrbit(mesh, images, rep))
                {
                    info.topological += distinct(images);
                    changed = true;
                }
            }
        }
    };

    auto matchByPosition = [&]() {
        const double tol = std::max(map.mTolerance, 1e-12);
        std::vector<Index> open;
        for (Index p : fresh)
            if (map.mOrbit[p] == kInvalid)
                open.push_back(p);
        const PointGrid grid = makeGrid(mesh, tol, open);
        for (Index a : open)
        {
            if (map.mOrbit[a] != kInvalid)
                continue;
            const bool onAxis = radial.axisDistance(pos(a)) <= tol;
            bool ok = true;
            for (int g = 0; g < n && ok; ++g)
            {
                images[size_t(g)] = g == 0 || (onAxis && !radial.reflects(g))
                                        ? a
                                        : grid.nearest(mesh, radial.apply(g, pos(a)), tol, [&](Index q) { return map.mOrbit[q] == kInvalid; });
                ok = images[size_t(g)] != kInvalid;
            }
            if (ok && map.addOrbit(mesh, images, canonicalElement(mesh, radial, images)))
                info.byPosition += distinct(images);
        }
    };

    if (!fresh.empty())
    {
        matchTopologically();
        matchByPosition();
        matchTopologically();

        const Index orbits = Index(map.mOrbitPoints.size() / size_t(n));
        if (nextId + int64_t(orbits - storedOrbits) >= int64_t(INT32_MAX - kSlotBits) / kSlotBits)
            map.store(mesh); // out of ids: renumber densely
        else
            for (Index o = storedOrbits; o < orbits; ++o)
            {
                const size_t base = size_t(o) * size_t(n);
                const Index first = map.mOrbitPoints[base];
                if (std::all_of(map.mOrbitPoints.begin() + base, map.mOrbitPoints.begin() + base + size_t(n), [&](Index q) { return q == first; }))
                    keys[first] = kSymmetryOnPlane;
                else
                    for (int g = 0; g < n; ++g)
                        if (const Index q = map.mOrbitPoints[base + size_t(g)]; map.mSlot[q] == g)
                            keys[q] = int32_t(nextId * kSlotBits + map.storedSlot(q) + 1);
                ++nextId;
            }
    }
    map.countUnmatched(mesh);
    info.unmatched = map.mUnmatched;
    if (sync)
        *sync = info;
    return map;
}

glm::dvec3 SymmetrySpec::apply(int g, const glm::dvec3& p) const
{
    if (radial)
        return radial->apply(g, p);
    glm::dvec3 r = p;
    if (g == 1)
        r[axis] = 2.0 * offset - r[axis];
    return r;
}

glm::dvec3 SymmetrySpec::applyVector(int g, const glm::dvec3& v) const
{
    if (radial)
        return radial->applyVector(g, v);
    glm::dvec3 r = v;
    if (g == 1)
        r[axis] = -r[axis];
    return r;
}

glm::dvec3 SymmetrySpec::snap(const glm::dvec3& p, double tolerance) const
{
    if (radial)
    {
        if (radial->axisDistance(p) <= tolerance)
            return radial->projectToAxis(p);
        if (radial->mirror())
            for (int k = 0; k < radial->count(); ++k)
                if (std::abs(radial->mirrorPlane(radial->count() + k).distance(p)) <= tolerance)
                    return radial->projectFixed(p, std::vector<int>{radial->count() + k});
        return p;
    }
    glm::dvec3 r = p;
    if (std::abs(r[axis] - offset) <= tolerance)
        r[axis] = offset;
    return r;
}

SymmetryMap SymmetryMap::buildFor(const Mesh& mesh, const SymmetrySpec& spec, double tolerance)
{
    return spec.radial ? buildRadial(mesh, *spec.radial, tolerance) : build(mesh, spec.plane(), tolerance);
}

SymmetryMap SymmetryMap::persistent(Mesh& mesh, const SymmetrySpec& spec, double tolerance, SymmetrySync* sync)
{
    if (spec.radial)
        return persistent(mesh, *spec.radial, tolerance, sync);
    return persistent(mesh, spec.axis, tolerance, sync, spec.offset);
}

void SymmetryMap::store(Mesh& mesh) const
{
    if (!mRadial)
    {
        store(mesh, mAxis < 0 ? 0 : mAxis);
        return;
    }
    std::span<int32_t> keys = mesh.pointAttrs().getOrAdd<int32_t>(radialLayerName(*mRadial));
    const int n = order();
    for (Index p = 0; p < mesh.pointCapacity() && p < keys.size(); ++p)
        keys[p] = 0;
    int64_t id = 0;
    for (size_t base = 0; base < mOrbitPoints.size(); base += size_t(n))
    {
        const Index first = mOrbitPoints[base];
        bool axis = true;
        for (int g = 0; g < n; ++g)
            axis &= mOrbitPoints[base + size_t(g)] == first;
        if (axis)
        {
            if (mesh.pointValid(first))
                keys[first] = kSymmetryOnPlane;
            continue;
        }
        for (int g = 0; g < n; ++g)
            if (const Index q = mOrbitPoints[base + size_t(g)]; mesh.pointValid(q) && mSlot[q] == g)
                keys[q] = int32_t(id * kSlotBits + storedSlot(q) + 1);
        ++id;
    }
}
} // namespace lila::geo
