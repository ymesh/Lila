#include <doctest/doctest.h>

#include "TestUtil.h"
#include "render/Renderer.h"
#include "ui/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::render;
using namespace stylisedtest;

namespace
{
constexpr int kDrawSize = 128;

/** A closed box, half size h, with face normals and wire edges. */
std::shared_ptr<RenderMesh> box(float h)
{
    auto m = std::make_shared<RenderMesh>();
    for (int axis = 0; axis < 3; ++axis)
        for (int sign = -1; sign <= 1; sign += 2)
        {
            glm::vec3 n(0.0f);
            n[axis] = float(sign);
            const glm::vec3 u = axis == 0 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 v = glm::cross(n, u);
            const uint32_t base = uint32_t(m->vertices.size());
            for (int c = 0; c < 4; ++c)
            {
                MeshVertex vert{};
                vert.pos = (n + u * ((c & 1) ? 1.0f : -1.0f) + v * ((c & 2) ? 1.0f : -1.0f)) * h;
                vert.normal = n;
                vert.tangent = glm::vec4(u, 1.0f);
                m->vertices.push_back(vert);
            }
            m->indices.insert(m->indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
            m->edgePoints.insert(m->edgePoints.end(), {m->vertices[base].pos, m->vertices[base + 1].pos, m->vertices[base + 1].pos,
                                                       m->vertices[base + 3].pos});
        }
    m->boundsMin = glm::vec3(-h);
    m->boundsMax = glm::vec3(h);
    return m;
}

FrameState frameAt(double frameRadius)
{
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.lighting = FrameState::Lighting::Studio;
    s.background = glm::vec3(0.0f);
    ui::Camera cam;
    cam.frame(glm::dvec3(0.0), frameRadius, 1.0);
    s.view = cam.view();
    s.proj = cam.proj(1.0);
    s.eye = cam.eye();
    return s;
}

std::vector<uint8_t> renderInstanced(const FrameState& s, Renderer::FrameStats* stats = nullptr, int repeat = 1)
{
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, kDrawSize, kDrawSize, bgra, &err, stats, repeat), err);
    return bgra;
}

/** Mean x of the lit pixels, in pixels, or -1 when nothing is lit. */
double litCentroidX(const std::vector<uint8_t>& bgra)
{
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < kDrawSize; ++y)
        for (int x = 0; x < kDrawSize; ++x)
        {
            const uint8_t* p = &bgra[(size_t(y) * kDrawSize + x) * 4];
            if (p[0] + p[1] + p[2] > 6)
            {
                sum += x;
                ++n;
            }
        }
    return n ? sum / n : -1.0;
}

/** A grid of side x side boxes, every draw sharing one mesh or each with its own copy of it. */
FrameState gridOfBoxes(int side, bool shareMesh)
{
    FrameState s = frameAt(side * 10.0);
    auto mesh = box(3.0f);
    for (int i = 0; i < side * side; ++i)
    {
        MeshDraw d;
        d.mesh = mesh;
        if (!shareMesh)
        {
            auto copy = std::make_shared<RenderMesh>(*mesh);
            copy->key = RenderMesh::nextKey();
            d.mesh = copy;
        }
        d.world = glm::translate(glm::mat4(1.0f), glm::vec3((i % side - side / 2) * 10.0f, 0.0f, (i / side - side / 2) * 10.0f));
        s.meshes.push_back(d);
    }
    return s;
}
} // namespace

TEST_CASE("Instanced draws: a shared mesh draws all its placements in one call, pixel for pixel")
{
    if (!gpu("instanced draw"))
        return;
    constexpr int kSide = 10;
    Renderer::FrameStats shared, unique;
    const auto a = renderInstanced(gridOfBoxes(kSide, true), &shared);
    const auto b = renderInstanced(gridOfBoxes(kSide, false), &unique);
    CHECK(shared.meshDrawCalls == 1);
    CHECK(shared.meshInstances == kSide * kSide);
    CHECK(unique.meshDrawCalls == kSide * kSide);
    CHECK(unique.meshInstances == kSide * kSide);
    CHECK(maxDiff(a, b) == 0);
    MESSAGE("100 boxes: " << shared.meshDrawCalls << " draw call shared vs " << unique.meshDrawCalls << " unique");

    // The same placements given as one draw's instance list.
    FrameState listed = frameAt(kSide * 10.0);
    MeshDraw d;
    d.mesh = box(3.0f);
    auto list = std::make_shared<std::vector<glm::mat4>>();
    for (int i = 0; i < kSide * kSide; ++i)
        list->push_back(glm::translate(glm::mat4(1.0f), glm::vec3((i % kSide - kSide / 2) * 10.0f, 0.0f, (i / kSide - kSide / 2) * 10.0f)));
    d.instances = list;
    listed.meshes.push_back(d);
    Renderer::FrameStats listedStats;
    CHECK(maxDiff(renderInstanced(listed, &listedStats), a) == 0);
    CHECK(listedStats.meshDrawCalls == 1);
}

TEST_CASE("Instanced draws: placements read their own world matrix")
{
    if (!gpu("instanced draw"))
        return;
    // One box moved right of centre: a transposed matrix would lose the translation and leave it in the middle.
    FrameState s = frameAt(40.0);
    MeshDraw d;
    d.mesh = box(5.0f);
    d.world = glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, 0.0f));
    s.meshes.push_back(d);
    const double right = litCentroidX(renderInstanced(s));
    s.meshes.edit()[0].world = glm::translate(glm::mat4(1.0f), glm::vec3(-20.0f, 0.0f, 0.0f));
    const double left = litCentroidX(renderInstanced(s));
    REQUIRE(right >= 0.0);
    REQUIRE(left >= 0.0);
    CHECK(right > kDrawSize * 0.6);
    CHECK(left < kDrawSize * 0.4);
}

TEST_CASE("Instanced draws: a live mesh's wire follows each new snapshot of its points")
{
    if (!gpu("instanced draw"))
        return;
    auto first = box(3.0f);
    first->key = RenderMesh::nextKey();
    first->dynamic = std::make_shared<DynamicVertices>();
    first->dynamic->vertices.assign(first->vertices.begin(), first->vertices.end());
    FrameState s = frameAt(30.0);
    MeshDraw d;
    d.mesh = first;
    d.flags = DrawWire;
    s.meshes.push_back(d);
    const double before = litCentroidX(renderInstanced(s));

    // A later snapshot of the same live mesh: same key and vertex buffer, points moved.
    auto moved = std::make_shared<RenderMesh>(*first);
    for (MeshVertex& v : moved->vertices)
        v.pos.x += 20.0f;
    for (glm::vec3& p : moved->edgePoints)
        p.x += 20.0f;
    {
        std::scoped_lock lock(moved->dynamic->mutex);
        moved->dynamic->vertices.assign(moved->vertices.begin(), moved->vertices.end());
        moved->dynamic->allDirty = true;
    }
    s.meshes.edit()[0].mesh = moved;
    const double after = litCentroidX(renderInstanced(s));
    REQUIRE(before >= 0.0);
    REQUIRE(after >= 0.0);
    CHECK(after > before + kDrawSize * 0.2);
}

TEST_CASE("Instanced draws: wire, selection and translucency per placement")
{
    if (!gpu("instanced draw"))
        return;
    FrameState s = gridOfBoxes(3, true);
    for (MeshDraw& d : s.meshes.edit())
        d.flags = DrawShaded | DrawWire;
    s.meshes.edit()[4].flags |= DrawSelected;
    const auto shaded = renderInstanced(s);
    // The same frame drawn per mesh copy matches, once its selected draw comes last as batches order highlighted wires.
    FrameState unique = gridOfBoxes(3, false);
    for (size_t i = 0; i < unique.meshes.size(); ++i)
        unique.meshes.edit()[i].flags = s.meshes[i].flags;
    std::vector<MeshDraw>& rotated = unique.meshes.edit();
    std::rotate(rotated.begin() + 4, rotated.begin() + 5, rotated.end());
    CHECK(maxDiff(shaded, renderInstanced(unique)) == 0);

    // Translucent placements sort one by one, so they match the per-copy frame too.
    for (FrameState* f : {&s, &unique})
        for (MeshDraw& d : f->meshes.edit())
        {
            Material m;
            m.flags |= MatTranslucent;
            m.opacity = 0.5f;
            d.materials = {m};
        }
    Renderer::FrameStats stats;
    CHECK(maxDiff(renderInstanced(s, &stats), renderInstanced(unique)) == 0);
    CHECK(stats.meshInstances == 9);
}

TEST_CASE("Instanced draws: timing for many placements of one mesh")
{
    if (!gpu("instanced draw"))
        return;
    constexpr int kSide = 70; // 4900 boxes
    Renderer::FrameStats shared, unique;
    renderInstanced(gridOfBoxes(kSide, true), &shared, 5);
    renderInstanced(gridOfBoxes(kSide, false), &unique, 5);
    CHECK(shared.meshDrawCalls == 1);
    CHECK(unique.meshDrawCalls == kSide * kSide);
    MESSAGE("4900 boxes, CPU record ms: instanced " << shared.cpuMs << ", one draw each " << unique.cpuMs << "; GPU ms: " << shared.gpuMs << " vs "
                                                   << unique.gpuMs);
}

namespace
{
/** Mean colour (rgb, 0 to 255) of the lit pixels in columns [x0, x1); zero when none are lit. */
glm::dvec3 litMean(const std::vector<uint8_t>& bgra, int x0, int x1)
{
    glm::dvec3 sum(0.0);
    int n = 0;
    for (int y = 0; y < kDrawSize; ++y)
        for (int x = x0; x < x1; ++x)
        {
            const uint8_t* p = &bgra[(size_t(y) * kDrawSize + x) * 4];
            if (p[0] + p[1] + p[2] > 6)
            {
                sum += glm::dvec3(p[2], p[1], p[0]);
                ++n;
            }
        }
    return n ? sum / double(n) : glm::dvec3(0.0);
}

/** Pixels that differ by more than a few levels in any channel. */
int changedPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        if (std::abs(int(a[i]) - int(b[i])) > 8 || std::abs(int(a[i + 1]) - int(b[i + 1])) > 8 || std::abs(int(a[i + 2]) - int(b[i + 2])) > 8)
            ++n;
    return n;
}

/** Three boxes left, middle and right as one instanced draw, coloured red, not at all and blue when colored. */
FrameState threeBoxes(bool colored)
{
    FrameState s = frameAt(30.0);
    MeshDraw d;
    d.mesh = box(5.0f);
    auto list = std::make_shared<std::vector<glm::mat4>>();
    for (float x : {-20.0f, 0.0f, 20.0f})
        list->push_back(glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f)));
    d.instances = list;
    if (colored)
        d.instanceColors = std::make_shared<std::vector<glm::vec4>>(std::vector<glm::vec4>{{0.8f, 0.05f, 0.05f, 1.0f}, glm::vec4(0.0f), {0.05f, 0.05f, 0.8f, 1.0f}});
    s.meshes.push_back(d);
    return s;
}
} // namespace

TEST_CASE("Instanced draws: each placement shows its instance colour")
{
    if (!gpu("instanced draw"))
        return;
    const auto plain = renderInstanced(threeBoxes(false));
    const auto colored = renderInstanced(threeBoxes(true));
    const int third = kDrawSize / 3;
    // The middle box alone: the outer boxes reach into the middle third under perspective.
    const int mid0 = kDrawSize * 7 / 16, mid1 = kDrawSize * 9 / 16;
    const glm::dvec3 left = litMean(colored, 0, third), middle = litMean(colored, mid0, mid1), right = litMean(colored, 2 * third, kDrawSize);
    MESSAGE("left " << left.r << " " << left.g << " " << left.b << ", middle " << middle.r << " " << middle.g << " " << middle.b << ", right " << right.r
                    << " " << right.g << " " << right.b);
    CHECK(left.r > 2.0 * left.g);
    CHECK(left.r > 2.0 * left.b);
    CHECK(right.b > 2.0 * right.r);
    CHECK(right.b > 2.0 * right.g);
    // The uncoloured placement is untouched.
    const glm::dvec3 plainMiddle = litMean(plain, mid0, mid1);
    CHECK(std::abs(middle.r - plainMiddle.r) < 1.0);
    CHECK(std::abs(middle.b - plainMiddle.b) < 1.0);

    // A material that keeps its own colour ignores them; one without the flag takes them.
    FrameState kept = threeBoxes(true);
    Material own;
    own.baseColor = glm::vec3(0.5f);
    own.flags |= MatNoInstanceColor;
    kept.meshes.edit()[0].materials = {own};
    FrameState keptPlain = threeBoxes(false);
    keptPlain.meshes.edit()[0].materials = {own};
    CHECK(maxDiff(renderInstanced(kept), renderInstanced(keptPlain)) == 0);
    own.flags &= ~uint32_t(MatNoInstanceColor);
    kept.meshes.edit()[0].materials = {own};
    CHECK(litMean(renderInstanced(kept), 0, third).r > 2.0 * litMean(renderInstanced(kept), 0, third).g);

    // Half an amount lands between the two.
    FrameState half = threeBoxes(true);
    auto halfColors = std::make_shared<std::vector<glm::vec4>>(*half.meshes[0].instanceColors);
    (*halfColors)[0].a = 0.5f;
    half.meshes.edit()[0].instanceColors = halfColors;
    const glm::dvec3 halfLeft = litMean(renderInstanced(half), 0, third), plainLeft = litMean(plain, 0, third);
    CHECK(halfLeft.g < plainLeft.g - 5.0);
    CHECK(halfLeft.g > left.g + 5.0);

    // Two tones: a material with MatInstanceColor2 takes the second colour, the others the first.
    FrameState toned = threeBoxes(true);
    auto second = std::make_shared<std::vector<glm::vec4>>(toned.meshes[0].instanceColors->size(), glm::vec4(0, 1, 0, 1));
    toned.meshes.edit()[0].instanceColors2 = second;
    Material first;
    first.baseColor = glm::vec3(0.5f);
    toned.meshes.edit()[0].materials = {first};
    const glm::dvec3 firstLeft = litMean(renderInstanced(toned), 0, third);
    CHECK(firstLeft.r > 2.0 * firstLeft.g);
    Material trim = first;
    trim.flags |= MatInstanceColor2;
    toned.meshes.edit()[0].materials = {trim};
    const glm::dvec3 secondLeft = litMean(renderInstanced(toned), 0, third);
    MESSAGE("second tone left " << secondLeft.r << " " << secondLeft.g << " " << secondLeft.b);
    CHECK(secondLeft.g > 2.0 * secondLeft.r);
    CHECK(secondLeft.g > 2.0 * secondLeft.b);
}

TEST_CASE("Instanced draws: selection still outlines coloured placements")
{
    if (!gpu("instanced draw"))
        return;
    auto selected = [](FrameState s) {
        s.meshes.edit()[0].flags = DrawShaded | DrawWire | DrawSelected;
        return s;
    };
    const int plainWire = changedPixels(renderInstanced(threeBoxes(false)), renderInstanced(selected(threeBoxes(false))));
    const int coloredWire = changedPixels(renderInstanced(threeBoxes(true)), renderInstanced(selected(threeBoxes(true))));
    MESSAGE("selection wire pixels: plain " << plainWire << ", coloured " << coloredWire);
    CHECK(plainWire > 50);
    CHECK(std::abs(coloredWire - plainWire) <= plainWire / 10);
}

TEST_CASE("Instanced draws: timing for 100k placements with and without instance colours")
{
    if (!gpu("instanced draw"))
        return;
    constexpr int kSide = 317; // 100,489 boxes
    FrameState s = frameAt(kSide * 10.0);
    MeshDraw d;
    d.mesh = box(3.0f);
    auto list = std::make_shared<std::vector<glm::mat4>>();
    auto colors = std::make_shared<std::vector<glm::vec4>>();
    for (int i = 0; i < kSide * kSide; ++i)
    {
        list->push_back(glm::translate(glm::mat4(1.0f), glm::vec3((i % kSide - kSide / 2) * 10.0f, 0.0f, (i / kSide - kSide / 2) * 10.0f)));
        colors->push_back(glm::vec4(float(i % 7) / 7.0f, float(i % 5) / 5.0f, float(i % 3) / 3.0f, 1.0f));
    }
    d.instances = list;
    s.meshes.push_back(d);
    // Alternated so clock ramps and caches favour neither.
    for (int round = 0; round < 3; ++round)
    {
        Renderer::FrameStats plain, colored;
        s.meshes.edit()[0].instanceColors = nullptr;
        renderInstanced(s, &plain, 10);
        s.meshes.edit()[0].instanceColors = colors;
        renderInstanced(s, &colored, 10);
        CHECK(plain.meshInstances == colored.meshInstances);
        CHECK(plain.meshDrawCalls == colored.meshDrawCalls);
        MESSAGE("100,489 boxes, CPU record ms: plain " << plain.cpuMs << ", coloured " << colored.cpuMs << "; GPU ms: " << plain.gpuMs << " vs "
                                                       << colored.gpuMs);
    }
}

TEST_CASE("Instanced draws: ray-traced reflections show instance colours")
{
    if (!gpu("instanced draw") || !Renderer::get().gpuInfo().rayTracing)
        return;
    // Every placement red over a mirror floor; the reflections' change from plain to coloured is what the colour adds.
    auto scene = [](bool colored, FrameState::Reflections r) {
        FrameState s = threeBoxes(false);
        if (colored)
            s.meshes.edit()[0].instanceColors = std::make_shared<std::vector<glm::vec4>>(3, glm::vec4(0.8f, 0.05f, 0.05f, 1.0f));
        s.reflections = r;
        s.lighting = FrameState::Lighting::Environment;
        s.reflectionCutoff = 1.0f;
        MeshDraw floor;
        floor.mesh = box(1.0f);
        floor.world = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -8.0f, 0.0f)), glm::vec3(200.0f, 0.5f, 200.0f));
        Material mirror;
        mirror.baseColor = glm::vec3(0.9f);
        mirror.metallic = 1.0f;
        mirror.roughness = 0.05f;
        mirror.flags |= MatNoInstanceColor;
        floor.materials = {mirror};
        s.meshes.push_back(floor);
        return s;
    };
    auto reflected = [&](bool colored) {
        const auto on = renderInstanced(scene(colored, FrameState::Reflections::RayTraced)), off = renderInstanced(scene(colored, FrameState::Reflections::Off));
        glm::dvec3 sum(0.0);
        for (size_t i = 0; i < on.size(); i += 4)
            sum += glm::dvec3(int(on[i + 2]) - int(off[i + 2]), int(on[i + 1]) - int(off[i + 1]), int(on[i]) - int(off[i]));
        return sum;
    };
    const glm::dvec3 plain = reflected(false), delta = reflected(true) - plain;
    MESSAGE("reflections against none, rgb sums: " << plain.r << " " << plain.g << " " << plain.b);
    MESSAGE("reflection change from the colour, rgb sums: " << delta.r << " " << delta.g << " " << delta.b);
    CHECK(delta.r > 2000.0);
    CHECK(delta.g < -2000.0);
    CHECK(delta.b < -2000.0);
}
