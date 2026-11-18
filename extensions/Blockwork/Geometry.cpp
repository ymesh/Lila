#include "Blockwork.h"

#include <lila/MeshOps.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace blockwork
{
using namespace lila;
using geo::Index;
using geo::kInvalid;
using geo::Mesh;

namespace
{
template <class T> std::span<T> faceLayer(Mesh& m, const char* name) { return m.faceAttrs().getOrAdd<T>(name); }

template <class T> std::optional<T> faceValue(const Mesh& m, const char* name, Index f)
{
    std::span<const T> v = m.faceAttrs().get<T>(name);
    if (v.size() <= f)
        return std::nullopt;
    return v[f];
}

/** Index of the largest absolute component. */
int dominantAxis(const glm::dvec3& n)
{
    glm::dvec3 a = glm::abs(n);
    return a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
}

double signedArea(const std::vector<glm::dvec2>& p)
{
    double s = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        const glm::dvec2& a = p[i];
        const glm::dvec2& b = p[(i + 1) % p.size()];
        s += a.x * b.y - b.x * a.y;
    }
    return 0.5 * s;
}

/** Corners of a convex or concave polygon the point lies inside, on the polygon's plane. */
bool insidePolygon(const std::vector<glm::dvec3>& poly, const glm::dvec3& normal, const glm::dvec3& p, double tolerance)
{
    // Project to the plane's dominant 2D axes, then test winding, counting points on an edge as inside.
    int drop = dominantAxis(normal);
    auto to2 = [&](const glm::dvec3& q) { return drop == 0 ? glm::dvec2(q.y, q.z) : drop == 1 ? glm::dvec2(q.x, q.z) : glm::dvec2(q.x, q.y); };
    glm::dvec2 pt = to2(p);
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        glm::dvec2 a = to2(poly[i]), b = to2(poly[j]);
        glm::dvec2 ab = b - a;
        double len = glm::length(ab);
        if (len > 1e-12)
        {
            double t = std::clamp(glm::dot(pt - a, ab) / (len * len), 0.0, 1.0);
            if (glm::length(pt - (a + ab * t)) <= tolerance)
                return true;
        }
        if ((a.y > pt.y) != (b.y > pt.y) && pt.x < (b.x - a.x) * (pt.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

std::vector<glm::dvec3> facePositions(const Mesh& m, Index f)
{
    std::vector<glm::dvec3> out;
    for (Index p : m.facePoints(f))
        out.push_back(m.position(p));
    return out;
}
} // namespace

void planeBasis(const glm::dvec3& normal, glm::dvec3& u, glm::dvec3& v)
{
    const glm::dvec3 n = glm::normalize(normal);
    const glm::dvec3 a = glm::abs(n);
    if (a.y >= a.x && a.y >= a.z)
        u = {1.0, 0.0, 0.0};
    else if (a.x >= a.z)
        u = {0.0, 0.0, 1.0};
    else
        u = {1.0, 0.0, 0.0};
    u = glm::normalize(u - n * glm::dot(u, n));
    v = acrossAxis(n, u);
}

glm::dvec3 acrossAxis(const glm::dvec3& normal, const glm::dvec3& u)
{
    const glm::dvec3 v = glm::normalize(glm::cross(glm::normalize(normal), u));
    // Across runs along a positive world axis (+Z on floors, +Y on walls), whichever way the surface faces.
    return v.x + v.y + v.z < 0.0 ? -v : v;
}

FaceMapping faceMapping(const Mesh& mesh, Index f)
{
    FaceMapping m;
    if (auto v = faceValue<int32_t>(mesh, kMapMode, f))
        m.mode = *v;
    if (auto v = faceValue<glm::vec2>(mesh, kMapOffset, f))
        m.offset = *v;
    if (auto v = faceValue<glm::vec2>(mesh, kMapScale, f))
        m.scale = *v;
    if (auto v = faceValue<float>(mesh, kMapRotate, f))
        m.rotate = *v;
    if (auto v = faceValue<glm::vec3>(mesh, kMapUAxis, f))
        m.uAxis = *v;
    if (auto v = faceValue<glm::vec3>(mesh, kMapVAxis, f))
        m.vAxis = *v;
    return m;
}

void setFaceMapping(Mesh& mesh, Index f, const FaceMapping& m)
{
    faceLayer<int32_t>(mesh, kMapMode)[f] = m.mode;
    faceLayer<glm::vec2>(mesh, kMapOffset)[f] = m.offset;
    faceLayer<glm::vec2>(mesh, kMapScale)[f] = m.scale;
    faceLayer<float>(mesh, kMapRotate)[f] = m.rotate;
    faceLayer<glm::vec3>(mesh, kMapUAxis)[f] = m.uAxis;
    faceLayer<glm::vec3>(mesh, kMapVAxis)[f] = m.vAxis;
}

void mappingAxes(const FaceMapping& m, const glm::dvec3& normal, glm::dvec3& u, glm::dvec3& v)
{
    const glm::dvec3 n = glm::length(normal) > 1e-12 ? glm::normalize(normal) : glm::dvec3(0.0, 1.0, 0.0);
    if (m.mode == kMapAxes && glm::length(m.uAxis) > 1e-6f && glm::length(m.vAxis) > 1e-6f)
    {
        u = glm::normalize(glm::dvec3(m.uAxis));
        v = glm::normalize(glm::dvec3(m.vAxis));
    }
    else
    {
        // The view of someone facing the face: up is world up on walls, -Z on floors, +Z on ceilings.
        const int axis = dominantAxis(n);
        const glm::dvec3 forward = m.mode == kMapWorld ? -glm::dvec3(axis == 0 ? glm::sign(n.x) : 0.0, axis == 1 ? glm::sign(n.y) : 0.0,
                                                                     axis == 2 ? glm::sign(n.z) : 0.0)
                                                       : -n;
        glm::dvec3 up = axis == 1 ? glm::dvec3(0.0, 0.0, n.y > 0.0 ? -1.0 : 1.0) : glm::dvec3(0.0, 1.0, 0.0);
        if (m.mode != kMapWorld)
        {
            glm::dvec3 flat = up - forward * glm::dot(up, forward);
            if (axis != 1)
                flat = glm::dvec3(0.0, 1.0, 0.0) - forward * glm::dot(glm::dvec3(0.0, 1.0, 0.0), forward);
            up = glm::length(flat) > 1e-6 ? glm::normalize(flat) : up;
        }
        u = glm::normalize(glm::cross(forward, up));
        v = glm::normalize(glm::cross(u, forward));
    }
    if (m.rotate != 0.0f)
    {
        const double r = glm::radians(double(m.rotate)), c = std::cos(r), s = std::sin(r);
        const glm::dvec3 ru = u * c + v * s, rv = v * c - u * s;
        u = ru;
        v = rv;
    }
}

glm::dvec2 mapPoint(const FaceMapping& m, const glm::dvec3& normal, const glm::dvec3& p, double tileSize)
{
    glm::dvec3 u, v;
    mappingAxes(m, normal, u, v);
    const double su = m.scale.x != 0.0f ? double(m.scale.x) : tileSize, sv = m.scale.y != 0.0f ? double(m.scale.y) : tileSize;
    return {glm::dot(p, u) / su + double(m.offset.x), glm::dot(p, v) / sv + double(m.offset.y)};
}

void applyMapping(Mesh& mesh, double tileSize)
{
    tileSize = tileSize > 1e-6 ? tileSize : 100.0;
    std::span<glm::vec2> uv = mesh.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const FaceMapping m = faceMapping(mesh, f);
        if (m.mode == kMapStored)
            continue;
        const glm::dvec3 n = mesh.faceNormal(f);
        mesh.forEachFaceCorner(f, [&](Index c) { uv[c] = glm::vec2(mapPoint(m, n, mesh.position(mesh.cornerPoint(c)), tileSize)); });
    }
}

const std::vector<std::string>& shapeNames()
{
    static const std::vector<std::string> k = {"box", "wedge", "cylinder", "arch", "stairs", "prism"};
    return k;
}

std::optional<Shape> shapeFromName(const std::string& name)
{
    const auto& k = shapeNames();
    auto it = std::find(k.begin(), k.end(), name);
    return it == k.end() ? std::nullopt : std::optional<Shape>(Shape(it - k.begin()));
}

Mesh extrudeProfile(std::vector<glm::dvec2> profile, const glm::dvec3& origin, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c,
                    double depth)
{
    // Drop repeated points, including a closing point equal to the first.
    std::vector<glm::dvec2> pts;
    for (const glm::dvec2& p : profile)
        if (pts.empty() || glm::length(p - pts.back()) > 1e-6)
            pts.push_back(p);
    while (pts.size() > 1 && glm::length(pts.front() - pts.back()) <= 1e-6)
        pts.pop_back();
    if (pts.size() < 3 || std::abs(signedArea(pts)) < 1e-9 || std::abs(depth) < 1e-9)
        return {};
    // Counter-clockwise around c = a x b and a positive depth give outward faces.
    const bool rightHanded = glm::dot(glm::cross(a, b), c) > 0.0;
    if ((signedArea(pts) < 0.0) == rightHanded)
        std::reverse(pts.begin(), pts.end());
    if (depth < 0.0)
        std::reverse(pts.begin(), pts.end());

    const Index n = Index(pts.size());
    std::vector<glm::dvec3> points;
    for (int layer = 0; layer < 2; ++layer)
        for (const glm::dvec2& p : pts)
            points.push_back(origin + a * p.x + b * p.y + c * (layer ? depth : 0.0));
    std::vector<Index> sizes, idx;
    // Bottom cap faces away from c, top cap toward it.
    sizes.push_back(n);
    for (Index i = 0; i < n; ++i)
        idx.push_back(n - 1 - i);
    sizes.push_back(n);
    for (Index i = 0; i < n; ++i)
        idx.push_back(n + i);
    for (Index i = 0; i < n; ++i)
    {
        const Index j = (i + 1) % n;
        sizes.push_back(4);
        idx.insert(idx.end(), {i, j, n + j, n + i});
    }
    return Mesh::fromPolygons(points, sizes, idx);
}

void hardenEdges(Mesh& mesh, double angle)
{
    std::span<bool> hard = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    const double limit = std::cos(glm::radians(angle));
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e) || mesh.edgeFaceCount(e) != 2)
            continue;
        const Index c = mesh.edgeCorner(e);
        const Index f0 = mesh.cornerFace(c), f1 = mesh.cornerFace(mesh.cornerRadial(c));
        hard[e] = glm::dot(mesh.faceNormal(f0), mesh.faceNormal(f1)) < limit;
    }
}

Mesh buildShape(Shape shape, Frame f, const ShapeOptions& o)
{
    // Normalise: positive extents, origin at the low corner. A prism's outline is already placed from the origin.
    if (shape == Shape::Prism)
        f.size.x = std::abs(f.size.x), f.size.y = std::abs(f.size.y);
    if (f.size.x < 0.0)
        f.origin += f.u * f.size.x, f.size.x = -f.size.x;
    if (f.size.y < 0.0)
        f.origin += f.v * f.size.y, f.size.y = -f.size.y;
    // A negative height grows the shape from the surface the other way, so its base stays on the surface it was drawn on.
    if (f.size.z < 0.0)
        f.normal = -f.normal, f.size.z = -f.size.z;
    const double w = f.size.x, d = f.size.y, h = f.size.z;
    // Turn the footprint so the shape's own direction (stairs climbing toward +v) faces the requested quarter.
    glm::dvec3 U = f.u, V = f.v, O = f.origin;
    double W = w, D = d;
    switch (((o.direction % 4) + 4) % 4)
    {
    case 1: U = f.v, V = -f.u, O = f.origin + f.u * w, W = d, D = w; break;
    case 2: U = -f.u, V = -f.v, O = f.origin + f.u * w + f.v * d; break;
    case 3: U = -f.v, V = f.u, O = f.origin + f.v * d, W = d, D = w; break;
    default: break;
    }
    const glm::dvec3 N = f.normal;

    Mesh m;
    switch (shape)
    {
    case Shape::Box:
        if (w > 1e-9 && d > 1e-9 && h > 1e-9)
            m = extrudeProfile({{0, 0}, {w, 0}, {w, d}, {0, d}}, f.origin, f.u, f.v, N, h);
        else
        {
            // A flat block is one face: a floor when it has no height, a wall when its footprint is a line.
            std::vector<glm::dvec3> pts;
            if (h <= 1e-9 && w > 1e-9 && d > 1e-9)
                pts = {f.origin, f.origin + f.u * w, f.origin + f.u * w + f.v * d, f.origin + f.v * d};
            else if (h > 1e-9 && (w > 1e-9) != (d > 1e-9))
            {
                const glm::dvec3 run = w > 1e-9 ? f.u * w : f.v * d;
                pts = {f.origin, f.origin + run, f.origin + run + N * h, f.origin + N * h};
            }
            if (pts.size() == 4)
            {
                std::vector<Index> sizes = {4}, idx = {0, 1, 2, 3};
                m = Mesh::fromPolygons(pts, sizes, idx);
                if (h <= 1e-9 && glm::dot(m.faceNormal(0), N) < 0.0)
                    geo::flipNormals(m, std::vector<Index>{0});
            }
        }
        break;
    case Shape::Wedge:
        // A ramp: full height at v = 0 sloping down to the floor at v = D, profile in (V, N) run along U.
        m = extrudeProfile({{0, 0}, {D, 0}, {0, h}}, O, V, N, U, W);
        break;
    case Shape::Stairs:
    {
        int steps = o.steps > 0 ? o.steps : o.stepHeight > 1e-6 ? std::max(1, int(std::lround(h / o.stepHeight))) : 8;
        steps = std::clamp(steps, 1, 512);
        std::vector<glm::dvec2> prof = {{0, 0}, {D, 0}};
        for (int i = steps; i >= 1; --i)
        {
            const double run = D * double(i) / steps, rise = h * double(steps - i + 1) / steps;
            prof.push_back({run, rise});
            prof.push_back({run - D / steps, rise});
        }
        prof.back() = {0, h};
        // Climb toward +V: the top step is at the far end.
        for (glm::dvec2& p : prof)
            p.x = D - p.x;
        m = extrudeProfile(prof, O, V, N, U, W);
        break;
    }
    case Shape::Arch:
    {
        if (o.opening)
        {
            // The doorway: the footprint's width, round at the far end across it, run through the height.
            const double r = W * 0.5, spring = std::max(0.0, D - r), rise = std::min(r, D);
            std::vector<glm::dvec2> prof = {{0, 0}, {W, 0}, {W, spring}};
            const int segs = std::max(2, o.sides);
            for (int i = 1; i < segs; ++i)
            {
                const double a = glm::pi<double>() * double(i) / segs;
                prof.push_back({r + r * std::cos(a), spring + rise * std::sin(a)});
            }
            prof.push_back({0, spring});
            m = extrudeProfile(prof, O, U, V, N, h);
            break;
        }
        // A doorway: a frame thickness wide around an opening with a round top, open at the floor. Profile in (U, N).
        const double t = std::clamp(o.thickness, 0.0, W * 0.49);
        const double ow = W - 2.0 * t, r = ow * 0.5;
        const double spring = std::max(0.0, h - t - r);
        if (ow <= 1e-6 || h - t <= 1e-6)
        {
            m = extrudeProfile({{0, 0}, {W, 0}, {W, h}, {0, h}}, O, U, N, V, D);
            break;
        }
        std::vector<glm::dvec2> prof = {{0, 0}, {t, 0}, {t, spring}};
        const int segs = std::max(2, o.sides);
        const double rise = std::min(r, h - t - spring);
        for (int i = 1; i < segs; ++i)
        {
            const double a = glm::pi<double>() * (1.0 - double(i) / segs);
            prof.push_back({t + r + r * std::cos(a), spring + rise * std::sin(a)});
        }
        prof.insert(prof.end(), {{W - t, spring}, {W - t, 0}, {W, 0}, {W, h}, {0, h}});
        m = extrudeProfile(prof, O, U, N, V, D);
        break;
    }
    case Shape::Cylinder:
    {
        const int sides = std::clamp(o.sides, 3, 256);
        const double rx = w * 0.5, ry = d * 0.5;
        const double scale = o.radiusToCorners ? 1.0 : 1.0 / std::cos(glm::pi<double>() / sides);
        std::vector<glm::dvec2> prof;
        for (int i = 0; i < sides; ++i)
        {
            const double a = 2.0 * glm::pi<double>() * (double(i) + (o.radiusToCorners ? 0.0 : 0.5)) / sides;
            prof.push_back({rx + rx * scale * std::cos(a), ry + ry * scale * std::sin(a)});
        }
        m = extrudeProfile(prof, f.origin, f.u, f.v, N, h);
        if (m.empty())
            break;
        // Caps are faces 0 and 1; sides stay soft.
        if (o.skipCaps)
            geo::deleteFaces(m, std::vector<Index>{0, 1});
        else if (!o.ngonCaps)
            geo::pokeFaces(m, std::vector<Index>{0, 1});
        m.compact();
        break;
    }
    case Shape::Prism:
        m = extrudeProfile(o.profile, f.origin, f.u, f.v, N, h);
        break;
    }
    if (!m.empty())
        hardenEdges(m, shape == Shape::Cylinder ? 360.0 / std::clamp(o.sides, 3, 256) + 1.0 : 40.0);
    return m;
}

Mesh hollow(const Mesh& src, double thickness, bool inward, const std::vector<Index>& open)
{
    Mesh outer = src;
    outer.compact();
    // Each point moves to where its faces' planes meet once offset by the thickness (least squares, held near the
    // point when fewer than three planes meet there), so walls stay one thickness at corners.
    const double t = inward ? thickness : -thickness;
    std::vector<glm::dvec3> inner(outer.pointCapacity());
    for (Index p = 0; p < outer.pointCapacity(); ++p)
    {
        if (!outer.pointValid(p))
            continue;
        std::vector<glm::dvec3> normals;
        for (Index f : outer.pointFaces(p))
        {
            glm::dvec3 n = outer.faceNormal(f);
            if (std::none_of(normals.begin(), normals.end(), [&](const glm::dvec3& k) { return glm::dot(k, n) > 0.9999; }))
                normals.push_back(n);
        }
        const glm::dvec3 x0 = outer.position(p);
        glm::dmat3 A(1e-6);
        glm::dvec3 rhs = x0 * 1e-6;
        for (const glm::dvec3& n : normals)
        {
            A += glm::outerProduct(n, n);
            rhs += n * (glm::dot(n, x0) - t);
        }
        inner[p] = normals.empty() ? x0 : glm::inverse(A) * rhs;
    }

    // Open faces go from both walls; their rims are bridged.
    std::vector<char> isOpen(outer.faceCapacity(), 0);
    for (Index f : open)
        if (f < isOpen.size())
            isOpen[f] = 1;
    Mesh out;
    std::vector<Index> outerId(outer.pointCapacity(), kInvalid), innerId(outer.pointCapacity(), kInvalid);
    for (Index p = 0; p < outer.pointCapacity(); ++p)
        if (outer.pointValid(p))
        {
            outerId[p] = out.addPoint(outer.position(p));
            innerId[p] = out.addPoint(inner[p]);
        }
    // Copy the face mapping across with each face, so the walls texture like the block did.
    auto copyMapping = [&](Index from, Index to) { setFaceMapping(out, to, faceMapping(outer, from)); };
    for (Index f = 0; f < outer.faceCapacity(); ++f)
    {
        if (!outer.faceValid(f) || isOpen[f])
            continue;
        // The block's own surface keeps its winding inward (it faces out) and turns outward (it faces into the room).
        std::vector<Index> pts = outer.facePoints(f), a, b;
        if (!inward)
            std::reverse(pts.begin(), pts.end());
        for (Index p : pts)
            a.push_back(outerId[p]);
        for (auto it = pts.rbegin(); it != pts.rend(); ++it)
            b.push_back(innerId[*it]);
        Index fa = out.addFace(a), fb = out.addFace(b);
        if (fa != kInvalid)
            copyMapping(f, fa);
        if (fb != kInvalid)
            copyMapping(f, fb);
    }
    for (Index f = 0; f < outer.faceCapacity(); ++f)
    {
        if (!outer.faceValid(f) || !isOpen[f])
            continue;
        outer.forEachFaceCorner(f, [&](Index c) {
            const Index e = outer.cornerEdge(c);
            // Bridge only where the neighbour across the edge stays.
            bool keep = false;
            outer.forEachEdgeCorner(e, [&](Index k) { keep = keep || (outer.cornerFace(k) != f && !isOpen[outer.cornerFace(k)]); });
            if (!keep)
                return;
            const Index p0 = outer.cornerPoint(c), p1 = outer.cornerNextPoint(c);
            // The rim faces into the opening.
            std::vector<Index> quad = {outerId[p0], outerId[p1], innerId[p1], innerId[p0]};
            if (!inward)
                quad = {outerId[p1], outerId[p0], innerId[p0], innerId[p1]};
            out.addFace(quad);
        });
    }
    if (out.numFaces() > 0)
        hardenEdges(out);
    if (!src.materialNames().empty())
        out.materialNames() = src.materialNames();
    return out;
}

Mesh clip(const Mesh& src, const glm::dvec3& point, const glm::dvec3& normal, int keep)
{
    Mesh m = src;
    const glm::dvec3 n = glm::normalize(normal) * (keep < 0 ? -1.0 : 1.0);
    geo::cutAlongPlane(m, point, n, 1e-4);
    std::vector<Index> gone;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::dot(m.faceCentroid(f) - point, n) < -1e-6)
            gone.push_back(f);
    if (gone.empty())
        return m;
    geo::deleteFaces(m, gone);
    // Cap the opening with the faces the plane leaves open.
    std::vector<Index> rim;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            if (std::abs(glm::dot(a - point, n)) < 1e-3 && std::abs(glm::dot(b - point, n)) < 1e-3)
                rim.push_back(e);
        }
    if (!rim.empty())
        for (Index f : geo::fillHoles(m, rim))
            if (m.faceValid(f) && glm::dot(m.faceNormal(f), n) > 0.0)
                geo::flipNormals(m, std::vector<Index>{f});
    m.compact();
    hardenEdges(m);
    return m;
}

std::vector<Index> coveredFaces(const Mesh& a, const Mesh& b, double tolerance)
{
    std::vector<Index> out;
    for (Index fa = 0; fa < a.faceCapacity(); ++fa)
    {
        if (!a.faceValid(fa))
            continue;
        const glm::dvec3 na = a.faceNormal(fa);
        const std::vector<glm::dvec3> pa = facePositions(a, fa);
        for (Index fb = 0; fb < b.faceCapacity(); ++fb)
        {
            if (!b.faceValid(fb))
                continue;
            const glm::dvec3 nb = b.faceNormal(fb);
            if (glm::dot(na, nb) > -0.999)
                continue;
            const std::vector<glm::dvec3> pb = facePositions(b, fb);
            if (std::abs(glm::dot(pa[0] - pb[0], nb)) > tolerance)
                continue;
            if (std::all_of(pa.begin(), pa.end(), [&](const glm::dvec3& p) { return insidePolygon(pb, nb, p, tolerance); }))
            {
                out.push_back(fa);
                break;
            }
        }
    }
    return out;
}

Index faceAt(const Mesh& mesh, const glm::dvec3& point, const glm::dvec3& normal, double tolerance)
{
    Index best = kInvalid;
    double bestGap = tolerance;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const glm::dvec3 n = mesh.faceNormal(f);
        if (glm::dot(n, normal) < 0.98)
            continue;
        const std::vector<glm::dvec3> pts = facePositions(mesh, f);
        const double gap = std::abs(glm::dot(point - pts[0], n));
        if (gap <= bestGap && insidePolygon(pts, n, point - n * glm::dot(point - pts[0], n), tolerance))
            best = f, bestGap = gap;
    }
    return best;
}

double snap(double value, double step) { return step > 0.0 ? std::round(value / step) * step : value; }

glm::dvec3 snapPoint(const glm::dvec3& p, double step) { return {snap(p.x, step), snap(p.y, step), snap(p.z, step)}; }
} // namespace blockwork
