#include "curve/Along.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "geo/Attribute.h"
#include "geo/Normals.h"
#include "geo/Ops.h"

#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace lila::curve
{
namespace
{
using geo::Index;

constexpr size_t kMaxCopies = 100000;

D3 unitOr(const D3& v, const D3& fallback)
{
    double l = glm::length(v);
    return l > kEps ? v / l : fallback;
}

/** Columns: the frame's tangent, normal and binormal. */
glm::dmat3 frameMatrix(const Frame& f) { return glm::dmat3(f.t, f.n, f.b); }

/** Piece space to frame space: forward to the tangent, up to the normal, side to the binormal, after a roll about forward. */
glm::dmat3 layRotation(const Frame& f, const D3& F, const D3& U, const D3& S, double roll)
{
    double c = std::cos(roll), s = std::sin(roll);
    // Roll in local (forward, up, side) coordinates, then local to frame.
    glm::dmat3 rollM(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);
    glm::dmat3 toLocal = glm::transpose(glm::dmat3(F, U, S));
    return frameMatrix(f) * rollM * toLocal;
}

double wrapFraction(double s, double L, bool wrap)
{
    double t = L > kEps ? s / L : 0.0;
    return wrap ? t - std::floor(t) : std::clamp(t, 0.0, 1.0);
}

/** Turns the stored corner normals and tangents by each corner's point rotation, mixed by the envelope. */
void rotateCornerFrames(geo::Mesh& m, const std::vector<glm::dmat3>& rot, double envelope)
{
    auto normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    auto tangents = m.cornerAttrs().get<glm::vec4>(geo::attr::kTangent);
    if (normals.empty() && tangents.empty())
        return;
    for (Index c = 0; c < m.cornerCapacity(); ++c)
    {
        if (!m.cornerValid(c))
            continue;
        const glm::dmat3& r = rot[m.cornerPoint(c)];
        if (c < normals.size())
        {
            D3 n(normals[c]);
            normals[c] = glm::vec3(unitOr(glm::mix(n, r * n, envelope), n));
        }
        if (c < tangents.size())
        {
            D3 t(tangents[c]);
            D3 rt = unitOr(glm::mix(t, r * t, envelope), t);
            tangents[c] = glm::vec4(glm::vec3(rt), tangents[c].w);
        }
    }
}

/** Positions and stored normals through an affine transform. */
uint64_t splitmix(uint64_t& state)
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/** Uniform in [-1, 1]. */
double signedUnit(uint64_t& state) { return double(splitmix(state) >> 11) * (2.0 / double(1ull << 53)) - 1.0; }

glm::dmat3 eulerXYZ(const D3& e)
{
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), e.z, D3(0, 0, 1)) * glm::rotate(glm::dmat4(1.0), e.y, D3(0, 1, 0)) *
                      glm::rotate(glm::dmat4(1.0), e.x, D3(1, 0, 0)));
}

glm::dmat4 jitterFor(const ArrayAlongOptions& o, size_t index)
{
    if (o.randomRotate == D3(0.0) && o.randomScale == 0.0 && o.randomOffset == D3(0.0))
        return glm::dmat4(1.0);
    uint64_t state = uint64_t(o.seed) * 0x100000001B3ull + uint64_t(index) * 0x9E3779B97F4A7C15ull;
    D3 rot(signedUnit(state), signedUnit(state), signedUnit(state));
    double scale = std::max(0.01, 1.0 + o.randomScale * signedUnit(state));
    D3 off(signedUnit(state), signedUnit(state), signedUnit(state));
    glm::dmat4 m(eulerXYZ(rot * o.randomRotate) * scale);
    m[3] = glm::dvec4(off * o.randomOffset, 1.0);
    return m;
}

glm::dmat4 rigidPlacement(const Rail& rail, double s, const ArrayAlongOptions& o, bool wrap)
{
    D3 F, U, S;
    alongBasis(o.forward, F, U, S);
    Rail::Station st = rail.at(s, true, wrap);
    double t = wrapFraction(s, rail.length(), wrap);
    double sc = (o.scale ? o.scale(t) : 1.0) * st.scale;
    glm::dmat3 r = o.align ? layRotation(st.f, F, U, S, o.roll + (o.twist ? o.twist(t) : 0.0)) : glm::dmat3(1.0);
    glm::dmat4 m(r * sc);
    m[3] = glm::dvec4(st.p, 1.0);
    return m;
}

/** A target triangle with its points, point normals and surface coordinates. */
struct SurfTri
{
    D3 p[3];
    D3 n[3];
    D2 q[3];
    /** Surface derivatives along the two coordinates. */
    D3 du, dv;
};

/** Barycentric weights of q in the triangle's coordinates. */
D3 barycentric(const SurfTri& t, const D2& q)
{
    D2 e1 = t.q[1] - t.q[0], e2 = t.q[2] - t.q[0], d = q - t.q[0];
    double den = cross2(e1, e2);
    double b1 = cross2(d, e2) / den, b2 = cross2(e1, d) / den;
    return {1.0 - b1 - b2, b1, b2};
}

double distanceToTri(const SurfTri& t, const D2& q)
{
    D3 b = barycentric(t, q);
    if (b.x >= 0.0 && b.y >= 0.0 && b.z >= 0.0)
        return 0.0;
    double best = std::numeric_limits<double>::max();
    for (int k = 0; k < 3; ++k)
    {
        D2 a = t.q[k], c = t.q[(k + 1) % 3], ab = c - a;
        double len2 = glm::dot(ab, ab);
        double f = len2 > kEps ? std::clamp(glm::dot(q - a, ab) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, glm::distance(q, a + ab * f));
    }
    return best;
}

/** Target triangles bucketed in a grid over their coordinates. */
class SurfaceLookup
{
public:
    explicit SurfaceLookup(std::vector<SurfTri> tris) : mTris(std::move(tris))
    {
        for (const SurfTri& t : mTris)
            for (const D2& q : t.q)
            {
                mLo = glm::min(mLo, q);
                mHi = glm::max(mHi, q);
            }
        mN = std::clamp(int(std::sqrt(double(mTris.size()))), 1, 512);
        mCells.resize(size_t(mN) * size_t(mN));
        for (uint32_t i = 0; i < mTris.size(); ++i)
        {
            const SurfTri& t = mTris[i];
            D2 lo = glm::min(glm::min(t.q[0], t.q[1]), t.q[2]), hi = glm::max(glm::max(t.q[0], t.q[1]), t.q[2]);
            auto [x0, y0] = cell(lo);
            auto [x1, y1] = cell(hi);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    mCells[size_t(y) * size_t(mN) + size_t(x)].push_back(i);
        }
    }

    /** The triangle holding q (or the nearest one) and q's barycentric weights in it, unclamped. */
    std::pair<const SurfTri*, D3> find(const D2& q) const
    {
        auto [x, y] = cell(q);
        for (uint32_t i : mCells[size_t(y) * size_t(mN) + size_t(x)])
        {
            D3 b = barycentric(mTris[i], q);
            if (b.x >= -1e-9 && b.y >= -1e-9 && b.z >= -1e-9)
                return {&mTris[i], b};
        }
        const SurfTri* best = nullptr;
        double bd = std::numeric_limits<double>::max();
        for (const SurfTri& t : mTris)
            if (double d = distanceToTri(t, q); d < bd)
            {
                bd = d;
                best = &t;
            }
        return {best, barycentric(*best, q)};
    }

private:
    std::pair<int, int> cell(const D2& q) const
    {
        D2 size = glm::max(mHi - mLo, D2(1e-12));
        D2 f = (q - mLo) / size * double(mN);
        return {std::clamp(int(f.x), 0, mN - 1), std::clamp(int(f.y), 0, mN - 1)};
    }

    std::vector<SurfTri> mTris;
    std::vector<std::vector<uint32_t>> mCells;
    D2 mLo{std::numeric_limits<double>::max()}, mHi{-std::numeric_limits<double>::max()};
    int mN = 1;
};
} // namespace

void alongBasis(AlongAxis forward, D3& f, D3& up, D3& side)
{
    int axis = int(forward) / 2;
    double sign = int(forward) % 2 == 0 ? 1.0 : -1.0;
    f = D3(0.0);
    f[axis] = sign;
    up = axis == 1 ? D3(0, 0, 1) : D3(0, 1, 0);
    side = glm::cross(f, up);
}

Rail::Rail(const Spline& s, const RailOptions& o) : mSpline(s)
{
    LILA_ZONE();

    if (!isValid(s))
        return;
    mPl = tessellate(s, o.tess);
    if (mPl.size() < 2 || mPl.length < 1e-9)
        return;
    mClosed = s.closed;
    std::vector<double> tilt = tiltAlong(s, mPl);
    FrameOptions fo;
    fo.up = unitOr(o.upVector, D3(0, 1, 0));
    if (o.up == UpMode::AutoNormals)
        fo.mode = FrameMode::FixedUp;
    else
        fo.startNormal = perpUnit(fo.up, mPl.tangent(0), 1e-9);
    mFrames = frames(mPl, fo, o.up == UpMode::AimCurve ? std::span<const double>() : std::span<const double>(tilt));

    if (o.up == UpMode::AimCurve && o.aim && isValid(*o.aim))
    {
        Polyline aim = tessellate(*o.aim, o.tess);
        size_t segs = aim.closed ? aim.size() : aim.size() - 1;
        for (size_t i = 0; i < mPl.size() && aim.size() >= 2; ++i)
        {
            const D3& p = mPl.points[i];
            D3 best = aim.points[0];
            double bd = std::numeric_limits<double>::max();
            for (size_t k = 0; k < segs; ++k)
            {
                D3 a = aim.points[k], b = aim.points[(k + 1) % aim.size()], ab = b - a;
                double len2 = glm::dot(ab, ab);
                D3 q = a + ab * (len2 > kEps ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0);
                if (double d = glm::dot(p - q, p - q); d < bd)
                {
                    bd = d;
                    best = q;
                }
            }
            Frame& f = mFrames[i];
            // Where the aim point lies on the tangent line the previous normal carries on.
            D3 n = perpUnit(best - p, f.t, 1e-9);
            if (glm::length(n) < 0.5)
                n = i > 0 ? perpUnit(mFrames[i - 1].n, f.t, 1e-9) : f.n;
            if (glm::length(n) < 0.5)
                n = f.n;
            if (i < tilt.size() && tilt[i] != 0.0)
                n = glm::rotate(n, tilt[i], f.t);
            f.n = n;
            f.b = glm::normalize(glm::cross(f.t, f.n));
        }
    }

    mScale.assign(mPl.size(), 1.0);
    if (!s.radius.empty())
        for (size_t i = 0; i < mPl.size(); ++i)
            mScale[i] = pointValueAt(s, s.radius, 1.0, mPl.params[i]);
    mLength = mPl.length;
}

Rail::Station Rail::inside(double s) const
{
    double frac = 0.0;
    size_t i = mPl.segmentAt(s, &frac);
    size_t j = (i + 1) % mPl.size();
    Station st;
    st.p = glm::mix(mPl.points[i], mPl.points[j], frac);
    const Frame &a = mFrames[i], &b = mFrames[j];
    st.f.t = unitOr(glm::mix(a.t, b.t, frac), a.t);
    D3 n = perpUnit(glm::mix(a.n, b.n, frac), st.f.t, 1e-9);
    st.f.n = glm::length(n) > 0.5 ? n : perpUnit(a.n, st.f.t, 1e-9);
    if (glm::length(st.f.n) < 0.5)
        st.f.n = a.n;
    st.f.b = glm::normalize(glm::cross(st.f.t, st.f.n));
    st.scale = glm::mix(mScale[i], mScale[j], frac);
    return st;
}

Rail::Station Rail::at(double s, bool extrapolate, bool wrap) const
{
    if (!valid())
        return {};
    if (mClosed && wrap)
        return inside(s - std::floor(s / mLength) * mLength);
    double end = mLength;
    if (s >= 0.0 && s <= end)
        return inside(s);
    // Past an end: straight on along the tangent there. A closed curve opened at its seam ends on its last segment.
    bool before = s < 0.0;
    Station st = inside(before ? 0.0 : (mClosed ? end - 1e-9 * std::max(1.0, end) : end));
    if (extrapolate)
        st.p += st.f.t * (before ? s : s - end);
    return st;
}

std::pair<double, double> forwardExtent(const geo::Mesh& m, AlongAxis forward)
{
    D3 F, U, S;
    alongBasis(forward, F, U, S);
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            double x = glm::dot(m.position(p), F);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
    return lo <= hi ? std::pair{lo, hi} : std::pair{0.0, 0.0};
}

void deformAlongCurve(geo::Mesh& m, const Rail& rail, const DeformAlongOptions& o)
{
    LILA_ZONE();

    if (!rail.valid())
        throw std::invalid_argument("the curve has no length");
    D3 F, U, S;
    alongBasis(o.forward, F, U, S);
    auto [x0, x1] = o.extent ? *o.extent : forwardExtent(m, o.forward);
    const double L = rail.length();
    const bool wrap = rail.closed() && o.loop;
    const double start = o.offset + o.offsetFraction * L;
    double r0 = start, r1 = rail.closed() ? start + L : L;
    if (o.range)
        std::tie(r0, r1) = *o.range;
    const bool stretch = o.stretch && x1 - x0 > 1e-9;
    const double env = std::clamp(o.envelope, 0.0, 1.0);

    std::vector<glm::dmat3> rot(m.pointCapacity(), glm::dmat3(1.0));
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        const D3 pos = m.position(p);
        double x = glm::dot(pos, F), y = glm::dot(pos, U), z = glm::dot(pos, S);
        double s = stretch ? r0 + (x - x0) / (x1 - x0) * (r1 - r0) : start + (x - x0);
        double t = wrapFraction(s, L, wrap);
        Rail::Station st = rail.at(s, o.extrapolate, wrap);
        double sc = (o.scale ? o.scale(t) : 1.0) * st.scale;
        double ang = o.roll + (o.twist ? o.twist(t) : 0.0);
        double c = std::cos(ang), sn = std::sin(ang);
        double y2 = y * c - z * sn, z2 = y * sn + z * c;
        D3 laid = st.p + (st.f.n * y2 + st.f.b * z2) * sc;
        m.setPosition(p, glm::mix(pos, laid, env));
        rot[p] = layRotation(st.f, F, U, S, ang);
    }
    rotateCornerFrames(m, rot, env);
}

std::vector<ArrayPlacement> arrayPlacements(const Rail& rail, const ArrayAlongOptions& o, double pieceLength, double startLength, double endLength)
{
    LILA_ZONE();

    if (!rail.valid())
        throw std::invalid_argument("the curve has no length");
    const double L = rail.length();
    const bool hasStart = startLength >= 0.0, hasEnd = endLength >= 0.0;
    const double a = std::max(0.0, o.startOffset);
    const double b = rail.closed() ? a + L - std::max(0.0, o.endOffset) : L - std::max(0.0, o.endOffset);
    if (b - a < 1e-9)
        throw std::invalid_argument("the start and end offsets leave no room on the curve");
    const double span = b - a;
    // A whole closed loop: the last copy would land on the first, so the step divides the loop into count parts.
    const bool loop = rail.closed() && o.endOffset <= 1e-9 && !hasStart && !hasEnd;
    const bool wrap = rail.closed();
    auto spacing = [&] {
        double d = o.spacing > 1e-9 ? o.spacing : pieceLength;
        if (d < 1e-6)
            throw std::invalid_argument("the piece has no length along the forward axis: give a spacing");
        return d;
    };
    auto checkCount = [](double n) {
        if (n > double(kMaxCopies))
            throw std::invalid_argument("more than 100000 copies: raise the spacing or lower the count");
    };

    std::vector<ArrayPlacement> out;
    if (!o.deform)
    {
        std::vector<double> pos;
        if (o.fill == ArrayFill::Count)
        {
            int n = std::max(1, o.count);
            checkCount(n);
            double step = loop ? span / n : (n > 1 ? span / (n - 1) : 0.0);
            for (int i = 0; i < n; ++i)
                pos.push_back(a + step * i);
        }
        else
        {
            double d = spacing();
            double q = span / d;
            checkCount(q);
            size_t n = 0;
            double step = d;
            if (loop)
            {
                n = size_t(std::max(1.0, o.fitToLength ? std::round(q) : std::floor(q + 1e-9)));
                step = o.fitToLength ? span / double(n) : d;
            }
            else
            {
                n = size_t(o.fitToLength ? std::max(1.0, std::round(q)) : std::floor(q + 1e-9)) + 1;
                step = o.fitToLength && n > 1 ? span / double(n - 1) : d;
            }
            for (size_t i = 0; i < n; ++i)
                pos.push_back(a + step * double(i));
        }
        for (size_t i = 0; i < pos.size(); ++i)
        {
            ArrayPlacement pl;
            pl.s0 = pl.s1 = pos[i];
            if (hasStart && i == 0)
                pl.piece = ArrayPiece::Start;
            else if (hasEnd && i + 1 == pos.size())
                pl.piece = ArrayPiece::End;
            pl.jitter = jitterFor(o, i);
            pl.transform = rigidPlacement(rail, pos[i], o, wrap) * pl.jitter;
            out.push_back(pl);
        }
        return out;
    }

    const double m0 = a + (hasStart ? startLength : 0.0), m1 = b - (hasEnd ? endLength : 0.0);
    if (m1 - m0 < -1e-9)
        throw std::invalid_argument("the start and end pieces are longer than the curve");
    const double mid = std::max(0.0, m1 - m0);
    size_t n = 0;
    double slot = 0.0;
    if (o.fill == ArrayFill::Count)
    {
        n = size_t(std::max(0, o.count - (hasStart ? 1 : 0) - (hasEnd ? 1 : 0)));
        if (!hasStart && !hasEnd)
            n = std::max<size_t>(n, 1);
        checkCount(double(n));
        slot = n > 0 ? mid / double(n) : 0.0;
    }
    else
    {
        double d = spacing();
        double q = mid / d;
        checkCount(q);
        n = size_t(o.fitToLength ? std::round(q) : std::floor(q + 1e-9));
        if (!hasStart && !hasEnd)
            n = std::max<size_t>(n, 1);
        slot = o.fitToLength && n > 0 ? mid / double(n) : d;
    }
    auto add = [&](double s0, double s1, ArrayPiece piece) {
        ArrayPlacement pl;
        pl.s0 = s0;
        pl.s1 = s1;
        pl.piece = piece;
        pl.jitter = jitterFor(o, out.size());
        pl.transform = rigidPlacement(rail, s0, o, wrap) * pl.jitter;
        out.push_back(pl);
    };
    if (hasStart)
        add(a, a + startLength, ArrayPiece::Start);
    for (size_t i = 0; i < n; ++i)
        add(m0 + slot * double(i), m0 + slot * double(i + 1), ArrayPiece::Middle);
    if (hasEnd)
        add(b - endLength, b, ArrayPiece::End);
    return out;
}

geo::Mesh arrayAlongCurve(const geo::Mesh& piece, const geo::Mesh* start, const geo::Mesh* end, const Rail& rail, const ArrayAlongOptions& o,
                          std::vector<ArrayPlacement>* placements)
{
    LILA_ZONE();

    auto extentOf = [&](const geo::Mesh* m) { return m ? forwardExtent(*m, o.forward) : std::pair{0.0, -1.0}; };
    auto midExt = extentOf(&piece), startExt = extentOf(start), endExt = extentOf(end);
    std::vector<ArrayPlacement> places =
        arrayPlacements(rail, o, midExt.second - midExt.first, start ? startExt.second - startExt.first : -1.0, end ? endExt.second - endExt.first : -1.0);
    geo::Mesh out;
    for (const ArrayPlacement& pl : places)
    {
        const geo::Mesh& src = pl.piece == ArrayPiece::Start ? *start : pl.piece == ArrayPiece::End ? *end : piece;
        geo::Mesh copy = src;
        if (o.deform)
        {
            transformMesh(copy, pl.jitter);
            DeformAlongOptions d;
            d.forward = o.forward;
            d.roll = o.roll;
            d.stretch = true;
            d.range = std::pair{pl.s0, pl.s1};
            d.extent = pl.piece == ArrayPiece::Start ? startExt : pl.piece == ArrayPiece::End ? endExt : midExt;
            d.scale = o.scale;
            d.twist = o.twist;
            deformAlongCurve(copy, rail, d);
        }
        else
            transformMesh(copy, pl.transform);
        out.append(copy);
    }
    if (o.deform && o.weld && places.size() > 1)
    {
        std::vector<Index> border;
        for (Index p = 0; p < out.pointCapacity(); ++p)
            if (out.pointValid(p) && out.isBoundaryPoint(p))
                border.push_back(p);
        geo::MergeOptions mo;
        mo.distance = std::max(1e-4, rail.length() * 1e-7);
        if (!border.empty())
            geo::mergeByDistance(out, border, mo);
        out.compact();
    }
    if (placements)
        *placements = std::move(places);
    return out;
}

std::vector<glm::dvec2> gridParam(const geo::Mesh& m, std::span<const Index> faces)
{
    LILA_ZONE();

    std::vector<uint8_t> inSet(m.faceCapacity(), 0);
    size_t count = 0;
    for (Index f : faces)
        if (m.faceValid(f) && !inSet[f])
        {
            if (m.faceSize(f) != 4)
                throw std::invalid_argument("the faces must all be quads to form a grid");
            inSet[f] = 1;
            ++count;
        }
    if (count == 0)
        throw std::invalid_argument("no faces to make a grid from");
    constexpr int kUnset = std::numeric_limits<int>::min();
    std::vector<glm::ivec2> coord(m.pointCapacity(), glm::ivec2(kUnset));
    auto setCoord = [&](Index p, glm::ivec2 c) {
        if (coord[p].x == kUnset)
            coord[p] = c;
        else if (coord[p] != c)
            throw std::invalid_argument("the faces are not a regular grid (a pole inside, or the patch closes on itself)");
    };
    Index first = geo::kInvalid;
    for (Index f = 0; f < m.faceCapacity() && first == geo::kInvalid; ++f)
        if (inSet[f])
            first = f;
    std::vector<Index> fp = m.facePoints(first);
    const glm::ivec2 square[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; ++k)
        setCoord(fp[size_t(k)], square[k]);
    std::vector<uint8_t> done(m.faceCapacity(), 0);
    std::vector<Index> queue{first};
    done[first] = 1;
    size_t reached = 0;
    while (!queue.empty())
    {
        Index f = queue.back();
        queue.pop_back();
        ++reached;
        std::vector<Index> pts = m.facePoints(f);
        for (int k = 0; k < 4; ++k)
        {
            Index pa = pts[size_t(k)], pb = pts[size_t((k + 1) % 4)];
            Index pd = pts[size_t((k + 3) % 4)], pc = pts[size_t((k + 2) % 4)];
            Index e = m.findEdge(pa, pb);
            if (e == geo::kInvalid)
                continue;
            Index c0 = m.edgeCorner(e), c = c0;
            do
            {
                Index g = m.cornerFace(c);
                c = m.cornerRadial(c);
                if (g == f || !inSet[g] || done[g])
                    continue;
                std::vector<Index> q = m.facePoints(g);
                auto at = [&](Index p) { return size_t(std::find(q.begin(), q.end(), p) - q.begin()); };
                size_t ia = at(pa), ib = at(pb);
                // In g, the point next to a (away from b) continues the row d -> a, and likewise for b.
                size_t ix = (ia + 1) % 4 == ib ? (ia + 3) % 4 : (ia + 1) % 4;
                size_t iy = (ib + 1) % 4 == ia ? (ib + 3) % 4 : (ib + 1) % 4;
                setCoord(q[ix], 2 * coord[pa] - coord[pd]);
                setCoord(q[iy], 2 * coord[pb] - coord[pc]);
                done[g] = 1;
                queue.push_back(g);
            } while (c != c0);
        }
    }
    if (reached != count)
        throw std::invalid_argument("the faces must be one connected patch");
    glm::ivec2 lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
    for (const glm::ivec2& c : coord)
        if (c.x != kUnset)
        {
            lo = glm::min(lo, c);
            hi = glm::max(hi, c);
        }
    // Every grid cell must be a face: a rectangle of (w x h) cells has exactly that many quads.
    glm::ivec2 size = hi - lo;
    if (size_t(size.x) * size_t(size.y) != count)
        throw std::invalid_argument("the faces are not a full rectangle of quads");
    std::vector<D2> out(m.pointCapacity(), D2(-1.0));
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (coord[p].x != kUnset)
            out[p] = D2(double(coord[p].x - lo.x) / size.x, double(coord[p].y - lo.y) / size.y);
    return out;
}

void flowAlongSurface(geo::Mesh& source, const geo::Mesh& target, const FlowOptions& o)
{
    LILA_ZONE();

    std::vector<Index> faces;
    if (o.faces.empty())
    {
        for (Index f = 0; f < target.faceCapacity(); ++f)
            if (target.faceValid(f))
                faces.push_back(f);
    }
    else
        for (Index f : o.faces)
            if (target.faceValid(f))
                faces.push_back(f);
    if (faces.empty())
        throw std::invalid_argument("the target has no faces to flow onto");

    std::vector<D2> grid;
    std::span<const glm::vec2> uv;
    if (o.param == FlowParam::Grid)
        grid = gridParam(target, faces);
    else
    {
        uv = target.cornerAttrs().get<glm::vec2>(o.uvSet);
        if (uv.empty())
            throw std::invalid_argument("the target has no " + o.uvSet + " UVs: lay out UVs, or flow by grid");
    }
    std::vector<glm::vec3> pn = geo::pointNormals(target);

    std::vector<SurfTri> tris;
    double mirrored = 0.0;
    for (Index f : faces)
    {
        std::vector<Index> cs = target.faceCorners(f);
        for (size_t k = 1; k + 1 < cs.size(); ++k)
        {
            SurfTri t;
            Index c3[3] = {cs[0], cs[k], cs[k + 1]};
            for (int j = 0; j < 3; ++j)
            {
                Index p = target.cornerPoint(c3[j]);
                t.p[j] = target.position(p);
                t.n[j] = D3(pn[p]);
                t.q[j] = o.param == FlowParam::Grid ? grid[p] : D2(uv[c3[j]]);
            }
            D2 e1 = t.q[1] - t.q[0], e2 = t.q[2] - t.q[0];
            double den = cross2(e1, e2);
            if (std::abs(den) < 1e-14)
                continue;
            D3 f1 = t.p[1] - t.p[0], f2 = t.p[2] - t.p[0];
            // Solve for the surface derivatives along the two coordinates.
            t.du = (f1 * e2.y - f2 * e1.y) / den;
            t.dv = (f2 * e1.x - f1 * e2.x) / den;
            D3 normal = glm::cross(f1, f2);
            mirrored += (den < 0.0 ? 1.0 : -1.0) * glm::length(normal);
            tris.push_back(t);
        }
    }
    if (tris.empty())
        throw std::invalid_argument("the target's coordinates are degenerate (every face has zero UV area)");
    SurfaceLookup lookup(std::move(tris));

    geo::Bounds bb = source.bounds();
    if (!bb.valid())
        return;
    glm::dvec4 fp = o.footprint ? *o.footprint : glm::dvec4(bb.min.x, bb.min.z, bb.max.x, bb.max.z);
    double w = fp.z - fp.x, d = fp.w - fp.y;
    double base = o.baseAtOrigin ? 0.0 : bb.min.y;
    double env = std::clamp(o.envelope, 0.0, 1.0);
    std::vector<glm::dmat3> rot(source.pointCapacity(), glm::dmat3(1.0));
    for (Index p = 0; p < source.pointCapacity(); ++p)
    {
        if (!source.pointValid(p))
            continue;
        D3 pos = source.position(p);
        double tu = w > 1e-12 ? (pos.x - fp.x) / w : 0.5;
        // Z runs against v, so the footprint's top view keeps its handedness on the surface (X, -Z, Y right handed).
        double tv = d > 1e-12 ? (fp.w - pos.z) / d : 0.5;
        if (o.swapUv)
            std::swap(tu, tv);
        D2 q(o.rect.x + tu * (o.rect.z - o.rect.x), o.rect.y + tv * (o.rect.w - o.rect.y));
        auto [tri, b] = lookup.find(q);
        D3 at = tri->p[0] * b.x + tri->p[1] * b.y + tri->p[2] * b.z;
        D3 n = unitOr(tri->n[0] * b.x + tri->n[1] * b.y + tri->n[2] * b.z, unitOr(glm::cross(tri->p[1] - tri->p[0], tri->p[2] - tri->p[0]), D3(0, 1, 0)));
        D3 laid = at + n * (pos.y - base) * o.heightScale;
        source.setPosition(p, glm::mix(pos, laid, env));
        D3 eu = perpUnit(tri->du, n, 1e-9);
        if (glm::length(eu) < 0.5)
            eu = perpUnit(D3(1, 0, 0), n, 1e-9);
        rot[p] = glm::dmat3(eu, n, glm::cross(eu, n));
    }
    rotateCornerFrames(source, rot, env);
    // A mirrored layout (or a swap) turns the flowed faces inside out.
    if ((mirrored > 0.0) != o.swapUv)
    {
        std::vector<Index> all;
        for (Index f = 0; f < source.faceCapacity(); ++f)
            if (source.faceValid(f))
                all.push_back(f);
        geo::flipNormals(source, all);
    }
}
} // namespace lila::curve
