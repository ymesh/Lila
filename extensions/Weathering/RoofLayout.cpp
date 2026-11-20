#include "RoofLayout.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace weathering::roof
{
using gen::hash01;
using gen::unit;

bool Region::inside(const glm::dvec2& p) const
{
    for (const auto& f : faces)
        if (gen::insidePolygon(f, p))
            return true;
    return false;
}

std::vector<Region> regionsOf(const Mesh& host, std::span<const Index> faces, double maxSlope, std::vector<std::vector<Index>>* regionFaces)
{
    std::vector<uint8_t> picked(host.faceCapacity(), 0);
    const double minUp = std::cos(glm::radians(std::clamp(maxSlope, 0.0, 89.9)));
    if (faces.empty())
    {
        for (Index f = 0; f < host.faceCapacity(); ++f)
            if (host.faceValid(f) && host.faceArea(f) > 1e-6 && host.faceNormal(f).y >= minUp)
                picked[f] = 1;
    }
    else
        for (Index f : faces)
            if (host.faceValid(f) && host.faceArea(f) > 1e-6)
                picked[f] = 1;

    std::vector<int> regionOf(host.faceCapacity(), -1);
    std::vector<std::vector<Index>> groups;
    const double sameNormal = std::cos(glm::radians(3.0));
    for (Index seed = 0; seed < host.faceCapacity(); ++seed)
    {
        if (!picked[seed] || regionOf[seed] >= 0)
            continue;
        const int r = int(groups.size());
        groups.emplace_back();
        const glm::dvec3 n0 = host.faceNormal(seed);
        const glm::dvec3 c0 = host.faceCentroid(seed);
        std::vector<Index> stack{seed};
        regionOf[seed] = r;
        while (!stack.empty())
        {
            const Index f = stack.back();
            stack.pop_back();
            groups[size_t(r)].push_back(f);
            for (Index c : host.faceCorners(f))
            {
                const Index o = host.oppositeFace(host.cornerEdge(c), f);
                if (o == lila::geo::kInvalid || !picked[o] || regionOf[o] >= 0)
                    continue;
                if (glm::dot(host.faceNormal(o), n0) < sameNormal || std::abs(glm::dot(host.faceCentroid(o) - c0, n0)) > 1.0)
                    continue;
                regionOf[o] = r;
                stack.push_back(o);
            }
        }
    }

    std::vector<Region> out;
    for (const std::vector<Index>& grp : groups)
    {
        Region R;
        glm::dvec3 n(0.0);
        for (Index f : grp)
            n += host.faceAreaVector(f);
        R.normal = unit(n);
        R.origin = host.position(host.facePoints(grp.front()).front());
        if (R.normal.y > 0.999)
        {
            glm::dvec3 lo(1e300), hi(-1e300);
            for (Index f : grp)
                for (Index p : host.facePoints(f))
                {
                    lo = glm::min(lo, host.position(p));
                    hi = glm::max(hi, host.position(p));
                }
            // Flat: rows run along the longer side.
            R.up = hi.x - lo.x >= hi.z - lo.z ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
            R.up = unit(R.up - R.normal * glm::dot(R.up, R.normal));
        }
        else
            R.up = unit(glm::dvec3(0, 1, 0) - R.normal * R.normal.y);
        R.across = unit(glm::cross(R.up, R.normal));
        R.a0 = R.v0 = 1e300;
        R.a1 = R.v1 = -1e300;
        for (Index f : grp)
        {
            std::vector<glm::dvec2> poly;
            for (Index p : host.facePoints(f))
            {
                const glm::dvec3 d = host.position(p) - R.origin;
                const glm::dvec2 q(glm::dot(d, R.across), glm::dot(d, R.up));
                poly.push_back(q);
                R.a0 = std::min(R.a0, q.x);
                R.a1 = std::max(R.a1, q.x);
                R.v0 = std::min(R.v0, q.y);
                R.v1 = std::max(R.v1, q.y);
            }
            R.faces.push_back(std::move(poly));
        }
        out.push_back(std::move(R));
    }
    if (regionFaces)
        *regionFaces = std::move(groups);
    return out;
}

namespace
{
struct Sized
{
    gen::Fit fit;
    double w = 1, t = 1, l = 1; // across, thickness, down the slope (scaled)
};

std::vector<Sized> sizedPieces(std::span<const Bounds> pieces, double scale, bool lengthAlongZ)
{
    std::vector<Sized> out;
    for (const Bounds& b : pieces)
    {
        Sized s;
        s.fit = gen::fitPiece(b, lengthAlongZ, !lengthAlongZ);
        s.w = s.fit.size.x * scale;
        s.t = s.fit.size.y * scale;
        s.l = s.fit.size.z * scale;
        out.push_back(s);
    }
    return out;
}

/** Ridge caps along edges where two sloped regions meet at both their tops. */
void ridgeCaps(const Input& in, const Options& o, const std::vector<Region>& regions, const std::vector<std::vector<Index>>& regionFaces,
               double coverThickness, Result& r)
{
    if (in.caps.empty() || !o.ridge)
        return;
    const Mesh& m = *in.host;
    std::vector<int> regionOf(m.faceCapacity(), -1);
    for (size_t k = 0; k < regionFaces.size(); ++k)
        for (Index f : regionFaces[k])
            regionOf[f] = int(k);
    const std::vector<Sized> caps = sizedPieces(in.caps, o.scale, false);
    double capLength = 1e300;
    for (const Sized& s : caps)
        capLength = std::min(capLength, s.w);
    const double missing = o.missing.value_or(o.damage * 0.25) * 0.5;
    uint32_t counter = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        const Index c = m.edgeCorner(e);
        const Index f1 = m.cornerFace(c), f2 = m.cornerFace(m.cornerRadial(c));
        const int r1 = regionOf[f1], r2 = regionOf[f2];
        if (r1 < 0 || r2 < 0 || r1 == r2)
            continue;
        const Region& A = regions[size_t(r1)];
        const Region& B = regions[size_t(r2)];
        if (A.normal.y > 0.98 || B.normal.y > 0.98)
            continue;
        const glm::dvec3 p0 = m.position(m.edgePoint(e, 0)), p1 = m.position(m.edgePoint(e, 1));
        const double len = glm::length(p1 - p0);
        if (len < 1e-3 || std::abs(p1.y - p0.y) > 0.1 * len)
            continue;
        const glm::dvec3 mid = (p0 + p1) * 0.5;
        auto atTop = [&](const Region& R) { return glm::dot(mid - R.origin, R.up) > R.v1 - std::max(2.0, 0.01 * (R.v1 - R.v0)); };
        if (!atTop(A) || !atTop(B))
            continue;
        const glm::dvec3 along = unit(p1 - p0);
        const glm::dvec3 bis = unit(A.normal + B.normal);
        const glm::dvec3 side = glm::cross(along, bis);
        const double step = capLength * (1.0 - std::clamp(o.ridgeOverlap, 0.0, 0.9));
        const int count = len <= capLength ? 1 : int(std::ceil((len - capLength) / step)) + 1;
        const double pitch = count > 1 ? (len - capLength) / (count - 1) : 0.0;
        for (int k = 0; k < count; ++k)
        {
            const uint32_t id = counter++;
            if (hash01(o.seed, 7001, id) < missing)
                continue;
            const int pick = gen::pickWeighted(in.capWeights, int(caps.size()), hash01(o.seed, 7002, id));
            const Sized& s = caps[size_t(pick)];
            const double at = count > 1 ? capLength * 0.5 + pitch * k : len * 0.5;
            const glm::dvec3 center = p0 + along * at + bis * (coverThickness * 1.2 + s.t * 0.5);
            Placed P;
            P.xf = gen::place(s.fit, center, along, bis, side, glm::dvec3(o.scale));
            P.slot = 1;
            P.piece = pick;
            P.region = r1;
            P.random = float(hash01(o.seed, 7003, id));
            r.placed.push_back(P);
            ++r.caps;
        }
    }
}
} // namespace

Result layout(const Input& in, const Options& o)
{
    Result r;
    if (!in.host || in.cover.empty())
        return r;
    std::vector<std::vector<Index>> regionFaces;
    r.regions = regionsOf(*in.host, in.faces, o.maxSlope, &regionFaces);
    const std::vector<Sized> pieces = sizedPieces(in.cover, o.scale, true);
    double lMin = 1e300, wMin = 1e300, tMax = 0.0;
    for (const Sized& s : pieces)
    {
        lMin = std::min(lMin, s.l);
        wMin = std::min(wMin, s.w);
        tMax = std::max(tMax, s.t);
    }
    const double neat = std::clamp(o.neatness, 0.0, 1.0);
    const double messy = 1.0 - neat;
    const double overlap = std::clamp(o.overlap, 0.0, 0.9);
    const double sideOverlap = std::clamp(o.sideOverlap, 0.0, 0.9);
    const double damage = std::clamp(o.damage, 0.0, 1.0);
    const double missing = std::clamp(o.missing.value_or(damage * 0.25), 0.0, 1.0);
    const double lifted = std::clamp(o.lifted.value_or(damage * 0.35), 0.0, 1.0);
    const double liftAngle = o.liftAngle > 0.0 ? o.liftAngle : 6.0 + 20.0 * damage;

    uint32_t counter = 0;
    for (size_t ri = 0; ri < r.regions.size(); ++ri)
    {
        const Region& R = r.regions[ri];
        const glm::dvec3 down = -R.up;
        const double vStart = R.v0 - std::max(0.0, o.overhang);
        // A little past the top, so the last row's turned corners still reach it.
        const double vLen = R.v1 + 2.0 - vStart;
        const double pitchNominal = std::max(1e-3, lMin * (1.0 - overlap));
        const int rows = vLen <= lMin ? 1 : int(std::ceil((vLen - lMin) / pitchNominal)) + 1;
        const double pitch = rows > 1 ? (vLen - lMin) / (rows - 1) : 0.0;
        r.rows += rows;
        const double neatStep = wMin * (1.0 - sideOverlap);
        for (int row = 0; row < rows; ++row)
        {
            const double vb = vStart + pitch * row;
            const double neatShift = (row % 2) * std::clamp(o.stagger, 0.0, 1.0) * neatStep;
            const double shift = neat * neatShift + messy * hash01(o.seed, 101, uint32_t(ri), uint32_t(row)) * neatStep;
            double a = R.a0 - shift;
            for (int col = 0; a < R.a1 && col < 100000; ++col)
            {
                const uint32_t id = counter++;
                const int pick = gen::pickWeighted(in.coverWeights, int(pieces.size()), hash01(o.seed, 1, id));
                const Sized& s = pieces[size_t(pick)];
                const double step = neat >= 0.999 ? neatStep : (neat * neatStep + messy * s.w * (1.0 - sideOverlap));
                double da = (hash01(o.seed, 2, id) - 0.5) * 2.0 * sideOverlap * s.w * 0.25 * messy;
                double dv = (hash01(o.seed, 3, id) - 0.5) * 2.0 * overlap * s.l * 0.3 * messy;
                // Jitter never pulls the outermost pieces in from the face's edges.
                if (row == 0)
                    dv = -std::abs(dv);
                if (row == rows - 1)
                    dv = std::abs(dv);
                if (col == 0)
                    da = -std::abs(da);
                if (a + step >= R.a1)
                    da = std::abs(da);
                const double turn = (hash01(o.seed, 4, id) - 0.5) * 2.0 * 1.0 * messy;
                const double ac = a + s.w * 0.5 + da;
                const double vc = vb + s.l * 0.5 + dv;
                a += step;
                const double cs = std::cos(glm::radians(turn)), sn = std::sin(glm::radians(turn));
                std::array<glm::dvec2, 4> foot;
                const glm::dvec2 half(s.w * 0.5, s.l * 0.5);
                const glm::dvec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                bool in2 = R.inside({ac, vc});
                for (int k = 0; k < 4; ++k)
                {
                    const glm::dvec2 q = corners[k] * half;
                    foot[size_t(k)] = glm::dvec2(ac + q.x * cs - q.y * sn, vc + q.x * sn + q.y * cs);
                    in2 = in2 || R.inside(glm::dvec2(ac, vc) + corners[k] * half * 0.9);
                }
                if (!in2)
                    continue;
                ++r.candidates;
                if (hash01(o.seed, 5, id) < missing)
                {
                    ++r.missing;
                    continue;
                }
                const double ua = std::clamp((ac - R.a0) / std::max(1e-6, R.a1 - R.a0), 0.0, 1.0);
                const double uv = std::clamp((vc - R.v0) / std::max(1e-6, R.v1 - R.v0), 0.0, 1.0);
                const double sagOff = o.sag * (1.0 - std::sin(std::numbers::pi * ua) * std::sin(std::numbers::pi * uv));
                const glm::dvec3 base = R.origin + R.across * ac + R.up * vc + R.normal * sagOff;
                const glm::dvec3 center = base + R.normal * (s.t * 0.5);
                glm::dmat4 xf = gen::place(s.fit, center, R.across, R.normal, down, glm::dvec3(o.scale));
                xf = gen::rotateAbout(center, R.normal, turn) * xf;
                auto at = [&](double bx, double by, double bz) {
                    return glm::dvec3(xf * glm::inverse(s.fit.toBox) * glm::dvec4(bx * s.fit.size.x, by * s.fit.size.y, bz * s.fit.size.z, 1.0));
                };
                // Each row's lower end rests on the row below; the upper end stays on the deck.
                if (row > 0)
                {
                    const glm::dvec3 pivot = at(0.0, -0.5, -0.5);
                    xf = gen::rotateAbout(pivot, R.across, -glm::degrees(std::atan2(tMax, s.l))) * xf;
                }
                Placed P;
                P.lifted = hash01(o.seed, 6, id) < lifted;
                if (P.lifted)
                {
                    const double side = hash01(o.seed, 8, id) < 0.5 ? -1.0 : 1.0;
                    // Peel about a line through the opposite upper corner that leaves the piece on one side, so only lifts.
                    const glm::dvec3 pivot = at(-0.5 * side, -0.5, -0.5);
                    const glm::dvec3 other = at(-1.0 * side, -0.5, 0.5);
                    const glm::dvec3 corner = at(0.5 * side, -0.5, 0.5);
                    const glm::dvec3 axis = unit(other - pivot);
                    double angle = liftAngle * (0.6 + 0.4 * hash01(o.seed, 9, id));
                    const glm::dvec3 moved(gen::rotateAbout(pivot, axis, angle) * glm::dvec4(corner, 1.0));
                    if (glm::dot(moved - corner, R.normal) < 0.0)
                        angle = -angle;
                    xf = gen::rotateAbout(pivot, axis, angle) * xf;
                    ++r.lifted;
                }
                P.xf = xf;
                P.piece = pick;
                P.region = int(ri);
                P.row = row;
                P.random = float(hash01(o.seed, 10, id));
                P.footprint = foot;
                r.placed.push_back(P);
            }
        }
    }
    ridgeCaps(in, o, r.regions, regionFaces, tMax, r);
    return r;
}
} // namespace weathering::roof
