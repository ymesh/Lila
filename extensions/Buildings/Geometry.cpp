// Rings, triangulation with holes, the soup buildings are written into and its mesh, colours and hashes.

#include "Buildings.h"

#include <lila/Mesh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace buildings
{
double signedArea(const Ring& r)
{
    double a = 0.0;
    for (size_t i = 0, n = r.size(); i < n; ++i)
    {
        const Vec2 p = r[i], q = r[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

Ring cleanRing(Ring r, bool ccw)
{
    Ring out;
    for (const Vec2& p : r)
        if (out.empty() || glm::length(p - out.back()) > 0.5)
            out.push_back(p);
    while (out.size() > 1 && glm::length(out.front() - out.back()) <= 0.5)
        out.pop_back();
    // Collinear points make zero-width facades.
    for (bool changed = true; changed && out.size() > 3;)
    {
        changed = false;
        for (size_t i = 0; i < out.size() && out.size() > 3; ++i)
        {
            const Vec2 a = out[(i + out.size() - 1) % out.size()], b = out[i], c = out[(i + 1) % out.size()];
            const double cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::abs(cross) < 1e-3 * std::max(1.0, glm::length(c - a)))
            {
                out.erase(out.begin() + std::ptrdiff_t(i));
                changed = true;
                break;
            }
        }
    }
    if (out.size() >= 3 && (signedArea(out) > 0.0) != ccw)
        std::reverse(out.begin(), out.end());
    return out;
}

Vec2 centroid(const Ring& r)
{
    double a = 0.0;
    Vec2 c{0.0};
    for (size_t i = 0, n = r.size(); i < n; ++i)
    {
        const Vec2 p = r[i], q = r[(i + 1) % n];
        const double w = p.x * q.y - q.x * p.y;
        a += w;
        c += (p + q) * w;
    }
    if (std::abs(a) < 1e-9)
    {
        Vec2 m{0.0};
        for (const Vec2& p : r)
            m += p;
        return r.empty() ? m : m / double(r.size());
    }
    return c / (3.0 * a);
}

bool inside(const Ring& r, Vec2 p)
{
    bool in = false;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
            in = !in;
    return in;
}

Ring offsetRing(const Ring& r, double d)
{
    Ring out;
    const size_t n = r.size();
    const double sign = signedArea(r) >= 0.0 ? 1.0 : -1.0;
    for (size_t i = 0; i < n; ++i)
    {
        const Vec2 a = r[(i + n - 1) % n], b = r[i], c = r[(i + 1) % n];
        const Vec2 n0 = outward(a, b) * sign, n1 = outward(b, c) * sign;
        Vec2 m = n0 + n1;
        const double l = glm::length(m);
        if (l < 1e-9)
        {
            out.push_back(b + n0 * d);
            continue;
        }
        m /= l;
        const double cosHalf = std::max(0.25, glm::dot(m, n0));
        out.push_back(b + m * (d / cosHalf));
    }
    return out;
}

namespace
{
Ring clipHalf(const Ring& poly, Vec2 a, Vec2 b)
{
    Ring out;
    auto side = [&](Vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
    for (size_t i = 0, n = poly.size(); i < n; ++i)
    {
        const Vec2 p = poly[i], q = poly[(i + 1) % n];
        const double sp = side(p), sq = side(q);
        if (sp >= 0.0)
            out.push_back(p);
        if ((sp >= 0.0) != (sq >= 0.0))
            out.push_back(p + (q - p) * (sp / (sp - sq)));
    }
    return out;
}

double cross2(Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

bool pointInTri(Vec2 p, Vec2 a, Vec2 b, Vec2 c)
{
    const double d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
    return d1 > 1e-9 && d2 > 1e-9 && d3 > 1e-9;
}

/** Ear clipping of a counter-clockwise polygon given as indices into pts; tolerant of the duplicate bridge points. */
void earClipIdx(const std::vector<Vec2>& pts, std::vector<uint32_t> idx, std::vector<uint32_t>& tris)
{
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < 100000)
    {
        bool cut = false;
        const size_t n = idx.size();
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t ia = idx[(i + n - 1) % n], ib = idx[i], ic = idx[(i + 1) % n];
            const Vec2 a = pts[ia], b = pts[ib], c = pts[ic];
            if (cross2(a, b, c) <= 1e-9)
                continue;
            bool blocked = false;
            for (size_t k = 0; k < n && !blocked; ++k)
            {
                const uint32_t ik = idx[k];
                if (ik == ia || ik == ib || ik == ic)
                    continue;
                const Vec2 p = pts[ik];
                if (glm::length(p - a) < 1e-6 || glm::length(p - b) < 1e-6 || glm::length(p - c) < 1e-6)
                    continue;
                blocked = pointInTri(p, a, b, c);
            }
            if (blocked)
                continue;
            tris.insert(tris.end(), {ia, ib, ic});
            idx.erase(idx.begin() + std::ptrdiff_t(i));
            cut = true;
            break;
        }
        if (!cut)
        {
            // Degenerate leftovers: fan them so nothing is dropped silently.
            for (size_t i = 1; i + 1 < idx.size(); ++i)
                tris.insert(tris.end(), {idx[0], idx[i], idx[i + 1]});
            return;
        }
    }
    if (idx.size() == 3)
        tris.insert(tris.end(), {idx[0], idx[1], idx[2]});
}
} // namespace

double overlapArea(const Ring& a0, const Ring& b0)
{
    const Ring a = cleanRing(a0), b = cleanRing(b0);
    if (a.size() < 3 || b.size() < 3)
        return 0.0;
    // b in triangles (each convex), a clipped against each.
    std::vector<Vec2> pts(b.begin(), b.end());
    std::vector<uint32_t> idx(b.size()), tris;
    for (uint32_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    earClipIdx(pts, idx, tris);
    double total = 0.0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        Ring clip = a;
        const Vec2 p[3] = {pts[tris[t]], pts[tris[t + 1]], pts[tris[t + 2]]};
        for (int e = 0; e < 3 && clip.size() >= 3; ++e)
            clip = clipHalf(clip, p[e], p[(e + 1) % 3]);
        if (clip.size() >= 3)
            total += std::abs(signedArea(clip));
    }
    return total;
}

void triangulateWithHoles(const Ring& outer, const std::vector<Ring>& holes, std::vector<Vec2>& pts, std::vector<uint32_t>& tris)
{
    pts.clear();
    tris.clear();
    Ring o = cleanRing(outer, true);
    if (o.size() < 3)
        return;
    std::vector<uint32_t> poly;
    for (const Vec2& p : o)
    {
        poly.push_back(uint32_t(pts.size()));
        pts.push_back(p);
    }
    std::vector<Ring> hs;
    for (const Ring& h : holes)
    {
        Ring c = cleanRing(h, false);
        if (c.size() >= 3)
            hs.push_back(std::move(c));
    }
    // Rightmost holes first, each bridged from its rightmost point to the nearest visible outline point.
    std::sort(hs.begin(), hs.end(), [](const Ring& a, const Ring& b) {
        auto mx = [](const Ring& r) {
            double m = -1e300;
            for (const Vec2& p : r)
                m = std::max(m, p.x);
            return m;
        };
        return mx(a) > mx(b);
    });
    for (const Ring& h : hs)
    {
        size_t hi = 0;
        for (size_t k = 1; k < h.size(); ++k)
            if (h[k].x > h[hi].x)
                hi = k;
        const Vec2 m = h[hi];
        size_t best = 0;
        double bestD = 1e300;
        for (size_t k = 0; k < poly.size(); ++k)
        {
            const Vec2 p = pts[poly[k]];
            const double d = glm::length(p - m);
            if (d >= bestD)
                continue;
            // Visible: the bridge crosses no outline or hole edge.
            bool crosses = false;
            auto hits = [&](Vec2 a, Vec2 b) {
                if (glm::length(a - p) < 1e-6 || glm::length(b - p) < 1e-6 || glm::length(a - m) < 1e-6 || glm::length(b - m) < 1e-6)
                    return false;
                const double d1 = cross2(m, p, a), d2 = cross2(m, p, b), d3 = cross2(a, b, m), d4 = cross2(a, b, p);
                return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
            };
            for (size_t e = 0; e < poly.size() && !crosses; ++e)
                crosses = hits(pts[poly[e]], pts[poly[(e + 1) % poly.size()]]);
            for (const Ring& other : hs)
                for (size_t e = 0; e < other.size() && !crosses; ++e)
                    crosses = hits(other[e], other[(e + 1) % other.size()]);
            if (!crosses)
            {
                bestD = d;
                best = k;
            }
        }
        std::vector<uint32_t> insert;
        for (size_t k = 0; k <= h.size(); ++k)
        {
            insert.push_back(uint32_t(pts.size()));
            pts.push_back(h[(hi + k) % h.size()]);
        }
        insert.push_back(uint32_t(pts.size()));
        pts.push_back(pts[poly[best]]);
        poly.insert(poly.begin() + std::ptrdiff_t(best) + 1, insert.begin(), insert.end());
    }
    earClipIdx(pts, poly, tris);
}

// Soup ------------------------------------------------------------------------------------------------------------------

const char* matName(int m)
{
    static const char* kNames[MatCount] = {"bld_render", "bld_trim",  "bld_base", "bld_roof",     "bld_rooftop", "bld_glass",
                                           "bld_frame",  "bld_door",  "bld_metal", "bld_sign",    "bld_signFace", "bld_awning",
                                           "bld_concrete", "bld_stone", "bld_water", "bld_wood", "bld_dark", "bld_glassClear"};
    return m >= 0 && m < MatCount ? kNames[m] : "bld_render";
}

const TrimStrip& trimStrip(int t)
{
    static const TrimStrip kStrips[TrimCount] = {
        {"Render", 1280, 500.0, "Flat"},    {"Plinth", 256, 100.0, "Chipped"}, {"Cornice", 128, 50.0, "Ogee"},
        {"Band", 64, 25.0, "Raised"},       {"Coping", 64, 25.0, "Step"},      {"Pilaster", 64, 25.0, "Fluting"},
        {"RoofTile", 128, 50.0, "Ribs"},    {"Sheet", 64, 25.0, "Ribs"},
    };
    return kStrips[std::clamp(t, 0, int(TrimCount) - 1)];
}

void trimRange(int t, double& v0, double& v1)
{
    int top = 0;
    for (int k = 0; k < t; ++k)
        top += trimStrip(k).px;
    const int bottom = top + trimStrip(t).px;
    v1 = 1.0 - double(top) / 2048.0;
    v0 = 1.0 - double(bottom) / 2048.0;
}

void Soup::clear() { *this = Soup{}; }

void Soup::reserve(size_t f)
{
    sizes.reserve(f);
    mat.reserve(f);
    trim.reserve(f);
    owner.reserve(f);
    part.reserve(f);
    bay.reserve(f);
    colour.reserve(f);
    pts.reserve(f * 4);
    uvs.reserve(f * 4);
}

void Soup::shrink()
{
    sizes.shrink_to_fit();
    mat.shrink_to_fit();
    trim.shrink_to_fit();
    owner.shrink_to_fit();
    part.shrink_to_fit();
    bay.shrink_to_fit();
    colour.shrink_to_fit();
    pts.shrink_to_fit();
    uvs.shrink_to_fit();
}

void Soup::poly(const Vec3* p, const glm::vec2* uv, size_t n, int32_t m, int32_t t, int32_t o, int32_t pt, int32_t b, glm::vec4 c)
{
    if (n < 3)
        return;
    for (size_t k = 0; k < n; ++k)
    {
        pts.push_back(p[k]);
        uvs.push_back(uv ? uv[k] : glm::vec2(0.0f));
    }
    sizes.push_back(uint32_t(n));
    mat.push_back(m);
    trim.push_back(t);
    owner.push_back(o);
    part.push_back(pt);
    bay.push_back(b);
    colour.push_back(c);
}

void Soup::append(const Soup& o, int32_t ownerOffset, int32_t bayOffset)
{
    pts.insert(pts.end(), o.pts.begin(), o.pts.end());
    uvs.insert(uvs.end(), o.uvs.begin(), o.uvs.end());
    sizes.insert(sizes.end(), o.sizes.begin(), o.sizes.end());
    mat.insert(mat.end(), o.mat.begin(), o.mat.end());
    trim.insert(trim.end(), o.trim.begin(), o.trim.end());
    part.insert(part.end(), o.part.begin(), o.part.end());
    colour.insert(colour.end(), o.colour.begin(), o.colour.end());
    for (int32_t v : o.owner)
        owner.push_back(v < 0 ? v : v + ownerOffset);
    for (int32_t v : o.bay)
        bay.push_back(v < 0 ? v : v + bayOffset);
}

lila::geo::Mesh toMesh(const Soup& s, const std::vector<std::string>& materialNames)
{
    using namespace lila::geo;
    std::vector<Index> sizes(s.sizes.begin(), s.sizes.end()), idx(s.pts.size());
    for (size_t k = 0; k < idx.size(); ++k)
        idx[k] = Index(k);
    Mesh m = Mesh::fromPolygons(s.pts, sizes, idx);
    m.materialNames() = materialNames;
    const size_t nf = m.numFaces();
    // fromPolygons keeps face order when no face is skipped; the soup never holds degenerate faces it knows of.
    if (nf != s.sizes.size())
        return m;
    auto& fa = m.faceAttrs();
    for (std::string_view n : {attr::kMaterial, std::string_view("part"), std::string_view("bldBay"), std::string_view("trim")})
        fa.add(n, AttrType::Int);
    auto& ca = m.cornerAttrs();
    ca.add(attr::uvName(0), AttrType::Float2);
    // The first colour set: what exporters write as the vertex colour MF_LilaBuildingVariation reads in Unreal.
    colors::add(m, "colorSet");
    auto mat = fa.get<int32_t>(attr::kMaterial);
    auto part = fa.get<int32_t>("part");
    auto bay = fa.get<int32_t>("bldBay");
    auto trim = fa.get<int32_t>("trim");
    auto uv = ca.get<glm::vec2>(attr::uvName(0));
    auto col = colors::values(m, "colorSet");
    for (size_t f = 0; f < nf; ++f)
    {
        mat[f] = s.mat[f];
        part[f] = s.part[f];
        bay[f] = s.bay[f];
        trim[f] = s.trim[f] + 1;
    }
    // Points are unique per corner and in order, so corner c of face f sits on point offset + c.
    for (Index f = 0; f < Index(nf); ++f)
        for (Index c : m.faceCorners(f))
        {
            const Index p = m.cornerPoint(c);
            uv[c] = s.uvs[p];
            col[c] = s.colour[f];
        }
    return m;
}

// Colours and hashes ----------------------------------------------------------------------------------------------------

namespace
{
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float toSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
} // namespace

glm::vec3 hexColour(std::string_view hex, glm::vec3 fallback)
{
    std::string h(hex);
    if (!h.empty() && h[0] == '#')
        h.erase(0, 1);
    if (h.size() == 3)
        h = {h[0], h[0], h[1], h[1], h[2], h[2]};
    if (h.size() != 6)
        return fallback;
    unsigned v = 0;
    if (std::sscanf(h.c_str(), "%x", &v) != 1)
        return fallback;
    return {toLinear(float((v >> 16) & 255) / 255.0f), toLinear(float((v >> 8) & 255) / 255.0f), toLinear(float(v & 255) / 255.0f)};
}

std::string colourHex(glm::vec3 c)
{
    char buf[8];
    auto q = [](float v) { return int(std::lround(std::clamp(toSrgb(std::clamp(v, 0.0f, 1.0f)), 0.0f, 1.0f) * 255.0f)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", q(c.r), q(c.g), q(c.b));
    return buf;
}

uint64_t fnv(std::string_view s, uint64_t h)
{
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(uint64_t v)
{
    char buf[20];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
    return buf;
}

double unitRandom(uint64_t key, uint64_t salt)
{
    uint64_t x = key ^ (salt * 0x9E3779B97F4A7C15ull);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return double(x >> 11) / double(1ull << 53);
}
void measureClearance(std::vector<Spec>& specs, double reach)
{
    struct Seg
    {
        Vec2 a, b;
        size_t owner;
    };
    std::vector<Seg> segs;
    const double cell = std::max(reach, 500.0);
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    auto key = [](int64_t x, int64_t z) { return (uint64_t(uint32_t(int32_t(x))) << 32) | uint32_t(int32_t(z)); };
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const Ring& r = specs[i].footprint;
        for (size_t e = 0; e < r.size(); ++e)
        {
            const Seg s{r[e], r[(e + 1) % r.size()], i};
            const Vec2 lo = glm::min(s.a, s.b), hi = glm::max(s.a, s.b);
            for (int64_t x = int64_t(std::floor(lo.x / cell)); x <= int64_t(std::floor(hi.x / cell)); ++x)
                for (int64_t z = int64_t(std::floor(lo.y / cell)); z <= int64_t(std::floor(hi.y / cell)); ++z)
                    grid[key(x, z)].push_back(uint32_t(segs.size()));
            segs.push_back(s);
        }
    }
    std::vector<uint32_t> near;
    for (size_t i = 0; i < specs.size(); ++i)
    {
        Spec& sp = specs[i];
        const Ring& r = sp.footprint;
        const double sign = signedArea(r) >= 0.0 ? 1.0 : -1.0;
        sp.clearance.assign(r.size(), reach);
        for (size_t e = 0; e < r.size(); ++e)
        {
            const Vec2 a = r[e], b = r[(e + 1) % r.size()];
            if (glm::length(b - a) < 1.0)
                continue;
            const Vec2 n = outward(a, b) * sign;
            const Vec2 lo = glm::min(glm::min(a, b), glm::min(a, b) + n * reach), hi = glm::max(glm::max(a, b), glm::max(a, b) + n * reach);
            near.clear();
            for (int64_t x = int64_t(std::floor(lo.x / cell)); x <= int64_t(std::floor(hi.x / cell)); ++x)
                for (int64_t z = int64_t(std::floor(lo.y / cell)); z <= int64_t(std::floor(hi.y / cell)); ++z)
                    if (auto it = grid.find(key(x, z)); it != grid.end())
                        near.insert(near.end(), it->second.begin(), it->second.end());
            std::sort(near.begin(), near.end());
            near.erase(std::unique(near.begin(), near.end()), near.end());
            // The most open of three points along the wall: a neighbour has to face the whole wall to close it.
            double open = 0.0;
            for (double t : {0.15, 0.5, 0.85})
            {
                const Vec2 p = a + (b - a) * t;
                double hit = reach;
                for (uint32_t k : near)
                {
                    const Seg& s = segs[k];
                    if (s.owner == i)
                        continue;
                    const Vec2 d = s.b - s.a;
                    const double den = n.x * d.y - n.y * d.x;
                    if (std::abs(den) < 1e-12)
                        continue;
                    const Vec2 w = s.a - p;
                    const double along = (w.x * d.y - w.y * d.x) / den, u = (w.x * n.y - w.y * n.x) / den;
                    if (u >= 0.0 && u <= 1.0 && along >= -2.0)
                        hit = std::min(hit, std::max(0.0, along));
                }
                open = std::max(open, hit);
            }
            sp.clearance[e] = open;
        }
    }
}

} // namespace buildings
