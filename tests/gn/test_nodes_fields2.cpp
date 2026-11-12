#include "Nodes2Util.h"

using namespace nodes2test;
using gn::Domain;
using gn::SocketType;

TEST_CASE("field average, min max and variance per group")
{
    // Values 0..5 in two groups: even and odd indices.
    auto in = points({D3(0), D3(1), D3(2), D3(3), D3(4), D3(5)});
    Run r = run({N{"I", "Index", {}, {}}, N{"Mod", "IntegerMath", {}, {{"operation", "modulo"}, {"b", 2}}}, N{"A", "FieldAverage", {}, {}}, N{"M", "FieldMinMax", {}, {}},
                 N{"V", "FieldVariance", {}, {}}, N{"All", "FieldAverage", {}, {}}},
                {L{"I", "value", "Mod", "a"}, L{"I", "value", "A", "value"}, L{"Mod", "value", "A", "groupId"}, L{"I", "value", "M", "value"}, L{"Mod", "value", "M", "groupId"},
                 L{"I", "value", "V", "value"}, L{"Mod", "value", "V", "groupId"}, L{"I", "value", "All", "value"}, L{"GroupInput", "geometry", "GroupOutput", "geometry"},
                 L{"A", "mean", "GroupOutput", "mean"}, L{"A", "median", "GroupOutput", "median"}, L{"M", "min", "GroupOutput", "min"}, L{"M", "max", "GroupOutput", "max"},
                 L{"V", "variance", "GroupOutput", "var"}, L{"V", "standardDeviation", "GroupOutput", "sd"}, L{"All", "median", "GroupOutput", "allMedian"}},
                {{"mean", SocketType::Float}, {"median", SocketType::Float}, {"min", SocketType::Float}, {"max", SocketType::Float}, {"var", SocketType::Float},
                 {"sd", SocketType::Float}, {"allMedian", SocketType::Float}},
                in);
    const geo::Mesh& m = *r.geometry->points;
    CHECK(onPoints<double>(r.values[0], m) == std::vector<double>{2, 3, 2, 3, 2, 3});
    CHECK(onPoints<double>(r.values[1], m) == std::vector<double>{2, 3, 2, 3, 2, 3});
    CHECK(onPoints<double>(r.values[2], m) == std::vector<double>{0, 1, 0, 1, 0, 1});
    CHECK(onPoints<double>(r.values[3], m) == std::vector<double>{4, 5, 4, 5, 4, 5});
    for (double v : onPoints<double>(r.values[4], m))
        CHECK(v == doctest::Approx(8.0 / 3.0));
    for (double v : onPoints<double>(r.values[5], m))
        CHECK(v == doctest::Approx(std::sqrt(8.0 / 3.0)));
    // Six values: the median is the middle pair's mean.
    CHECK(onPoints<double>(r.values[6], m)[0] == doctest::Approx(2.5));

    Run vec = run({N{"P", "Position", {}, {}}, N{"M", "FieldMinMax", {}, {{"dataType", "vector"}}}},
                  {L{"P", "value", "M", "value"}, L{"GroupInput", "geometry", "GroupOutput", "geometry"}, L{"M", "max", "GroupOutput", "max"}},
                  {{"max", SocketType::Vector}}, points({D3(1, 9, 0), D3(5, 2, 0)}));
    CHECK(near(onPoints<D3>(vec.values[0], *vec.geometry->points)[0], D3(5, 9, 0)));
}

TEST_CASE("euler orders round trip and rotation to euler")
{
    for (const char* order : {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"})
    {
        CAPTURE(order);
        Run r = run({N{"E", "EulerToRotation", {}, {{"order", order}, {"euler", {0.3, -0.4, 0.5}}}}, N{"R", "RotationToEuler", {}, {{"order", order}}}},
                    {L{"E", "rotation", "R", "rotation"}, L{"R", "euler", "GroupOutput", "e"}}, {{"e", SocketType::Vector}});
        CHECK(near(single<D3>(r.values[0]), D3(0.3, -0.4, 0.5), 1e-9));
    }
    // XYZ is Lila's own rotation, so it passes through unchanged.
    Run same = run({N{"E", "EulerToRotation", {}, {{"euler", {0.1, 0.2, 0.3}}}}}, {L{"E", "rotation", "GroupOutput", "r"}}, {{"r", SocketType::Rotation}});
    CHECK(near(single<D3>(same.values[0]), D3(0.1, 0.2, 0.3), 1e-9));
    // ZYX applies Z first: a quarter turn about Z then about Y differs from XYZ order.
    Run zyx = run({N{"E", "EulerToRotation", {}, {{"order", "ZYX"}, {"euler", {0, gn::nodes::kPi / 2, gn::nodes::kPi / 2}}}}, N{"V", "VectorRotate", {}, {{"rotationType", "euler"}, {"vector", {1, 0, 0}}}}},
                  {L{"E", "rotation", "V", "rotation"}, L{"V", "vector", "GroupOutput", "v"}}, {{"v", SocketType::Vector}});
    // X turned about Z goes to Y, which a turn about Y leaves alone.
    CHECK(near(single<D3>(zyx.values[0]), D3(0, 1, 0), 1e-9));
}

TEST_CASE("matrix svd")
{
    std::vector<N> nodes = {N{"C", "CombineMatrix", {}, {{"c0r0", 2.0}, {"c1r0", 1.0}, {"c1r1", 3.0}, {"c2r2", -1.0}, {"c0r2", 0.5}}}, N{"S", "MatrixSVD", {}, {}}};
    Run r = run(nodes, {L{"C", "matrix", "S", "matrix"}, L{"S", "u", "GroupOutput", "u"}, L{"S", "s", "GroupOutput", "s"}, L{"S", "v", "GroupOutput", "v"}},
                {{"u", SocketType::Matrix}, {"s", SocketType::Vector}, {"v", SocketType::Matrix}});
    glm::dmat3 u(single<glm::dmat4>(r.values[0])), v(single<glm::dmat4>(r.values[2]));
    D3 s = single<D3>(r.values[1]);
    glm::dmat3 a(1.0);
    a[0] = D3(2, 0, 0.5);
    a[1] = D3(1, 3, 0);
    a[2] = D3(0, 0, -1);
    glm::dmat3 rebuilt = u * glm::dmat3(s.x, 0, 0, 0, s.y, 0, 0, 0, s.z) * glm::transpose(v);
    for (int c = 0; c < 3; ++c)
        CHECK(near(rebuilt[c], a[c], 1e-9));
    CHECK(s.x >= s.y);
    CHECK(s.y >= s.z);
    CHECK(s.z >= 0.0);
    glm::dmat3 utu = glm::transpose(u) * u, vtv = glm::transpose(v) * v;
    for (int c = 0; c < 3; ++c)
    {
        CHECK(near(utu[c], glm::dmat3(1.0)[c], 1e-9));
        CHECK(near(vtv[c], glm::dmat3(1.0)[c], 1e-9));
    }
}

TEST_CASE("string nodes")
{
    Run f = run({N{"F", "FindInString", {}, {{"string", "a-b-c"}, {"search", "-"}}}}, {L{"F", "firstFound", "GroupOutput", "first"}, L{"F", "count", "GroupOutput", "n"}},
                {{"first", SocketType::Int}, {"n", SocketType::Int}});
    CHECK(single<int64_t>(f.values[0]) == 1);
    CHECK(single<int64_t>(f.values[1]) == 2);
    Run none = run({N{"F", "FindInString", {}, {{"string", "abc"}, {"search", "x"}}}}, {L{"F", "firstFound", "GroupOutput", "first"}}, {{"first", SocketType::Int}});
    CHECK(single<int64_t>(none.values[0]) == -1);

    Run fmt = run({N{"S", "String", {}, {{"value", "wall"}}},
                   N{"F", "FormatString", {}, {{"format", "{0}_{a:03d} {b:.2f} {{x}}"}, {"a", 7.0}, {"b", 3.14159}}}},
                  {L{"S", "value", "F", "strings"}, L{"F", "string", "GroupOutput", "s"}}, {{"s", SocketType::String}});
    CHECK(fmt.values[0].string == "wall_007 3.14 {x}");

    auto match = [](const char* op, const char* s, const char* key, bool cs = true) {
        Run r = run({N{"M", "MatchString", {}, {{"operation", op}, {"string", s}, {"key", key}, {"caseSensitive", cs}}}}, {L{"M", "result", "GroupOutput", "r"}},
                    {{"r", SocketType::Bool}});
        return single<uint8_t>(r.values[0]) != 0;
    };
    CHECK(match("startsWith", "SM_Rock", "SM_"));
    CHECK_FALSE(match("endsWith", "SM_Rock", "SM_"));
    CHECK(match("contains", "SM_Rock", "rock", false));
    CHECK_FALSE(match("contains", "SM_Rock", "rock"));
    CHECK(match("regex", "rock_012", "_[0-9]{3}$"));

    Run val = run({N{"V", "StringToValue", {}, {{"string", "  12.5cm"}}}, N{"I", "StringToValue", {}, {{"dataType", "int"}, {"string", "42 apples"}}}},
                  {L{"V", "value", "GroupOutput", "f"}, L{"V", "length", "GroupOutput", "len"}, L{"I", "value", "GroupOutput", "i"}},
                  {{"f", SocketType::Float}, {"len", SocketType::Int}, {"i", SocketType::Int}});
    CHECK(single<double>(val.values[0]) == doctest::Approx(12.5));
    CHECK(single<int64_t>(val.values[1]) == 6);
    CHECK(single<int64_t>(val.values[2]) == 42);
}
