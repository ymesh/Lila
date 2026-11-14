#include "retopo/Corpus.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace lila;

namespace
{
retopo::QuadRemeshResult remesh(const geo::Mesh& m, int faces, int symmetry = -1)
{
    retopo::QuadRemeshOptions o{.targetFaces = faces};
    o.symmetryAxis = symmetry;
    if (const char* a = std::getenv("LILA_AB_ADAPT"))
        o.adaptivity = std::atof(a);
    if (const char* t = std::getenv("LILA_AB_TIMEOUT"))
        o.timeout = std::atof(t);
    return retopo::quadRemesh(m, o);
}

void check(const char* name, const geo::Mesh& m, int faces, geo::Index maxIrregular)
{
    retopo::QuadRemeshResult r = remesh(m, faces);
    REQUIRE(r.ok());
    if (std::getenv("LILA_RETOPO_DUMP"))
        geo::writeObjFile((std::filesystem::temp_directory_path() / (std::string("flow_") + name + ".obj")).string(), r.mesh);
    MESSAGE(name << ": " << r.metrics.summary());
    CHECK(r.layout == "flow");
    CHECK(r.defects == 0);
    CHECK(r.metrics.triangles + r.metrics.ngons == 0);
    CHECK(r.metrics.irregular <= maxIrregular);
    CHECK(std::abs(double(r.mesh.numFaces()) / faces - 1.0) <= 0.1);
}
} // namespace

TEST_CASE("flow layout turns only at a few poles")
{
    check("sphere", geo::makeSphere({.radius = 50.0, .subdivAxis = 48, .subdivHeight = 48}), 600, 10);
    check("cube", geo::makeCube({.subdivX = 8, .subdivY = 8, .subdivZ = 8}), 600, 8);
    check("cylinder", geo::makeCylinder({.radius = 30.0, .height = 120.0, .subdivAxis = 48, .subdivHeight = 8, .subdivCaps = 4, .capFill = geo::CapFill::Fan}), 600, 8);
}

TEST_CASE("a torus, with no poles, comes out as a clean grid")
{
    retopo::QuadRemeshResult r = remesh(geo::makeTorus({.subdivAxis = 64, .subdivHeight = 32}), 800);
    REQUIRE(r.ok());
    MESSAGE(r.metrics.summary());
    CHECK(r.metrics.irregular == 0);
}

TEST_CASE("a symmetric remesh mirrors its half with one straight loop down the middle")
{
    geo::Mesh m = geo::makeSphere({.radius = 50.0, .subdivAxis = 48, .subdivHeight = 48});
    retopo::QuadRemeshResult r = remesh(m, 800, 0);
    REQUIRE(r.ok());
    MESSAGE(r.metrics.summary());
    CHECK(r.defects == 0);
    // Every point's mirror image is a point.
    std::vector<glm::dvec3> pts;
    for (geo::Index p = 0; p < r.mesh.pointCapacity(); ++p)
        if (r.mesh.pointValid(p))
            pts.push_back(r.mesh.position(p));
    int unmatched = 0;
    for (const glm::dvec3& p : pts)
    {
        bool found = false;
        for (const glm::dvec3& q : pts)
            if (glm::distance(q, glm::dvec3(-p.x, p.y, p.z)) < 1e-6)
            {
                found = true;
                break;
            }
        unmatched += !found;
    }
    CHECK(unmatched == 0);
}

TEST_CASE("zz flow corpus timing" * doctest::skip())
{
    // One corpus case per run, by index: lila_retopo_tests -tc="zz flow corpus timing" -ns, with LILA_CORPUS_CASE=n.
    const char* which = std::getenv("LILA_CORPUS_CASE");
    if (!which)
        return;
    std::vector<corpus::Case> cases = corpus::all();
    size_t i = size_t(std::atoi(which));
    if (i >= cases.size())
        return;
    auto start = std::chrono::steady_clock::now();
    retopo::QuadRemeshResult r = remesh(cases[i].mesh, 600);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%s: %.2f s %s, %d defects\n", cases[i].name.c_str(), s, r.ok() ? r.metrics.summary().c_str() : r.error.c_str(), r.defects);
    if (const char* out = std::getenv("LILA_AB_OUT"))
        std::ofstream(out) << geo::writeObj(r.mesh);
    if (const char* src = std::getenv("LILA_AB_SRC"))
        std::ofstream(src) << geo::writeObj(cases[i].mesh);
}

TEST_CASE("zz remesh obj" * doctest::skip())
{
    // LILA_AB_OBJ=world-space obj, LILA_AB_MATRIX=its world matrix (16 numbers, column major), LILA_AB_FACES, LILA_AB_SYM, LILA_AB_OUT=result obj.
    const char* path = std::getenv("LILA_AB_OBJ");
    if (!path)
        return;
    geo::ObjReadResult read = geo::readObjFile(path);
    REQUIRE(read.ok());
    geo::Mesh m = read.objects.front().mesh;
    if (const char* mat = std::getenv("LILA_AB_MATRIX"))
    {
        std::istringstream in(mat);
        glm::dmat4 w(1.0);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                in >> w[c][r];
        glm::dmat4 inv = glm::inverse(w);
        for (glm::dvec3& p : m.positions())
            p = glm::dvec3(inv * glm::dvec4(p, 1.0));
    }
    retopo::QuadRemeshOptions o;
    o.targetFaces = std::getenv("LILA_AB_FACES") ? std::atoi(std::getenv("LILA_AB_FACES")) : 500;
    o.symmetryAxis = std::getenv("LILA_AB_SYM") ? std::atoi(std::getenv("LILA_AB_SYM")) : -1;
    o.resolveGaps = !std::getenv("LILA_NO_GAPS");
    o.useHardEdges = !std::getenv("LILA_NO_HARD");
    if (const char* a = std::getenv("LILA_AB_ADAPT"))
        o.adaptivity = std::atof(a);
    if (const char* a = std::getenv("LILA_AB_AROUND"))
        o.edgesAround = std::atoi(a);
    if (const char* a = std::getenv("LILA_AB_SHARP"))
        o.sharpAngle = std::atof(a);
    if (std::getenv("LILA_AB_GRID"))
        o.method = retopo::RemeshMethod::Grid;
    if (const char* t = std::getenv("LILA_AB_TOL"))
        o.countTolerance = std::atof(t);
    const auto started = std::chrono::steady_clock::now();
    retopo::QuadRemeshResult r = retopo::quadRemesh(m, o);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%s, %d defects, %s, %.2f s\n", r.ok() ? r.metrics.summary().c_str() : r.error.c_str(), r.defects, r.layout.c_str(), took);
    for (const retopo::RemeshAttempt& at : r.attempts)
        std::printf("  %s %u faces %d defects %u irregular\n", at.layout.c_str(), unsigned(at.faces), at.defects, unsigned(at.irregular));
    if (const char* out = std::getenv("LILA_AB_OUT"))
        std::ofstream(out) << geo::writeObj(r.mesh);
}

TEST_CASE("quad remesh gives up past its timeout and says how far it got")
{
    retopo::QuadRemeshOptions o{.targetFaces = 4000};
    o.timeout = 1e-6;
    retopo::QuadRemeshResult r = retopo::quadRemesh(geo::makeSphere({.radius = 50.0, .subdivAxis = 96, .subdivHeight = 96}), o);
    REQUIRE(!r.ok());
    MESSAGE(r.error);
    CHECK(r.error.rfind("gave up after", 0) == 0);
    CHECK(r.mesh.numFaces() == 0);
}
