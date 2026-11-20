#include "TestUtil.h"

#include "Pieces.h"
#include "Placeholders.h"

using namespace weatheringtest;

TEST_CASE("weathering: extension loads after Stagehand and registers its presets hook")
{
    fresh();
    CHECK(ops::registry().find("weatheringPlaceholders"));
    CHECK(ops::registry().find("arrangementRegister"));
    CHECK(ops::registry().find("libraryMaster"));
}

TEST_CASE("weathering: pieces resolve from meshes, groups, sets and weights")
{
    fresh();
    const std::string a = cube("PieceA", {0, 0, 0}, {10, 10, 10});
    const std::string b = cube("PieceB", {30, 0, 0}, {10, 20, 10});
    run("group", {{"args", {a, b}}, {"name", "Kit"}});
    const std::vector<weathering::Piece> fromGroup = weathering::resolvePieces(g(), json("Kit"), "test");
    CHECK(fromGroup.size() == 2);
    const std::vector<weathering::Piece> weighted =
        weathering::resolvePieces(g(), json::array({{{"piece", "PieceA"}, {"weight", 3.0}}, "PieceB"}), "test");
    REQUIRE(weighted.size() == 2);
    CHECK(weighted[0].weight == doctest::Approx(3.0));
    CHECK(weathering::pickPiece(weighted, 0.1) == 0);
    CHECK(weathering::pickPiece(weighted, 0.7) == 0);
    CHECK(weathering::pickPiece(weighted, 0.8) == 1);
    CHECK_THROWS(weathering::resolvePieces(g(), json("NoSuchThing"), "test"));
}

TEST_CASE("weathering: unit hash is deterministic and spread over [0, 1)")
{
    double lo = 1.0, hi = 0.0;
    for (uint32_t k = 0; k < 2000; ++k)
    {
        const double u = weathering::unitHash(7, k);
        CHECK(u == weathering::unitHash(7, k));
        lo = std::min(lo, u);
        hi = std::max(hi, u);
    }
    CHECK(lo < 0.01);
    CHECK(hi > 0.99);
    CHECK(hi < 1.0);
}
