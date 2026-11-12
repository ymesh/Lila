#include "Nodes2Util.h"


using namespace nodes2test;
using gn::Domain;
using gn::SocketType;

TEST_CASE("spline cyclic and resolution")
{
    auto in = curves({{D3(0), D3(100, 0, 0), D3(100, 0, 100)}, {D3(0, 50, 0), D3(10, 50, 0)}}, {false, true});
    Run r = run({N{"S", "SetSplineCyclic", {}, {{"cyclic", true}}}, N{"Idx", "Index", {}, {}}, N{"Eq", "Compare", {}, {{"dataType", "int"}, {"operation", "equal"}, {"b", 0}}},
                 N{"C", "IsSplineCyclic", {}, {}}},
                {L{"GroupInput", "geometry", "S", "curve"}, L{"Idx", "value", "Eq", "a"}, L{"Eq", "result", "S", "selection"}, L{"S", "curve", "GroupOutput", "geometry"},
                 L{"C", "cyclic", "GroupOutput", "cyclic"}},
                {{"cyclic", SocketType::Bool}}, in);
    REQUIRE(r.geometry->curves);
    const gn::CurveSet& cs = *r.geometry->curves;
    CHECK(cs.cyclic == std::vector<uint8_t>{1, 1});
    CHECK(onCurves<uint8_t>(r.values[0], cs, Domain::Curve) == std::vector<uint8_t>{1, 1});

    // A round Bezier loop: resolution sets the segments per span when it is turned into points.
    curve::Spline sp;
    sp.kind = curve::Kind::Bezier;
    sp.closed = true;
    sp.points = {D3(0), D3(100, 0, 0), D3(100, 0, 100), D3(0, 0, 100)};
    sp.normalize();
    for (size_t i = 0; i < sp.size(); ++i)
        curve::setHandleType(sp, i, curve::HandleType::Auto);
    gn::CurveSet bez;
    bez.addSpline(sp);
    Run res = run({N{"S", "SetSplineResolution", {}, {{"resolution", 5}}}, N{"R", "SplineResolution", {}, {}}},
                  {L{"GroupInput", "geometry", "S", "curve"}, L{"S", "curve", "GroupOutput", "geometry"}, L{"R", "resolution", "GroupOutput", "res"}},
                  {{"res", SocketType::Int}}, gn::curveGeometry(bez));
    const gn::CurveSet& rc = *res.geometry->curves;
    CHECK(onCurves<int64_t>(res.values[0], rc, Domain::Curve) == std::vector<int64_t>{5});
    gn::CurveSet ev = rc.evaluated();
    REQUIRE(ev.numCurves() == 1);
    CHECK(ev.numPoints() == 4 * 5);
}

TEST_CASE("handle positions read and set")
{
    curve::Spline sp;
    sp.kind = curve::Kind::Bezier;
    sp.points = {D3(0), D3(100, 0, 0)};
    sp.normalize();
    gn::CurveSet cs;
    cs.addSpline(sp);
    gn::GeometryPtr in = gn::curveGeometry(cs);
    Run r = run({N{"H", "HandlePositions", {}, {{"relative", true}}}, N{"S", "SetHandlePositions", {}, {{"mode", "right"}, {"offset", {0, 10, 0}}}},
                 N{"H2", "HandlePositions", {}, {}}},
                {L{"GroupInput", "geometry", "S", "curve"}, L{"S", "curve", "GroupOutput", "geometry"}, L{"H", "right", "GroupOutput", "rel"},
                 L{"H2", "right", "GroupOutput", "abs"}},
                {{"rel", SocketType::Vector}, {"abs", SocketType::Vector}}, in);
    std::vector<D3> rel = onCurves<D3>(r.values[0], cs);
    // Vector handles point a third of the way to the neighbour.
    CHECK(near(rel[0], D3(100.0 / 3.0, 0, 0), 1e-4));
    const gn::CurveSet& out = *r.geometry->curves;
    std::vector<D3> before = onCurves<D3>(r.values[1], cs), after = onCurves<D3>(r.values[1], out);
    CHECK(near(after[0], before[0] + D3(0, 10, 0), 1e-4));
    CHECK(out.pointAttrs.get<int32_t>("handle_type_right")[0] == int32_t(curve::HandleType::Free));
}

TEST_CASE("set curve normal writes tilt")
{
    // A line along X: minimum twist gives some normal; yUp turns it square to Y.
    auto in = curves({{D3(0), D3(50, 0, 0), D3(100, 0, 0)}});
    Run r = run({N{"S", "SetCurveNormal", {}, {{"mode", "free"}, {"normal", {0, 0, 1}}}}, N{"Nm", "CurveNormal", {}, {}}},
                {L{"GroupInput", "geometry", "S", "curve"}, L{"S", "curve", "GroupOutput", "geometry"}, L{"Nm", "normal", "GroupOutput", "n"}},
                {{"n", SocketType::Vector}}, in);
    for (const D3& n : onCurves<D3>(r.values[0], *r.geometry->curves))
        CHECK(near(n, D3(0, 0, 1), 1e-6));

    Run y = run({N{"S", "SetCurveNormal", {}, {{"mode", "yUp"}}}, N{"Nm", "CurveNormal", {}, {}}},
                {L{"GroupInput", "geometry", "S", "curve"}, L{"S", "curve", "GroupOutput", "geometry"}, L{"Nm", "normal", "GroupOutput", "n"}},
                {{"n", SocketType::Vector}}, in);
    for (const D3& n : onCurves<D3>(y.values[0], *y.geometry->curves))
        CHECK(std::abs(n.y) < 1e-6);
}

TEST_CASE("offset point in curve")
{
    auto in = curves({{D3(0), D3(1, 0, 0), D3(2, 0, 0)}, {D3(0, 5, 0), D3(1, 5, 0), D3(2, 5, 0)}}, {false, true});
    Run r = run({N{"O", "OffsetPointInCurve", {}, {{"offset", 1}}}},
                {L{"GroupInput", "geometry", "GroupOutput", "geometry"}, L{"O", "isValidOffset", "GroupOutput", "ok"}, L{"O", "pointIndex", "GroupOutput", "i"}},
                {{"ok", SocketType::Bool}, {"i", SocketType::Int}}, in);
    const gn::CurveSet& cs = *in->curves;
    CHECK(onCurves<uint8_t>(r.values[0], cs) == std::vector<uint8_t>{1, 1, 0, 1, 1, 1});
    CHECK(onCurves<int64_t>(r.values[1], cs) == std::vector<int64_t>{1, 2, 2, 4, 5, 3});
}

TEST_CASE("points to curves groups and sorts")
{
    auto in = points({D3(3, 0, 0), D3(0, 0, 0), D3(0, 9, 0), D3(1, 0, 0), D3(0, 8, 0)});
    // Group by y > 5, sorted by x then by y.
    Run r = run({N{"P", "Position", {}, {}}, N{"Sep", "SeparateXYZ", {}, {}}, N{"G", "Compare", {}, {{"operation", "greaterThan"}, {"b", 5.0}}},
                 N{"W", "Math", {}, {{"operation", "add"}}}, N{"C", "PointsToCurves", {}, {}}},
                {L{"P", "value", "Sep", "vector"}, L{"Sep", "y", "G", "a"}, L{"G", "result", "C", "curveGroupId"}, L{"Sep", "x", "W", "a"}, L{"Sep", "y", "W", "b"},
                 L{"W", "value", "C", "weight"}, L{"GroupInput", "geometry", "C", "points"}, L{"C", "curves", "GroupOutput", "geometry"}},
                {}, in);
    REQUIRE(r.geometry->curves);
    const gn::CurveSet& cs = *r.geometry->curves;
    REQUIRE(cs.numCurves() == 2);
    CHECK(cs.curveSize(0) == 3);
    CHECK(near(cs.positions[0], D3(0, 0, 0)));
    CHECK(near(cs.positions[1], D3(1, 0, 0)));
    CHECK(near(cs.positions[2], D3(3, 0, 0)));
    CHECK(near(cs.positions[3], D3(0, 8, 0)));
}

TEST_CASE("curve intersect finds crossings and splits")
{
    // Two lines crossing at (50, 0, 0), one of them 1 cm above on Y.
    auto in = curves({{D3(0, 0, 0), D3(100, 0, 0)}, {D3(50, 1, -50), D3(50, 1, 50)}});
    Run ground = run({N{"X", "CurveIntersect", {}, {{"mode", "ground"}}}},
                     {L{"GroupInput", "geometry", "X", "curves"}, L{"X", "curves", "GroupOutput", "geometry"}, L{"X", "points", "GroupOutput", "pts"}},
                     {{"pts", SocketType::Geometry}}, in);
    REQUIRE(ground.values[0].geometry->points);
    CHECK(ground.values[0].geometry->points->numPoints() == 1);
    CHECK(near(ground.values[0].geometry->points->position(0), D3(50, 0.5, 0)));
    CHECK(ground.geometry->curves->numCurves() == 4);

    // In 3D they miss by 1 cm: a distance of 0.5 finds nothing, 2 finds the crossing.
    auto count = [&](double d) {
        Run r = run({N{"X", "CurveIntersect", {}, {{"distance", d}}}},
                    {L{"GroupInput", "geometry", "X", "curves"}, L{"X", "curves", "GroupOutput", "geometry"}, L{"X", "points", "GroupOutput", "pts"}},
                    {{"pts", SocketType::Geometry}}, in);
        return r.values[0].geometry->points ? r.values[0].geometry->points->numPoints() : 0;
    };
    CHECK(count(0.5) == 0);
    CHECK(count(2.0) == 1);

    // A closed square crossed once by a line through two sides: two cuts open it into two pieces.
    auto loop = curves({{D3(0, 0, 0), D3(10, 0, 0), D3(10, 0, 10), D3(0, 0, 10)}, {D3(-5, 0, 5), D3(15, 0, 5)}}, {true, false});
    Run lr = run({N{"X", "CurveIntersect", {}, {{"mode", "ground"}}}}, {L{"GroupInput", "geometry", "X", "curves"}, L{"X", "curves", "GroupOutput", "geometry"}}, {}, loop);
    CHECK(lr.geometry->curves->numCurves() == 2 + 3);
}

TEST_CASE("curve boolean 2d")
{
    auto square = [](double x0, double z0, double s) {
        return std::vector<D3>{D3(x0, 0, z0), D3(x0 + s, 0, z0), D3(x0 + s, 0, z0 + s), D3(x0, 0, z0 + s)};
    };
    auto a = curves({square(0, 0, 10)}, {true});
    auto b = curves({square(5, 0, 10)}, {true});
    auto area = [](const gn::CurveSet& cs) {
        double total = 0.0;
        for (size_t c = 0; c < cs.numCurves(); ++c)
        {
            auto p = cs.curvePoints(c);
            double s = 0.0;
            for (size_t k = 0; k < p.size(); ++k)
                s += p[k].x * p[(k + 1) % p.size()].z - p[(k + 1) % p.size()].x * p[k].z;
            total += std::abs(s) * 0.5;
        }
        return total;
    };
    auto op = [&](const char* name) {
        gn::ExecEnv env;
        env.params["b"] = gn::Value{.geometry = b};
        gn::Group g = gn::Group::makeDefault("T");
        g.inputs.push_back({.name = "b", .type = SocketType::Geometry});
        g.nodes.push_back(N{"B", "CurveBoolean2D", {}, {{"operation", name}}});
        g.links = {L{"GroupInput", "geometry", "B", "a"}, L{"GroupInput", "b", "B", "b"}, L{"B", "curves", "GroupOutput", "geometry"}};
        gn::PlanPtr plan = gn::compile(g);
        REQUIRE_MESSAGE(plan->error.empty(), plan->error);
        env.input = a;
        gn::GeometryPtr out = gn::execute(*plan, env);
        REQUIRE(out->curves);
        for (const D3& p : out->curves->positions)
            CHECK(p.y == doctest::Approx(0.0));
        return area(*out->curves);
    };
    CHECK(op("union") == doctest::Approx(150.0));
    CHECK(op("intersect") == doctest::Approx(50.0));
    CHECK(op("difference") == doctest::Approx(50.0));
    CHECK(op("xor") == doctest::Approx(100.0));
}

TEST_CASE("sample uv surface")
{
    // On a grid, sampling the position at a corner's uv gives that corner's point; halfway along an edge, the middle.
    auto grid = run({N{"G", "MeshGrid", {}, {}}}, {L{"G", "mesh", "GroupOutput", "geometry"}}).geometry;
    REQUIRE(grid->mesh);
    const geo::Mesh& m = *grid->mesh;
    auto uv = m.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    auto sample = [&](glm::dvec2 at, bool* ok) {
        Run r = run({N{"G", "MeshGrid", {}, {}}, N{"P", "Position", {}, {}}, N{"S", "SampleUVSurface", {}, {{"dataType", "vector"}, {"sampleUv", {at.x, at.y, 0}}}}},
                    {L{"G", "mesh", "S", "mesh"}, L{"P", "value", "S", "value"}, L{"G", "mesh", "GroupOutput", "geometry"}, L{"S", "value", "GroupOutput", "v"},
                     L{"S", "isValid", "GroupOutput", "ok"}},
                    {{"v", SocketType::Vector}, {"ok", SocketType::Bool}});
        *ok = single<uint8_t>(r.values[1]) != 0;
        return single<D3>(r.values[0]);
    };
    bool ok = false;
    for (geo::Index c = 0; c < m.numCorners(); c += 3)
    {
        D3 v = sample(glm::dvec2(uv[c]), &ok);
        CHECK(ok);
        CHECK(near(v, m.position(m.cornerPoint(c)), 1e-3));
    }
    geo::Index c0 = m.faceCorner(0), c1 = m.cornerNext(c0);
    D3 mid = sample(0.5 * (glm::dvec2(uv[c0]) + glm::dvec2(uv[c1])), &ok);
    CHECK(near(mid, 0.5 * (m.position(m.cornerPoint(c0)) + m.position(m.cornerPoint(c1))), 1e-3));
    sample(glm::dvec2(3, 3), &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("deform curves on surface")
{
    // A strand standing on a grid; the surface turns 90 degrees about Z and moves up 10: the strand follows.
    auto strand = curves({{D3(10, 0, 10), D3(10, 20, 10)}});
    gn::ExecEnv env;
    gn::Group g = gn::Group::makeDefault("T");
    g.nodes.push_back(N{"Rest", "MeshGrid", {}, {{"sizeX", 100.0}, {"sizeY", 100.0}}});
    g.nodes.push_back(N{"T", "TransformGeometry", {}, {{"rotation", {0, 0, 90}}, {"translation", {0, 10, 0}}}});
    g.nodes.push_back(N{"D", "DeformCurvesOnSurface", {}, {}});
    g.links = {L{"Rest", "mesh", "T", "geometry"}, L{"T", "geometry", "D", "surface"}, L{"Rest", "mesh", "D", "rest"}, L{"GroupInput", "geometry", "D", "curves"},
               L{"D", "curves", "GroupOutput", "geometry"}};
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    env.input = strand;
    gn::GeometryPtr out = gn::execute(*plan, env);
    REQUIRE(out->curves);
    glm::dmat4 xf(1.0);
    xf = glm::dmat4(gn::nodes::eulerToMatrix(D3(0, 0, gn::nodes::kPi / 2)));
    xf[3] = glm::dvec4(0, 10, 0, 1);
    for (size_t k = 0; k < 2; ++k)
        CHECK(near(out->curves->positions[k], D3(xf * glm::dvec4(strand->curves->positions[k], 1.0)), 1e-6));
}
