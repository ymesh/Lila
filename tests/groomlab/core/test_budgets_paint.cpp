// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Painting budgets: a texture map's stamp and its viewport preview at 4096 x 4096, and a guide
// brush stamp over 5k guides, alone and through the interactive tool (BrushTool). Each test
// prints its numbers ("[ budget ] ..."); on CI (shared runners) they are reported but not
// enforced, as in test_budgets.cpp.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>

#include "groomlab/brush.h"
#include "groom/GroomEvaluator.h"
#include "groom/GroomStrokeHost.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/stroke.h"
#include "groomlab/texture.h"
#include "groomlab/view_projection.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

bool enforced() {
    if (const char* force = std::getenv("GROOMLAB_ENFORCE_BUDGETS")) return *force && std::string(force) != "0";
    return std::getenv("CI") == nullptr;
}
#define EXPECT_BUDGET(value, limit)                \
    do {                                           \
        const double measured = (value);           \
        if (enforced()) EXPECT_LT(measured, limit); \
    } while (0)

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A UV sphere (u around, v up) of radius 1.
std::shared_ptr<ScalpMesh> uvSphere(int rings, int segments) {
    const auto base = test::makeSphere(rings, segments, 1.0f);
    auto mesh = std::make_shared<ScalpMesh>(base->points(), base->triangles());
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            uvs.push_back({static_cast<float>(v % (segments + 1)) / static_cast<float>(segments),
                           1.0f - static_cast<float>(v / (segments + 1)) / static_cast<float>(rings)});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

TEST(Budget, TexturePaintStampAndPreviewAt4096) {
    // A 4096 x 4096 color map on a head-sized sphere: opening it (the texel-to-scalp table, then
    // again with the table shared), per dab the stamp (recording the stroke's tiles for undo) and
    // the viewport preview's update (the dab's texels only), and the stroke's commit (the PNG
    // bytes and what reading them back gives).
    const auto scalp = uvSphere(60, 120);
    auto t0 = std::chrono::steady_clock::now();
    auto opened = std::make_unique<TexturePaint>(*scalp, 4096, 4096, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    const double openMs = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    TexturePaint paint(*scalp, 4096, 4096, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    const double reopenMs = msSince(t0);
    EXPECT_EQ(paint.table().get(), opened->table().get());
    opened.reset();
    TexturePreview preview;
    const MapDisplay display{kColorMap, -1, true};
    preview.update(paint, display);
    double stampMs = 0.0, previewMs = 0.0, worst = 0.0;
    const int dabs = 20;
    paint.beginEdit();
    for (int i = 0; i < dabs; ++i) {
        const float a = 0.6f + 0.02f * static_cast<float>(i);
        const Vec3f center{0.6f * std::cos(a), 0.8f, 0.6f * std::sin(a)};
        t0 = std::chrono::steady_clock::now();
        const TexturePaint::Reach reach = paint.reachOf(center, 0.06f, sphereWeight(center, 0.06f));
        paint.stampReach(reach, 0.5f, {1.0f, 0.5f, 0.0f, 1.0f}, kAllChannels);
        const double s = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        const TexelRect updated = preview.update(paint, display);
        const double p = msSince(t0);
        EXPECT_FALSE(updated.empty());
        stampMs += s;
        previewMs += p;
        worst = std::max(worst, s + p);
    }
    t0 = std::chrono::steady_clock::now();
    const TextureEdit edit = paint.endEdit();
    const double editMs = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> pixels = paint.toBytes();
    const double bytesMs = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> png = storedPngBytes(4096, 4096, 4, pixels);
    const double pngMs = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    const TexturePtr decoded = textureFromPixels(4096, 4096, 4, pixels);
    const double textureMs = msSince(t0);
    ASSERT_EQ(preview.rgba().size(), std::size_t{4096} * 4096 * 4);
    ASSERT_GT(png.size(), pixels.size());
    std::printf("[ budget ] 4096x4096 color texture map: open %.0f ms (shared table %.0f ms); per dab stamp %.2f ms + "
                "preview %.2f ms (worst dab %.1f ms of %d); commit: tiles %.0f ms (%.2f MB undo), bytes %.0f ms, "
                "PNG %.0f ms, cache texture %.0f ms\n",
                openMs, reopenMs, stampMs / dabs, previewMs / dabs, worst, dabs, editMs, edit.bytes() / 1e6, bytesMs, pngMs,
                textureMs);
    EXPECT_BUDGET(worst, 50.0);
    EXPECT_LT(edit.bytes(), std::size_t{16} << 20);
}

TEST(Budget, TexturePaintDabsOnASeam) {
    // Paint and smooth dabs (reach, then one pass) on a gray and a color map at 2048 and 4096,
    // centered on the sphere's UV seam (u = 0 | 1), a small and a big brush, and the preview's
    // update after each: the mean of 10 dabs each.
    const auto scalp = uvSphere(60, 120);
    for (const int size : {2048, 4096}) {
        for (const int channels : {kGrayMap, kColorMap}) {
            TexturePaint paint = channels == kGrayMap ? TexturePaint(*scalp, size, size, 0.0f)
                                                      : TexturePaint(*scalp, size, size, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
            TexturePreview preview;
            const MapDisplay display{channels, -1, true};
            preview.update(paint, display);
            paint.beginEdit();
            for (const float radius : {0.06f, 0.2f}) {
                double paintMs = 0.0, smoothMs = 0.0, previewMs = 0.0;
                std::size_t texels = 0;
                const int dabs = 10;
                for (int i = 0; i < dabs; ++i) {
                    const float y = 0.3f + 0.02f * static_cast<float>(i);
                    const float r = std::sqrt(1.0f - y * y);
                    const Vec3f center{r, y, 0.0f};  // on the seam: u = 0 on one side, 1 on the other
                    auto t0 = std::chrono::steady_clock::now();
                    const TexturePaint::Reach reach = paint.reachOf(center, radius, sphereWeight(center, radius));
                    paint.stampReach(reach, 0.5f, {1.0f, 0.5f, 0.0f, 1.0f}, kAllChannels);
                    paintMs += msSince(t0);
                    texels += reach.size();
                    t0 = std::chrono::steady_clock::now();
                    const TexturePaint::Reach again = paint.reachOf(center, radius, sphereWeight(center, radius));
                    paint.smoothReach(again, 1.0f, kAllChannels);
                    smoothMs += msSince(t0);
                    t0 = std::chrono::steady_clock::now();
                    preview.update(paint, display);
                    previewMs += msSince(t0);
                }
                std::printf("[ budget ] %dx%d %s map, radius %.2f (%zu texels): paint dab %.2f ms, smooth dab %.2f ms, "
                            "preview %.2f ms\n",
                            size, size, channels == kGrayMap ? "gray" : "color", radius, texels / dabs, paintMs / dabs,
                            smoothMs / dabs, previewMs / dabs);
                EXPECT_BUDGET((paintMs + smoothMs + previewMs) / dabs, radius < 0.1f ? 30.0 : 120.0);
            }
            paint.endEdit();
        }
    }
}

// `count` primaries of 15 points on the unit sphere, rooted on it.
GuideHierarchy guidesOnSphere(const ScalpMesh& scalp, int count) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(11);
    for (int i = 0; i < count; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.2f, 0.0f), 14));
    }
    h.evaluate(&scalp);
    return h;
}

// A camera at `eye` looking at `target` (y up) as Maya's world-to-clip matrix (row vectors).
std::array<double, 16> lookAt(Vec3f eye, Vec3f target, double fovDegrees, double aspect) {
    const Vec3f f = (target - eye).normalized();
    const Vec3f r = f.cross(Vec3f(0, 1, 0)).normalized();
    const Vec3f u = r.cross(f);
    const double view[4][4] = {{r.x, r.y, r.z, -r.dot(eye)}, {u.x, u.y, u.z, -u.dot(eye)}, {-f.x, -f.y, -f.z, f.dot(eye)}, {0, 0, 0, 1}};
    const double t = 1.0 / std::tan(fovDegrees * 3.14159265358979 / 360.0), n = 0.1, fa = 100.0;
    const double proj[4][4] = {{t / aspect, 0, 0, 0}, {0, t, 0, 0}, {0, 0, -(fa + n) / (fa - n), -2 * fa * n / (fa - n)}, {0, 0, -1, 0}};
    std::array<double, 16> m{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            double v = 0.0;
            for (int k = 0; k < 4; ++k) v += proj[col][k] * view[k][row];
            m[static_cast<std::size_t>(row * 4 + col)] = v;
        }
    }
    return m;
}

TEST(Budget, GuideStampWithCullingAt5kGuides) {
    // A comb dab over 5000 guides (most out of the brush's reach): with the world sphere, and with
    // the interactive brush's screen-space weight (a 60 pixel circle, the scalp hiding the back),
    // each weighing every guide and skipping those out of reach with a stroke cache: the same
    // guides, the same values.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    const GuideHierarchy start = guidesOnSphere(*scalp, 5000);
    const Vec3f eye{0.0f, 1.8f, 4.5f};
    const ViewProjection view(lookAt(eye, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    const auto visibility = std::make_shared<ScalpVisibility>(scalp, eye, (Vec3f{0.0f, 0.8f, 0.0f} - eye).normalized(), false);
    BrushSettings b;
    b.mode = BrushMode::Comb;
    b.radius = 0.2f;
    b.strength = 1.0f;
    b.collide = false;
    const int dabs = 20;
    auto run = [&](bool screen, bool cull) {
        GuideHierarchy h = start;
        BrushStrokeCache cache;
        double total = 0.0;
        std::size_t changed = 0;
        for (int i = 0; i < dabs; ++i) {
            const float sx = 560.0f + 4.0f * static_cast<float>(i), sy = 520.0f;
            Vec3f nearPoint, farPoint;
            view.unproject(sx, sy, nearPoint, farPoint);
            SurfacePoint hit;
            const Vec3f center = scalp->intersectRay(nearPoint, (farPoint - nearPoint).normalized(), &hit) ? hit.position
                                                                                                         : Vec3f{0.0f, 1.0f, 0.3f};
            const Vec3f drag{0.02f, 0.0f, 0.0f};
            const auto t0 = std::chrono::steady_clock::now();
            if (screen) {
                const BrushWeightFn weight = screenBrushWeight(view, sx, sy, 60.0f, 0.0f, visibility);
                changed += cull ? applyBrushStamp(h, scalp.get(), b, center, drag, weight, screenBrushReach(view, sx, sy, 60.0f), &cache)
                                : applyBrushStamp(h, scalp.get(), b, center, drag, weight);
            } else {
                changed += cull ? applyBrushStamp(h, scalp.get(), b, center, drag, {}, {}, &cache)
                                : applyBrushStamp(h, scalp.get(), b, center, drag);
            }
            total += msSince(t0);
        }
        EXPECT_GT(changed, 0u);
        return std::make_pair(total / dabs, h.contentHash());
    };
    const auto [sphereMs, sphereHash] = run(false, false);
    const auto [sphereCulledMs, sphereCulledHash] = run(false, true);
    const auto [screenMs, screenHash] = run(true, false);
    const auto [screenCulledMs, screenCulledHash] = run(true, true);
    EXPECT_EQ(sphereHash, sphereCulledHash);
    EXPECT_EQ(screenHash, screenCulledHash);
    std::printf("[ budget ] comb dab over 5000 guides: world sphere %.2f ms (culled with the stroke's cache %.2f ms), "
                "screen weight %.2f ms (culled %.2f ms)\n",
                sphereMs, sphereCulledMs, screenMs, screenCulledMs);
    EXPECT_BUDGET(std::max(sphereCulledMs, screenCulledMs), 5.0);

    // Children: 64 primaries with 62 children each, combed at the children's level (each edited
    // child turns its new shape into offsets from its parent, then follows it again).
    GuideHierarchy family = guidesOnSphere(*scalp, 64);
    ChildGenerationParams params;
    params.count = 62;
    family.generateChildren(family.findGroup("primary"), "children", params);
    family.evaluate(scalp.get());
    b.level = 1;
    b.radius = 0.8f;
    b.collide = true;
    auto comb = [&](bool cull) {
        GuideHierarchy h = family;
        BrushStrokeCache cache;
        double total = 0.0;
        std::size_t changed = 0;
        for (int i = 0; i < dabs; ++i) {
            const Vec3f center{0.03f * static_cast<float>(i), 1.0f, 0.2f};
            const auto t0 = std::chrono::steady_clock::now();
            changed += cull ? applyBrushStamp(h, scalp.get(), b, center, {0.02f, 0.0f, 0.0f}, {}, {}, &cache)
                            : applyBrushStamp(h, scalp.get(), b, center, {0.02f, 0.0f, 0.0f});
            total += msSince(t0);
        }
        return std::make_tuple(total / dabs, changed / static_cast<std::size_t>(dabs), h.contentHash());
    };
    const auto [familyMs, familyChanged, familyHash] = comb(false);
    const auto [familyCulledMs, familyCulledChanged, familyCulledHash] = comb(true);
    EXPECT_EQ(familyHash, familyCulledHash);
    std::printf("[ budget ] comb dab over %zu children (of 64 guides): %.2f ms, culled %.2f ms (%zu children changed a dab)\n",
                family.guides().size() - 64, familyMs, familyCulledMs, familyCulledChanged);
}

TEST(Budget, BrushToolDabOnASession) {
    // The interactive tool's dab (stroke.h: BrushTool on a GroomEvaluator's host, a 60 pixel circle
    // from a view matrix, the scalp masking the back, the preview handed to the session) over
    // 5000 guides, against the stamp it makes (applyBrushStamp with the same weight, reach and
    // stroke cache): the tool adds next to nothing per dab.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.hierarchyJson = guidesOnSphere(*scalp, 5000).toJson();
    GroomEvaluator session(std::move(data));
    lila::groom::GroomStrokeHost host(session);
    const ToolView view = ToolView::fromMatrix(lookAt({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 60.0f;
    tool.settings.brush.collide = false;
    const int dabs = 20;
    ASSERT_TRUE(tool.begin(host, view, 560.0f, 520.0f, 1.0f));
    double total = 0.0;
    for (int i = 1; i <= dabs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        tool.moveTo(host, view, 560.0f + 4.0f * static_cast<float>(i), 520.0f, 1.0f);
        total += msSince(t0);
    }
    const double toolMs = total / dabs;
    const auto t0 = std::chrono::steady_clock::now();
    tool.end(host, view);
    const double commitMs = msSince(t0);
    EXPECT_TRUE(tool.changed());
    ASSERT_EQ(host.edits().size(), 1u);
    std::printf("[ budget ] BrushTool comb dab over 5000 guides on a session: %.2f ms a dab, the commit %.1f ms\n", toolMs,
                commitMs);
    EXPECT_BUDGET(toolMs, 5.0);
}

}  // namespace
}  // namespace groomlab
