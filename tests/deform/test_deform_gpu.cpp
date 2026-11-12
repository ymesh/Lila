#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "render/DeformGpu.h"
#include "render/Renderer.h"
#include "scene/DeformEval.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/MeshCache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <sstream>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using namespace deformtest;

namespace
{
/** Starts the renderer and registers the GPU deformers once; false (and the GPU cases pass unchecked) without a device. */
bool gpu()
{
    static const bool ok = [] {
        render::Renderer& r = render::Renderer::get();
        if (!r.running() && !r.start())
        {
            MESSAGE("no GPU for deformer tests");
            return false;
        }
        render::registerGpuDeformers();
        MESSAGE("GPU deformers on " << r.deviceName());
        return true;
    }();
    return ok;
}

void useGpu(bool on, double minPoints = 0.0)
{
    PrefStore::get().set("general", "/deformers/gpu", on);
    PrefStore::get().set("general", "/deformers/gpuMinPoints", minPoints);
}

DeformerNode& deformer(const std::string& name)
{
    Node* n = graph().find(name);
    REQUIRE(n);
    REQUIRE(deformers::isDeformer(*n));
    return static_cast<DeformerNode&>(*n);
}

/**
 * Runs a deformer on its current input on the CPU and then the GPU and checks they agree: within tol of the mesh's
 * size, after the deformer visibly moved the points.
 */
void checkAgainstCpu(const std::string& name, double tol = 2e-5)
{
    CAPTURE(name);
    Graph& g = graph();
    g.evaluate();
    DeformerNode& d = deformer(name);
    MeshPtr in = std::get<MeshPtr>(g.value({d.id(), DeformerNode::kInMesh, -1}));
    REQUIRE(in);
    useGpu(false);
    const geo::Mesh cpu = d.run(*in);
    useGpu(true);
    bool ran = false;
    const geo::Mesh gpuOut = d.run(*in, &ran);
    REQUIRE(ran);
    const double size = glm::length(cpu.bounds().size());
    double moved = 0.0, err = 0.0;
    for (geo::Index p = 0; p < cpu.pointCapacity(); ++p)
    {
        moved = std::max(moved, glm::length(cpu.position(p) - in->position(p)));
        err = std::max(err, glm::length(cpu.position(p) - gpuOut.position(p)));
    }
    CHECK(moved > size * 1e-3);
    CHECK(err <= size * tol);
    MESSAGE(name << ": moved up to " << moved << ", GPU off by " << err << " (" << err / size << " of the size)");
}

std::string ball()
{
    std::string mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}, {"subdivisionsAxis", 48}, {"subdivisionsHeight", 32}})[0];
    run("move", {{"args", {mesh}}, {"by", v3(3, 1, -2)}});
    return mesh;
}
} // namespace

TEST_CASE("GPU nonlinear deformers match the CPU")
{
    if (!gpu())
        return;
    const std::vector<std::pair<std::string, json>> cases = {
        {"bend", {{"angle", 70.0}}},
        {"bend", {{"angle", 0.01}}},
        {"twist", {{"endAngle", 120.0}, {"lowBound", -0.5}}},
        {"taper", {{"endScale", 0.3}, {"curve", 0.5}}},
        {"squash", {{"factor", -0.4}, {"maxExpandPos", 0.3}}},
        {"sine", {{"amplitude", 0.3}, {"wavelength", 0.7}, {"offset", 3.25}, {"dropoff", 0.3}}},
        {"wave", {{"amplitude", 0.2}, {"wavelength", 0.4}, {"offset", 1.6}, {"dropoff", 0.5}, {"minRadius", 0.1}, {"maxRadius", 0.9}}},
        {"bend", {{"angle", 120.0}, {"falloffRadius", 1.1}, {"falloffSoftness", 0.6}}},
    };
    for (const auto& [type, params] : cases)
    {
        CAPTURE(type);
        run("newScene");
        std::string mesh = ball();
        json made = run("nonlinear", {{"args", {mesh}}, {"type", type}, {"params", params}});
        run("setAttr", {{"args", {made[0].get<std::string>() + ".rotateZ", 20.0}}});
        if (type == "bend" && params.value("angle", 0.0) < 1.0)
        {
            // A gentle bend still moves the far ends a little; the GPU must not lose it to float cancellation.
            Graph& g = graph();
            g.evaluate();
            DeformerNode& d = deformer(made[1]);
            MeshPtr in = std::get<MeshPtr>(g.value({d.id(), DeformerNode::kInMesh, -1}));
            useGpu(false);
            const geo::Mesh cpu = d.run(*in);
            useGpu(true);
            const geo::Mesh gpuOut = d.run(*in);
            double err = 0.0;
            for (geo::Index p = 0; p < cpu.pointCapacity(); ++p)
                err = std::max(err, glm::length(cpu.position(p) - gpuOut.position(p)));
            CHECK(err < 1e-4);
            continue;
        }
        checkAgainstCpu(made[1]);
    }
}

TEST_CASE("GPU ffd matches the CPU for every basis")
{
    if (!gpu())
        return;
    for (const char* interp : {"trilinear", "bernstein", "bspline"})
    {
        const std::string label = interp;
        CAPTURE(label);
        run("newScene");
        std::string mesh = ball();
        json made = run("lattice", {{"args", {mesh}}, {"divisions", v3(4, 5, 3)}, {"interpolation", interp}});
        editPoints(made[0], [](geo::Index i, const glm::dvec3& p) {
            return glm::dvec3(std::sin(double(i)) * 0.1, p.y > 0.2 ? 0.3 : 0.0, std::cos(double(i) * 1.7) * 0.08);
        });
        run("rotate", {{"args", {made[0]}}, {"by", v3(0, 25, 0)}});
        checkAgainstCpu(made[1]);
    }
}

TEST_CASE("GPU kelvinlet matches the CPU for grab, twist, scale and pinch")
{
    if (!gpu())
        return;
    for (const char* falloff : {"single", "biScale", "triScale"})
    {
        const std::string label = falloff;
        CAPTURE(label);
        run("newScene");
        std::string mesh = ball();
        json made = run("kelvinlet", {{"args", {mesh}}, {"radius", 6.0}, {"falloff", falloff}});
        const std::string h = made[0];
        run("move", {{"args", {h}}, {"by", v3(2, 1, 0)}});
        run("setAttr", {{"args", {h + ".rotateY", 30.0}}});
        run("setAttr", {{"args", {h + ".scaleX", 1.3}}});
        run("setAttr", {{"args", {h + ".scaleZ", 0.8}}});
        checkAgainstCpu(made[1]);
    }
}

TEST_CASE("GPU cage and wrap match the CPU in every mode")
{
    if (!gpu())
        return;
    auto scene = [](const char* cmd, json flags) {
        run("newScene");
        std::string mesh = ball();
        std::string driver = run("polyCube", {{"name", "Proxy"}, {"width", 30}, {"height", 30}, {"depth", 30}, {"subdivisionsX", 2},
                                              {"subdivisionsY", 2}, {"subdivisionsZ", 2}})[0];
        run("move", {{"args", {driver}}, {"by", v3(3, 1, -2)}});
        flags["args"] = {mesh, driver};
        std::string d = run(cmd, flags)[0];
        editPoints(driver, [](geo::Index, const glm::dvec3& p) { return p.x > 0 ? glm::dvec3(6, p.y > 0 ? 3 : 0, 0) : glm::dvec3(0.0, 0.0, -1.0); });
        return d;
    };
    checkAgainstCpu(scene("cage", json::object()));
    for (const char* mode : {"meanValue", "green", "somigliana", "harmonic"})
    {
        const std::string label = mode;
        CAPTURE(label);
        checkAgainstCpu(scene("cage", {{"coordinates", mode}}));
    }
    for (const char* mode : {"offset", "surface", "snap"})
    {
        const std::string label = mode;
        CAPTURE(label);
        checkAgainstCpu(scene("wrap", {{"wrapMode", mode}, {"scaleCompensation", 0.5}}));
    }
}

TEST_CASE("GPU delta mush matches the CPU")
{
    if (!gpu())
        return;
    for (bool pin : {true, false})
    {
        CAPTURE(pin);
        run("newScene");
        std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 5}, {"height", 100}, {"subdivisionsAxis", 24}, {"subdivisionsHeight", 40},
                                                {"caps", false}})[0];
        json made = run("nonlinear", {{"args", {mesh}}, {"type", "bend"}, {"params", {{"angle", 0.0}}}});
        std::string mush = run("deltaMush", {{"args", {mesh}}, {"iterations", 12}})[0];
        run("setAttr", {{"args", {mush + ".pinBorders", pin}}});
        run("setAttr", {{"args", {made[1].get<std::string>() + ".angle", 90.0}}});
        checkAgainstCpu(mush, 5e-5);
    }
}

TEST_CASE("GPU deformers feed the stack, and exports re-run them on the CPU")
{
    if (!gpu())
        return;
    run("newScene");
    std::string mesh = ball();
    json bend = run("nonlinear", {{"args", {mesh}}, {"type", "bend"}, {"params", {{"angle", 60.0}}}});
    json lat = run("lattice", {{"args", {mesh}}, {"divisions", v3(3, 3, 3)}});
    editPoints(lat[0], [](geo::Index, const glm::dvec3& p) { return p.y > 0.2 ? glm::dvec3(0.2, 0.3, 0) : glm::dvec3(0.0); });

    useGpu(true);
    deformEval::resetStats();
    run("setAttr", {{"args", {bend[1].get<std::string>() + ".angle", 75.0}}});
    graph().evaluate();
    CHECK(deformEval::stats().gpuRuns == 2);
    CHECK(deformer(bend[1]).ranOnGpu());
    CHECK(deformer(lat[1]).ranOnGpu());
    MeshPtr shown = graph().find(mesh)->as<MeshPtr>(MeshNode::kOutMesh);

    MeshPtr exported = modifiers::exportMesh(graph(), graph().find(mesh)->id());
    REQUIRE(exported);
    CHECK(exported != shown);
    useGpu(false);
    run("setAttr", {{"args", {bend[1].get<std::string>() + ".angle", 60.0}}});
    graph().evaluate();
    run("setAttr", {{"args", {bend[1].get<std::string>() + ".angle", 75.0}}});
    graph().evaluate();
    MeshPtr cpu = graph().find(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    CHECK_FALSE(deformer(lat[1]).ranOnGpu());
    double exact = 0.0, shownErr = 0.0;
    for (geo::Index p = 0; p < cpu->pointCapacity(); ++p)
    {
        exact = std::max(exact, glm::length(exported->position(p) - cpu->position(p)));
        shownErr = std::max(shownErr, glm::length(shown->position(p) - cpu->position(p)));
    }
    CHECK(exact == 0.0);
    CHECK(shownErr < 1e-3);
    CHECK(shownErr > 0.0);
}

TEST_CASE("GPU deformers: below the point threshold, with the pref off or in a CpuOnly scope the CPU runs")
{
    if (!gpu())
        return;
    run("newScene");
    std::string mesh = ball();
    json made = run("nonlinear", {{"args", {mesh}}, {"type", "twist"}});
    Graph& g = graph();
    g.evaluate();
    DeformerNode& d = deformer(made[1]);
    MeshPtr in = std::get<MeshPtr>(g.value({d.id(), DeformerNode::kInMesh, -1}));
    bool ran = true;
    useGpu(true, 1e9);
    d.run(*in, &ran);
    CHECK_FALSE(ran);
    useGpu(false);
    d.run(*in, &ran);
    CHECK_FALSE(ran);
    useGpu(true);
    {
        deformEval::CpuOnly cpu;
        d.run(*in, &ran);
        CHECK_FALSE(ran);
    }
    d.run(*in, &ran);
    CHECK(ran);
}

TEST_CASE("Deformer benchmark: bend, ffd and delta mush on 500k points" * doctest::skip())
{
    // Run with: lila_deform_gpu_tests --no-skip -tc="Deformer benchmark*"
    const bool haveGpu = gpu();
    std::ostringstream report;
    for (bool onGpu : {false, true})
    {
        if (onGpu && !haveGpu)
            break;
        useGpu(onGpu, 20000.0);
        run("newScene");
        std::string mesh =
            run("polySphere", {{"name", "Dense"}, {"radius", 50}, {"subdivisionsAxis", 1000}, {"subdivisionsHeight", 500}})[0];
        json bend = run("nonlinear", {{"args", {mesh}}, {"type", "bend"}, {"params", {{"angle", 0.0}}}});
        json lat = run("lattice", {{"args", {mesh}}, {"divisions", v3(5, 5, 5)}, {"interpolation", "bspline"}});
        std::string mush = run("deltaMush", {{"args", {mesh}}, {"iterations", 10}})[0];
        graph().evaluate();
        const std::string bendName = bend[1], latName = lat[0], ffdName = lat[1];
        const size_t points = graph().find(mesh)->as<MeshPtr>(MeshNode::kOutMesh)->pointCapacity();

        auto timed = [&](const std::function<void()>& change) {
            std::vector<double> total, view, b, f, m;
            for (int rep = 0; rep < 5; ++rep)
            {
                change();
                const auto t0 = std::chrono::steady_clock::now();
                graph().evaluate();
                const auto t1 = std::chrono::steady_clock::now();
                ui::renderMeshFor(graph().find(mesh)->as<MeshPtr>(MeshNode::kOutMesh));
                const auto t2 = std::chrono::steady_clock::now();
                total.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                view.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
                b.push_back(deformer(bendName).as<double>(DeformerNode::kEvalTime));
                f.push_back(deformer(ffdName).as<double>(DeformerNode::kEvalTime));
                m.push_back(deformer(mush).as<double>(DeformerNode::kEvalTime));
            }
            auto median = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };
            std::ostringstream s;
            s.precision(1);
            s << std::fixed << "evaluate " << median(total) << " ms (bend " << median(b) << ", ffd " << median(f) << ", deltaMush " << median(m)
              << "), viewport copy " << median(view) << " ms";
            return s.str();
        };
        int step = 0;
        const std::string all = timed([&] { run("setAttr", {{"args", {bendName + ".angle", 10.0 + 5.0 * ++step}}}); });
        const std::string top = timed([&] {
            ++step;
            editPoints(latName, [&](geo::Index i, const glm::dvec3&) { return i == 62 ? glm::dvec3(0.0, 0.01 * step, 0.0) : glm::dvec3(0.0); });
        });
        report << (onGpu ? "GPU " : "CPU ") << points << " points\n  change bend (3 deformers): " << all
               << "\n  move a lattice point (ffd, deltaMush): " << top << "\n";
    }
    MESSAGE(report.str());
}

TEST_CASE("A node's live render buffer never mixes two vertex layouts of the same topology")
{
    // Stored split normals are not part of the layout key, yet they decide how corners weld into render vertices.
    auto withNormals = [](bool split, double shift) {
        auto m = std::make_shared<geo::Mesh>(geo::makeCube());
        m->cornerAttrs().remove(geo::attr::kNormal);
        const std::vector<glm::dvec3> faces = geo::faceNormals(*m);
        const std::vector<glm::vec3> points = geo::pointNormals(*m);
        std::span<glm::vec3> n = m->cornerAttrs().add(std::string(geo::attr::kNormal), geo::AttrType::Float3).values<glm::vec3>();
        for (geo::Index c = 0; c < m->cornerCapacity(); ++c)
            if (m->cornerValid(c))
                n[c] = split ? glm::vec3(faces[m->cornerFace(c)]) : points[m->cornerPoint(c)];
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                m->setPosition(p, m->position(p) + glm::dvec3(shift, 0.0, 0.0));
        return MeshPtr(std::move(m));
    };
    const NodeId owner = 987654;
    const MeshPtr smooth = withNormals(false, 0.0), smoothMoved = withNormals(false, 1.0);
    ui::renderMeshFor(smooth, false, owner);
    auto first = ui::renderMeshFor(smoothMoved, false, owner);
    REQUIRE(first);
    REQUIRE(first->dynamic);

    // Other meshes push the smooth layout out of the cache, so the split one is built in full under the same key.
    std::vector<MeshPtr> others;
    for (int k = 1; k <= 12; ++k)
    {
        geo::CubeOptions o;
        o.subdivX = k + 1;
        others.push_back(std::make_shared<geo::Mesh>(geo::makeCube(o)));
        ui::renderMeshFor(others.back());
    }
    const MeshPtr split = withNormals(true, 0.0), splitMoved = withNormals(true, 1.0);
    auto full = ui::renderMeshFor(split, false, owner);
    auto second = ui::renderMeshFor(splitMoved, false, owner);
    REQUIRE(full);
    REQUIRE(second);
    REQUIRE(full->vertices.size() != first->vertices.size());
    CHECK(second->vertices.size() == full->vertices.size());
    // The renderer keeps one GPU copy per key, indices included, so a shared key must mean the same layout.
    if (second->key == first->key)
    {
        CHECK(second->indices == first->indices);
        CHECK(second->vertices.size() == first->vertices.size());
    }
    CHECK(second->dynamic != first->dynamic);
}
