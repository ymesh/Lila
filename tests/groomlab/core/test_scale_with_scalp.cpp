// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Scaling a groom with its scalp (GroomEvaluator::setScaleSource, groomScale): a scalp k times as
// big (its area k^2 times the reference) makes the groom at 1x scaled by k, whatever the stored
// params, guides and keyed controls say: as many strands, their points k times as far from the
// origin the scalp was scaled about. Measured on the rest pose, so a deforming scalp keeps one k;
// a re-meshed scalp of the same surface keeps k at 1.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groom/GroomEvaluator.h"
#include "groomlab/presets.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

using json = nlohmann::json;

constexpr double kFps = 24.0;

std::vector<Vec3f> scaled(std::vector<Vec3f> points, float k) {
    for (Vec3f& p : points) p = p * k;
    return points;
}

// The mesh k times as big about the origin (its rest pose too).
MeshPtr scaledMesh(const MeshPtr& mesh, float k) {
    auto out = std::make_shared<ScalpMesh>(scaled(mesh->points(), k), mesh->triangles());
    if (mesh->hasUVs()) out->setCornerUVs(mesh->cornerUVs());
    if (mesh->hasRest()) out->setRestPoints(scaled(mesh->restPoints(), k));
    return out;
}

// Every triangle split in four at its edges' midpoints: another mesh of the same surface.
MeshPtr splitMesh(const MeshPtr& mesh) {
    std::vector<Vec3f> points = mesh->points();
    std::vector<ScalpMesh::Triangle> triangles;
    std::map<std::pair<int, int>, int> mids;
    auto mid = [&](int a, int b) {
        const auto key = std::make_pair(std::min(a, b), std::max(a, b));
        const auto it = mids.find(key);
        if (it != mids.end()) return it->second;
        points.push_back((points[static_cast<std::size_t>(a)] + points[static_cast<std::size_t>(b)]) * 0.5f);
        return mids[key] = static_cast<int>(points.size() - 1);
    };
    for (const auto& t : mesh->triangles()) {
        const int a = static_cast<int>(t[0]), b = static_cast<int>(t[1]), c = static_cast<int>(t[2]);
        const int ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
        triangles.push_back({a, ab, ca});
        triangles.push_back({ab, b, bc});
        triangles.push_back({ca, bc, c});
        triangles.push_back({ab, bc, ca});
    }
    return std::make_shared<ScalpMesh>(points, triangles);
}

// The largest distance of b's points from a's scaled by k, as a share of the groom's size at k;
// the strands' IDs must be the same.
double difference(const StrandSet& a, const StrandSet& b, float k, double size) {
    EXPECT_EQ(a.strandCount(), b.strandCount());
    if (a.ids() != b.ids()) {
        ADD_FAILURE() << "other strands at x" << k;
        return 1.0;
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < a.strandCount(); ++i) {
        if (a.strandPointCount(i) != b.strandPointCount(i)) return 1.0;
        const Vec3f* p = a.strandPoints(i);
        const Vec3f* q = b.strandPoints(i);
        for (std::size_t j = 0; j < a.strandPointCount(i); ++j) worst = std::max(worst, double((q[j] - p[j] * k).length()));
    }
    if (a.widths().size() == b.widths().size()) {
        for (std::size_t i = 0; i < a.widths().size(); ++i) worst = std::max(worst, double(std::abs(b.widths()[i] - a.widths()[i] * k)));
    }
    return worst / (size * k);
}

GroomData strokesGroom(const std::string& graph = {}) {
    GroomData data = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom").data;
    if (!graph.empty()) data.graphJson = graph;
    return data;
}

std::string presetText(const std::string& name) {
    std::ifstream in(std::filesystem::u8path(std::string(GROOMLAB_SHIPPED_PRESETS) + "/" + name + ".json"), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return presetGraphJson(text.str(), *presetScalpSize(*strokesGroom().scalp));
}

}  // namespace

TEST(ScaleWithScalp, TheUnitTableIsThePresetsOne) {
    EXPECT_DOUBLE_EQ(*unitScaleFactor("length", 2.0), 2.0);
    EXPECT_DOUBLE_EQ(*unitScaleFactor("width", 2.0), 2.0);
    EXPECT_DOUBLE_EQ(*unitScaleFactor("density", 2.0), 0.25);
    EXPECT_DOUBLE_EQ(*unitScaleFactor("perLength", 2.0), 0.5);
    for (const char* unit : {"", "angle", "turns", "fraction", "seconds", "pixels", "color"}) EXPECT_FALSE(unitScaleFactor(unit, 2.0)) << unit;
    // Every unit the operators' params say is one of those (a new one needs a row in the table).
    const std::set<std::string> known = {"", "length", "width", "density", "perLength", "angle", "turns", "fraction", "seconds", "pixels", "color"};
    for (const json& op : json::parse(operatorSpecsJson())) {
        for (const json& p : op.value("params", json::array())) {
            EXPECT_TRUE(known.count(p.value("unit", std::string()))) << op.value("type", std::string()) << "." << p.value("name", std::string())
                                                                     << ": unit " << p.value("unit", std::string());
        }
    }
    // A graph scaled by 2 is a preset fitted to a scalp twice its size; 1 leaves the text.
    const std::string graph = presetText("everyday_sim");
    EXPECT_EQ(scaleGraphJson(graph, 1.0), graph);
    json withSize = json::parse(graph);
    withSize["size"] = 1.0;
    EXPECT_EQ(json::parse(scaleGraphJson(graph, 2.0)), json::parse(fitPresetJson(withSize.dump(), 2.0)));
}

TEST(ScaleWithScalp, AScaledScalpMakesTheGroomScaledAfterwards) {
    for (const std::string& graph : {std::string(), presetText("curly"), presetText("hair_basics")}) {
        const GroomData data = strokesGroom(graph);
        const double size = *presetScalpSize(*data.scalp);
        GroomEvaluator session(data);
        EXPECT_DOUBLE_EQ(session.groomScale(), 1.0);
        EXPECT_GT(session.scaleReference(), 0.0) << "the first area measured is the reference";
        const StrandsPtr base = session.evaluate(0.1f, 0.0);
        ASSERT_TRUE(base && base->strandCount() > 0);
        for (const float k : {0.5f, 2.0f}) {
            session.setScalp(scaledMesh(data.scalp, k));
            EXPECT_NEAR(session.groomScale(), k, 1e-4);
            const StrandsPtr big = session.evaluate(0.1f, 0.0);
            ASSERT_TRUE(big);
            EXPECT_LT(difference(*base, *big, k, size), 1e-3) << "x" << k;
            // The same from a session made at that size with the reference stored (a groom file).
            GroomData stored = data;
            stored.scalp = scaledMesh(data.scalp, k);
            stored.scaleReference = session.scaleReference();
            GroomEvaluator fresh(stored);
            EXPECT_NEAR(fresh.groomScale(), k, 1e-4);
            EXPECT_LT(difference(*base, *fresh.evaluate(0.1f, 0.0), k, size), 1e-3) << "x" << k << " from the file";
        }
        session.setScalp(data.scalp);
        EXPECT_EQ(session.groomScale(), 1.0);
        EXPECT_EQ(session.evaluate(0.1f, 0.0)->positions(), base->positions()) << "back at 1x: as it was";
    }
}

TEST(ScaleWithScalp, TheHairColorIsTheSameAtEveryScale) {
    // HairColor's channels (melanin, its randomness, tips) per strand and point: the same at 2x
    // and back at 1x as at 1x (the color is no size).
    json graph = json::parse(presetText("hair_basics"));
    for (json& n : graph["nodes"]) {
        if (n.value("type", std::string()) == "HairColor") {
            n["params"]["melaninRandom"] = 0.3;
            n["params"]["tipMelanin"] = 0.1;
        }
    }
    const GroomData data = strokesGroom(graph.dump());
    GroomEvaluator session(data);
    const StrandsPtr base = session.evaluate(0.1f, 0.0);
    ASSERT_TRUE(base);
    auto same = [&](const StrandSet& s, const std::string& what) {
        ASSERT_EQ(s.ids(), base->ids()) << what;
        for (const std::string& name : base->strandChannelNames()) {
            ASSERT_TRUE(s.strandChannel(name)) << what << ": " << name;
            const auto& a = *base->strandChannel(name);
            const auto& b = *s.strandChannel(name);
            ASSERT_EQ(a.size(), b.size()) << what << ": " << name;
            std::size_t differ = 0;
            for (std::size_t i = 0; i < a.size(); ++i) differ += std::abs(a[i] - b[i]) > 1e-4f * std::max(1.0f, std::abs(a[i])) ? 1 : 0;
            EXPECT_EQ(differ, 0u) << what << ": strand channel " << name;
        }
        for (const std::string& name : base->pointChannelNames()) {
            ASSERT_TRUE(s.pointChannel(name)) << what << ": " << name;
            const auto& a = *base->pointChannel(name);
            const auto& b = *s.pointChannel(name);
            ASSERT_EQ(a.size(), b.size()) << what << ": " << name;
            std::size_t differ = 0;
            for (std::size_t i = 0; i < a.size(); ++i) differ += std::abs(a[i] - b[i]) > 1e-4f * std::max(1.0f, std::abs(a[i])) ? 1 : 0;
            EXPECT_EQ(differ, 0u) << what << ": point channel " << name;
        }
    };
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    same(*session.evaluate(0.1f, 0.0), "at 2x");
    session.setScalp(data.scalp);
    same(*session.evaluate(0.1f, 0.0), "back at 1x");
}

TEST(ScaleWithScalp, OffKeepsTheSizesInSceneUnits) {
    const GroomData data = strokesGroom();
    GroomEvaluator session(data);
    const StrandsPtr base = session.evaluate(0.1f, 0.0);
    session.setScaleWithScalp(false);
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    EXPECT_EQ(session.groomScale(), 1.0);
    const StrandsPtr big = session.evaluate(0.1f, 0.0);
    EXPECT_GT(big->strandCount(), base->strandCount() * 3) << "the same density on four times the skin";
    session.setScaleWithScalp(true);
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4);
    EXPECT_EQ(session.evaluate(0.1f, 0.0)->strandCount(), base->strandCount());
}

TEST(ScaleWithScalp, SetScaleReferenceMakesTheSizeNow1x) {
    const GroomData data = strokesGroom();
    GroomEvaluator session(data);
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4);
    session.setScaleReference(session.scaleArea());
    EXPECT_EQ(session.groomScale(), 1.0);
    EXPECT_DOUBLE_EQ(session.scaleReference(), session.scaleArea());
}

TEST(ScaleWithScalp, ARemeshedScalpOfTheSameSurfaceKeeps1x) {
    const GroomData data = strokesGroom();
    GroomEvaluator session(data);
    session.setScalp(splitMesh(data.scalp));
    EXPECT_EQ(session.groomScale(), 1.0);
}

TEST(ScaleWithScalp, ADeformingScalpKeepsOneScaleOnItsRestPose) {
    GroomData data = strokesGroom();
    auto rested = std::make_shared<ScalpMesh>(*data.scalp);
    rested->setRestPoints(data.scalp->points());
    data.scalp = rested;
    GroomEvaluator session(data);
    for (const float squash : {0.5f, 1.5f, 0.8f}) {
        std::vector<Vec3f> points = data.scalp->points();
        for (Vec3f& p : points) p = {p.x / std::sqrt(squash), p.y * squash, p.z / std::sqrt(squash)};
        session.updateScalpPoints(points);
        EXPECT_EQ(session.groomScale(), 1.0) << "squash " << squash;
    }
    // Its rest pose scaled (a rig scaled at its root, measured by the host on the bound mesh): k.
    session.setScalp(scaledMesh(rested, 2.0f));
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4);
    // A rest pose that does not take the scalp's scale (a host's), sized to the area measured.
    const MeshPtr sized = withRestArea(rested, 9.0 * restSurfaceArea(*rested));
    EXPECT_EQ(sized->points(), rested->points());
    EXPECT_NEAR(restSurfaceArea(*sized), 9.0 * restSurfaceArea(*rested), 1e-3 * restSurfaceArea(*rested));
    session.setScalp(sized);
    EXPECT_NEAR(session.groomScale(), 3.0, 1e-4);
    EXPECT_EQ(withRestArea(sized, restSurfaceArea(*sized)), sized) << "the same size: the same mesh";
}

TEST(ScaleWithScalp, APoseStretchingPartOfTheScalpKeepsItsPoseFreeArea) {
    // poseFreeArea: a uniform scale comes through (a rig scaled at its root), a pose that stretches
    // some triangles (a shoulder raised) does not; no rest pose: the plain area.
    GroomData data = strokesGroom();
    auto rested = std::make_shared<ScalpMesh>(*data.scalp);
    rested->setRestPoints(data.scalp->points());
    const double rest = restSurfaceArea(*rested);
    std::vector<Vec3f> doubled = rested->points();
    for (Vec3f& p : doubled) p = p * 2.0f;
    EXPECT_NEAR(poseFreeArea(*rested, doubled), 4.0 * rest, 1e-4 * rest);
    std::vector<Vec3f> posed = rested->points();
    float top = posed.front().y;
    for (const Vec3f& p : posed) top = std::max(top, p.y);
    for (Vec3f& p : posed) {
        if (p.y > top - 0.15f * top) p = {p.x * 1.6f, p.y, p.z * 1.6f};  // the crown stretched
    }
    EXPECT_GT(std::fabs(surfaceArea(posed, rested->triangles()) / rest - 1.0), 0.02) << "the pose stretches";
    EXPECT_NEAR(poseFreeArea(*rested, posed), rest, 1e-3 * rest);
    EXPECT_NEAR(poseFreeArea(*data.scalp, doubled), surfaceArea(doubled, data.scalp->triangles()), 1e-6);
}

TEST(ScaleWithScalp, KeyedLengthsScaleWithTheirParams) {
    // A keyed param of the curly preset's Curl that measures a length or a per-length rate, keyed to its
    // own value: the groom is the same keyed or not, at 1x and at 2x.
    const GroomData data = strokesGroom(presetText("curly"));
    json graph = json::parse(data.graphJson);
    std::map<std::string, json> specs;
    for (const json& op : json::parse(operatorSpecsJson())) specs[op.value("type", std::string())] = op;
    std::string control;
    float value = 0.0f;
    for (json& n : graph["nodes"]) {
        const auto spec = specs.find(n.value("type", std::string()));
        if (!control.empty() || spec == specs.end() || !n.contains("params") || n.value("type", std::string()) != "Curl") continue;
        for (const json& p : spec->second["params"]) {
            const std::string unit = p.value("unit", std::string()), name = p.value("name", std::string());
            if ((unit != "length" && unit != "perLength") || !p.value("animatable", false) || !n["params"].contains(name) ||
                !n["params"][name].is_number()) {
                continue;
            }
            control = "ctl_" + name;
            value = n["params"][name].get<float>();
            n["paramControls"] = {{name, control}};
            break;
        }
    }
    ASSERT_FALSE(control.empty()) << "curly keys no length";
    EXPECT_EQ(controlScaleFactors(graph.dump(), 2.0).count(control), 1u);
    EXPECT_TRUE(controlScaleFactors(graph.dump(), 1.0).empty());
    GroomData keyed = data;
    keyed.graphJson = graph.dump();
    keyed.controls = {{control, value}};
    for (const float k : {1.0f, 2.0f}) {
        GroomData a = data, b = keyed;
        a.scalp = b.scalp = scaledMesh(data.scalp, k);
        a.scaleReference = b.scaleReference = data.scalp->restTotalArea();
        GroomEvaluator plain(a), withKey(b);
        EXPECT_EQ(plain.evaluate(0.1f, 0.0)->positions(), withKey.evaluate(0.1f, 0.0)->positions()) << control << " at x" << k;
        GroomData other = b;
        other.controls = {{control, value * 1.5f}};
        EXPECT_NE(GroomEvaluator(other).evaluate(0.1f, 0.0)->positions(), withKey.evaluate(0.1f, 0.0)->positions()) << control << " keyed shows";
    }
}

TEST(ScaleWithScalp, TheGuidesScaleAboutTheirRootsAndAreStoredAt1x) {
    GroomData data = strokesGroom();
    GroomEvaluator session(data);
    const HierarchyPtr at1 = session.hierarchy();
    ASSERT_TRUE(at1);
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    const HierarchyPtr at2 = session.hierarchy();
    ASSERT_TRUE(at2);
    EXPECT_FLOAT_EQ(at2->scale(), 2.0f);
    ASSERT_EQ(at1->guides().size(), at2->guides().size());
    double worst = 0.0;
    for (std::size_t g = 0; g < at1->guides().size(); ++g) {
        const auto& a = at1->guides()[g].points;
        const auto& b = at2->guides()[g].points;
        ASSERT_EQ(a.size(), b.size());
        for (std::size_t j = 0; j < a.size(); ++j) worst = std::max(worst, double((b[j] - a[j] * 2.0f).length()));
    }
    EXPECT_LT(worst, 1e-3 * *presetScalpSize(*data.scalp)) << "the guides are the 1x ones scaled";
    // What is stored stays at 1x: a copy edited and stored, read back at 1x, is where it was.
    GuideHierarchy edited = *session.authoredHierarchy();
    const std::string stored = edited.toJson();
    GuideHierarchy back = GuideHierarchy::fromJson(stored);
    back.evaluate(data.scalp.get());
    double drift = 0.0;
    for (std::size_t g = 0; g < at1->guides().size(); ++g) {
        const auto& a = session.authoredHierarchy()->guides()[g].points;
        const auto& b = back.guides()[g].points;
        for (std::size_t j = 0; j < a.size(); ++j) drift = std::max(drift, double((a[j] * 0.5f - b[j]).length()));
    }
    EXPECT_LT(drift, 1e-3 * *presetScalpSize(*data.scalp));
}

TEST(ScaleWithScalp, AGroomFileKeepsTheReferencesAndTheSource) {
    for (const ScaleSource source : {ScaleSource::Transform, ScaleSource::Off}) {
        GroomFile file;
        file.data.scalp = test::makeSphere(6, 12);
        file.data.scaleReference = 12.5;
        file.data.scaleTransformReference = 2.5;
        file.data.scaleSource = source;
        const GroomFile back = groomFileFromJson(groomFileToJson(file));
        EXPECT_DOUBLE_EQ(back.data.scaleReference, 12.5);
        EXPECT_DOUBLE_EQ(back.data.scaleTransformReference, 2.5);
        EXPECT_EQ(back.data.scaleSource, source) << scaleSourceName(source);
    }
    GroomFile plain;
    plain.data.scalp = test::makeSphere(6, 12);
    const json text = json::parse(groomFileToJson(plain));
    EXPECT_FALSE(text.contains("scaleReference")) << "written only when set";
    EXPECT_FALSE(text.contains("scaleTransformReference")) << "written only when set";
    EXPECT_FALSE(text.contains("scaleSource")) << "written only when not area";
    const GroomFile plainBack = groomFileFromJson(text.dump());
    EXPECT_EQ(plainBack.data.scaleSource, ScaleSource::Area);
    EXPECT_EQ(plainBack.data.scaleReference, 0.0);
}

TEST(ScaleWithScalp, TransformOnlyFollowsTheTransformAndLeavesRemodels) {
    const GroomData data = strokesGroom();
    GroomEvaluator session(data);
    session.setScaleSource(ScaleSource::Transform);
    session.setScaleTransform(1.0);
    session.setScalp(data.scalp);
    EXPECT_EQ(session.groomScale(), 1.0);
    // A remodel: the area 20% more, the transform as it was: k stays.
    session.setScalp(scaledMesh(data.scalp, static_cast<float>(std::sqrt(1.2))));
    EXPECT_EQ(session.groomScale(), 1.0) << "a remodel leaves Transform Only";
    // The transform scaled 2x (and the area with it): k 2.
    session.setScaleTransform(2.0);
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4);
    // Frozen: the transform back at 1 (the points keep the size): k 1 again (it does not survive a freeze).
    session.setScaleTransform(1.0);
    EXPECT_EQ(session.groomScale(), 1.0);
    // Surface Area for the same scalp: its area's k (survives the freeze).
    session.setScaleSource(ScaleSource::Area);
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4);
    // Set Scale Reference with the transform: Transform Only 1x at the transform now.
    session.setScaleSource(ScaleSource::Transform);
    session.setScaleTransform(3.0);
    session.setScaleTransformReference(3.0);
    EXPECT_EQ(session.groomScale(), 1.0);
}

TEST(ScaleWithScalp, AHostWithoutTransformsKeepsTheScaleTransformOnlyOpenedWith) {
    GroomData data = strokesGroom();
    data.scalp = scaledMesh(data.scalp, 2.0f);
    data.scaleReference = strokesGroom().scalp->restTotalArea();
    data.scaleSource = ScaleSource::Transform;
    GroomEvaluator session(data);
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4) << "the area's scale on the first measure";
    session.setScalp(scaledMesh(data.scalp, 1.5f));
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4) << "then kept";
}

TEST(ScaleWithScalp, TheScalpsOwnAreaChangingIsSaidOnce) {
    const GroomData data = strokesGroom();
    GroomEvaluator session(data);
    session.setScaleTransform(1.0);
    session.setScalp(data.scalp);
    EXPECT_EQ(session.scaleNoticeSerial(), 0u) << "the first measure says nothing";
    // The transform scaled (the area with it): no notice.
    session.setScaleTransform(2.0);
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    EXPECT_NEAR(session.groomScale(), 2.0, 1e-4);
    EXPECT_EQ(session.scaleNoticeSerial(), 0u) << "a scaled transform is no surprise";
    // Frozen (the transform back at 1, the same scalp): nothing changes, nothing said.
    session.setScaleTransform(1.0);
    session.setScalp(scaledMesh(data.scalp, 2.0f));
    EXPECT_EQ(session.scaleNoticeSerial(), 0u);
    // Remodelled 25% bigger in area with the transform standing: said once.
    session.setScalp(scaledMesh(data.scalp, 2.0f * static_cast<float>(std::sqrt(1.25))));
    EXPECT_EQ(session.scaleNoticeSerial(), 1u);
    EXPECT_NE(session.scaleNotice().find("area changed 25%"), std::string::npos) << session.scaleNotice();
    EXPECT_NE(session.scaleNotice().find("x1.12"), std::string::npos) << session.scaleNotice();
    EXPECT_NE(session.scaleNotice().find("Set Scale Reference"), std::string::npos);
    // Set Scale Reference: k 1 again, the area as it was: nothing said.
    session.setScaleReference(session.scaleArea());
    EXPECT_EQ(session.scaleNoticeSerial(), 1u);
    // Transform Only: a remodel says nothing (it leaves the groom).
    session.setScaleSource(ScaleSource::Transform);
    session.setScalp(scaledMesh(data.scalp, 3.0f));
    EXPECT_EQ(session.scaleNoticeSerial(), 1u);
}

TEST(ScaleWithScalp, ASimulatedGroomScaledBehavesAsTheOneAt1xScaled) {
    const GroomData data = strokesGroom(presetText("everyday_sim"));
    const double size = *presetScalpSize(*data.scalp);
    GroomEvaluator at1(data);
    GroomData big = data;
    big.scalp = scaledMesh(data.scalp, 2.0f);
    big.scaleReference = at1.scaleReference();
    GroomEvaluator at2(big);
    ASSERT_NEAR(at2.groomScale(), 2.0, 1e-4);
    std::vector<Vec3f> first;
    for (int frame = 1; frame <= 12; ++frame) {
        GroomEvaluator::Request request;
        request.time = frame / kFps;
        request.density = 0.1f;
        const StrandsPtr a = at1.evaluate(request).strands;
        const StrandsPtr b = at2.evaluate(request).strands;
        ASSERT_TRUE(a && b);
        if (frame == 1 || frame == 12) EXPECT_LT(difference(*a, *b, 2.0f, size), 2e-3) << "frame " << frame;
        if (frame == 1) first = a->positions();
        if (frame == 12) EXPECT_NE(a->positions(), first) << "the simulation moved nothing";
    }
}

}  // namespace groomlab
