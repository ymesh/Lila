#include <doctest/doctest.h>

#include "Blocks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <numbers>
#include <string>

using namespace tailor;

namespace
{

float signedArea(const std::vector<Vec2>& o)
{
    float a = 0.f;
    for (size_t i = 0; i < o.size(); ++i)
    {
        const Vec2& p = o[i];
        const Vec2& q = o[(i + 1) % o.size()];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return 0.5f * a;
}

float orient(const Vec2& a, const Vec2& b, const Vec2& c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

bool segmentsCross(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d)
{
    const float d1 = orient(c, d, a), d2 = orient(c, d, b);
    const float d3 = orient(a, b, c), d4 = orient(a, b, d);
    return ((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f)) && d1 != 0.f && d2 != 0.f && d3 != 0.f && d4 != 0.f;
}

bool selfIntersects(const std::vector<Vec2>& o)
{
    const size_t n = o.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue; // adjacent through the closing edge
            if (segmentsCross(o[i], o[(i + 1) % n], o[j], o[(j + 1) % n]))
                return true;
        }
    return false;
}

float runLength(const Garment& g, const SeamSide& s)
{
    const auto& o = g.panels[s.panel].outline;
    const int n = int(o.size());
    float len = 0.f;
    for (int i = s.from; i != s.to; i = (i + 1) % n)
        len += std::hypot(o[(i + 1) % n][0] - o[i][0], o[(i + 1) % n][1] - o[i][1]);
    return len;
}

void checkGarment(const Garment& g, const std::string& name)
{
    CAPTURE(name);
    REQUIRE(!g.panels.empty());
    for (const Panel& p : g.panels)
    {
        CAPTURE(p.name);
        REQUIRE(p.outline.size() >= 3);
        CHECK(signedArea(p.outline) > 0.f);
        CHECK_FALSE(selfIntersects(p.outline));
        CHECK(p.fabric >= 0);
        CHECK(p.fabric < int(g.fabrics.size()));
        for (size_t i = 0; i < p.outline.size(); ++i)
        {
            const Vec2& a = p.outline[i];
            const Vec2& b = p.outline[(i + 1) % p.outline.size()];
            CHECK(std::hypot(b[0] - a[0], b[1] - a[1]) > 1e-4f);
        }
    }
    for (size_t si = 0; si < g.seams.size(); ++si)
    {
        CAPTURE(si);
        const Seam& s = g.seams[si];
        REQUIRE(!s.a.empty());
        REQUIRE(!s.b.empty());
        float la = 0.f, lb = 0.f;
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
            {
                REQUIRE(side.panel >= 0);
                REQUIRE(side.panel < int(g.panels.size()));
                const int n = int(g.panels[side.panel].outline.size());
                REQUIRE(side.from >= 0);
                REQUIRE(side.from < n);
                REQUIRE(side.to >= 0);
                REQUIRE(side.to < n);
                CHECK(side.from != side.to);
                (sides == &s.a ? la : lb) += runLength(g, side);
            }
        CHECK(la > 0.f);
        // Sleeve caps and cuffs ease or gather on purpose; checkPattern judges those.
        bool gathered = false;
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
                for (const char* piece : {"Sleeve", "Cuff", "Ankle Cuff", "Band", "Puff", "Tier", "Flounce"})
                    gathered |= g.panels[side.panel].name.rfind(piece, 0) == 0;
        if (!gathered)
            CHECK(std::abs(la - lb) <= 0.05f * std::max(la, lb));
    }
}

} // namespace

TEST_CASE("blocks: every block with defaults")
{
    REQUIRE(blocks().size() >= 15);
    for (const Block& b : blocks())
    {
        Garment g;
        std::string error;
        REQUIRE(buildBlock(b.id, g, &error));
        checkGarment(g, b.id);
    }
}

TEST_CASE("blocks: parameter variations")
{
    const std::vector<std::pair<std::string, BlockParamValues>> cases = {
        {"tee", {{"sleeveLength", 0.f}}},
        {"tee", {{"slit", 0.15f}, {"hemFlare", 0.3f}, {"ease", 0.5f}}},
        {"tunic", {{"length", 1.5f}, {"sleeveLength", 0.75f}}},
        {"robe", {{"openFront", 0.f}}},
        {"trousers", {{"length", 0.4f}}},
        {"skirt", {{"length", 0.15f}, {"flare", 0.f}}},
        {"cloak", {{"hood", 0.f}, {"closedSides", 1.f}}},
        {"cloak", {{"frontCoverage", 0.f}}},
        {"cape", {{"hood", 1.f}}},
        {"tabard", {{"sideTie", 0.f}}},
        {"poncho", {{"sideTie", 0.2f}, {"width", 0.3f}}},
        {"banner", {{"vertical", 0.f}}},
        {"banner", {{"tail", 0.f}}},
        {"tent", {{"sides", 3.f}, {"door", 0.f}}},
        {"tent", {{"sides", 16.f}}},
        {"belt", {{"fitRadius", 0.3f}, {"width", 0.1f}}},
        {"cloakTunicBelt", {{"hood", 0.f}}},
    };
    for (const auto& [id, values] : cases)
    {
        Garment g;
        REQUIRE(buildBlock(id, g, nullptr, Body{}, values));
        checkGarment(g, id);
    }
}

TEST_CASE("blocks: other bodies")
{
    Body child;
    child.height = 1.2f, child.chest = 0.64f, child.waist = 0.58f, child.hip = 0.66f, child.shoulderWidth = 0.3f;
    child.armLength = 0.42f, child.inseam = 0.52f, child.neck = 0.29f, child.head = 0.52f;
    Body large;
    large.chest = 1.25f, large.waist = 1.2f, large.hip = 1.25f, large.shoulderWidth = 0.52f;
    for (const Body& body : {child, large})
        for (const Block& b : blocks())
        {
            Garment g;
            REQUIRE(buildBlock(b.id, g, nullptr, body));
            checkGarment(g, b.id);
        }
}

TEST_CASE("blocks: registry")
{
    std::string error;
    Garment g;
    CHECK_FALSE(buildBlock("nope", g, &error));
    CHECK(!error.empty());
    const auto names = blockNames();
    CHECK(std::find(names.begin(), names.end(), "cloakTunicBelt") != names.end());
    const Block* tent = findBlock("tent");
    REQUIRE(tent);
    bool sawInt = false;
    for (const BlockParam& p : tent->params)
        sawInt |= p.name == "sides" && p.kind == BlockParam::Kind::Int && p.defaultValue == 6.f;
    CHECK(sawInt);
}

TEST_CASE("blocks: promo outfit layers and buckle")
{
    std::vector<BlockAnchor> anchors;
    const Garment g = cloakTunicBelt(Body{}, {}, &anchors);
    REQUIRE(anchors.size() == 1);
    CHECK(anchors[0].name == "buckle");
    REQUIRE(anchors[0].panel >= 0);
    CHECK(g.panels[anchors[0].panel].name == "Belt");
    CHECK(g.panels[anchors[0].panel].layer == 1);
    CHECK(anchors[0].position[2] > 0.f);
    CHECK(g.fabrics.size() == 3);
    bool hasCloak = false, hasHood = false;
    for (const Panel& p : g.panels)
    {
        hasHood |= p.name.rfind("Hood", 0) == 0 && p.layer == 2;
        hasCloak |= p.name == "Back" && p.layer == 2;
        if (p.name.rfind("Sleeve", 0) == 0)
            CHECK(p.layer == 0);
    }
    CHECK(hasCloak);
    CHECK(hasHood);
    // The belt starts outside the tunic and cinches through fabric shrink.
    const Panel& beltPanel = g.panels[anchors[0].panel];
    CHECK(g.fabrics[beltPanel.fabric].shrinkWarp < 1.f);
}

TEST_CASE("blocks: panels start outside the body")
{
    const Body body;
    for (const char* id : {"tee", "tunic", "robe", "skirt", "cloak", "poncho", "tabard"})
    {
        CAPTURE(id);
        Garment g;
        REQUIRE(buildBlock(id, g));
        for (const Panel& p : g.panels)
        {
            if (p.name.rfind("Sleeve", 0) == 0 || p.name.rfind("Hood", 0) == 0)
                continue;
            CHECK(std::hypot(p.origin[0], p.origin[2]) > body.chestRadius());
        }
    }
}

TEST_CASE("mannequin is a closed body the blocks fit around")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    REQUIRE(tris.size() > 1000);

    std::map<std::pair<int, int>, int> edgeUse;
    double volume = 0.0;
    float top = 0.f;
    for (const auto& t : tris)
    {
        for (int k = 0; k < 3; ++k)
            ++edgeUse[{std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3])}];
        const Vec3 &a = pts[t[0]], &b = pts[t[1]], &c = pts[t[2]];
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    for (const Vec3& p : pts)
        top = std::max(top, p[1]);
    int open = 0;
    int pinched = 0;
    for (const auto& [e, uses] : edgeUse)
    {
        open += uses % 2;
        pinched += uses > 2;
    }
    // Surface nets can pinch two sheets along an edge on diagonal limbs; that is still watertight.
    CHECK(open == 0);
    CHECK(pinched < 20);
    CHECK(volume > 0.04);
    CHECK(volume < 0.12);
    CHECK(top == doctest::Approx(body.height).epsilon(0.03));

    const Sdf sdf = sdfFromMesh(pts, tris, 0.02f, 0.1f);
    CHECK(sdf.sample({0.f, body.chestY(), 0.f}) < -0.05f);
    CHECK(sdf.sample({0.f, body.chestY(), body.chestRadius() + 0.1f}) > 0.f);
}

TEST_CASE("tunic drapes onto the mannequin without passing through")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);

    Garment g;
    REQUIRE(buildBlock("tunic", g));
    for (Panel& p : g.panels)
        p.particleDistance = 0.03f;
    const ClothMesh cm = buildClothMesh(g);
    ClothSolver solver;
    solver.reset(cm, g, SolverSettings{});
    solver.setColliders({&sdf});
    for (int frame = 0; frame < 90; ++frame)
        solver.step();

    int inside = 0, finite = 0;
    float lowest = 10.f;
    for (const Vec3& p : solver.positions())
    {
        finite += std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        inside += sdf.sample(p) < -0.005f;
        lowest = std::min(lowest, p[1]);
    }
    CHECK(finite == int(solver.positions().size()));
    CHECK(inside < int(solver.positions().size()) / 100);
    // Hangs from the shoulders instead of falling off.
    CHECK(lowest > 0.4f);
    float maxGap = 0.f;
    for (const auto& pr : cm.seamPairs)
    {
        const Vec3 &a = solver.positions()[pr[0]], &b = solver.positions()[pr[1]];
        maxGap = std::max(maxGap, std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
    }
    CHECK(maxGap < 0.015f);
}

TEST_CASE("perf: outfit drape" * doctest::skip())
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    Garment g;
    REQUIRE(buildBlock("cloakTunicBelt", g));
    const ClothMesh cm = buildClothMesh(g);
    for (bool self : {false, true})
    {
        SolverSettings s;
        s.selfCollision = self;
        ClothSolver solver;
        solver.reset(cm, g, s);
        solver.setColliders({&sdf});
        const auto t0 = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 20; ++frame)
            solver.step();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(cm.rest3d.size() << " verts, self " << self << ": " << ms / 20.0 << " ms/frame");
    }
}

TEST_CASE("measuring a mannequin gives back its measurements")
{
    for (const Body& want : {Body{}, Body{.height = 1.62f, .chest = 1.1f, .waist = 1.0f, .hip = 1.08f}})
    {
        std::vector<Vec3> pts;
        std::vector<std::array<int, 3>> tris;
        mannequin(want, pts, tris);
        for (Vec3& p : pts)
            p = {p[0] + 2.f, p[1] + 0.5f, p[2] - 1.f};
        Vec3 offset;
        const Body got = measureBody(pts, tris, &offset);
        CAPTURE(want.height);
        CHECK(got.height == doctest::Approx(want.height).epsilon(0.03));
        CHECK(got.chest == doctest::Approx(want.chest).epsilon(0.06));
        CHECK(got.waist == doctest::Approx(want.waist).epsilon(0.06));
        CHECK(got.hip == doctest::Approx(want.hip).epsilon(0.06));
        CHECK(offset[0] == doctest::Approx(2.f).epsilon(0.01));
        CHECK(offset[1] == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(std::abs(offset[2] + 1.f) < 0.03f);
    }
}

TEST_CASE("every sleeve style drafts a cap that fits its armhole and drapes")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    for (int st = 0; st < int(SleeveStyle::Count); ++st)
    {
        const std::string style = sleeveStyleName(SleeveStyle(st));
        CAPTURE(style);
        Garment g;
        REQUIRE(buildBlock("tee", g, nullptr, body, {{"sleeveStyle", float(st)}, {"sleeveLength", 0.55f}}));
        for (const Panel& p : g.panels)
        {
            CHECK(signedArea(p.outline) > 0.f);
            CHECK_FALSE(selfIntersects(p.outline));
        }
        bool sleeves = false;
        for (const Panel& p : g.panels)
            sleeves |= p.name.rfind("Sleeve", 0) == 0;
        CHECK(sleeves == (SleeveStyle(st) != SleeveStyle::Batwing));
        for (Panel& p : g.panels)
            p.particleDistance = 0.035f;
        const ClothMesh cm = buildClothMesh(g);
        ClothSolver solver;
        solver.reset(cm, g, SolverSettings{});
        solver.setColliders({&sdf});
        for (int frame = 0; frame < 80; ++frame)
            solver.step();
        float maxGap = 0.f;
        int worst = -1;
        for (size_t k = 0; k < cm.seamPairs.size(); ++k)
        {
            const Vec3 &a = solver.positions()[cm.seamPairs[k][0]], &b = solver.positions()[cm.seamPairs[k][1]];
            REQUIRE(std::isfinite(a[0]));
            const float d = std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
            if (d > maxGap)
                maxGap = d, worst = cm.seamOfPair[k];
        }
        CAPTURE(worst);
        // Within two cloth particles: closed bar the corners, where the coarse mesh can't meet exactly yet.
        CHECK(maxGap < 0.07f);
    }
}

TEST_CASE("pattern check: sleeves, fit and seams")
{
    const Body body;
    auto count = [](const std::vector<PatternIssue>& issues, PatternIssue::Level level, const std::string& word) {
        return std::count_if(issues.begin(), issues.end(), [&](const PatternIssue& i) { return i.level == level && i.message.find(word) != std::string::npos; });
    };

    Garment tee;
    REQUIRE(buildBlock("tee", tee, nullptr, body));
    const auto clean = checkPattern(tee, &body);
    for (const PatternIssue& i : clean)
    {
        CAPTURE(i.message);
        CHECK(i.level == PatternIssue::Info);
    }

    // A dropped-shoulder top drafts its shoulder past the shoulder point, so its flat cap is fine.
    Garment drop;
    REQUIRE(buildBlock("tee", drop, nullptr, body, {{"sleeveStyle", float(SleeveStyle::DropShoulder)}}));
    CHECK(count(checkPattern(drop, &body), PatternIssue::Warning, "shoulder point") == 0);

    // The same flat cap on a set-in shoulder: what a pattern maker spots at once.
    Garment flat = tee;
    for (Panel& p : flat.panels)
        if (p.name.rfind("Sleeve", 0) == 0)
            for (Vec2& v : p.outline)
                if (v[1] < 0.f)
                    v[1] *= 0.15f;
    const auto flatIssues = checkPattern(flat, &body);
    CHECK(count(flatIssues, PatternIssue::Warning, "shoulder point") == 2);
    CHECK(count(flatIssues, PatternIssue::Error, "shorter than the armhole") == 2);
    for (const PatternIssue& i : flatIssues)
        CHECK_FALSE(i.tip.empty());

    // No ease round the chest on a body bigger than the pattern.
    Garment tight;
    REQUIRE(buildBlock("tee", tight, nullptr, body, {{"ease", 0.f}}));
    Body big = body;
    big.chest = 1.1f;
    CHECK(count(checkPattern(tight, &big), PatternIssue::Error, "smaller round the chest") == 1);

    // A seam whose sides were cut different lengths.
    Garment off = tee;
    off.panels[0].outline[0][1] -= 0.04f;
    CHECK(count(checkPattern(off, &body), PatternIssue::Warning, "differ by") >= 1);
}

TEST_CASE("sleeve templates build and pass the pattern check")
{
    const Body body;
    for (const char* id : {"dropShoulderTop", "shirt", "peasantBlouse", "puffBlouse", "poetShirt", "bellSleeveDress", "flutterTop",
                           "capSleeveTop", "batwingTop", "legOfMuttonBlouse", "raglanTee", "marieBlouse", "flounceSleeveTop",
                           "slitSleeveTop", "tieredSleeveBlouse", "petalSleeveTop", "capeSleeveTop"})
    {
        CAPTURE(std::string(id));
        Garment g;
        REQUIRE(buildBlock(id, g, nullptr, body));
        CHECK(buildClothMesh(g).tris.size() > 100);
        for (const PatternIssue& i : checkPattern(g, &body))
        {
            CAPTURE(i.message);
            CHECK(i.level != PatternIssue::Error);
        }
    }
}

namespace
{
Vec2 chainPoint(const Garment& g, const std::vector<SeamSide>& chain, float f)
{
    float total = 0.f;
    for (const SeamSide& s : chain)
        total += runLength(g, s);
    float want = std::clamp(f, 0.f, 1.f) * total;
    for (const SeamSide& s : chain)
    {
        const auto& o = g.panels[s.panel].outline;
        const int n = int(o.size());
        for (int i = s.from; i != s.to; i = (i + 1) % n)
        {
            const Vec2 &p = o[i], &q = o[(i + 1) % n];
            const float l = std::hypot(q[0] - p[0], q[1] - p[1]);
            if (want <= l && l > 0.f)
                return {p[0] + (q[0] - p[0]) * want / l, p[1] + (q[1] - p[1]) * want / l};
            want -= l;
        }
    }
    return g.panels[chain.back().panel].outline[chain.back().to];
}

int chainPanel(const Garment& g, const std::vector<SeamSide>& chain, float f)
{
    float total = 0.f;
    for (const SeamSide& s : chain)
        total += runLength(g, s);
    float want = std::clamp(f, 0.f, 1.f) * total;
    for (const SeamSide& s : chain)
    {
        if (want <= runLength(g, s))
            return s.panel;
        want -= runLength(g, s);
    }
    return chain.back().panel;
}

/** Mean placed distance between a seam's sides, walked as the seam says (or the other way round). */
float seamGap(const Garment& g, const Seam& s, bool flip)
{
    const bool rev = s.reversed != flip;
    float sum = 0.f;
    const int n = 24;
    for (int k = 0; k <= n; ++k)
    {
        const float f = float(k) / float(n);
        const Vec3 a = placePatternPoint(g.panels[chainPanel(g, s.a, f)], chainPoint(g, s.a, f));
        const float fb = rev ? 1.f - f : f;
        const Vec3 b = placePatternPoint(g.panels[chainPanel(g, s.b, fb)], chainPoint(g, s.b, fb));
        sum += std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    }
    return sum / float(n + 1);
}
} // namespace

TEST_CASE("onesie: one piece, every seam walks its sides the same way")
{
    const std::vector<BlockParamValues> variants = {
        {},
        {{"hood", 0.f}},
        {{"feet", 0.f}},
        {{"sleeveStyle", float(SleeveStyle::Raglan)}},
        {{"sleeveStyle", float(SleeveStyle::SetIn)}, {"ease", 0.1f}},
        {{"tail", 1.f}},
    };
    Body child;
    child.height = 1.2f, child.chest = 0.64f, child.waist = 0.58f, child.hip = 0.66f, child.shoulderWidth = 0.3f;
    child.armLength = 0.42f, child.inseam = 0.52f, child.neck = 0.29f, child.head = 0.52f;
    for (const Body& body : {Body{}, child})
        for (const BlockParamValues& v : variants)
        {
            Garment g;
            REQUIRE(buildBlock("onesie", g, nullptr, body, v));
            checkGarment(g, "onesie");
            REQUIRE(g.fabrics.size() == (v.count("tail") ? 2u : 1u));
            CHECK(g.fabrics[0].name == "Plush");
            const bool hood = !v.count("hood"), feet = !v.count("feet");
            int hoods = 0, feetPanels = 0;
            for (const Panel& p : g.panels)
            {
                hoods += p.name.rfind("Hood", 0) == 0;
                feetPanels += p.name.rfind("Foot", 0) == 0;
            }
            CHECK(hoods == (hood ? 2 : 0));
            CHECK(feetPanels == (feet ? 4 : 0));
            // Seam 0 is the centre front, one run each side, for the zip.
            CHECK(g.seams[0].a.size() == 1);
            CHECK(g.seams[0].b.size() == 1);
            for (size_t si = 0; si < g.seams.size(); ++si)
            {
                CAPTURE(si);
                CHECK(seamGap(g, g.seams[si], false) < seamGap(g, g.seams[si], true));
            }
            CHECK(buildClothMesh(g).tris.size() > 500);
            // A tail's mouth starts right on the back it is tacked to.
            CHECK(g.tacks.empty() == !v.count("tail"));
            for (const Tack& t : g.tacks)
            {
                const Vec3 a = placePatternPoint(g.panels[t.panelA], t.atA), b = placePatternPoint(g.panels[t.panelB], t.atB);
                CHECK(std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) < 0.12f);
            }
        }
}

TEST_CASE("onesie tail drapes out of the back")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    Garment g;
    REQUIRE(buildBlock("onesie", g, nullptr, body, {{"tail", 1.f}, {"feet", 0.f}}));
    for (Panel& p : g.panels)
        p.particleDistance = std::max(p.particleDistance, 0.025f);
    const ClothMesh cm = buildClothMesh(g);
    ClothSolver solver;
    SolverSettings s;
    s.floorY = 0.f;
    solver.reset(cm, g, s);
    solver.setColliders({&sdf});
    for (int frame = 0; frame < 200; ++frame)
        solver.step();
    float tailBack = 0.f, tailGap = 0.f, tailLow = 10.f;
    for (size_t v = 0; v < solver.positions().size(); ++v)
        if (g.panels[cm.vertPanel[v]].name.rfind("Tail", 0) == 0)
            tailBack = std::min(tailBack, solver.positions()[v][2]), tailLow = std::min(tailLow, solver.positions()[v][1]);
    for (size_t i = 0; i < cm.seamPairs.size(); ++i)
    {
        const Vec3 &a = solver.positions()[cm.seamPairs[i][0]], &b = solver.positions()[cm.seamPairs[i][1]];
        if (g.panels[cm.vertPanel[cm.seamPairs[i][0]]].name.rfind("Tail", 0) == 0)
            tailGap = std::max(tailGap, std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
    }
    MESSAGE("tail reaches z " << tailBack << ", low " << tailLow << ", seam gap " << tailGap);
    for (const Vec3& q : solver.positions())
        REQUIRE(std::isfinite(q[0] + q[1] + q[2]));
    // Out from the back, drooping rather than hanging.
    CHECK(tailBack < -0.45f);
    CHECK(tailLow > 0.5f);
    CHECK(tailGap < 0.02f);
}

namespace
{
int countPanels(const Garment& g, const std::string& prefix, const std::string& contains = {})
{
    int n = 0;
    for (const Panel& p : g.panels)
        n += p.name.rfind(prefix, 0) == 0 && p.name.find(contains) != std::string::npos;
    return n;
}

/** Drapes g on the default mannequin; returns the solver for the caller to inspect. */
struct Draped
{
    ClothMesh cm;
    ClothSolver solver;
    float maxSeamGap = 0.f;
    int inside = 0, finite = 0;
};

void drapeOn(Garment& g, const Body& body, int frames, Draped& out)
{
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    for (Panel& p : g.panels)
        p.particleDistance = std::max(p.particleDistance, 0.02f);
    out.cm = buildClothMesh(g);
    SolverSettings s;
    s.floorY = 0.f;
    out.solver.reset(out.cm, g, s);
    out.solver.setColliders({&sdf});
    for (int frame = 0; frame < frames; ++frame)
        out.solver.step();
    for (const Vec3& p : out.solver.positions())
    {
        out.finite += std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        out.inside += sdf.sample(p) < -0.005f;
    }
    for (const auto& pr : out.cm.seamPairs)
    {
        const Vec3 &a = out.solver.positions()[pr[0]], &b = out.solver.positions()[pr[1]];
        out.maxSeamGap = std::max(out.maxSeamGap, std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
    }
}
} // namespace

TEST_CASE("suits and overalls: pieces, fabric, seams walk their sides the same way")
{
    struct Case
    {
        const char* id;
        BlockParamValues values;
        const char* fabric;
    };
    const std::vector<Case> cases = {
        {"jumpsuit", {}, "Spandex"},
        {"jumpsuit", {{"collar", 0.f}, {"kneePatches", 1.f}, {"elbowPatches", 1.f}, {"ankleCuffs", 1.f}}, "Spandex"},
        {"jumpsuit", {{"sleeveStyle", float(SleeveStyle::Raglan)}, {"collar", 2.f}}, "Spandex"},
        {"flightSuit", {}, "Nylon"},
        {"flightSuit", {{"sleeveLength", 0.2f}, {"elbowPatches", 1.f}}, "Nylon"},
        {"coverall", {}, "Twill"},
        {"overalls", {}, "Denim"},
        {"overalls", {{"bibPocket", 0.f}, {"kneePatches", 1.f}}, "Denim"},
        {"overalls", {{"length", 0.5f}, {"bibWidth", 0.4f}}, "Denim"},
    };
    Body child;
    child.height = 1.2f, child.chest = 0.64f, child.waist = 0.58f, child.hip = 0.66f, child.shoulderWidth = 0.3f;
    child.armLength = 0.42f, child.inseam = 0.52f, child.neck = 0.29f, child.head = 0.52f;
    for (const Body& body : {Body{}, child})
        for (const Case& c : cases)
        {
            CAPTURE(std::string(c.id));
            CAPTURE(c.values.size());
            Garment g;
            std::vector<BlockAnchor> anchors;
            REQUIRE(buildBlock(c.id, g, nullptr, body, c.values, &anchors));
            checkGarment(g, c.id);
            REQUIRE(g.fabrics.size() == 1);
            CHECK(g.fabrics[0].name == c.fabric);
            for (size_t si = 0; si < g.seams.size(); ++si)
            {
                CAPTURE(si);
                CHECK(seamGap(g, g.seams[si], false) < seamGap(g, g.seams[si], true));
            }
            CHECK(buildClothMesh(g).tris.size() > 500);
            auto value = [&](const char* name, float def) { return c.values.count(name) ? c.values.at(name) : def; };
            const Block* blk = findBlock(c.id);
            auto def = [&](const char* name) {
                for (const BlockParam& p : blk->params)
                    if (p.name == name)
                        return p.defaultValue;
                return 0.f;
            };
            if (std::string(c.id) == "overalls")
            {
                CHECK(countPanels(g, "Bib") - countPanels(g, "Bib Pocket") == 1);
                CHECK(countPanels(g, "Back Bib") == 1);
                CHECK(countPanels(g, "Bib Pocket") == (value("bibPocket", 1.f) > 0.5f ? 1 : 0));
                CHECK(countPanels(g, "Front", "Knee") == (value("kneePatches", 0.f) > 0.5f ? 2 : 0));
                REQUIRE(anchors.size() == 2);
                // Buckle L on the wearer's left, both at the bib's top, in front.
                CHECK(anchors[0].position[0] > 0.f);
                CHECK(anchors[1].position[0] < 0.f);
                CHECK(anchors[0].position[2] > 0.f);
                continue;
            }
            // Seam 0 is the centre front, one run each side, for the zip.
            CHECK(g.seams[0].a.size() == 1);
            CHECK(g.seams[0].b.size() == 1);
            CHECK(countPanels(g, "Collar") == (value("collar", def("collar")) > 0.5f ? 1 : 0));
            CHECK(countPanels(g, "Ankle Cuff") == (value("ankleCuffs", def("ankleCuffs")) > 0.5f ? 2 : 0));
            CHECK(countPanels(g, "Front", "Pocket") ==
                  (value("chestPockets", def("chestPockets")) > 0.5f ? 2 : 0) + (value("legPockets", def("legPockets")) > 0.5f ? 2 : 0));
            CHECK(countPanels(g, "Front", "Knee") == (value("kneePatches", def("kneePatches")) > 0.5f ? 2 : 0));
            const bool longSleeves = value("sleeveLength", def("sleeveLength")) > 0.4f;
            CHECK(countPanels(g, "Sleeve", "Elbow") == (value("elbowPatches", def("elbowPatches")) > 0.5f && longSleeves ? 2 : 0));
        }
}

TEST_CASE("jumpsuit drapes close to the body, seams closed")
{
    const Body body;
    Garment g;
    REQUIRE(buildBlock("jumpsuit", g, nullptr, body, {{"kneePatches", 1.f}, {"ankleCuffs", 1.f}}));
    Draped d;
    drapeOn(g, body, 400, d);
    MESSAGE("seam gap " << d.maxSeamGap << ", inside " << d.inside << " of " << d.solver.positions().size());
    CHECK(d.finite == int(d.solver.positions().size()));
    CHECK(d.inside < int(d.solver.positions().size()) / 50);
    CHECK(d.maxSeamGap < 0.02f);
    // Fitted: the torso cloth at chest height stays within a few cm of the body.
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    float far = 0.f;
    int n = 0;
    for (size_t v = 0; v < d.solver.positions().size(); ++v)
    {
        const Vec3& q = d.solver.positions()[v];
        if (g.panels[d.cm.vertPanel[v]].name.rfind("Front", 0) == 0 && std::abs(q[1] - body.chestY()) < 0.03f && std::abs(q[0]) < 0.08f)
            far += sdf.sample(q), ++n;
    }
    REQUIRE(n > 0);
    MESSAGE("chest standoff " << far / float(n));
    CHECK(far / float(n) < 0.03f);
}

TEST_CASE("overalls drape: straps over the shoulders, bib up on the chest")
{
    const Body body;
    Garment g;
    REQUIRE(buildBlock("overalls", g, nullptr, body));
    Draped d;
    drapeOn(g, body, 400, d);
    MESSAGE("seam gap " << d.maxSeamGap << ", inside " << d.inside);
    CHECK(d.finite == int(d.solver.positions().size()));
    CHECK(d.inside < int(d.solver.positions().size()) / 50);
    CHECK(d.maxSeamGap < 0.02f);
    float bibTop = 0.f, strapTop = 0.f;
    for (size_t v = 0; v < d.solver.positions().size(); ++v)
    {
        const std::string& name = g.panels[d.cm.vertPanel[v]].name;
        const Vec3& q = d.solver.positions()[v];
        if (name == "Bib")
            bibTop = std::max(bibTop, q[1]);
        if (name == "Bib" || name == "Back Bib")
            strapTop = std::max(strapTop, q[1]);
    }
    MESSAGE("strap top " << strapTop << ", shoulder " << body.shoulderY());
    // Hung from the shoulders rather than slid down to the hips.
    CHECK(strapTop > body.shoulderY() - 0.04f);
    CHECK(bibTop > body.waistY() + 0.15f);
}

TEST_CASE("booties: two closed feet, open at the top")
{
    Garment g;
    REQUIRE(buildBlock("booties", g));
    checkGarment(g, "booties");
    REQUIRE(g.panels.size() == 4);
    REQUIRE(g.seams.size() == 2);
    for (const Seam& s : g.seams)
        CHECK(seamGap(g, s, false) < seamGap(g, s, true));
    // The left foot's profiles sit either side of x > 0, toes forward.
    CHECK(g.panels[0].origin[0] > 0.f);
    CHECK(g.panels[1].origin[0] > g.panels[0].origin[0]);
    CHECK(g.panels[0].right[2] > 0.9f);
}

TEST_CASE("onesie drapes onto the mannequin, seams closed, feet on the feet")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);

    Garment g;
    REQUIRE(buildBlock("onesie", g));
    for (Panel& p : g.panels)
        p.particleDistance = std::max(p.particleDistance, 0.025f);
    const ClothMesh cm = buildClothMesh(g);
    ClothSolver solver;
    SolverSettings s;
    s.floorY = 0.f;
    solver.reset(cm, g, s);
    solver.setColliders({&sdf});
    for (int frame = 0; frame < 400; ++frame)
        solver.step();

    int inside = 0, finite = 0;
    for (const Vec3& p : solver.positions())
    {
        finite += std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        inside += sdf.sample(p) < -0.005f;
    }
    CHECK(finite == int(solver.positions().size()));
    CHECK(inside < int(solver.positions().size()) / 50);
    float maxGap = 0.f;
    for (const auto& pr : cm.seamPairs)
    {
        const Vec3 &a = solver.positions()[pr[0]], &b = solver.positions()[pr[1]];
        maxGap = std::max(maxGap, std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
    }
    // The four-way junction at each inseam hem closes last.
    CHECK(maxGap < 0.02f);
    // The hood stays up on the head and the feet stay down on the floor.
    float hoodTop = 0.f, footTop = 0.f;
    for (size_t v = 0; v < solver.positions().size(); ++v)
    {
        const std::string& name = g.panels[cm.vertPanel[v]].name;
        if (name.rfind("Hood", 0) == 0)
            hoodTop = std::max(hoodTop, solver.positions()[v][1]);
        if (name.rfind("Foot", 0) == 0)
            footTop = std::max(footTop, solver.positions()[v][1]);
    }
    CHECK(hoodTop > body.neckY() + 0.15f);
    CHECK(footTop < 0.3f);
}

TEST_CASE("a big cartoon head moves the neck, and the blocks follow it")
{
    // A lathed body: a short torso, a thin neck and a head nearly as wide as the body is tall.
    auto radius = [](float y) {
        if (y < 0.6f)
            return 0.15f;
        if (y < 0.68f)
            return 0.08f;
        const float c = 0.9f, r = 0.25f, d = y - c;
        return std::max(std::sqrt(std::max(r * r - d * d, 0.f)), y < c ? 0.08f : 0.f);
    };
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    const int rings = 230, segs = 48;
    const float top = 1.15f;
    pts.push_back({0.f, 0.f, 0.f});
    for (int i = 1; i < rings; ++i)
    {
        const float y = top * float(i) / float(rings);
        for (int k = 0; k < segs; ++k)
        {
            const float a = 2.f * std::numbers::pi_v<float> * float(k) / float(segs);
            pts.push_back({radius(y) * std::cos(a), y, radius(y) * std::sin(a)});
        }
    }
    pts.push_back({0.f, top, 0.f});
    const int last = int(pts.size()) - 1;
    auto at = [&](int ring, int k) { return 1 + (ring - 1) * segs + (k % segs); };
    for (int k = 0; k < segs; ++k)
    {
        tris.push_back({0, at(1, k + 1), at(1, k)});
        for (int i = 1; i + 1 < rings; ++i)
        {
            tris.push_back({at(i, k), at(i, k + 1), at(i + 1, k + 1)});
            tris.push_back({at(i, k), at(i + 1, k + 1), at(i + 1, k)});
        }
        tris.push_back({at(rings - 1, k), at(rings - 1, k + 1), last});
    }

    const Body body = measureBody(pts, tris);
    CHECK(body.height == doctest::Approx(top).epsilon(0.02));
    CHECK(body.neckHeight > 0.58f);
    CHECK(body.neckHeight < 0.72f);
    CHECK(body.shoulderY() < body.neckHeight);
    CHECK(body.head > 1.4f);
    CHECK(body.chest == doctest::Approx(2.f * std::numbers::pi_v<float> * 0.15f).epsilon(0.05));

    // A neck given by hand wins over the one found.
    const Body told = measureBody(pts, tris, nullptr, 0.5f);
    CHECK(told.neckHeight == doctest::Approx(0.5f));
    CHECK(told.shoulderY() < 0.5f);

    // A human-proportioned mannequin keeps the usual landmarks.
    std::vector<Vec3> mp;
    std::vector<std::array<int, 3>> mt;
    mannequin(Body{}, mp, mt);
    CHECK(measureBody(mp, mt).neckHeight == 0.f);

    for (const char* id : {"onesie", "tee", "cloak"})
    {
        CAPTURE(std::string(id));
        Garment g;
        REQUIRE(buildBlock(id, g, nullptr, body));
        checkGarment(g, id);
        for (const Panel& p : g.panels)
        {
            CAPTURE(p.name);
            float hi = -1e30f;
            for (const Vec2& q : p.outline)
                hi = std::max(hi, placePatternPoint(p, q)[1]);
            if (p.name.rfind("Hood", 0) == 0)
                CHECK(hi > top - 0.02f);
            else if (p.name.rfind("Sleeve", 0) != 0)
                CHECK(hi < body.neckHeight + 0.08f);
        }
    }
}
