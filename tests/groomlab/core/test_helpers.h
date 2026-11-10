// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/portable_math.h"
#include "groomlab/presets.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab::test {

// Whether this build evaluates sin/exp/pow... as the bit-exact recordings were made
// (GROOMLAB_PORTABLE_MATH: the same bits on every platform, portable_math.h). A build with it off
// (the C runtime's float functions) checks what follows from them within the tolerances alone.
#if GROOMLAB_PORTABLE_MATH
constexpr bool kRecordedMath = true;
#else
constexpr bool kRecordedMath = false;
#endif

// Unit square in the XZ plane, normal +Y, split into cells x cells quads.
inline std::shared_ptr<const ScalpMesh> makePlane(int cells = 4, float size = 1.0f) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    const float step = size / static_cast<float>(cells);
    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) points.emplace_back(x * step, 0.0f, z * step);
    }
    auto at = [cells](int x, int z) { return z * (cells + 1) + x; };
    for (int z = 0; z < cells; ++z) {
        for (int x = 0; x < cells; ++x) {
            tris.push_back({at(x, z), at(x, z + 1), at(x + 1, z)});
            tris.push_back({at(x, z + 1), at(x + 1, z + 1), at(x + 1, z)});
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(tris));
}

// UV sphere of `radius`, `rings` x `segments` quads split into triangles
// (outward normals). Pole rows are degenerate triangles; fine for queries.
inline std::shared_ptr<const ScalpMesh> makeSphere(int rings, int segments, float radius = 1.0f) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int r = 0; r <= rings; ++r) {
        const float theta = 3.14159265f * static_cast<float>(r) / rings;
        for (int s = 0; s <= segments; ++s) {
            const float phi = 6.2831853f * static_cast<float>(s) / segments;
            points.emplace_back(radius * std::sin(theta) * std::cos(phi), radius * std::cos(theta),
                                radius * std::sin(theta) * std::sin(phi));
        }
    }
    auto at = [segments](int r, int s) { return r * (segments + 1) + s; };
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            tris.push_back({at(r, s), at(r, s + 1), at(r + 1, s)});
            tris.push_back({at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)});
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(tris));
}

// A thin flap, as an ear stands out of a head: two faces `thickness` apart (z = -thickness / 2,
// normal -z, and z = +thickness / 2, normal +z) rising from y = 0 to `height`, joined over the top
// by a half cylinder about the x axis, all along x in [0, depth]; with `ground`, standing on a
// floor (y = 0, normal +y) out to z = -halfWidth and +halfWidth, one surface with it. About
// `step` between vertices.
inline std::shared_ptr<const ScalpMesh> makeFin(float height, float thickness, float depth, float step, bool ground = false,
                                               float halfWidth = 0.0f) {
    // The profile in (y, z), from the floor on the -z side round to the floor on the +z side.
    std::vector<std::array<float, 2>> profile;
    auto line = [&](float y0, float z0, float y1, float z1) {
        const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(y1 - y0, z1 - z0) / step)));
        for (int i = profile.empty() ? 0 : 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            profile.push_back({y0 + (y1 - y0) * t, z0 + (z1 - z0) * t});
        }
    };
    const float r = 0.5f * thickness;
    if (ground) line(0.0f, -halfWidth, 0.0f, -r);
    line(0.0f, -r, height, -r);
    const int arc = std::max(4, static_cast<int>(std::ceil(3.14159265f * r / step)));
    for (int i = 1; i <= arc; ++i) {
        const float a = 3.14159265f * static_cast<float>(i) / static_cast<float>(arc);
        profile.push_back({height + r * std::sin(a), -r * std::cos(a)});
    }
    line(height, r, 0.0f, r);
    if (ground) line(0.0f, r, 0.0f, halfWidth);
    const int columns = std::max(1, static_cast<int>(std::ceil(depth / step)));
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    const int rows = static_cast<int>(profile.size());
    for (int c = 0; c <= columns; ++c) {
        const float x = depth * static_cast<float>(c) / static_cast<float>(columns);
        for (const auto& p : profile) points.emplace_back(x, p[0], p[1]);
    }
    auto at = [rows](int c, int i) { return c * rows + i; };
    for (int c = 0; c < columns; ++c) {
        for (int i = 0; i + 1 < rows; ++i) {
            tris.push_back({at(c, i), at(c, i + 1), at(c + 1, i)});
            tris.push_back({at(c, i + 1), at(c + 1, i + 1), at(c + 1, i)});
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(tris));
}

// A straight guide from `root` along `dir`, `segments` + 1 points.
inline std::vector<Vec3f> straightCurve(Vec3f root, Vec3f dir, int segments = 4) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= segments; ++i) pts.push_back(root + dir * (static_cast<float>(i) / segments));
    return pts;
}

// A camera at `eye` looking at `target` (y up), vertical field of view `fovDegrees`, as Maya's
// world-to-clip matrix (row vectors: clip = [x y z 1] * M, rows first; OpenGL clip space).
inline std::array<double, 16> lookAtProjection(Vec3f eye, Vec3f target, double fovDegrees, double aspect) {
    const Vec3f f = (target - eye).normalized();
    const Vec3f r = f.cross(Vec3f(0, 1, 0)).normalized();
    const Vec3f u = r.cross(f);
    // Column-vector view matrix rows: r, u, -f; translation -R e.
    const double view[4][4] = {{r.x, r.y, r.z, -r.dot(eye)}, {u.x, u.y, u.z, -u.dot(eye)}, {-f.x, -f.y, -f.z, f.dot(eye)}, {0, 0, 0, 1}};
    const double t = 1.0 / std::tan(fovDegrees * 3.14159265358979 / 360.0), n = 0.1, fa = 100.0;
    const double proj[4][4] = {{t / aspect, 0, 0, 0}, {0, t, 0, 0}, {0, 0, -(fa + n) / (fa - n), -2 * fa * n / (fa - n)}, {0, 0, -1, 0}};
    double pv[4][4] = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 4; ++k) pv[i][j] += proj[i][k] * view[k][j];
        }
    }
    std::array<double, 16> m{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) m[static_cast<std::size_t>(row * 4 + col)] = pv[col][row];
    }
    return m;
}

// The share of its radius Curl gives a point `arc` along its strand, at share t of its points,
// turning at `frequency`: eased in from the root over the first quarter turn, or the first quarter
// of the strand where that comes first (curl_op.cpp curlEase).
inline float curlEase(float arc, float t, float frequency) {
    const float x = std::min(1.0f, std::max(4.0f * std::fabs(frequency) * arc, 4.0f * t));
    return x * x * (3.0f - 2.0f * x);
}

// The guide groups a preset reads ("requires": {"groups"}), made as a host makes them on applying
// it, but drawn in: each holds a copy of every fourth of the groom's guides (the hierarchy's, else
// the flat ones), as an artist would draw a few hand-placed feathers, so what reads it grows
// something. A group the groom has already is left alone.
inline void drawRequiredGroups(const std::string& presetJson, GroomData& data) {
    const std::vector<PresetGroup> groups = presetRequiredGroups(presetJson);
    if (groups.empty()) return;
    GuideHierarchy hierarchy = data.hierarchyJson.empty() ? GuideHierarchy() : GuideHierarchy::fromJson(data.hierarchyJson);
    const GuideSet existing = !hierarchy.guides().empty() ? hierarchy.flatten(-1) : data.guides ? *data.guides : GuideSet();
    for (const PresetGroup& g : groups) {
        if (hierarchy.findGroup(g.name) != kNoGroup) continue;
        const GroupId id = hierarchy.addGroup(g.name);
        for (std::size_t i = 0; i < existing.guides().size(); i += 4) hierarchy.addGuide(id, existing.guides()[i].points);
    }
    hierarchy.evaluate(data.scalp.get());
    data.hierarchyJson = hierarchy.toJson();
}

// The painted maps a preset reads ("requires"), made as a host makes them on applying it, but
// painted: each a vertex map over the scalp, its channels triangle waves across the scalp's bounds
// (bands of 0 to 1, from basic arithmetic alone, so the same bits on every platform), so what
// reads them reads something varying (both looks of a Blend by a map, for one). The guide groups
// it reads too (drawRequiredGroups).
inline void paintRequiredMaps(const std::string& presetJson, GroomData& data) {
    drawRequiredGroups(presetJson, data);
    const std::vector<PresetMap> maps = presetRequiredMaps(presetJson);
    if (maps.empty() || !data.scalp) return;
    auto scalp = std::make_shared<ScalpMesh>(*data.scalp);
    Vec3f lo = scalp->points().front(), hi = lo;
    for (const Vec3f& p : scalp->points()) {
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const Vec3f extent(std::max(hi.x - lo.x, 1e-6f), std::max(hi.y - lo.y, 1e-6f), std::max(hi.z - lo.z, 1e-6f));
    for (const PresetMap& m : maps) {
        std::vector<ScalpMesh::Color> colors(scalp->triangleCount() * 3);
        for (std::size_t c = 0; c < colors.size(); ++c) {
            const Vec3f p = scalp->points()[static_cast<std::size_t>(scalp->triangles()[c / 3][c % 3])];
            const float u = (p.x - lo.x) / extent.x, v = (p.y - lo.y) / extent.y, w = (p.z - lo.z) / extent.z;
            for (int k = 0; k < 3; ++k) {
                const float t = 1.5f * u + static_cast<float>(k + 1) * v + 0.5f * w;
                colors[c][static_cast<std::size_t>(k)] = std::fabs(2.0f * (t - std::floor(t)) - 1.0f);
            }
            colors[c][3] = 1.0f;
        }
        scalp->setCornerColors(m.name, colors);
        data.maps[m.name] = MapInfo{"vertices", "", m.color.empty() ? 1 : 4};
    }
    data.scalp = scalp;
}

}  // namespace groomlab::test
