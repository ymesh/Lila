#include "TestUtil.h"

#include "geo/Heightfield.h"
#include "geo/Primitives.h"
#include "render/HeightfieldRenderer.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "ui/Camera.h"
#include "ui/HeightfieldDisplay.h"
#include "ui/MeshCache.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace lila;
using namespace terraintest;

namespace
{
/** Rolling hills of nx by nz samples, 100 cm cells, centred on the origin. */
std::shared_ptr<geo::Heightfield> drawHills(int n, double amplitude = 3000.0)
{
    auto h = std::make_shared<geo::Heightfield>(geo::makeHeightfield(n, n, 100.0, glm::dvec2(-50.0 * (n - 1)), 0.0f));
    for (int z = 0; z < h->nz; ++z)
        for (int x = 0; x < h->nx; ++x)
        {
            const glm::dvec2 p = h->samplePos(x, z);
            h->heights[h->index(x, z)] = float(amplitude * (std::sin(p.x / 9000.0) * std::cos(p.y / 7000.0) + 0.3 * std::sin(p.x / 1700.0 + p.y / 2300.0)));
        }
    return h;
}

render::HeightfieldDraw drawOf(const std::shared_ptr<geo::Heightfield>& h)
{
    render::HeightfieldDraw d;
    d.heights = std::shared_ptr<const std::vector<float>>(h, &h->heights);
    d.nx = h->nx;
    d.nz = h->nz;
    d.cell = float(h->cell);
    d.origin = glm::vec2(h->origin);
    return d;
}

render::HeightfieldView perspective(const glm::dvec3& eye, const glm::dvec3& at, int heightPx = 1080)
{
    const glm::dmat4 view = glm::lookAt(eye, at, glm::dvec3(0.0, 1.0, 0.0));
    // Reversed-Z with an infinite far plane, as the viewport draws.
    const double f = 1.0 / std::tan(glm::radians(25.0)), aspect = 16.0 / 9.0, nearZ = 10.0;
    glm::dmat4 proj(0.0);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][3] = -1.0;
    proj[3][2] = nearZ;
    render::HeightfieldView v;
    v.viewProj = proj * view;
    v.eye = eye;
    v.focalPx = f * heightPx * 0.5;
    return v;
}

/** Each grid cell's patch level (-1 where no patch covers it) and how many patches cover it. */
struct Coverage
{
    std::vector<int> lod, count;
    int cols = 0;
};

Coverage coverage(const render::HeightfieldDraw& d, const render::HeightfieldSelection& sel)
{
    Coverage c;
    c.cols = d.nx - 1;
    c.lod.assign(size_t(d.nx - 1) * size_t(d.nz - 1), -1);
    c.count.assign(c.lod.size(), 0);
    for (const render::HeightfieldPatch& p : sel.patches)
    {
        const int span = int(p.spacing) * render::kHeightfieldPatch;
        for (int z = int(p.origin.y); z < std::min(int(p.origin.y) + span, d.nz - 1); ++z)
            for (int x = int(p.origin.x); x < std::min(int(p.origin.x) + span, d.nx - 1); ++x)
            {
                const size_t k = size_t(z) * size_t(c.cols) + size_t(x);
                ++c.count[k];
                c.lod[k] = int(p.lod);
            }
    }
    return c;
}

/** The largest level difference between neighbouring covered cells. */
int worstStep(const Coverage& c)
{
    int worst = 0;
    const int rows = int(c.lod.size()) / c.cols;
    for (int z = 0; z < rows; ++z)
        for (int x = 0; x < c.cols; ++x)
        {
            const int here = c.lod[size_t(z) * size_t(c.cols) + size_t(x)];
            if (here < 0)
                continue;
            if (x + 1 < c.cols)
                if (int r = c.lod[size_t(z) * size_t(c.cols) + size_t(x + 1)]; r >= 0)
                    worst = std::max(worst, std::abs(r - here));
            if (z + 1 < rows)
                if (int r = c.lod[size_t(z + 1) * size_t(c.cols) + size_t(x)]; r >= 0)
                    worst = std::max(worst, std::abs(r - here));
        }
    return worst;
}
} // namespace

TEST_CASE("heightfield draw: the height pyramid holds each block's lowest and highest sample")
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-500.0f, 900.0f);
    const int nx = 301, nz = 190;
    std::vector<float> h(size_t(nx) * nz);
    for (float& v : h)
        v = u(rng);
    const render::HeightPyramid p = render::buildHeightPyramid(h, nx, nz);
    REQUIRE(p.levels() >= 2);
    CHECK(p.dims.back() == glm::ivec2(1, 1));
    const int base = 2 * render::kHeightfieldPatch;
    for (int level = 0; level < p.levels(); ++level)
        for (int bz = 0; bz < p.dims[level].y; ++bz)
            for (int bx = 0; bx < p.dims[level].x; ++bx)
            {
                const int size = base << level;
                float lo = FLT_MAX, hi = -FLT_MAX;
                for (int z = bz * size; z <= std::min(bz * size + size, nz - 1); ++z)
                    for (int x = bx * size; x <= std::min(bx * size + size, nx - 1); ++x)
                    {
                        lo = std::min(lo, h[size_t(z) * nx + x]);
                        hi = std::max(hi, h[size_t(z) * nx + x]);
                    }
                const glm::vec2 got = p.ranges[level][size_t(bz) * p.dims[level].x + bx];
                CHECK(got.x == lo);
                CHECK(got.y == hi);
            }
}

TEST_CASE("heightfield draw: patches cover a fully seen grid once, finer near the camera, never two levels apart")
{
    auto h = drawHills(2049);
    const render::HeightfieldDraw d = drawOf(h);
    const render::HeightPyramid py = render::buildHeightPyramid(h->heights, h->nx, h->nz);

    // From high above the middle: everything in view.
    const render::HeightfieldSelection top = render::selectHeightfieldPatches(d, py, perspective({0.0, 900000.0, 1.0}, {0.0, 0.0, 0.0}));
    const Coverage all = coverage(d, top);
    CHECK(std::ranges::all_of(all.count, [](int n) { return n == 1; }));
    CHECK(worstStep(all) <= 1);

    // Low over a corner looking across: the cells under the camera are the finest, the far corner coarser.
    const glm::dvec3 eye(-90000.0, 6000.0, -90000.0);
    const render::HeightfieldSelection low = render::selectHeightfieldPatches(d, py, perspective(eye, {0.0, 0.0, 0.0}));
    const Coverage c = coverage(d, low);
    CHECK(std::ranges::all_of(c.count, [](int n) { return n <= 1; }));
    CHECK(worstStep(c) <= 1);
    const int nearCell = c.lod[size_t(150) * size_t(c.cols) + 150];
    const int farCell = c.lod[size_t(1900) * size_t(c.cols) + 1900];
    CHECK(nearCell == 0);
    CHECK(farCell > nearCell + 2);
    // Behind the camera is culled.
    const size_t covered = size_t(std::ranges::count_if(c.count, [](int n) { return n > 0; }));
    CHECK(covered < c.count.size());
    // Morph ranges grow with the level, each starting before it ends.
    for (int k = 0; k + 1 < low.levels; ++k)
    {
        CHECK(low.morph[k].x > 0.0f);
        CHECK(low.morph[k].y > 0.0f);
        if (k > 0)
            CHECK(low.morph[k].x > low.morph[k - 1].x);
    }
    MESSAGE("2049 squared: " << top.patches.size() << " patches from above, " << low.patches.size() << " low over a corner");
}

TEST_CASE("heightfield draw: an orthographic view draws the whole grid at one level")
{
    auto h = drawHills(1025);
    const render::HeightfieldDraw d = drawOf(h);
    const render::HeightPyramid py = render::buildHeightPyramid(h->heights, h->nx, h->nz);
    render::HeightfieldView v;
    const glm::dmat4 view = glm::lookAt(glm::dvec3(0.0, 100000.0, 0.0), glm::dvec3(0.0), glm::dvec3(0.0, 0.0, -1.0));
    v.viewProj = glm::orthoRH_ZO(-60000.0, 60000.0, -60000.0, 60000.0, 10.0, 1.0e6) * view;
    v.ortho = true;
    v.pixelWorld = 120000.0 / 1000.0;
    const render::HeightfieldSelection sel = render::selectHeightfieldPatches(d, py, v);
    REQUIRE_FALSE(sel.patches.empty());
    const uint32_t lod = sel.patches.front().lod;
    CHECK(lod > 0);
    CHECK(std::ranges::all_of(sel.patches, [&](const render::HeightfieldPatch& p) { return p.lod == lod; }));
    CHECK(std::ranges::all_of(coverage(d, sel).count, [](int n) { return n == 1; }));
}

TEST_CASE("heightfield draw: an 8K and a 16K grid draw a bounded number of triangles, picked in well under a millisecond")
{
    for (int n : {8193, 16385})
    {
        // The selection reads only the pyramid, so a flat grid's pyramid with hills' ranges stands in for the heights.
        render::HeightfieldDraw d;
        d.nx = d.nz = n;
        d.cell = 100.0f;
        d.origin = glm::vec2(-50.0f * float(n - 1));
        render::HeightPyramid py;
        glm::ivec2 dim((n - 2) / (2 * render::kHeightfieldPatch) + 1);
        while (true)
        {
            py.dims.push_back(dim);
            py.ranges.emplace_back(size_t(dim.x) * dim.y, glm::vec2(-4000.0f, 4000.0f));
            if (dim == glm::ivec2(1))
                break;
            dim = (dim + 1) / 2;
        }
        const glm::dvec3 eye(-0.4 * 50.0 * n, 9000.0, -0.4 * 50.0 * n);
        const auto start = std::chrono::steady_clock::now();
        const render::HeightfieldSelection sel = render::selectHeightfieldPatches(d, py, perspective(eye, {0.0, 0.0, 0.0}));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const size_t triangles = sel.patches.size() * size_t(render::kHeightfieldPatch) * render::kHeightfieldPatch * 2;
        MESSAGE(n << " squared: " << sel.patches.size() << " patches, " << triangles / 1000 << "K triangles (the grid has "
                  << (size_t(n - 1) * (n - 1) * 2) / 1000000 << "M), picked in " << ms << " ms");
        CHECK(sel.patches.size() > 50);
        CHECK(triangles < 8'000'000);
        CHECK(ms < 5.0);
        CHECK(sel.patches.front().lod >= 0);
        CHECK(std::ranges::any_of(sel.patches, [](const render::HeightfieldPatch& p) { return p.lod == 0; }));
    }
}

namespace
{
struct Shot
{
    std::vector<uint8_t> bgra;
    int w = 320, h = 200;
};

render::FrameState shotState(const ui::Camera& cam, double aspect)
{
    render::FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.taa = false;
    s.background = glm::vec3(0.0f);
    s.view = cam.view();
    s.proj = cam.proj(aspect);
    s.eye = cam.eye();
    return s;
}

/** Mean channel difference over the pixels either image covers, and the share of them off by more than 8 of 255. */
std::pair<double, double> difference(const Shot& a, const Shot& b)
{
    double sum = 0.0;
    size_t covered = 0, off = 0;
    for (size_t i = 0; i + 3 < a.bgra.size(); i += 4)
    {
        const bool lit = a.bgra[i] + a.bgra[i + 1] + a.bgra[i + 2] > 0 || b.bgra[i] + b.bgra[i + 1] + b.bgra[i + 2] > 0;
        if (!lit)
            continue;
        ++covered;
        int worst = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int d = std::abs(int(a.bgra[i + c]) - int(b.bgra[i + c]));
            sum += d;
            worst = std::max(worst, d);
        }
        off += worst > 8;
    }
    return {covered ? sum / (3.0 * double(covered)) : 0.0, covered ? double(off) / double(covered) : 0.0};
}
} // namespace

TEST_CASE("heightfield draw: shades like the terrain's mesh, and draws its wire, selection and overlays")
{
    auto& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for the heightfield draw test");
        return;
    }
    auto h = drawHills(257, 1500.0);
    h->mask("flow");
    for (size_t k = 0; k < h->size(); ++k)
        h->mask("flow")[k] = float(k % 257) / 256.0f;
    ui::Camera cam;
    cam.frame(glm::dvec3(0.0, 0.0, 0.0), 30000.0, 1.6);
    std::string err;
    auto shoot = [&](render::FrameState s) {
        Shot out;
        REQUIRE_MESSAGE(r.renderOffscreen(std::move(s), out.w, out.h, out.bgra, &err), err);
        return out;
    };

    // The same terrain as its mesh, through the mesh path.
    geo::HeightfieldMeshOptions mo;
    mo.masks = false;
    scene::MeshPtr mesh = std::make_shared<geo::Mesh>(geo::heightfieldToMesh(*h, mo));
    render::FrameState asMesh = shotState(cam, 1.6);
    render::MeshDraw md;
    md.mesh = ui::renderMeshFor(mesh);
    REQUIRE(md.mesh);
    asMesh.meshes.push_back(md);
    const Shot meshShot = shoot(asMesh);

    render::FrameState asHeightfield = shotState(cam, 1.6);
    asHeightfield.heightfields.push_back(drawOf(h));
    const Shot hfShot = shoot(asHeightfield);
    const auto [mean, share] = difference(meshShot, hfShot);
    MESSAGE("mesh vs heightfield draw: mean difference " << mean << " of 255, " << share * 100.0 << "% of pixels off by more than 8");
    CHECK(mean < 2.0);
    CHECK(share < 0.02);

    auto changed = [&](const Shot& a) { return difference(hfShot, a).second; };
    render::FrameState selected = asHeightfield;
    selected.heightfields[0].flags |= render::DrawSelected;
    selected.wireSelected = glm::vec4(0.1f, 0.9f, 0.2f, 1.0f);
    CHECK(changed(shoot(selected)) > 0.1);

    render::FrameState wire = asHeightfield;
    wire.heightfields[0].flags = render::DrawWire;
    wire.wireDefault = glm::vec4(1.0f);
    const Shot wireShot = shoot(wire);
    size_t wirePixels = 0, total = 0;
    for (size_t i = 0; i + 3 < wireShot.bgra.size(); i += 4, ++total)
        wirePixels += wireShot.bgra[i + 1] > 40;
    MESSAGE("wireframe covers " << 100.0 * double(wirePixels) / double(total) << "% of the frame");
    CHECK(wirePixels > total / 50);
    CHECK(wirePixels < total * 9 / 10);

    render::FrameState overlay = asHeightfield;
    overlay.heightfields[0].overlay = std::shared_ptr<const std::vector<float>>(h, h->findMask("flow"));
    CHECK(changed(shoot(overlay)) > 0.3);
    render::FrameState contours = asHeightfield;
    contours.heightfields[0].contourInterval = 200.0f;
    CHECK(changed(shoot(contours)) > 0.02);
    render::FrameState arrows = asHeightfield;
    arrows.heightfields[0].flowArrows = true;
    arrows.heightfields[0].arrowSpacing = 24.0f;
    const double arrowShare = changed(shoot(arrows));
    MESSAGE("flow arrows cover " << arrowShare * 100.0 << "% of the terrain");
    CHECK(arrowShare > 0.01);

    render::FrameState layered = asHeightfield;
    render::HeightfieldLayer layer;
    layer.weights = std::shared_ptr<const std::vector<float>>(h, h->findMask("flow"));
    layer.color = glm::vec3(0.05f, 0.3f, 0.05f);
    layered.heightfields[0].layers.push_back(layer);
    const Shot layeredShot = shoot(layered);
    // Greener where the weights are high, the material's grey where they are low.
    double greenGain = 0.0;
    for (size_t i = 0; i + 3 < layeredShot.bgra.size(); i += 4)
        greenGain += double(layeredShot.bgra[i + 1]) - double(layeredShot.bgra[i + 2]) - (double(hfShot.bgra[i + 1]) - double(hfShot.bgra[i + 2]));
    CHECK(greenGain > 0.0);
    CHECK(changed(layeredShot) > 0.2);
}

TEST_CASE("heightfield draw: casts into the shadow map like the terrain's mesh")
{
    auto& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for the heightfield shadow test");
        return;
    }
    auto h = drawHills(257, 3000.0);
    ui::Camera cam;
    cam.distance = 30000.0;
    cam.pitch = -55.0;
    cam.nearClip = 100.0;
    std::string err;
    auto shoot = [&](render::FrameState s) {
        Shot out;
        REQUIRE_MESSAGE(r.renderOffscreen(std::move(s), out.w, out.h, out.bgra, &err), err);
        return out;
    };

    // A low sun along +X, mapped over the whole terrain.
    render::LightDraw sun;
    sun.kind = render::LightDraw::Directional;
    sun.direction = glm::normalize(glm::vec3(1.0f, -0.15f, 0.2f));
    sun.radiance = glm::vec3(12.0f);
    sun.shadows = true;
    const double radius = 50.0 * 256.0 * 1.5;
    const glm::dvec3 dir(sun.direction);
    const glm::dmat4 view = glm::lookAt(-dir * radius * 2.0, glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0));
    glm::dmat4 reverse(1.0);
    reverse[2][2] = -1.0;
    reverse[3][2] = 1.0;
    sun.shadowViewProj = glm::mat4(reverse * glm::orthoRH_ZO(-radius, radius, -radius, radius, radius * 0.5, radius * 3.5) * view);
    sun.shadowTexel = float(2.0 * radius / 2048.0);
    sun.shadowDepth = glm::vec4(float(radius * 0.5), float(radius * 3.5), 0.005f, float(0.5 / radius));

    geo::HeightfieldMeshOptions mo;
    mo.masks = false;
    scene::MeshPtr mesh = std::make_shared<geo::Mesh>(geo::heightfieldToMesh(*h, mo));
    render::MeshDraw md;
    md.mesh = ui::renderMeshFor(mesh);
    REQUIRE(md.mesh);
    auto lit = [&](bool shadows) {
        render::FrameState s = shotState(cam, 1.6);
        s.lighting = render::FrameState::Lighting::Studio;
        s.lights.push_back(sun);
        s.lights.back().shadows = shadows;
        return s;
    };
    render::FrameState meshLit = lit(true);
    meshLit.meshes.push_back(md);
    const Shot meshShot = shoot(meshLit);
    render::FrameState meshUnshadowed = lit(false);
    meshUnshadowed.meshes.push_back(md);
    const Shot meshPlain = shoot(meshUnshadowed);
    render::FrameState hfLit = lit(true);
    hfLit.heightfields.push_back(drawOf(h));
    const Shot hfShot = shoot(hfLit);

    const double shadowed = difference(meshShot, meshPlain).second;
    const auto [mean, share] = difference(meshShot, hfShot);
    MESSAGE("shadows change " << shadowed * 100.0 << "% of the mesh; heightfield vs mesh with shadows: mean " << mean << ", " << share * 100.0
                              << "% off by more than 8");
    REQUIRE(shadowed > 0.05);
    CHECK(share < shadowed * 0.25);
    CHECK(mean < 3.0);
}

TEST_CASE("heightfield draw: stylised shading models shade it like the terrain's mesh")
{
    auto& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for the heightfield shading model test");
        return;
    }
    auto h = drawHills(257, 3000.0);
    ui::Camera cam;
    cam.distance = 30000.0;
    cam.pitch = -55.0;
    cam.nearClip = 100.0;
    std::string err;
    auto shoot = [&](render::FrameState s) {
        Shot out;
        REQUIRE_MESSAGE(r.renderOffscreen(std::move(s), out.w, out.h, out.bgra, &err), err);
        return out;
    };
    geo::HeightfieldMeshOptions mo;
    mo.masks = false;
    scene::MeshPtr mesh = std::make_shared<geo::Mesh>(geo::heightfieldToMesh(*h, mo));
    render::MeshDraw md;
    md.mesh = ui::renderMeshFor(mesh);
    REQUIRE(md.mesh);

    render::Material plain;
    plain.baseColor = glm::vec3(0.6f, 0.55f, 0.5f);
    render::FrameState defaultLit = shotState(cam, 1.6);
    defaultLit.heightfields.push_back(drawOf(h));
    defaultLit.heightfields[0].materials = std::vector<render::Material>{plain};
    const Shot hfLit = shoot(defaultLit);

    render::Material toon = plain;
    toon.style.model = render::ShadingModel::Toon;
    toon.style.toonSoftness = 0.01f;
    render::Material foliage = plain;
    foliage.style.model = render::ShadingModel::Foliage;
    foliage.style.foliageGradient = 1.0f;
    foliage.style.foliageBottomColor = glm::vec3(0.02f, 0.05f, 0.02f);
    foliage.style.foliageTopColor = glm::vec3(0.2f, 0.8f, 0.1f);
    for (const render::Material& m : {toon, foliage})
    {
        render::FrameState asMesh = shotState(cam, 1.6);
        render::MeshDraw styled = md;
        styled.materials = std::vector<render::Material>{m};
        asMesh.meshes.push_back(styled);
        render::FrameState asHeightfield = shotState(cam, 1.6);
        asHeightfield.heightfields.push_back(drawOf(h));
        asHeightfield.heightfields[0].materials = std::vector<render::Material>{m};
        const Shot hf = shoot(asHeightfield);
        const auto [mean, share] = difference(shoot(asMesh), hf);
        const double changed = difference(hfLit, hf).second;
        MESSAGE("model " << int(m.style.model) << ": changes " << changed * 100.0 << "% of the default lit heightfield; vs its mesh: mean " << mean
                         << ", " << share * 100.0 << "% off by more than 8");
        CHECK(changed > 0.2);
        CHECK(share < 0.03);
        CHECK(mean < 2.0);
    }
}

TEST_CASE("heightfield draw: an edit's dirty tiles, their copies and the pyramid's update")
{
    constexpr int T = render::kHeightfieldTile;
    const int nx = 5 * T + 1, nz = 4 * T + 1;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-500.0f, 900.0f);
    std::vector<float> a(size_t(nx) * nz);
    for (float& v : a)
        v = u(rng);
    std::vector<float> b = a;
    CHECK(render::heightfieldDirtyRects(a.data(), b.data(), nx, nz).empty());

    // A brush over tiles (1..2, 1..2), one sample in the last tile's corner, and a NaN that stays a NaN.
    for (int z = T + 10; z < 3 * T - 5; ++z)
        for (int x = T + 3; x < 2 * T + 7; ++x)
            b[size_t(z) * nx + x] += 50.0f;
    b[size_t(nz - 1) * nx + (nx - 1)] = 2000.0f;
    a[0] = b[0] = std::numeric_limits<float>::quiet_NaN();
    const std::vector<render::HeightfieldRect> rects = render::heightfieldDirtyRects(a.data(), b.data(), nx, nz);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0] == render::HeightfieldRect{T, T, 3 * T, 3 * T});
    CHECK(rects[1] == render::HeightfieldRect{5 * T, 4 * T, nx, nz});

    // Copying the packed rows into a reproduces b exactly, row runs joined where both sides run on.
    const std::vector<render::HeightfieldCopy> copies = render::heightfieldRectCopies(rects, nx);
    CHECK(copies.size() == size_t(2 * T + 1));
    std::vector<float> packed;
    for (const render::HeightfieldRect& r : rects)
        for (int z = r.z0; z < r.z1; ++z)
            packed.insert(packed.end(), b.begin() + ptrdiff_t(size_t(z) * nx + r.x0), b.begin() + ptrdiff_t(size_t(z) * nx + r.x1));
    REQUIRE(copies.back().src + copies.back().count == packed.size());
    std::vector<float> patched = a;
    for (const render::HeightfieldCopy& c : copies)
        std::copy_n(packed.begin() + ptrdiff_t(c.src), c.count, patched.begin() + ptrdiff_t(c.dst));
    CHECK(std::memcmp(patched.data(), b.data(), b.size() * sizeof(float)) == 0);
    const std::vector<render::HeightfieldCopy> wide = render::heightfieldRectCopies({{0, 3, nx, 9}}, nx);
    REQUIRE(wide.size() == 1);
    CHECK(wide[0] == render::HeightfieldCopy{0, size_t(3) * nx, size_t(6) * nx});

    // The pyramid patched over the rects matches one built from scratch, including a change on a block's shared edge.
    b[size_t(T + 20) * nx + 3 * T] = -3000.0f;
    render::HeightPyramid p = render::buildHeightPyramid(a, nx, nz);
    render::updateHeightPyramid(p, b, nx, nz, render::heightfieldDirtyRects(a.data(), b.data(), nx, nz));
    const render::HeightPyramid fresh = render::buildHeightPyramid(b, nx, nz);
    REQUIRE(p.levels() == fresh.levels());
    for (int level = 0; level < p.levels(); ++level)
        CHECK(p.ranges[level] == fresh.ranges[level]);
}

TEST_CASE("heightfield draw: an edited terrain draws the same patched as uploaded whole")
{
    auto& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for the heightfield patch test");
        return;
    }
    auto before = drawHills(513, 3000.0);
    ui::Camera cam;
    cam.distance = 60000.0;
    cam.pitch = -55.0;
    cam.nearClip = 100.0;
    std::string err;
    auto shoot = [&](const std::shared_ptr<geo::Heightfield>& h, uint64_t node) {
        render::FrameState s = shotState(cam, 1.6);
        s.heightfields.push_back(drawOf(h));
        s.heightfields[0].node = node;
        Shot out;
        REQUIRE_MESSAGE(r.renderOffscreen(std::move(s), out.w, out.h, out.bgra, &err), err);
        return out;
    };
    const Shot original = shoot(before, 77);
    // A crater dug into one corner: the next draw of node 77 patches only those tiles.
    auto after = std::make_shared<geo::Heightfield>(*before);
    for (int z = 300; z < 420; ++z)
        for (int x = 40; x < 200; ++x)
            after->heights[after->index(x, z)] -= 4000.0f;
    const Shot patched = shoot(after, 77);
    auto copy = std::make_shared<geo::Heightfield>(*after);
    const Shot whole = shoot(copy, 0);
    CHECK(difference(original, patched).second > 0.01);
    const auto [mean, share] = difference(patched, whole);
    MESSAGE("patched vs uploaded whole: mean " << mean << ", " << share * 100.0 << "% off by more than 8");
    CHECK(patched.bgra == whole.bgra);
}

TEST_CASE("heightfield draw: occludes the same with the AO prepass recorded in chunks")
{
    auto& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for the heightfield chunked prepass test");
        return;
    }
    // Ridges and pits a few AO radii across.
    auto h = std::make_shared<geo::Heightfield>(geo::makeHeightfield(257, 257, 10.0, glm::dvec2(-1280.0)));
    for (int z = 0; z < h->nz; ++z)
        for (int x = 0; x < h->nx; ++x)
            h->heights[h->index(x, z)] = float(60.0 * std::sin(x * 0.35) * std::cos(z * 0.3));
    ui::Camera cam;
    cam.distance = 2500.0;
    cam.pitch = -55.0;
    cam.nearClip = 10.0;
    render::FrameState s = shotState(cam, 1.6);
    s.ambientOcclusion = true;
    s.aoFast = false;
    s.aoRadius = 60.0f;
    s.heightfields.push_back(drawOf(h));
    // Enough separate meshes, out of view, that the prepass records its sections on worker threads.
    for (int i = 0; i < 2200; ++i)
    {
        geo::CubeOptions o;
        o.width = o.height = o.depth = 1.0 + 0.001 * i;
        render::MeshDraw md;
        md.mesh = ui::renderMeshFor(std::make_shared<geo::Mesh>(geo::makeCube(o)));
        md.world = glm::translate(glm::mat4(1.0f), glm::vec3(float(i), -1.0e6f, 0.0f));
        s.meshes.push_back(md);
    }
    std::string err;
    auto shoot = [&](bool chunks) {
#ifdef _WIN32
        _putenv_s("LILA_VK_NO_CHUNKS", chunks ? "" : "1");
#else
        chunks ? unsetenv("LILA_VK_NO_CHUNKS") : setenv("LILA_VK_NO_CHUNKS", "1", 1);
#endif
        Shot out;
        REQUIRE_MESSAGE(r.renderOffscreen(s, out.w, out.h, out.bgra, &err), err);
        return out;
    };
    const Shot inline_ = shoot(false);
    const Shot chunked = shoot(true);
    render::FrameState noAo = s;
    noAo.ambientOcclusion = false;
    Shot plain;
    REQUIRE_MESSAGE(r.renderOffscreen(noAo, plain.w, plain.h, plain.bgra, &err), err);
    const double occluded = difference(inline_, plain).second;
    const auto [mean, share] = difference(inline_, chunked);
    MESSAGE("AO changes " << occluded * 100.0 << "% of the terrain; chunked vs inline prepass: mean " << mean << ", " << share * 100.0 << "% off by more than 8");
    CHECK(occluded > 0.1);
    CHECK(share < 0.005);
    CHECK(mean < 0.5);
}

TEST_CASE("terrain display commands: options and paint layers round trip, layers undo")
{
    loadTerrain();
    if (!ops::find("terrainDisplay"))
        ui::heightfieldDisplay::registerCommands();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"erosion", 20}, {"name", "Ground"}});

    json s = run("terrainDisplay", {{"overlay", "flow"}, {"contours", 500.0}, {"flowArrows", true}, {"quadPixels", 6.0}});
    CHECK(s["draw"] == "heightfield");
    CHECK(s["overlay"] == "flow");
    CHECK(s["contours"].get<double>() == doctest::Approx(500.0));
    CHECK(s["quadPixels"].get<double>() == doctest::Approx(6.0));
    REQUIRE(s["terrains"].size() == 1);
    CHECK(s["terrains"][0]["node"] == "Ground");
    CHECK_FALSE(s.contains("warning"));
    s = run("terrainDisplay", {{"overlay", "nosuchmask"}});
    CHECK(s.contains("warning"));
    s = run("terrainDisplay", {{"overlay", ""}, {"contours", 0.0}, {"flowArrows", false}});
    // The heightfield draw is the default, so it stays with every overlay off.
    CHECK(s["draw"] == "heightfield");
    s = run("terrainDisplay", {{"draw", "mesh"}});
    CHECK(s["draw"] == "mesh");
    run("terrainDisplay", {{"draw", "heightfield"}});

    json l = run("terrainDisplayLayers", {{"args", {t["mesh"]}},
                                          {"layers", {{{"name", "rock"}, {"mask", "wear"}, {"color", "#807060"}},
                                                      {{"name", "silt"}, {"mask", "deposit"}, {"color", {0.8, 0.7, 0.5}}, {"tiling", 250.0}}}}});
    REQUIRE(l["layers"].size() == 2);
    CHECK(l["layers"][0]["found"] == true);
    CHECK(l["layers"][1]["coverage"].get<double>() > 0.0);

    scene::Graph& g = scene::Graph::current();
    const scene::NodeId id = g.find(t["mesh"].get<std::string>())->id();
    ui::heightfieldDisplay::Settings set;
    std::optional<render::HeightfieldDraw> d = ui::heightfieldDisplay::drawFor(g, id, set, true);
    REQUIRE(d);
    CHECK(d->layers.size() == 2);
    CHECK(d->layers[1].tiling == doctest::Approx(250.0f));

    run("undo");
    CHECK(run("terrainDisplayLayers", {{"args", {t["mesh"]}}})["layers"].empty());
    CHECK_THROWS(run("terrainDisplayLayers", {{"args", {t["mesh"]}}, {"layers", {{{"name", "bad"}, {"color", "green"}}}}}));
}
