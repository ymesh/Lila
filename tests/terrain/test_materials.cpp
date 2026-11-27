#include "TestUtil.h"
#include "Erosion.h"
#include "GroundCover.h"
#include "Materials.h"
#include "Terrain.h"

#include "geo/Heightfield.h"
#include "gn/Gn.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>
#include <tbb/task_arena.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
/** A ridged, eroded range 4 km across with a 1.2 km relief: flats, valleys, cliffs and scree. */
geo::Heightfield range(int n = 257)
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {400000.0, 400000.0}, n);
    terrain::NoiseParams p;
    p.featureSize = 120000.0;
    p.amplitude = 120000.0;
    p.ridged = 0.8;
    p.seed = 7;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 q = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(terrain::noiseHeight(p, q.x, q.y));
        }
    terrain::ErosionParams e;
    e.iterations = 30;
    terrain::erode(h, e);
    return h;
}

double maxSum(const geo::Heightfield& h, const std::string& prefix)
{
    double worst = 0.0;
    size_t outside = 0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        double s = 0.0;
        for (const auto& [name, v] : h.masks)
            if (name.starts_with(prefix))
            {
                outside += v[k] < 0.0f || v[k] > 1.0f + 1e-6f;
                s += v[k];
            }
        worst = std::max(worst, std::abs(s - 1.0));
    }
    CHECK(outside == 0);
    return worst;
}

double meanOf(const std::vector<float>& v)
{
    double s = 0.0;
    for (float x : v)
        s += x;
    return s / double(v.size());
}

bool sameMasks(const geo::Heightfield& a, const geo::Heightfield& b, const std::string& prefix)
{
    for (const auto& [name, v] : a.masks)
        if (name.starts_with(prefix))
        {
            const std::vector<float>* w = b.findMask(name);
            if (!w || w->size() != v.size() || std::memcmp(w->data(), v.data(), v.size() * sizeof(float)) != 0)
                return false;
        }
    return true;
}
} // namespace

TEST_CASE("materials: layer weights sum to 1 everywhere, match their rules, and are the same bytes at any thread count")
{
    geo::Heightfield h = range();
    terrain::MaterialRules rules = terrain::defaultMaterialRules(h, 0.0);
    rules.layers.push_back(terrain::MaterialRules::fromJson(json{{"layers", {{{"name", "moss"}, {"when", {{"wet", {0.6, nullptr}}, {"slope", {0, 20}}}}, {"noise", 0.4}}}}}).layers[0]);
    const terrain::RulesResult r = terrain::applyMaterialRules(h, rules);
    CHECK(r.layers.size() == rules.layers.size());
    CHECK(r.missing.empty());
    CHECK(r.maxSumError < 1e-5);
    CHECK(maxSum(h, "layer_") < 1e-5);

    // Coverage is the mask's mean, and every layer the rules allow shows up somewhere on this terrain.
    double total = 0.0;
    for (const terrain::Coverage& c : r.layers)
    {
        CHECK(c.coverage == doctest::Approx(meanOf(*h.findMask(c.mask))).epsilon(1e-9));
        total += c.coverage;
    }
    CHECK(total == doctest::Approx(1.0).epsilon(1e-6));
    for (const char* layer : {"grass", "rock", "riverbed", "snow"})
        CHECK(h.findMask(terrain::layerMask(layer)));

    // Rock goes on the steep ground and stays off the flat.
    terrain::SurfaceFields fields(h);
    const std::vector<float>& slope = *fields.get("slope");
    const std::vector<float>& rock = *h.findMask("layer_rock");
    const std::vector<float>& snow = *h.findMask("layer_snow");
    double steepRock = 0.0, flatRock = 0.0;
    size_t steep = 0, flat = 0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        if (slope[k] > 50.0f)
        {
            steepRock += rock[k] + snow[k];
            ++steep;
        }
        else if (slope[k] < 20.0f)
        {
            flatRock += rock[k];
            ++flat;
        }
    }
    REQUIRE(steep > 0);
    REQUIRE(flat > 0);
    CHECK(steepRock / double(steep) > 0.6);
    CHECK(flatRock / double(flat) < 0.05);

    // Determinism: again, and on one thread.
    geo::Heightfield again = range();
    terrain::applyMaterialRules(again, rules);
    CHECK(sameMasks(h, again, "layer_"));
    geo::Heightfield single = range();
    tbb::task_arena one(1);
    one.execute([&] { terrain::applyMaterialRules(single, rules); });
    CHECK(sameMasks(h, single, "layer_"));

    // A rule over a mask the terrain lacks reads 0 and is reported.
    geo::Heightfield bare = range(65);
    terrain::MaterialRules odd = terrain::MaterialRules::fromJson(json{{"layers", {{{"name", "grass"}}, {{"name", "lava"}, {"when", {{"heat", {0.5, 1}}}}}}}});
    const terrain::RulesResult missing = terrain::applyMaterialRules(bare, odd);
    CHECK(missing.missing == std::vector<std::string>{"heat"});
    CHECK(missing.layers[1].coverage == 0.0);
    CHECK(missing.layers[0].coverage == doctest::Approx(1.0));

    CHECK_THROWS(terrain::MaterialRules::fromJson(json{{"layers", {{{"name", "a b"}}}}}));
    CHECK_THROWS(terrain::MaterialRules::fromJson(json{{"layers", {{{"name", "a"}}, {{"name", "a"}}}}}));
}

TEST_CASE("materials: a term's blur softens a class mask so the layer's noise breaks its edge off the grid")
{
    // A class mask stepping from 1 to 0 down the middle of the grid, as a land cover import writes one.
    geo::Heightfield h = geo::makeHeightfield(129, 129, 1000.0, {0.0, 0.0});
    std::vector<float>& cls = h.mask("landcover_forest");
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            cls[h.index(x, z)] = x < 64 ? 1.0f : 0.0f;
    auto edges = [&](const json& term) {
        terrain::MaterialRules r = terrain::MaterialRules::fromJson(
            json{{"layers", {{{"name", "grass"}}, {{"name", "forest"}, {"when", {{"landcover_forest", term}}}, {"noise", {{"amount", 0.6}, {"scale", 4000.0}}}}}}});
        terrain::applyMaterialRules(h, r);
        const std::vector<float>& f = *h.findMask("layer_forest");
        std::set<int> at;
        for (int z = 0; z < h.nz; ++z)
        {
            int x = 0;
            while (x + 1 < h.nx && !(f[h.index(x, z)] >= 0.5f && f[h.index(x + 1, z)] < 0.5f))
                ++x;
            at.insert(x);
        }
        return at;
    };
    const std::set<int> hard = edges({{"range", {0.5, nullptr}}, {"fade", 0.5}});
    const std::set<int> soft = edges({{"range", {0.5, nullptr}}, {"fade", 0.5}, {"blur", 6000.0}});
    MESSAGE("edge columns: hard " << hard.size() << ", blurred " << soft.size());
    CHECK(hard.size() == 1);
    CHECK(soft.size() >= 5);
    CHECK(terrain::MaterialRules::fromJson(json{{"layers", {{{"name", "a"}}, {{"name", "b"}, {"when", {{"x", {{"range", {0, 1}}, {"blur", 500.0}}}}}}}}})
              .toJson()["layers"][1]["when"]["x"]["blur"] == 500.0);
}

TEST_CASE("materials: default layers on a low coast with a deep seabed: sand, grass, forest and rock, no snow")
{
    // 20 km across X: seabed down to 3 km under the sea, a coastal plain to 20 m, then hills to 800 m ribbed along Z.
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {2000000.0, 2000000.0}, 257);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            const double t = p.x / 2000000.0 + 0.5;
            double y = 0.0;
            if (t < 0.3)
                y = -300000.0 * (0.3 - t) / 0.3;
            else if (t < 0.55)
                y = 2000.0 * (t - 0.3) / 0.25;
            else
                y = 2000.0 + (t - 0.55) / 0.45 * (40000.0 + 40000.0 * std::sin(p.y * 2.0 * math::kPi / 400000.0));
            h.heights[h.index(x, z)] = float(y);
        }
    const terrain::RulesResult r = terrain::applyMaterialRules(h, terrain::defaultMaterialRules(h, 0.0));
    CHECK(r.maxSumError < 1e-5);
    terrain::SurfaceFields fields(h);
    const std::vector<float>& slope = *fields.get("slope");
    double snowMax = 0.0, sand = 0.0, grass = 0.0, forest = 0.0;
    size_t shore = 0, plain = 0, hills = 0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        const float y = h.heights[k];
        snowMax = std::max(snowMax, double((*h.findMask("layer_snow"))[k]));
        if (y > 0.0f && y < 200.0f && slope[k] < 5.0f)
        {
            sand += (*h.findMask("layer_sand"))[k];
            ++shore;
        }
        else if (y > 800.0f && y < 1900.0f && slope[k] < 5.0f)
        {
            grass += (*h.findMask("layer_grass"))[k];
            ++plain;
        }
        else if (y > 10000.0f && slope[k] > 15.0f && slope[k] < 32.0f)
        {
            forest += (*h.findMask("layer_forest"))[k];
            ++hills;
        }
    }
    REQUIRE(shore > 0);
    REQUIRE(plain > 0);
    REQUIRE(hills > 0);
    CHECK(snowMax < 0.01);
    CHECK(sand / double(shore) > 0.5);
    CHECK(grass / double(plain) > 0.5);
    CHECK(forest / double(hills) > 0.4);

    // The same ground with no sea level keeps the relative snow rule: the top of the range.
    geo::Heightfield dry = h;
    terrain::applyMaterialRules(dry, terrain::defaultMaterialRules(dry));
    CHECK(meanOf(*dry.findMask("layer_snow")) > 0.05);
}

TEST_CASE("biomes: temperature falls with height, the lee is drier, and biome weights sum to 1")
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {400000.0, 100000.0}, 257);
    // A north-south ridge 4 km high in the middle; the wind blows from -X toward +X (azimuth 90).
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const double px = h.samplePos(x, z).x;
            h.heights[h.index(x, z)] = float(400000.0 * std::exp(-(px * px) / (2.0 * 40000.0 * 40000.0)));
        }
    terrain::BiomeSpec spec = terrain::defaultBiomes();
    spec.climate.windAzimuth = 90.0;
    spec.climate.groundWater = 0.0;
    const terrain::RulesResult r = terrain::applyBiomes(h, spec);
    CHECK(r.maxSumError < 1e-5);
    CHECK(maxSum(h, "biome_") < 1e-5);
    const std::vector<float>& t = *h.findMask("temperature");
    const std::vector<float>& m = *h.findMask("moisture");
    const int mid = h.nz / 2;
    const size_t top = h.index(h.nx / 2, mid), west = h.index(h.nx / 8, mid), east = h.index(h.nx - 1 - h.nx / 8, mid);
    CHECK(terrain::maskToTemperature(t[top]) == doctest::Approx(14.0 - 0.65 * 40.0).epsilon(0.05));
    CHECK(terrain::maskToTemperature(t[west]) > 12.0);
    CHECK(m[east] < m[west] - 0.1);
    CHECK((*h.findMask("biome_snow"))[top] > 0.9f);
    CHECK((*h.findMask("biome_snow"))[west] < 0.01f);

    // A painted mask forces its biome.
    geo::Heightfield p = h;
    std::vector<float>& paint = p.mask("paintedWetland");
    std::fill(paint.begin(), paint.end(), 1.0f);
    terrain::BiomeSpec painted = spec;
    for (terrain::BiomeRule& b : painted.biomes)
        if (b.name == "wetland")
            b.paint = "paintedWetland";
    terrain::applyBiomes(p, painted);
    CHECK((*p.findMask("biome_wetland"))[top] == doctest::Approx(1.0f));

    // Layer rules see the biomes and the temperature.
    terrain::MaterialRules rules = terrain::defaultMaterialRules(h);
    CHECK(rules.layers.back().name == "snow");
    terrain::applyMaterialRules(h, rules);
    CHECK((*h.findMask("layer_snow"))[top] > 0.5f);
    CHECK((*h.findMask("layer_snow"))[west] < 0.01f);
}

TEST_CASE("materials: splats hold every sample's weights in bytes summing to 255")
{
    geo::Heightfield h = range(65);
    terrain::applyMaterialRules(h, terrain::defaultMaterialRules(h, 0.0));
    std::vector<std::string> masks;
    for (const auto& [name, v] : h.masks)
        if (name.starts_with("layer_"))
            masks.push_back(name);
    REQUIRE(masks.size() >= 5);
    const auto splats = terrain::packSplats(h, masks);
    CHECK(splats.size() == (masks.size() + 3) / 4);
    size_t badSum = 0, far = 0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        int sum = 0;
        for (size_t i = 0; i < masks.size(); ++i)
        {
            const int b = splats[i / 4][k * 4 + i % 4];
            sum += b;
            far += std::abs(b - (*h.findMask(masks[i]))[k] * 255.0) > 1.0;
        }
        badSum += sum != 255;
    }
    CHECK(badSum == 0);
    CHECK(far == 0);
}

TEST_CASE("ground cover: pieces keep their footprints clear, fit their biome and rules, and scatter the same way every time")
{
    geo::Heightfield h = range(129);
    terrain::applyBiomes(h, terrain::defaultBiomes());
    terrain::applyMaterialRules(h, terrain::defaultMaterialRules(h));
    terrain::CoverSpec spec = terrain::CoverSpec::fromJson(json{
        {"seed", 3},
        {"rules",
         {{{"name", "trees"}, {"items", {"tree"}}, {"density", 0.002}, {"footprint", 400.0}, {"when", {{"biome", "meadow"}, {"slope", {0, 30}}}}},
          {{"name", "rocks"}, {"items", {"rockA", "rockB"}}, {"density", 0.004}, {"when", {{"slope", {10, 45}}}}},
          {{"name", "grass"}, {"items", {"tuft"}}, {"density", 0.01}, {"footprint", 30.0}, {"solid", false}, {"when", {{"layer", "grass"}}}}}}});
    const std::vector<std::vector<double>> radius = {{400.0}, {120.0, 80.0}, {30.0}};
    const terrain::CoverResult r = terrain::scatterCover(h, spec, radius);
    REQUIRE(r.pieces.size() > 100);
    CHECK(r.missing.empty());
    CHECK(r.thinned == 1.0);
    for (const terrain::CoverRuleStats& s : r.rules)
        CHECK(s.placed > 0);

    size_t checked = 0, overlaps = 0, unfit = 0, offBiome = 0, offLayer = 0, offGround = 0;
    for (size_t i = 0; i < r.pieces.size(); ++i)
    {
        const terrain::CoverPiece& a = r.pieces[i];
        unfit += a.fit < 0.05f;
        offGround += std::abs(a.position.y - h.heightAt(a.position.x, a.position.z)) > 1e-6;
        if (a.rule == 0)
            offBiome += h.maskAt("biome_meadow", a.position.x, a.position.z) < a.fit - 1e-5;
        if (a.rule == 2)
            offLayer += h.maskAt("layer_grass", a.position.x, a.position.z) < a.fit - 1e-5;
        // Solid pieces (trees, rocks) never overlap any other piece's footprint; grass only keeps clear of solids and itself.
        for (size_t j = 0; j < i; ++j)
        {
            const terrain::CoverPiece& b = r.pieces[j];
            if (!spec.rules[size_t(b.rule)].solid && b.rule != a.rule)
                continue;
            const double d = glm::length(glm::dvec2(a.position.x - b.position.x, a.position.z - b.position.z));
            overlaps += d < a.radius + b.radius - 1e-6;
            ++checked;
        }
    }
    CHECK(overlaps == 0);
    CHECK(unfit == 0);
    CHECK(offBiome == 0);
    CHECK(offLayer == 0);
    CHECK(offGround == 0);
    CHECK(checked > 0);

    // Same inputs, same pieces; on one thread too.
    const terrain::CoverResult again = terrain::scatterCover(h, spec, radius);
    terrain::CoverResult single;
    tbb::task_arena one(1);
    one.execute([&] { single = terrain::scatterCover(h, spec, radius); });
    REQUIRE(again.pieces.size() == r.pieces.size());
    REQUIRE(single.pieces.size() == r.pieces.size());
    size_t differ = 0;
    for (size_t i = 0; i < r.pieces.size(); ++i)
        differ += again.pieces[i].position != r.pieces[i].position || single.pieces[i].position != r.pieces[i].position ||
                  single.pieces[i].item != r.pieces[i].item;
    CHECK(differ == 0);

    // maxCount thins every rule by one factor and says so.
    terrain::CoverSpec capped = spec;
    capped.maxCount = 50;
    const terrain::CoverResult thin = terrain::scatterCover(h, capped, radius);
    CHECK(thin.thinned < 1.0);
    CHECK(thin.pieces.size() < 120);
}

TEST_CASE("ground cover: a terrain's grid mesh gives back its heightfield, stepped or not")
{
    geo::Heightfield h = range(65);
    terrain::applyMaterialRules(h, terrain::defaultMaterialRules(h));
    const geo::Heightfield back = terrain::heightfieldFromGridMesh(geo::heightfieldToMesh(h));
    REQUIRE(back.nx == h.nx);
    REQUIRE(back.nz == h.nz);
    CHECK(back.cell == doctest::Approx(h.cell));
    CHECK(back.heights == h.heights);
    CHECK(*back.findMask("layer_rock") == *h.findMask("layer_rock"));

    geo::HeightfieldMeshOptions o;
    o.step = 3;
    const geo::Heightfield stepped = terrain::heightfieldFromGridMesh(geo::heightfieldToMesh(h, o));
    REQUIRE(!stepped.empty());
    CHECK(stepped.cell == doctest::Approx(h.cell * 3));
    double worst = 0.0;
    for (int z = 0; z + 1 < stepped.nz; ++z)
        for (int x = 0; x + 1 < stepped.nx; ++x)
        {
            const glm::dvec2 p = stepped.samplePos(x, z);
            worst = std::max(worst, std::abs(stepped.at(x, z) - h.heightAt(p.x, p.y)));
        }
    CHECK(worst < 1e-2);
    CHECK(terrain::heightfieldFromGridMesh(geo::Mesh()).empty());
}

TEST_CASE("terrain commands: biomes, layers and ground cover as nodes, coverage matching the masks, splats in the export")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {200000.0, 200000.0}}, {"resolution", 129}, {"amplitude", 80000.0}, {"ridged", 0.8}, {"erosion", 20}, {"name", "Valley"}});
    const std::string mesh = t["mesh"];

    json b = run("terrainBiomes", {{"args", {mesh}}});
    REQUIRE(b["biomes"].size() == 6);
    CHECK(b["maxSumError"].get<double>() < 1e-5);
    CHECK(b["temperature"]["max"].get<double>() > b["temperature"]["min"].get<double>());

    json r = run("terrainMaterialRules", {{"args", {mesh}}});
    CHECK(r["maxSumError"].get<double>() < 1e-5);
    CHECK(r["missing"].empty());
    json info = run("terrainInfo", {{"args", {mesh}}});
    double total = 0.0;
    for (const json& l : r["layers"])
    {
        CHECK(info["masks"].contains(l["mask"].get<std::string>()));
        CHECK(l["coverage"].get<double>() == doctest::Approx(info["masks"][l["mask"].get<std::string>()]["mean"].get<double>()).epsilon(1e-5));
        total += l["coverage"].get<double>();
    }
    CHECK(total == doctest::Approx(1.0).epsilon(1e-5));
    for (const json& bi : b["biomes"])
        CHECK(bi["coverage"].get<double>() == doctest::Approx(info["masks"][bi["mask"].get<std::string>()]["mean"].get<double>()).epsilon(1e-5));

    // A layer added by name; its coverage comes out of the others' share.
    json moss = {{"name", "moss"}, {"when", {{"moisture", json::array({0.5, nullptr})}, {"slope", json::array({0, 25})}}}};
    json more = run("terrainMaterialRules", {{"args", {mesh}}, {"set", moss}});
    CHECK(more["layers"].back()["name"] == "moss");
    CHECK(more["layers"].back()["coverage"].get<double>() > 0.0);
    CHECK(more["maxSumError"].get<double>() < 1e-5);
    json removed = run("terrainMaterialRules", {{"args", {mesh}}, {"remove", {"moss"}}});
    CHECK(removed["layers"].size() == more["layers"].size() - 1);
    CHECK_FALSE(run("terrainInfo", {{"args", {mesh}}})["masks"].contains("layer_moss"));

    // A stamp added afterwards lands before the surface nodes, which stay last.
    run("terrainStampPad", {{"args", {mesh}}, {"id", "pad"}, {"footprint", {{-5000.0, -5000.0}, {5000.0, -5000.0}, {5000.0, 5000.0}, {-5000.0, 5000.0}}}});
    const json groups = run("gnGroupInfo", {{"args", {t["group"]}}});
    std::string intoOutput, intoLayers, intoBiomes;
    for (const json& l : groups["definition"]["links"])
    {
        if (l[2] == "GroupOutput" && l[3] == "geometry")
            intoOutput = l[0];
        if (l[2] == "Layers")
            intoLayers = l[0];
        if (l[2] == "Biomes")
            intoBiomes = l[0];
    }
    CHECK(intoOutput == "Layers");
    CHECK(intoLayers == "Biomes");
    CHECK(intoBiomes.starts_with("Stamps"));

    const fs::path folder = fs::temp_directory_path() / "lila_terrain_materials_export";
    fs::remove_all(folder);
    json files = run("terrainExport", {{"args", {mesh}}, {"target", "files"}, {"folder", folder.string()}});
    REQUIRE(files["files"].contains("splats"));
    CHECK(files["files"]["splats"].size() == (removed["layers"].size() + 3) / 4);
    for (const json& s : files["files"]["splats"])
        CHECK(fs::exists(s["file"].get<std::string>()));
    json unreal = run("terrainExport", {{"args", {mesh}}, {"target", "unreal"}, {"folder", folder.string()}});
    CHECK(unreal["layers"].size() >= removed["layers"].size());

    json img = run("terrainMaskImages", {{"args", {mesh}}, {"masks", {"layer_rock"}}, {"folder", folder.string()}});
    CHECK(fs::exists(img["files"]["layer_rock"].get<std::string>()));
    json sat = run("terrainSatmap", {{"args", {mesh}}, {"file", (folder / "sat.png").string()}, {"shade", 0.5}});
    CHECK(fs::exists(sat["file"].get<std::string>()));
    CHECK(sat["layers"].size() == removed["layers"].size());

    json cover = run("terrainGroundCover", {{"args", {mesh}}, {"density", 4.0}});
    CHECK(cover["placeholders"].size() == 4);
    CHECK(cover["mesh"].get<std::string>().find("Valley_GroundCover") != std::string::npos);
    REQUIRE(cover["rules"].is_array());
    size_t count = 0;
    for (const json& rule : cover["rules"])
        count += rule["placed"].get<size_t>();
    CHECK(count > 0);
    CHECK(cover["count"].get<size_t>() == count);
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* mod = g.find(cover["modifier"].get<std::string>());
    REQUIRE(mod);
    gn::GeometryPtr geom = gn::modifierGeometry(mod->id());
    REQUIRE(geom);
    REQUIRE(geom->instances);
    CHECK(geom->instances->size() == count);
    // Every piece is a slot keyed by its rule's grid cell, so the same seed gives the same ids.
    const json pieces = run("slotFind", {{"tag", {"groundcover"}}, {"limit", 0}});
    CHECK(pieces["count"] == count);
    REQUIRE(pieces["slots"].size() > 0);
    CHECK(pieces["slots"][0]["id"].get<std::string>().find("/rule") != std::string::npos);

    // Again with the same seed: same count; another seed moves them.
    json same = run("terrainGroundCover", {{"args", {mesh}}, {"density", 4.0}});
    CHECK(same["count"] == cover["count"]);
    CHECK(same["placeholders"].size() == 4);
    g.evaluate();
    CHECK(run("slotFind", {{"tag", {"groundcover"}}, {"limit", 0}}) == pieces);
    run("newScene");
}
