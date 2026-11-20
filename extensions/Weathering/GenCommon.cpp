#include "GenCommon.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace weathering::gen
{
namespace
{
glm::dvec3 newell(std::span<const glm::dvec3> p)
{
    glm::dvec3 n(0.0);
    for (size_t k = 0; k < p.size(); ++k)
    {
        const glm::dvec3& a = p[k];
        const glm::dvec3& b = p[(k + 1) % p.size()];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

/** Adds a face through the points, turned so its normal points along `out`. */
void faceFacing(Mesh& m, std::vector<Index> pts, const glm::dvec3& out)
{
    std::vector<glm::dvec3> p;
    for (Index i : pts)
        p.push_back(m.position(i));
    if (glm::dot(newell(p), out) < 0.0)
        std::ranges::reverse(pts);
    m.addFace(pts);
}

glm::dvec3 apply(const glm::dmat4& xf, const glm::dvec3& p) { return glm::dvec3(xf * glm::dvec4(p, 1.0)); }
} // namespace

Bounds boundsOf(const Mesh& m)
{
    Bounds b;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            b.extend(m.position(p));
    if (!b.valid())
    {
        b.extend(glm::dvec3(-0.5));
        b.extend(glm::dvec3(0.5));
    }
    return b;
}

Fit fitPiece(const Bounds& b, bool lengthAlongZ, bool lengthAlongX)
{
    glm::dvec3 size = glm::max(b.size(), glm::dvec3(1e-3));
    Fit f;
    f.toBox = glm::translate(glm::dmat4(1.0), -b.center());
    const bool turn = (lengthAlongZ && size.x > size.z) || (lengthAlongX && size.z > size.x);
    if (turn)
    {
        // A quarter turn about Y: X goes to -Z, Z to X.
        f.toBox = glm::rotate(glm::dmat4(1.0), std::numbers::pi / 2.0, glm::dvec3(0, 1, 0)) * f.toBox;
        std::swap(size.x, size.z);
    }
    f.size = size;
    return f;
}

glm::dmat4 place(const Fit& f, const glm::dvec3& center, const glm::dvec3& ax, const glm::dvec3& ay, const glm::dvec3& az, const glm::dvec3& scale)
{
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(ax * scale.x, 0.0);
    m[1] = glm::dvec4(ay * scale.y, 0.0);
    m[2] = glm::dvec4(az * scale.z, 0.0);
    m[3] = glm::dvec4(center, 1.0);
    return m * f.toBox;
}

glm::dmat4 rotateAbout(const glm::dvec3& point, const glm::dvec3& axis, double degrees)
{
    return glm::translate(glm::dmat4(1.0), point) * glm::rotate(glm::dmat4(1.0), glm::radians(degrees), axis) *
           glm::translate(glm::dmat4(1.0), -point);
}

glm::dvec3 unit(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

glm::dvec3 perpendicular(const glm::dvec3& v)
{
    const glm::dvec3 ref = std::abs(v.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    return unit(glm::cross(v, ref));
}

void addBox(Mesh& m, const glm::dvec3& lo, const glm::dvec3& hi, const glm::dmat4& xf)
{
    Index p[8];
    for (int k = 0; k < 8; ++k)
        p[k] = m.addPoint(apply(xf, glm::dvec3(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z)));
    const glm::dvec3 c = apply(xf, (lo + hi) * 0.5);
    const int faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto& f : faces)
    {
        const glm::dvec3 mid = (m.position(p[f[0]]) + m.position(p[f[2]])) * 0.5;
        faceFacing(m, {p[f[0]], p[f[1]], p[f[2]], p[f[3]]}, mid - c);
    }
}

void addExtrusion(Mesh& m, std::span<const glm::dvec2> profile, double z0, double z1, const glm::dmat4& xf)
{
    const size_t n = profile.size();
    if (n < 3)
        return;
    double area = 0.0;
    for (size_t k = 0; k < n; ++k)
        area += profile[k].x * profile[(k + 1) % n].y - profile[(k + 1) % n].x * profile[k].y;
    const double wind = area >= 0.0 ? 1.0 : -1.0;
    std::vector<Index> lo(n), hi(n);
    for (size_t k = 0; k < n; ++k)
    {
        lo[k] = m.addPoint(apply(xf, glm::dvec3(profile[k], z0)));
        hi[k] = m.addPoint(apply(xf, glm::dvec3(profile[k], z1)));
    }
    const glm::dmat3 lin(xf);
    const glm::dmat3 nrm = glm::transpose(glm::inverse(lin));
    for (size_t k = 0; k < n; ++k)
    {
        const size_t j = (k + 1) % n;
        const glm::dvec2 e = profile[j] - profile[k];
        const glm::dvec3 out = nrm * glm::dvec3(e.y * wind, -e.x * wind, 0.0);
        faceFacing(m, {lo[k], lo[j], hi[j], hi[k]}, out);
    }
    const double dz = z1 >= z0 ? 1.0 : -1.0;
    faceFacing(m, hi, nrm * glm::dvec3(0, 0, dz));
    faceFacing(m, lo, nrm * glm::dvec3(0, 0, -dz));
}

void addBeam(Mesh& m, const glm::dvec3& a, const glm::dvec3& b, double size, double extend)
{
    const glm::dvec3 d = unit(b - a, glm::dvec3(0, 1, 0));
    const glm::dvec3 s = perpendicular(d);
    const glm::dvec3 t = glm::cross(d, s);
    const double len = glm::length(b - a);
    glm::dmat4 xf(1.0);
    xf[0] = glm::dvec4(s, 0.0);
    xf[1] = glm::dvec4(t, 0.0);
    xf[2] = glm::dvec4(d, 0.0);
    xf[3] = glm::dvec4(a, 1.0);
    const double h = size * 0.5;
    addBox(m, glm::dvec3(-h, -h, -extend), glm::dvec3(h, h, len + extend), xf);
}

void addCylinder(Mesh& m, double radius, double y0, double y1, int sides, const glm::dmat4& xf)
{
    sides = std::max(3, sides);
    std::vector<glm::dvec2> ring;
    for (int k = 0; k < sides; ++k)
    {
        const double a = 2.0 * std::numbers::pi * k / sides;
        ring.emplace_back(std::cos(a) * radius, std::sin(a) * radius);
    }
    // Extruded along Z, then Z turned onto Y.
    glm::dmat4 toY(1.0);
    toY[0] = glm::dvec4(1, 0, 0, 0);
    toY[1] = glm::dvec4(0, 0, -1, 0);
    toY[2] = glm::dvec4(0, 1, 0, 0);
    addExtrusion(m, ring, y0, y1, xf * toY);
}

void addQuad(Mesh& m, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::dvec3& d)
{
    const Index f = m.addFace({m.addPoint(a), m.addPoint(b), m.addPoint(c), m.addPoint(d)});
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    const glm::vec2 corner[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    int k = 0;
    for (Index cn : m.faceCorners(f))
        uv[cn] = corner[k++ % 4];
}

void setMaterial(Mesh& m, const std::string& name)
{
    m.materialNames() = {name};
    auto mat = m.faceAttrs().getOrAdd<int32_t>("material");
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            mat[f] = 0;
}

void boxUvs(Mesh& m, double scale)
{
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    const double s = 1.0 / std::max(scale, 1e-6);
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 n = glm::abs(m.faceNormal(f));
        for (Index c : m.faceCorners(f))
        {
            const glm::dvec3 p = m.position(m.cornerPoint(c));
            if (n.x >= n.y && n.x >= n.z)
                uv[c] = glm::vec2(float(p.z * s), float(p.y * s));
            else if (n.y >= n.z)
                uv[c] = glm::vec2(float(p.x * s), float(p.z * s));
            else
                uv[c] = glm::vec2(float(p.x * s), float(p.y * s));
        }
    }
}

bool insidePolygon(std::span<const glm::dvec2> poly, const glm::dvec2& p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        const glm::dvec2& a = poly[i];
        const glm::dvec2& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

bool closedAndConnected(const Mesh& m)
{
    if (m.numFaces() == 0)
        return false;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    std::vector<uint8_t> seen(m.faceCapacity(), 0);
    std::vector<Index> stack;
    for (Index f = 0; f < m.faceCapacity() && stack.empty(); ++f)
        if (m.faceValid(f))
        {
            stack.push_back(f);
            seen[f] = 1;
        }
    size_t reached = 0;
    while (!stack.empty())
    {
        const Index f = stack.back();
        stack.pop_back();
        ++reached;
        for (Index c : m.faceCorners(f))
        {
            const Index e = m.cornerEdge(c);
            const Index o = m.oppositeFace(e, f);
            if (o != lila::geo::kInvalid && !seen[o])
            {
                seen[o] = 1;
                stack.push_back(o);
            }
        }
    }
    return reached == m.numFaces();
}

std::vector<double> parseNumbers(const std::string& s)
{
    std::vector<double> out;
    std::stringstream in(s);
    std::string item;
    while (std::getline(in, item, ','))
    {
        try
        {
            out.push_back(std::stod(item));
        }
        catch (const std::exception&)
        {
            out.push_back(1.0);
        }
    }
    return out;
}

std::string joinNumbers(std::span<const double> v)
{
    std::string out;
    for (double x : v)
    {
        std::ostringstream o;
        o << x;
        out += (out.empty() ? "" : ",") + o.str();
    }
    return out;
}

double hash01(uint32_t seed, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = seed * 0x9E3779B9u ^ (a + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (b + 0x165667B1u) * 0xC2B2AE35u ^ (c + 0x27D4EB2Fu) * 0x165667B1u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return double(h) / 4294967296.0;
}

int pickWeighted(std::span<const double> weights, int count, double u)
{
    if (count <= 0)
        return 0;
    auto w = [&](int k) { return size_t(k) < weights.size() ? std::max(0.0, weights[size_t(k)]) : 1.0; };
    double total = 0.0;
    for (int k = 0; k < count; ++k)
        total += w(k);
    if (total <= 0.0)
        return 0;
    double t = std::clamp(u, 0.0, 0.999999) * total;
    for (int k = 0; k < count; ++k)
    {
        t -= w(k);
        if (t < 0.0)
            return k;
    }
    return count - 1;
}
} // namespace weathering::gen
