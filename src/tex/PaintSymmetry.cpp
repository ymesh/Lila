#include "tex/PaintSymmetry.h"

#include "geo/Mesh.h"

#include <algorithm>
#include <cmath>

namespace lila::tex::paint
{
struct PaintSymmetry::Index
{
    /** Sorted corner points -> triangle. */
    std::map<std::array<uint32_t, 3>, uint32_t> triangles;
    /** Per triangle its uv island, and each island's uv bounds, for per-island uv mirrors. */
    std::vector<uint32_t> islandOf;
    std::map<uint32_t, std::pair<glm::vec2, glm::vec2>> islandBounds;
};

namespace
{
constexpr double kPi = 3.14159265358979323846;

glm::dmat4 rotation(int axis, double angle)
{
    glm::dvec3 a(0.0);
    a[std::clamp(axis, 0, 2)] = 1.0;
    const double c = std::cos(angle), s = std::sin(angle);
    glm::dmat4 m(1.0);
    for (int col = 0; col < 3; ++col)
    {
        glm::dvec3 e(0.0);
        e[col] = 1.0;
        const glm::dvec3 r = e * c + glm::cross(a, e) * s + a * glm::dot(a, e) * (1.0 - c);
        m[col] = glm::dvec4(r, 0.0);
    }
    return m;
}

glm::dmat4 reflection(const geo::SymmetryPlane& p)
{
    glm::dmat4 m(1.0);
    for (int col = 0; col < 3; ++col)
    {
        glm::dvec3 e(0.0);
        e[col] = 1.0;
        m[col] = glm::dvec4(e - 2.0 * glm::dot(e, p.normal) * p.normal, 0.0);
    }
    m[3] = glm::dvec4(2.0 * p.offset * p.normal, 1.0);
    return m;
}
} // namespace

uint32_t PaintSymmetry::spatialCount() const
{
    if (group)
        return uint32_t(group->order());
    return uint32_t(std::max(radial.count, 1)) * (mirror ? 2u : 1u);
}

uint32_t PaintSymmetry::count() const { return spatialCount() * (uvMirror ? 2u : 1u); }

bool PaintSymmetry::mirrored(uint32_t copy) const
{
    // A uv mirror turns its copies over again.
    if (uvMirror && copy >= spatialCount())
        return !mirrored(copy - spatialCount());
    if (group)
        return copy < count() && group->reflects(int(copy));
    return mirror && copy >= uint32_t(std::max(radial.count, 1));
}

glm::dmat4 PaintSymmetry::transform(uint32_t copy) const
{
    // A uv mirror has no transform in space; its copies take their spatial copy's.
    if (uvMirror && copy >= spatialCount())
        return transform(copy - spatialCount());
    if (group)
    {
        if (copy >= count())
            return glm::dmat4(1.0);
        const glm::dmat3 l = group->linear(int(copy));
        glm::dmat4 t(l);
        t[3] = glm::dvec4(group->origin() - l * group->origin(), 1.0);
        return t;
    }
    const uint32_t n = uint32_t(std::max(radial.count, 1));
    const uint32_t r = copy % n;
    glm::dmat4 t = r ? rotation(radial.axis, 2.0 * kPi * double(r) / double(n)) : glm::dmat4(1.0);
    if (mirrored(copy))
        t = reflection(*mirror) * t;
    return t;
}

nlohmann::json PaintSymmetry::toJson() const
{
    if (!any())
        return nullptr;
    nlohmann::json j;
    if (group)
    {
        auto v = [](const glm::dvec3& p) { return nlohmann::json::array({p.x, p.y, p.z}); };
        j = {{"group",
              {{"origin", v(group->origin())}, {"axis", v(group->axis())}, {"reference", v(group->reference())}, {"count", group->count()},
               {"mirror", group->mirror()}}}};
    }
    else
    {
        j = {{"radial", {{"count", radial.count}, {"axis", radial.axis}}}, {"mirror", nullptr}};
        if (mirror)
            j["mirror"] = {mirror->normal.x, mirror->normal.y, mirror->normal.z, mirror->offset};
    }
    if (uvMirror)
        j["uvMirror"] = {{"axis", uvMirror->axis == 0 ? "u" : "v"}, {"center", uvMirror->center}, {"perIsland", uvMirror->perIsland}};
    return j;
}

PaintSymmetry PaintSymmetry::fromJson(const nlohmann::json& j)
{
    PaintSymmetry s;
    if (!j.is_object())
        return s;
    if (const auto u = j.find("uvMirror"); u != j.end() && u->is_object())
    {
        UvMirror m;
        m.axis = u->value("axis", std::string("u")) == "v" ? 1 : 0;
        m.center = u->value("center", 0.5);
        m.perIsland = u->value("perIsland", false);
        s.uvMirror = m;
    }
    if (const auto g = j.find("group"); g != j.end() && g->is_object())
    {
        auto v = [&](const char* key, glm::dvec3 fallback) {
            const auto it = g->find(key);
            return it != g->end() && it->is_array() && it->size() == 3 ? glm::dvec3((*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>())
                                                                      : fallback;
        };
        s.group = geo::RadialSymmetry(v("origin", glm::dvec3(0.0)), v("axis", glm::dvec3(0.0, 1.0, 0.0)), g->value("count", 2),
                                      g->value("mirror", false), v("reference", glm::dvec3(0.0)));
        return s;
    }
    if (const auto it = j.find("mirror"); it != j.end() && it->is_array() && it->size() == 4)
    {
        geo::SymmetryPlane p;
        p.normal = glm::dvec3((*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>());
        if (glm::length(p.normal) > 1e-12)
        {
            p.normal = glm::normalize(p.normal);
            p.offset = (*it)[3].get<double>();
            s.mirror = p;
        }
    }
    if (const auto it = j.find("radial"); it != j.end() && it->is_object())
    {
        s.radial.count = std::clamp(it->value("count", 1), 1, 64);
        s.radial.axis = std::clamp(it->value("axis", 1), 0, 2);
    }
    return s;
}

bool PaintSymmetry::copyPoint(const PaintMesh& mesh, uint32_t tri, float u, float v, uint32_t copy, Dab& out) const
{
    if (tri >= mesh.triangleCount())
        return false;
    if (uvMirror && copy >= spatialCount())
    {
        // The spatial copy first, then its uv mirrored across the line (or its island's middle).
        Dab base;
        base.triangle = tri, base.u = u, base.v = v;
        if (copy - spatialCount() > 0 && !copyPoint(mesh, tri, u, v, copy - spatialCount(), base))
            return false;
        glm::vec2 uv = mesh.uv(base.triangle, base.u, base.v);
        double c = uvMirror->center;
        if (uvMirror->perIsland)
        {
            if (mIndexed != &mesh || !mIndex)
            {
                mIndex = std::make_shared<Index>();
                mIndexed = &mesh;
            }
            if (mIndex->islandOf.empty())
            {
                mIndex->islandOf = mesh.islands();
                for (uint32_t t = 0; t < mesh.triangleCount(); ++t)
                {
                    auto it = mIndex->islandBounds.try_emplace(mIndex->islandOf[t], glm::vec2(1e30f), glm::vec2(-1e30f)).first;
                    auto& [lo, hi] = it->second;
                    for (int k = 0; k < 3; ++k)
                    {
                        lo = glm::min(lo, mesh.uvs[t * 3 + size_t(k)]);
                        hi = glm::max(hi, mesh.uvs[t * 3 + size_t(k)]);
                    }
                }
            }
            const auto& [lo, hi] = mIndex->islandBounds[mIndex->islandOf[base.triangle]];
            c = 0.5 * (lo[uvMirror->axis] + hi[uvMirror->axis]);
        }
        uv[uvMirror->axis] = float(2.0 * c - uv[uvMirror->axis]);
        uint32_t t2 = 0;
        float u2 = 0.0f, v2 = 0.0f;
        if (!mesh.atUv(uv, t2, u2, v2, 1e-3f) || !mesh.paintable[t2])
            return false;
        out.triangle = t2;
        out.u = u2;
        out.v = v2;
        return true;
    }
    // A plain mirror (or a radial group's element) follows the mesh's point pairs when there are some: exact on a
    // deformed mesh too.
    if (map && (group ? copy < count() : copy == uint32_t(std::max(radial.count, 1))) && mesh.points.size() == mesh.triangleCount() * 3)
    {
        if (mIndexed != &mesh || !mIndex)
        {
            mIndex = std::make_shared<Index>();
            mIndexed = &mesh;
        }
        if (mIndex->triangles.empty())
        {
            for (uint32_t t = 0; t < mesh.triangleCount(); ++t)
            {
                std::array<uint32_t, 3> k = {mesh.points[t * 3], mesh.points[t * 3 + 1], mesh.points[t * 3 + 2]};
                std::sort(k.begin(), k.end());
                mIndex->triangles.emplace(k, t);
            }
            mIndexed = &mesh;
        }
        std::array<uint32_t, 3> q;
        bool ok = true;
        for (int i = 0; i < 3; ++i)
        {
            const geo::Index point = mesh.points[size_t(tri) * 3 + size_t(i)];
            const geo::Index m = group ? map->image(int(copy), point) : map->mirrorPoint(point);
            ok = ok && m != geo::kInvalid;
            q[size_t(i)] = uint32_t(m);
        }
        if (ok)
        {
            std::array<uint32_t, 3> key = q;
            std::sort(key.begin(), key.end());
            if (auto it = mIndex->triangles.find(key); it != mIndex->triangles.end() && mesh.paintable[it->second])
            {
                const uint32_t t2 = it->second;
                const float w[3] = {1.0f - u - v, u, v};
                float w2[3] = {0.0f, 0.0f, 0.0f};
                for (int j = 0; j < 3; ++j)
                    for (int i = 0; i < 3; ++i)
                        if (mesh.points[size_t(t2) * 3 + size_t(j)] == q[size_t(i)])
                            w2[j] = w[i];
                out.triangle = t2;
                out.u = w2[1];
                out.v = w2[2];
                return true;
            }
        }
    }
    const glm::dvec3 p = glm::dvec3(transform(copy) * glm::dvec4(glm::dvec3(mesh.point(tri, u, v)), 1.0));
    geo::BvhClosest hit;
    // The copy of a surface point lies on the surface of a symmetric mesh; allow some asymmetry.
    const glm::vec3* c = &mesh.positions[size_t(tri) * 3];
    const double reach = std::max({glm::length(glm::dvec3(c[1] - c[0])), glm::length(glm::dvec3(c[2] - c[0])), 1e-6});
    if (!mesh.bvh->closestPoint(p, hit, reach) || hit.triangle >= mesh.triangleCount() || !mesh.paintable[hit.triangle])
        return false;
    out.triangle = uint32_t(hit.triangle);
    out.u = float(hit.bary.y);
    out.v = float(hit.bary.z);
    return true;
}

std::vector<Dab> PaintSymmetry::copies(const PaintMesh& mesh, const Dab& d) const
{
    std::vector<Dab> out{d};
    out.back().copy = 0;
    if (!any() || d.triangle >= mesh.triangleCount())
        return out;
    const glm::vec3 n0 = mesh.normal(d.triangle, d.u, d.v), t0 = tipFrame(n0), b0 = glm::cross(n0, t0);
    const glm::vec3 axis = t0 * std::cos(d.angle) + b0 * std::sin(d.angle);
    for (uint32_t k = 1; k < count(); ++k)
    {
        Dab c = d;
        if (!copyPoint(mesh, d.triangle, d.u, d.v, k, c))
            continue;
        c.copy = k;
        if (uvMirror && k >= spatialCount())
        {
            // Mirrored in uv: the tip turns over the way the uv axis flips.
            c.angle = uvMirror->axis == 0 ? float(kPi) - d.angle : -d.angle;
            out.push_back(c);
            continue;
        }
        // The tip's axis carried over, measured from the frame where the copy lands.
        const glm::vec3 a = glm::vec3(glm::dmat3(transform(k)) * glm::dvec3(axis));
        const glm::vec3 n = mesh.normal(c.triangle, c.u, c.v), t = tipFrame(n), b = glm::cross(n, t);
        c.angle = std::atan2(glm::dot(a, b), glm::dot(a, t));
        out.push_back(c);
    }
    return out;
}

PaintSymmetry brushSymmetry(const brush::BrushSettings& b, const geo::Mesh* mesh, const std::optional<geo::SymmetrySpec>& spec)
{
    if (!spec || !spec->radial || !b.mirror)
        return brushSymmetry(b, mesh, spec ? std::clamp(spec->axis, 0, 2) : 0, spec ? spec->offset : 0.0);
    PaintSymmetry s;
    s.group = *spec->radial;
    if (mesh)
    {
        geo::Mesh copy = *mesh;
        auto map = std::make_shared<geo::SymmetryMap>(geo::SymmetryMap::persistent(copy, *spec->radial, 1e-3));
        if (!map->empty())
            s.map = map;
    }
    return s;
}

PaintSymmetry brushSymmetry(const brush::BrushSettings& b, const geo::Mesh* mesh, int axis, double offset)
{
    PaintSymmetry s;
    s.radial = b.radial;
    if (b.mirror)
    {
        s.mirror = geo::axisPlane(axis, offset);
        if (mesh)
        {
            geo::Mesh copy = *mesh;
            auto map = std::make_shared<geo::SymmetryMap>(geo::SymmetryMap::persistent(copy, axis, 1e-3, nullptr, offset));
            if (!map->empty())
                s.map = map;
        }
    }
    return s;
}
} // namespace lila::tex::paint
