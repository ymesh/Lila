#include "scene/MathNodes.h"

#include "core/Profile.h"
#include "deform/Surface.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <span>

namespace lila::scene::mathnodes
{
namespace
{
class MathNode;
using ComputeFn = void (*)(MathNode&);

/** One utility node: its type's compute function reads and writes attrs by name. */
class MathNode : public Node
{
public:
    explicit MathNode(ComputeFn fn) : mFn(fn) {}

    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        mFn(*this);
    }

    int a(std::string_view name) const { return type().attrIndex(name); }
    double f(std::string_view name) const
    {
        const PlugValue& v = value(a(name));
        if (auto* d = std::get_if<double>(&v))
            return *d;
        if (auto* i = std::get_if<int64_t>(&v))
            return double(*i);
        if (auto* b = std::get_if<bool>(&v))
            return *b ? 1.0 : 0.0;
        return 0.0;
    }
    int64_t i(std::string_view name) const { return int64_t(std::llround(f(name))); }
    bool b(std::string_view name) const { return f(name) != 0.0; }
    const glm::dvec3& v(std::string_view name) const { return as<glm::dvec3>(a(name)); }
    const glm::dmat4& m(std::string_view name) const { return as<glm::dmat4>(a(name)); }

    template <class T> void out(std::string_view name, T value) { setOutput(a(name), std::move(value)); }

private:
    ComputeFn mFn;
};

// ------------------------------------------------------------------------------------------------ attr helpers

const uint32_t kIn = AttrInput | AttrStorable | AttrKeyable;
const uint32_t kInHidden = AttrInput | AttrStorable;

AttrDef scalar(std::string name, std::string shortName, double def, std::string doc, uint32_t flags = kIn)
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Float, .defaultValue = def, .flags = flags,
            .doc = std::move(doc)};
}

AttrDef integer(std::string name, std::string shortName, int64_t def, std::string doc)
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Int, .defaultValue = def, .flags = kIn,
            .doc = std::move(doc)};
}

AttrDef boolean(std::string name, std::string shortName, bool def, std::string doc)
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Bool, .defaultValue = def, .flags = kIn,
            .doc = std::move(doc)};
}

AttrDef enumAttr(std::string name, std::string shortName, int64_t def, std::vector<std::string> names, std::string doc)
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Enum, .defaultValue = def, .flags = kInHidden,
            .enumNames = std::move(names), .doc = std::move(doc)};
}

/** A Vec3 whose children are named name + suffixes (Maya's X/Y/Z or R/G/B, with short names to match). */
AttrDef vec3(std::string name, std::string shortName, glm::dvec3 def, std::string doc, uint32_t flags = kIn,
             std::array<std::string, 3> suffix = {"X", "Y", "Z"}, std::array<std::string, 3> shortSuffix = {"x", "y", "z"})
{
    AttrDef d{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = flags, .doc = std::move(doc)};
    for (int k = 0; k < 3; ++k)
    {
        d.childNames[size_t(k)] = name + suffix[size_t(k)];
        d.childShort[size_t(k)] = shortName + shortSuffix[size_t(k)];
    }
    return d;
}

/** A Vec3 with its children's names given in full, for Maya attributes whose children aren't named after them. */
AttrDef vec3Named(std::string name, std::string shortName, glm::dvec3 def, std::string doc, std::array<std::string, 3> children,
                  std::array<std::string, 3> childShort, uint32_t flags = kIn)
{
    AttrDef d{.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Vec3, .defaultValue = def, .flags = flags,
              .doc = std::move(doc)};
    d.childNames = std::move(children);
    d.childShort = std::move(childShort);
    return d;
}

AttrDef color(std::string name, std::string shortName, glm::dvec3 def, std::string doc, uint32_t flags = kIn)
{
    return vec3(std::move(name), std::move(shortName), def, std::move(doc), flags, {"R", "G", "B"}, {"r", "g", "b"});
}

AttrDef matrix(std::string name, std::string shortName, std::string doc, uint32_t flags = kInHidden)
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
            .flags = flags, .doc = std::move(doc)};
}

AttrDef array(AttrDef d)
{
    d.flags |= AttrArray;
    return d;
}

AttrDef output(AttrDef d)
{
    d.flags = AttrOutput;
    return d;
}

/** An array's elements as doubles, by ascending index. */
std::vector<std::pair<int, double>> scalars(const MathNode& n, std::string_view name)
{
    std::vector<std::pair<int, double>> out;
    for (const auto& [i, v] : n.elements(n.a(name)))
        out.push_back({i, std::holds_alternative<double>(v) ? std::get<double>(v) : 0.0});
    return out;
}

// ------------------------------------------------------------------------------------------------ math

glm::dvec3 orderAngles(const glm::dmat3& r, int64_t order) { return matrixToEuler(r, RotateOrder(std::clamp<int64_t>(order, 0, 5))); }
glm::dmat3 eulerMatrix(const glm::dvec3& deg, int64_t order) { return glm::dmat3(eulerToMatrix(deg, RotateOrder(std::clamp<int64_t>(order, 0, 5)))); }

glm::dmat4 lerpDecomposed(const glm::dmat4& a, const glm::dmat4& b, double tw, double rw, double sw, double hw)
{
    Decomposed da = decompose(a), db = decompose(b), out;
    out.translate = glm::mix(da.translate, db.translate, tw);
    out.scale = glm::mix(da.scale, db.scale, sw);
    out.shear = glm::mix(da.shear, db.shear, hw);
    glm::dquat qa = glm::quat_cast(da.rotation), qb = glm::quat_cast(db.rotation);
    out.rotation = glm::mat3_cast(glm::slerp(qa, qb, rw));
    return compose(out);
}

/** Maya's ramp interpolations: 0 none (step), 1 linear, 2 smooth, 3 spline. Keys sorted by position. */
template <class V> V rampAt(const std::vector<std::tuple<double, V, int64_t>>& keys, double t, V fallback)
{
    if (keys.empty())
        return fallback;
    if (t <= std::get<0>(keys.front()))
        return std::get<1>(keys.front());
    if (t >= std::get<0>(keys.back()))
        return std::get<1>(keys.back());
    size_t k = 0;
    while (k + 1 < keys.size() && std::get<0>(keys[k + 1]) <= t)
        ++k;
    const auto& [p0, v0, interp] = keys[k];
    const auto& [p1, v1, i1] = keys[k + 1];
    (void)i1;
    double span = p1 - p0;
    double u = span > 1e-12 ? (t - p0) / span : 0.0;
    switch (interp)
    {
    case 0: return v0;
    case 2: return v0 + (v1 - v0) * (u * u * (3.0 - 2.0 * u));
    case 3:
    {
        // Catmull-Rom through the neighbours, the end keys repeated.
        const V& vm = std::get<1>(keys[k > 0 ? k - 1 : k]);
        const V& vp = std::get<1>(keys[k + 2 < keys.size() ? k + 2 : k + 1]);
        double u2 = u * u, u3 = u2 * u;
        return (v0 * 2.0 + (v1 - vm) * u + (vm * 2.0 - v0 * 5.0 + v1 * 4.0 - vp) * u2 + (v0 * 3.0 - vm - v1 * 3.0 + vp) * u3) * 0.5;
    }
    default: return v0 + (v1 - v0) * u;
    }
}

glm::dvec3 componentWise(const glm::dvec3& a, const glm::dvec3& b, double (*op)(double, double))
{
    return {op(a.x, b.x), op(a.y, b.y), op(a.z, b.z)};
}

// ------------------------------------------------------------------------------------------------ computes

void multiplyDivide(MathNode& n)
{
    const glm::dvec3 a = n.v("input1"), b = n.v("input2");
    switch (n.i("operation"))
    {
    case 0: n.out("output", a); break;
    case 2: n.out("output", componentWise(a, b, [](double x, double y) { return y == 0.0 ? 0.0 : x / y; })); break;
    case 3: n.out("output", componentWise(a, b, [](double x, double y) { return std::pow(x, y); })); break;
    default: n.out("output", a * b); break;
    }
}

void plusMinusAverage(MathNode& n)
{
    const int64_t op = n.i("operation");
    auto fold = [&]<class V>(const std::vector<std::pair<int, PlugValue>>& items, V zero) {
        V acc = zero;
        size_t count = 0;
        for (const auto& [k, value] : items)
        {
            const V x = std::get<V>(value);
            if (count == 0)
                acc = x;
            else if (op == 2)
                acc = acc - x;
            else if (op != 0)
                acc = acc + x;
            ++count;
        }
        if (op == 3 && count > 0)
            acc = acc / double(count);
        return acc;
    };
    n.out("output1D", fold(n.elements(n.a("input1D")), 0.0));
    n.out("output2D", fold(n.elements(n.a("input2D")), glm::dvec3(0.0)));
    n.out("output3D", fold(n.elements(n.a("input3D")), glm::dvec3(0.0)));
}

void condition(MathNode& n)
{
    const double a = n.f("firstTerm"), b = n.f("secondTerm");
    bool r = false;
    switch (n.i("operation"))
    {
    case 0: r = a == b; break;
    case 1: r = a != b; break;
    case 2: r = a > b; break;
    case 3: r = a >= b; break;
    case 4: r = a < b; break;
    case 5: r = a <= b; break;
    }
    n.out("outColor", r ? n.v("colorIfTrue") : n.v("colorIfFalse"));
}

void reverse(MathNode& n) { n.out("output", glm::dvec3(1.0) - n.v("input")); }

void clampNode(MathNode& n)
{
    const glm::dvec3 in = n.v("input"), lo = n.v("min"), hi = n.v("max");
    glm::dvec3 o;
    for (int k = 0; k < 3; ++k)
        o[k] = std::min(std::max(in[k], lo[k]), hi[k]);
    n.out("output", o);
}

void setRange(MathNode& n)
{
    const glm::dvec3 v = n.v("value"), lo = n.v("min"), hi = n.v("max"), oldLo = n.v("oldMin"), oldHi = n.v("oldMax");
    glm::dvec3 o;
    for (int k = 0; k < 3; ++k)
    {
        const double span = oldHi[k] - oldLo[k];
        const double t = span == 0.0 ? 0.0 : std::clamp((v[k] - oldLo[k]) / span, 0.0, 1.0);
        o[k] = lo[k] + t * (hi[k] - lo[k]);
    }
    n.out("outValue", o);
}

void remapValue(MathNode& n)
{
    const double span = n.f("inputMax") - n.f("inputMin");
    const double t = span == 0.0 ? 0.0 : (n.f("inputValue") - n.f("inputMin")) / span;
    std::vector<std::tuple<double, double, int64_t>> values;
    const int vp = n.a("value.position"), vv = n.a("value.floatValue"), vi = n.a("value.interp");
    for (const auto& [k, pos] : n.elements(vp))
        values.push_back({std::get<double>(pos), n.elementOr<double>(vv, k), n.elementOr<int64_t>(vi, k)});
    std::sort(values.begin(), values.end(), [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    const double r = values.empty() ? std::clamp(t, 0.0, 1.0) : rampAt(values, t, 0.0);
    n.out("outValue", n.f("outputMin") + r * (n.f("outputMax") - n.f("outputMin")));

    std::vector<std::tuple<double, glm::dvec3, int64_t>> colors;
    const int cp = n.a("color.position"), cc = n.a("color.color"), ci = n.a("color.interp");
    for (const auto& [k, pos] : n.elements(cp))
        colors.push_back({std::get<double>(pos), n.elementOr<glm::dvec3>(cc, k), n.elementOr<int64_t>(ci, k)});
    std::sort(colors.begin(), colors.end(), [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    const glm::dvec3 c = colors.empty() ? glm::dvec3(std::clamp(t, 0.0, 1.0)) : rampAt(colors, t, glm::dvec3(0.0));
    n.out("outColor", glm::dvec3(n.f("outputMin")) + c * (n.f("outputMax") - n.f("outputMin")));
}

void blendColors(MathNode& n)
{
    const double b = n.f("blender");
    n.out("output", n.v("color1") * b + n.v("color2") * (1.0 - b));
}

void blendTwoAttr(MathNode& n)
{
    const auto in = scalars(n, "input");
    const double ab = n.f("attributesBlender");
    if (in.empty())
        return n.out("output", 0.0);
    if (in.size() == 1)
        return n.out("output", in[0].second);
    // With more than two inputs the blender walks from one to the next: 0 is the first, 1 the second, ...
    const double t = std::clamp(ab, 0.0, double(in.size() - 1));
    const size_t k = std::min(size_t(t), in.size() - 2);
    const double u = t - double(k);
    n.out("output", in[k].second * (1.0 - u) + in[k + 1].second * u);
}

void blendWeighted(MathNode& n)
{
    const int w = n.a("weight");
    double sum = 0.0;
    for (const auto& [k, v] : scalars(n, "input"))
        sum += v * n.elementOr<double>(w, k);
    n.out("output", sum);
}

void pairBlend(MathNode& n)
{
    const double w = n.f("weight");
    const glm::dvec3 t1 = n.v("inTranslate1"), t2 = n.v("inTranslate2");
    glm::dvec3 t;
    for (int k = 0; k < 3; ++k)
    {
        const int64_t mode = n.i(std::array<const char*, 3>{"translateXMode", "translateYMode", "translateZMode"}[size_t(k)]);
        t[k] = mode == 1 ? t1[k] : mode == 2 ? t2[k] : t1[k] + (t2[k] - t1[k]) * w;
    }
    n.out("outTranslate", t);

    const glm::dvec3 r1 = n.v("inRotate1"), r2 = n.v("inRotate2");
    const int64_t mode = n.i("rotateMode"), order = n.i("rotateOrder");
    glm::dvec3 r;
    if (mode == 1)
        r = r1;
    else if (mode == 2)
        r = r2;
    else if (n.i("rotInterpolation") == 1)
    {
        glm::dquat q1 = glm::quat_cast(eulerMatrix(r1, order)), q2 = glm::quat_cast(eulerMatrix(r2, order));
        r = eulerNear(glm::mat3_cast(glm::slerp(q1, q2, w)), RotateOrder(std::clamp<int64_t>(order, 0, 5)), r1 + (r2 - r1) * w);
    }
    else
        r = r1 + (r2 - r1) * w;
    n.out("outRotate", r);
}

void choice(MathNode& n)
{
    const int s = int(n.i("selector"));
    n.out("output", n.elementOr<double>(n.a("input"), s));
    n.out("outputVector", n.elementOr<glm::dvec3>(n.a("inputVector"), s));
    n.out("outputMatrix", n.elementOr<glm::dmat4>(n.a("inputMatrix"), s));
}

void distanceBetween(MathNode& n)
{
    const glm::dvec3 p1(n.m("inMatrix1") * glm::dvec4(n.v("point1"), 1.0));
    const glm::dvec3 p2(n.m("inMatrix2") * glm::dvec4(n.v("point2"), 1.0));
    n.out("distance", glm::length(p1 - p2));
}

void vectorProduct(MathNode& n)
{
    const glm::dvec3 a = n.v("input1"), b = n.v("input2");
    glm::dvec3 o = a;
    switch (n.i("operation"))
    {
    case 1: o = glm::dvec3(glm::dot(n.b("normalizeOutput") && glm::length(a) > 0 ? glm::normalize(a) : a,
                                    n.b("normalizeOutput") && glm::length(b) > 0 ? glm::normalize(b) : b));
        n.out("output", o);
        return;
    case 2: o = glm::cross(a, b); break;
    case 3: o = glm::dmat3(n.m("matrix")) * a; break;
    case 4: o = glm::dvec3(n.m("matrix") * glm::dvec4(a, 1.0)); break;
    default: break;
    }
    if (n.b("normalizeOutput") && glm::length(o) > 0.0)
        o = glm::normalize(o);
    n.out("output", o);
}

void unitConversion(MathNode& n) { n.out("output", n.f("input") * n.f("conversionFactor")); }
void multDoubleLinear(MathNode& n) { n.out("output", n.f("input1") * n.f("input2")); }
void addDoubleLinear(MathNode& n) { n.out("output", n.f("input1") + n.f("input2")); }

void multMatrix(MathNode& n)
{
    glm::dmat4 m(1.0);
    for (const auto& [k, v] : n.elements(n.a("matrixIn")))
        m = std::get<glm::dmat4>(v) * m;
    n.out("matrixSum", m);
}

void addMatrix(MathNode& n)
{
    glm::dmat4 m(0.0);
    for (const auto& [k, v] : n.elements(n.a("matrixIn")))
        m += std::get<glm::dmat4>(v);
    n.out("matrixSum", m);
}

void wtAddMatrix(MathNode& n)
{
    glm::dmat4 m(0.0);
    const int w = n.a("wtMatrix.weightIn");
    for (const auto& [k, v] : n.elements(n.a("wtMatrix.matrixIn")))
        m += std::get<glm::dmat4>(v) * n.elementOr<double>(w, k);
    n.out("matrixSum", m);
}

void inverseMatrix(MathNode& n) { n.out("outputMatrix", glm::inverse(n.m("inputMatrix"))); }
void transposeMatrix(MathNode& n) { n.out("outputMatrix", glm::transpose(n.m("inputMatrix"))); }
void holdMatrix(MathNode& n) { n.out("outMatrix", n.m("inMatrix")); }

void fourByFourMatrix(MathNode& n)
{
    glm::dmat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[r][c] = n.f(std::string("in") + char('0' + r) + char('0' + c));
    n.out("output", m);
}

void decomposeMatrix(MathNode& n)
{
    const Decomposed d = decompose(n.m("inputMatrix"));
    n.out("outputTranslate", d.translate);
    n.out("outputRotate", orderAngles(d.rotation, n.i("inputRotateOrder")));
    n.out("outputScale", d.scale);
    n.out("outputShear", d.shear);
    const glm::dquat q = glm::quat_cast(d.rotation);
    n.out("outputQuatX", q.x);
    n.out("outputQuatY", q.y);
    n.out("outputQuatZ", q.z);
    n.out("outputQuatW", q.w);
}

void composeMatrix(MathNode& n)
{
    Decomposed d;
    d.translate = n.v("inputTranslate");
    d.scale = n.v("inputScale");
    d.shear = n.v("inputShear");
    if (n.b("useEulerRotation"))
        d.rotation = eulerMatrix(n.v("inputRotate"), n.i("inputRotateOrder"));
    else
    {
        glm::dquat q(n.f("inputQuatW"), n.f("inputQuatX"), n.f("inputQuatY"), n.f("inputQuatZ"));
        d.rotation = glm::length(q) > 1e-12 ? glm::mat3_cast(glm::normalize(q)) : glm::dmat3(1.0);
    }
    n.out("outputMatrix", compose(d));
}

void blendMatrix(MathNode& n)
{
    const glm::dmat4 in = n.m("inputMatrix");
    if (!n.b("enable"))
        return n.out("outputMatrix", in);
    const int tm = n.a("target.targetMatrix"), use = n.a("target.useMatrix"), wt = n.a("target.weight");
    const int sw = n.a("target.scaleWeight"), tw = n.a("target.translateWeight"), rw = n.a("target.rotateWeight"), hw = n.a("target.shearWeight");
    glm::dmat4 m = in;
    for (const auto& [k, v] : n.elements(tm))
    {
        if (!n.elementOr<bool>(use, k))
            continue;
        const double w = n.elementOr<double>(wt, k);
        m = lerpDecomposed(m, std::get<glm::dmat4>(v), w * n.elementOr<double>(tw, k), w * n.elementOr<double>(rw, k),
                           w * n.elementOr<double>(sw, k), w * n.elementOr<double>(hw, k));
    }
    const double env = n.f("envelope");
    n.out("outputMatrix", env >= 1.0 ? m : lerpDecomposed(in, m, env, env, env, env));
}

void pickMatrix(MathNode& n)
{
    Decomposed d = decompose(n.m("inputMatrix"));
    if (!n.b("useTranslate"))
        d.translate = glm::dvec3(0.0);
    if (!n.b("useRotate"))
        d.rotation = glm::dmat3(1.0);
    if (!n.b("useScale"))
        d.scale = glm::dvec3(1.0);
    if (!n.b("useShear"))
        d.shear = glm::dvec3(0.0);
    n.out("outputMatrix", compose(d));
}

/** Shortest rotation taking unit a onto unit b. */
glm::dmat3 arc(const glm::dvec3& a, const glm::dvec3& b)
{
    const double c = std::clamp(glm::dot(a, b), -1.0, 1.0);
    glm::dvec3 axis = glm::cross(a, b);
    if (glm::length(axis) < 1e-12)
    {
        if (c > 0.0)
            return glm::dmat3(1.0);
        glm::dvec3 any = std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
        return glm::mat3_cast(glm::angleAxis(glm::pi<double>(), glm::normalize(glm::cross(a, any))));
    }
    return glm::mat3_cast(glm::angleAxis(std::atan2(glm::length(axis), c), glm::normalize(axis)));
}

void aimMatrix(MathNode& n)
{
    const glm::dmat4 in = n.m("inputMatrix");
    if (!n.b("enable"))
        return n.out("outputMatrix", in);
    Decomposed d = decompose(in);
    const glm::dvec3 origin = d.translate;
    auto wanted = [&](int64_t mode, const char* matrixName, const char* vectorName) -> glm::dvec3 {
        const glm::dmat4 t = n.m(matrixName);
        if (mode == 1)
            return glm::dvec3(t[3]) - origin;
        if (mode == 2)
            return rigutil::orthonormal(glm::dmat3(t)) * n.v(vectorName);
        return glm::dvec3(0.0);
    };
    glm::dmat3 r = d.rotation;
    const glm::dvec3 primaryAxis = n.v("primaryInputAxis"), secondaryAxis = n.v("secondaryInputAxis");
    const glm::dvec3 aim = wanted(n.i("primaryMode"), "primaryTargetMatrix", "primaryTargetVector");
    if (glm::length(aim) > 1e-12 && glm::length(primaryAxis) > 1e-12)
        r = arc(glm::normalize(r * primaryAxis), glm::normalize(aim)) * r;
    const glm::dvec3 up = wanted(n.i("secondaryMode"), "secondaryTargetMatrix", "secondaryTargetVector");
    if (glm::length(up) > 1e-12 && glm::length(secondaryAxis) > 1e-12 && glm::length(primaryAxis) > 1e-12)
    {
        // Turn about the aimed primary axis so the secondary axis leans toward its target.
        const glm::dvec3 axis = glm::normalize(r * primaryAxis);
        const glm::dvec3 have = r * secondaryAxis - glm::dot(r * secondaryAxis, axis) * axis;
        const glm::dvec3 want = up - glm::dot(up, axis) * axis;
        if (glm::length(have) > 1e-12 && glm::length(want) > 1e-12)
        {
            const glm::dvec3 h = glm::normalize(have), w = glm::normalize(want);
            const double angle = std::atan2(glm::dot(glm::cross(h, w), axis), glm::dot(h, w));
            r = glm::mat3_cast(glm::angleAxis(angle, axis)) * r;
        }
    }
    Decomposed outD = d;
    outD.rotation = r;
    glm::dmat4 out = compose(outD);
    const double env = n.f("envelope");
    n.out("outputMatrix", env >= 1.0 ? out : lerpDecomposed(in, out, env, env, env, env));
}

/** Maya's weighted geometric mean of the target scales (a negative scale keeps its sign), times the offset. */
/** The mesh point nearest inPosition, with the surface normal and uv there: the mesh's own points placed by inputMatrix. */
void closestPointOnMesh(MathNode& n)
{
    const MeshPtr& mesh = n.as<MeshPtr>(n.a("inMesh"));
    if (!mesh)
        return;
    const glm::dmat4 m = n.m("inputMatrix");
    geo::Mesh world = *mesh;
    for (geo::Index p = 0; p < world.pointCapacity(); ++p)
        world.setPosition(p, glm::dvec3(m * glm::dvec4(mesh->position(p), 1.0)));
    deform::SurfacePoint sp;
    if (!deform::locateNearest(world, n.v("inPosition"), geo::attr::uvName(0), sp))
        return;
    const glm::dvec3 a = world.position(sp.points[0]), b = world.position(sp.points[1]), c = world.position(sp.points[2]);
    n.out("position", a * sp.bary[0] + b * sp.bary[1] + c * sp.bary[2]);
    const glm::dvec3 nrm = glm::cross(b - a, c - a);
    n.out("normal", glm::length(nrm) > 1e-20 ? glm::normalize(nrm) : glm::dvec3(0.0));
    if (sp.hasUv)
    {
        const glm::dvec2 uv = sp.uv[0] * sp.bary[0] + sp.uv[1] * sp.bary[1] + sp.uv[2] * sp.bary[2];
        n.out("parameterU", uv.x);
        n.out("parameterV", uv.y);
    }
    const int nearest = sp.bary[0] >= sp.bary[1] && sp.bary[0] >= sp.bary[2] ? 0 : sp.bary[1] >= sp.bary[2] ? 1 : 2;
    n.out("closestVertexIndex", int64_t(sp.points[nearest]));
}

void scaleConstraint(MathNode& n)
{
    const int w = n.a("targets.weight");
    glm::dvec3 logSum(0.0), sign(1.0);
    double total = 0.0;
    for (const auto& [k, v] : n.elements(n.a("targets.scale")))
    {
        const double weight = n.elementOr<double>(w, k);
        if (weight <= 0.0)
            continue;
        const glm::dvec3 s = std::get<glm::dvec3>(v);
        for (int c = 0; c < 3; ++c)
        {
            logSum[c] += std::log(std::max(std::abs(s[c]), 1e-12)) * weight;
            if (s[c] < 0.0)
                sign[c] = -sign[c];
        }
        total += weight;
    }
    if (total <= 0.0)
        return n.out("constraintScale", n.v("restScale"));
    const glm::dvec3 mean(std::exp(logSum.x / total), std::exp(logSum.y / total), std::exp(logSum.z / total));
    n.out("constraintScale", mean * sign * n.v("offset"));
}

void poleVectorConstraint(MathNode& n)
{
    const int w = n.a("targets.weight");
    glm::dvec3 sum(0.0);
    double total = 0.0;
    for (const auto& [k, v] : n.elements(n.a("targets.matrix")))
    {
        const double weight = n.elementOr<double>(w, k);
        if (weight <= 0.0)
            continue;
        sum += glm::dvec3(std::get<glm::dmat4>(v)[3]) * weight;
        total += weight;
    }
    if (total <= 0.0)
        return n.out("constraintTranslate", n.v("restTranslate"));
    const glm::dvec3 pivot(n.m("pivotMatrix") * glm::dvec4(n.v("pivot"), 1.0));
    n.out("constraintTranslate", glm::dvec3(n.m("spaceMatrix") * glm::dvec4(sum / total - pivot, 0.0)) + n.v("offset"));
}

// ------------------------------------------------------------------------------------------------ registration

struct Spec
{
    const char* name;
    const char* icon;
    const char* doc;
    std::vector<AttrDef> attrs;
    ComputeFn fn;
};

void registerSpec(Spec s)
{
    NodeType t;
    t.name = s.name;
    t.icon = s.icon;
    t.doc = s.doc;
    t.ownAttrs = std::move(s.attrs);
    ComputeFn fn = s.fn;
    t.create = [fn] { return std::make_unique<MathNode>(fn); };
    types::registerType(std::move(t));
}

AttrDef grouped(std::string group, AttrDef d)
{
    d.name = group + "." + d.name;
    d.flags |= AttrArray;
    return d;
}
} // namespace

Decomposed decompose(const glm::dmat4& m)
{
    Decomposed d;
    d.translate = glm::dvec3(m[3]);
    const glm::dvec3 a0(m[0]), a1(m[1]), a2(m[2]);
    d.scale.x = glm::length(a0);
    const glm::dvec3 r0 = d.scale.x > 1e-12 ? a0 / d.scale.x : glm::dvec3(1, 0, 0);
    glm::dvec3 u1 = a1 - glm::dot(a1, r0) * r0;
    d.scale.y = glm::length(u1);
    glm::dvec3 r1 = std::abs(r0.z) < 0.9 ? glm::cross(glm::dvec3(0, 0, 1), r0) : glm::cross(r0, glm::dvec3(1, 0, 0));
    r1 = d.scale.y > 1e-12 ? u1 / d.scale.y : glm::normalize(r1);
    glm::dvec3 u2 = a2 - glm::dot(a2, r0) * r0 - glm::dot(a2, r1) * r1;
    d.scale.z = glm::length(u2);
    glm::dvec3 r2 = glm::cross(r0, r1);
    if (glm::dot(u2, r2) < 0.0)
        d.scale.z = -d.scale.z;
    if (d.scale.y > 1e-12)
        d.shear.x = glm::dot(a1, r0) / d.scale.y;
    if (std::abs(d.scale.z) > 1e-12)
    {
        d.shear.y = glm::dot(a2, r0) / d.scale.z;
        d.shear.z = glm::dot(a2, r1) / d.scale.z;
    }
    d.rotation = glm::dmat3(r0, r1, r2);
    return d;
}

glm::dmat4 compose(const Decomposed& d)
{
    glm::dmat3 shear(1.0);
    shear[1][0] = d.shear.x;
    shear[2][0] = d.shear.y;
    shear[2][1] = d.shear.z;
    const glm::dmat3 s(glm::dvec3(d.scale.x, 0, 0), glm::dvec3(0, d.scale.y, 0), glm::dvec3(0, 0, d.scale.z));
    glm::dmat4 m(d.rotation * shear * s);
    m[3] = glm::dvec4(d.translate, 1.0);
    return m;
}

void registerTypes()
{
    if (types::find("multiplyDivide"))
        return;
    const std::vector<std::string> orders = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"};

    registerSpec({"unitConversion", "math-function", "Multiplies its input by conversionFactor.",
                  {scalar("input", "i", 0.0, "The value converted."), scalar("conversionFactor", "cf", 1.0, "The factor.", kInHidden),
                   output(scalar("output", "o", 0.0, "input times conversionFactor."))},
                  unitConversion});
    registerSpec({"multDoubleLinear", "math-x-plus-y", "Multiplies two values.",
                  {scalar("input1", "i1", 0.0, "First factor."), scalar("input2", "i2", 1.0, "Second factor."),
                   output(scalar("output", "o", 0.0, "The product."))},
                  multDoubleLinear});
    registerSpec({"addDoubleLinear", "math-x-plus-y", "Adds two values.",
                  {scalar("input1", "i1", 0.0, "First term."), scalar("input2", "i2", 0.0, "Second term."),
                   output(scalar("output", "o", 0.0, "The sum."))},
                  addDoubleLinear});
    registerSpec({"multiplyDivide", "math-x-divide-y", "Multiplies, divides or raises input1 by input2, per component.",
                  {enumAttr("operation", "op", 1, {"noOperation", "multiply", "divide", "power"}, "What to do with the inputs."),
                   vec3("input1", "i1", glm::dvec3(0.0), "First operand."), vec3("input2", "i2", glm::dvec3(1.0), "Second operand."),
                   output(vec3("output", "o", glm::dvec3(0.0), "The result."))},
                  multiplyDivide});
    registerSpec({"plusMinusAverage", "math-function", "Sums, subtracts or averages any number of values, 2D or 3D vectors.",
                  {enumAttr("operation", "op", 1, {"noOperation", "sum", "subtract", "average"}, "What to do with the inputs."),
                   array(scalar("input1D", "i1", 0.0, "Values.")),
                   array(vec3("input2D", "i2", glm::dvec3(0.0), "2D vectors (z unused).", kIn, {"x", "y", "z"}, {"x", "y", "z"})),
                   array(vec3("input3D", "i3", glm::dvec3(0.0), "3D vectors.", kIn, {"x", "y", "z"}, {"x", "y", "z"})),
                   output(scalar("output1D", "o1", 0.0, "Result of the values.")),
                   output(vec3("output2D", "o2", glm::dvec3(0.0), "Result of the 2D vectors.", kIn, {"x", "y", "z"}, {"x", "y", "z"})),
                   output(vec3("output3D", "o3", glm::dvec3(0.0), "Result of the 3D vectors.", kIn, {"x", "y", "z"}, {"x", "y", "z"}))},
                  plusMinusAverage});
    registerSpec({"condition", "arrows-split", "Compares firstTerm with secondTerm and outputs colorIfTrue or colorIfFalse.",
                  {enumAttr("operation", "op", 0, {"equal", "notEqual", "greaterThan", "greaterOrEqual", "lessThan", "lessOrEqual"}, "The comparison."),
                   scalar("firstTerm", "ft", 0.0, "Left side."), scalar("secondTerm", "st", 0.0, "Right side."),
                   color("colorIfTrue", "ct", glm::dvec3(0.0), "Output when the comparison holds."),
                   color("colorIfFalse", "cf", glm::dvec3(1.0), "Output otherwise."),
                   output(color("outColor", "oc", glm::dvec3(0.0), "The chosen value."))},
                  condition});
    registerSpec({"reverse", "arrows-exchange", "Outputs 1 minus its input, per component.",
                  {vec3("input", "i", glm::dvec3(0.0), "The values."), output(vec3("output", "o", glm::dvec3(1.0), "1 - input."))}, reverse});
    registerSpec({"clamp", "arrows-minimize", "Keeps each component of input between min and max.",
                  {color("min", "mn", glm::dvec3(0.0), "Lower limits."), color("max", "mx", glm::dvec3(0.0), "Upper limits."),
                   color("input", "ip", glm::dvec3(0.0), "The values."), output(color("output", "op", glm::dvec3(0.0), "The clamped values."))},
                  clampNode});
    registerSpec({"setRange", "arrows-horizontal", "Maps value from oldMin..oldMax onto min..max, clamped, per component.",
                  {vec3("value", "v", glm::dvec3(0.0), "The values."), vec3("min", "n", glm::dvec3(0.0), "New range start."),
                   vec3("max", "m", glm::dvec3(0.0), "New range end."), vec3("oldMin", "on", glm::dvec3(0.0), "Old range start."),
                   vec3("oldMax", "om", glm::dvec3(0.0), "Old range end."), output(vec3("outValue", "o", glm::dvec3(0.0), "The mapped values."))},
                  setRange});
    registerSpec({"remapValue", "chart-line", "Remaps inputValue from inputMin..inputMax through a ramp onto outputMin..outputMax.",
                  {scalar("inputValue", "i", 0.0, "The value."), scalar("inputMin", "imn", 0.0, "Input range start."),
                   scalar("inputMax", "imx", 1.0, "Input range end."), scalar("outputMin", "omn", 0.0, "Output range start."),
                   scalar("outputMax", "omx", 1.0, "Output range end."),
                   grouped("value", scalar("position", "vlp", 0.0, "Ramp key position.")),
                   grouped("value", scalar("floatValue", "vlfv", 0.0, "Ramp key value.")),
                   grouped("value", integer("interp", "vli", 1, "Ramp key interpolation: 0 none, 1 linear, 2 smooth, 3 spline.")),
                   grouped("color", scalar("position", "clp", 0.0, "Colour ramp key position.")),
                   grouped("color", vec3Named("color", "clc", glm::dvec3(0.0), "Colour ramp key colour.", {"color_ColorR", "color_ColorG", "color_ColorB"},
                                              {"clcr", "clcg", "clcb"})),
                   grouped("color", integer("interp", "cli", 1, "Colour ramp key interpolation.")),
                   output(scalar("outValue", "ov", 0.0, "The remapped value.")), output(color("outColor", "oc", glm::dvec3(0.0), "The remapped colour."))},
                  remapValue});
    registerSpec({"blendColors", "color-swatch", "Blends color1 and color2 by blender (1 is all color1).",
                  {scalar("blender", "b", 0.5, "How much of color1."), color("color1", "c1", glm::dvec3(1, 0, 0), "First value."),
                   color("color2", "c2", glm::dvec3(0, 0, 1), "Second value."), output(color("output", "op", glm::dvec3(0.5, 0, 0.5), "The blend."))},
                  blendColors});
    registerSpec({"blendTwoAttr", "arrows-join", "Blends its inputs by attributesBlender: 0 the first, 1 the second.",
                  {array(scalar("input", "i", 0.0, "Values.")), scalar("attributesBlender", "ab", 0.0, "Blend position."),
                   output(scalar("output", "o", 0.0, "The blend."))},
                  blendTwoAttr});
    registerSpec({"blendWeighted", "arrows-join", "Sums its inputs, each times its weight: how several set driven keys on one channel add up.",
                  {array(scalar("input", "i", 0.0, "Values.")), array(scalar("weight", "w", 1.0, "Each value's weight.")),
                   output(scalar("output", "o", 0.0, "The weighted sum."))},
                  blendWeighted});
    registerSpec({"pairBlend", "arrows-join-2", "Blends two translations and rotations by weight (0 the first pair, 1 the second).",
                  {vec3Named("inTranslate1", "it1", glm::dvec3(0.0), "First translation.", {"inTranslateX1", "inTranslateY1", "inTranslateZ1"},
                             {"itx1", "ity1", "itz1"}),
                   vec3Named("inRotate1", "ir1", glm::dvec3(0.0), "First rotation, degrees.", {"inRotateX1", "inRotateY1", "inRotateZ1"},
                             {"irx1", "iry1", "irz1"}),
                   vec3Named("inTranslate2", "it2", glm::dvec3(0.0), "Second translation.", {"inTranslateX2", "inTranslateY2", "inTranslateZ2"},
                             {"itx2", "ity2", "itz2"}),
                   vec3Named("inRotate2", "ir2", glm::dvec3(0.0), "Second rotation, degrees.", {"inRotateX2", "inRotateY2", "inRotateZ2"},
                             {"irx2", "iry2", "irz2"}),
                   scalar("weight", "w", 1.0, "Blend: 0 the first pair, 1 the second."),
                   enumAttr("rotateOrder", "ro", 0, orders, "Rotate order of the rotations."),
                   enumAttr("translateXMode", "txm", 0, {"blend", "in1", "in2"}, "Blend X, or take one input's."),
                   enumAttr("translateYMode", "tym", 0, {"blend", "in1", "in2"}, "Blend Y, or take one input's."),
                   enumAttr("translateZMode", "tzm", 0, {"blend", "in1", "in2"}, "Blend Z, or take one input's."),
                   enumAttr("rotateMode", "rm", 0, {"blend", "in1", "in2"}, "Blend the rotations, or take one input's."),
                   enumAttr("rotInterpolation", "ri", 0, {"euler", "quaternion"}, "Blend the Euler angles or slerp."),
                   output(vec3("outTranslate", "ot", glm::dvec3(0.0), "Blended translation.")),
                   output(vec3("outRotate", "or", glm::dvec3(0.0), "Blended rotation, degrees."))},
                  pairBlend});
    registerSpec({"choice", "list-numbers", "Passes on the input selector picks: values, vectors or matrices.",
                  {integer("selector", "s", 0, "Index of the input passed on."), array(scalar("input", "i", 0.0, "Values.")),
                   array(vec3("inputVector", "iv", glm::dvec3(0.0), "Vectors.")), array(matrix("inputMatrix", "im", "Matrices.")),
                   output(scalar("output", "o", 0.0, "The chosen value.")), output(vec3("outputVector", "ov", glm::dvec3(0.0), "The chosen vector.")),
                   output(matrix("outputMatrix", "om", "The chosen matrix."))},
                  choice});
    registerSpec({"distanceBetween", "ruler-measure", "Distance between point1 and point2, each placed by its matrix.",
                  {vec3("point1", "p1", glm::dvec3(0.0), "First point."), matrix("inMatrix1", "im1", "First point's space."),
                   vec3("point2", "p2", glm::dvec3(0.0), "Second point."), matrix("inMatrix2", "im2", "Second point's space."),
                   output(scalar("distance", "d", 0.0, "The distance."))},
                  distanceBetween});
    registerSpec({"vectorProduct", "vector", "Dot or cross product of two vectors, or a vector or point times a matrix.",
                  {enumAttr("operation", "op", 1, {"noOperation", "dotProduct", "crossProduct", "vectorMatrixProduct", "pointMatrixProduct"}, "The product."),
                   vec3("input1", "i1", glm::dvec3(0.0), "First vector."), vec3("input2", "i2", glm::dvec3(0.0), "Second vector."),
                   matrix("matrix", "m", "Matrix for the matrix products."), boolean("normalizeOutput", "no", false, "Make the result unit length."),
                   output(vec3("output", "o", glm::dvec3(0.0), "The result."))},
                  vectorProduct});

    registerSpec({"multMatrix", "matrix", "Multiplies matrices, the first applied first.",
                  {array(matrix("matrixIn", "i", "The matrices.")), output(matrix("matrixSum", "o", "Their product."))}, multMatrix});
    registerSpec({"addMatrix", "matrix", "Adds matrices.", {array(matrix("matrixIn", "i", "The matrices.")), output(matrix("matrixSum", "o", "Their sum."))},
                  addMatrix});
    registerSpec({"wtAddMatrix", "matrix", "Adds matrices, each times its weight.",
                  {grouped("wtMatrix", matrix("matrixIn", "m", "A matrix.")), grouped("wtMatrix", scalar("weightIn", "w", 0.0, "Its weight.")),
                   output(matrix("matrixSum", "o", "The weighted sum."))},
                  wtAddMatrix});
    registerSpec({"inverseMatrix", "matrix", "Inverts a matrix.",
                  {matrix("inputMatrix", "imat", "The matrix."), output(matrix("outputMatrix", "omat", "Its inverse."))}, inverseMatrix});
    registerSpec({"transposeMatrix", "matrix", "Transposes a matrix.",
                  {matrix("inputMatrix", "imat", "The matrix."), output(matrix("outputMatrix", "omat", "Its transpose."))}, transposeMatrix});
    registerSpec({"holdMatrix", "matrix", "Passes a matrix on.", {matrix("inMatrix", "i", "The matrix."), output(matrix("outMatrix", "o", "The same."))},
                  holdMatrix});
    {
        std::vector<AttrDef> attrs;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                std::string rc = std::string() + char('0' + r) + char('0' + c);
                attrs.push_back(scalar("in" + rc, "i" + rc, r == c ? 1.0 : 0.0, "Row " + std::to_string(r) + ", column " + std::to_string(c) + "."));
            }
        attrs.push_back(output(matrix("output", "o", "The matrix.")));
        registerSpec({"fourByFourMatrix", "matrix", "Builds a matrix from its sixteen values, Maya's row by row.", std::move(attrs), fourByFourMatrix});
    }
    registerSpec({"decomposeMatrix", "matrix", "Splits a matrix into translate, rotate (degrees, in inputRotateOrder), scale, shear and a quaternion.",
                  {matrix("inputMatrix", "imat", "The matrix."), enumAttr("inputRotateOrder", "ro", 0, orders, "Order of outputRotate."),
                   output(vec3("outputTranslate", "ot", glm::dvec3(0.0), "Translation.")),
                   output(vec3("outputRotate", "or", glm::dvec3(0.0), "Rotation, degrees.")),
                   output(vec3("outputScale", "os", glm::dvec3(1.0), "Scale.")), output(vec3("outputShear", "osh", glm::dvec3(0.0), "Shear xy, xz, yz.")),
                   output(scalar("outputQuatX", "oqx", 0.0, "Rotation quaternion x.")), output(scalar("outputQuatY", "oqy", 0.0, "Rotation quaternion y.")),
                   output(scalar("outputQuatZ", "oqz", 0.0, "Rotation quaternion z.")), output(scalar("outputQuatW", "oqw", 1.0, "Rotation quaternion w."))},
                  decomposeMatrix});
    registerSpec({"composeMatrix", "matrix", "Builds a matrix from translate, rotate (degrees or a quaternion), scale and shear.",
                  {vec3("inputTranslate", "it", glm::dvec3(0.0), "Translation."), vec3("inputRotate", "ir", glm::dvec3(0.0), "Rotation, degrees."),
                   vec3("inputScale", "is", glm::dvec3(1.0), "Scale."), vec3("inputShear", "ish", glm::dvec3(0.0), "Shear xy, xz, yz."),
                   enumAttr("inputRotateOrder", "ro", 0, orders, "Order of inputRotate."),
                   boolean("useEulerRotation", "uer", true, "Use inputRotate; off uses the quaternion."),
                   scalar("inputQuatX", "iqwx", 0.0, "Quaternion x."), scalar("inputQuatY", "iqwy", 0.0, "Quaternion y."),
                   scalar("inputQuatZ", "iqwz", 0.0, "Quaternion z."), scalar("inputQuatW", "iqw", 1.0, "Quaternion w."),
                   output(matrix("outputMatrix", "omat", "The matrix."))},
                  composeMatrix});
    registerSpec({"blendMatrix", "matrix", "Blends inputMatrix toward each target in turn, translation, rotation (slerp), scale and shear weighted apart.",
                  {boolean("enable", "enb", true, "Off passes inputMatrix on."), scalar("envelope", "env", 1.0, "How much of the blend applies."),
                   matrix("inputMatrix", "imat", "The matrix blended from."),
                   grouped("target", matrix("targetMatrix", "tmat", "A target.")), grouped("target", boolean("useMatrix", "umt", true, "Use this target.")),
                   grouped("target", scalar("weight", "wgt", 1.0, "Its weight.")),
                   grouped("target", scalar("scaleWeight", "sca", 1.0, "Its scale weight.")),
                   grouped("target", scalar("translateWeight", "tra", 1.0, "Its translation weight.")),
                   grouped("target", scalar("rotateWeight", "rot", 1.0, "Its rotation weight.")),
                   grouped("target", scalar("shearWeight", "she", 1.0, "Its shear weight.")),
                   output(matrix("outputMatrix", "omat", "The blend."))},
                  blendMatrix});
    registerSpec({"pickMatrix", "matrix", "Keeps only the chosen parts of a matrix: translation, rotation, scale, shear.",
                  {matrix("inputMatrix", "imat", "The matrix."), boolean("useScale", "sca", true, "Keep the scale."),
                   boolean("useTranslate", "tra", true, "Keep the translation."), boolean("useShear", "she", true, "Keep the shear."),
                   boolean("useRotate", "rot", true, "Keep the rotation."), output(matrix("outputMatrix", "tmat", "The result."))},
                  pickMatrix});
    registerSpec({"aimMatrix", "target-arrow", "Turns inputMatrix so its primary axis aims at (or aligns with) a target, and its secondary axis leans toward another.",
                  {boolean("enable", "enb", true, "Off passes inputMatrix on."), scalar("envelope", "env", 1.0, "How much of the turn applies."),
                   matrix("inputMatrix", "imat", "The matrix turned."),
                   vec3Named("primaryInputAxis", "pmi", glm::dvec3(1, 0, 0), "Local axis aimed.",
                             {"primaryInputAxisX", "primaryInputAxisY", "primaryInputAxisZ"}, {"pmx", "pmy", "pmz"}),
                   enumAttr("primaryMode", "prmd", 1, {"lockAxis", "aim", "align"}, "Aim at the target's position, or align with its vector."),
                   vec3Named("primaryTargetVector", "pmiv", glm::dvec3(1, 0, 0), "Align mode: the target-space vector.",
                             {"primaryTargetVectorX", "primaryTargetVectorY", "primaryTargetVectorZ"}, {"pmvx", "pmvy", "pmvz"}),
                   matrix("primaryTargetMatrix", "pmat", "The primary target."),
                   vec3Named("secondaryInputAxis", "smi", glm::dvec3(0, 1, 0), "Local axis leaned.",
                             {"secondaryInputAxisX", "secondaryInputAxisY", "secondaryInputAxisZ"}, {"smx", "smy", "smz"}),
                   enumAttr("secondaryMode", "sm", 0, {"none", "aim", "align"}, "Lean toward the target's position, or its vector."),
                   vec3Named("secondaryTargetVector", "smiv", glm::dvec3(0, 1, 0), "Align mode: the target-space vector.",
                             {"secondaryTargetVectorX", "secondaryTargetVectorY", "secondaryTargetVectorZ"}, {"smvx", "smvy", "smvz"}),
                   matrix("secondaryTargetMatrix", "smat", "The secondary target."), output(matrix("outputMatrix", "tmat", "The turned matrix."))},
                  aimMatrix});

    registerSpec({"closestPointOnMesh", "point",
                  "The point of a mesh nearest a position, with the surface normal and uv there.",
                  {{.name = "inMesh", .shortName = "im", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
                    .doc = "The mesh's outMesh."},
                   matrix("inputMatrix", "ix", "Places the mesh: its world matrix.", AttrInput | AttrHidden),
                   vec3("inPosition", "ip", glm::dvec3(0.0), "The position, world space."),
                   output(vec3("position", "p", glm::dvec3(0.0), "The nearest surface point.")),
                   output(vec3("normal", "n", glm::dvec3(0.0), "The surface normal there.")),
                   output(scalar("parameterU", "u", 0.0, "U there.")), output(scalar("parameterV", "v", 0.0, "V there.")),
                   output(integer("closestVertexIndex", "vt", 0, "The nearest of the closest triangle's points."))},
                  closestPointOnMesh});
    registerSpec({"scaleConstraint", "link", "Drives a transform's scale with the weighted average of its targets' scales times an offset.",
                  {grouped("targets", vec3("scale", "ts", glm::dvec3(1.0), "A target's scale.", AttrInput | AttrHidden)),
                   grouped("targets", scalar("weight", "tw", 1.0, "Its weight.")), vec3("offset", "o", glm::dvec3(1.0), "Multiplies the result."),
                   vec3("restScale", "rs", glm::dvec3(1.0), "The scale while every weight is 0.", kInHidden),
                   output(vec3("constraintScale", "cs", glm::dvec3(1.0), "Drives the object's scale."))},
                  scaleConstraint});
    registerSpec({"poleVectorConstraint", "link", "Drives an ikHandle's poleVector toward the weighted average of its targets' positions, from the chain's start joint.",
                  {grouped("targets", matrix("matrix", "tm", "A target's world matrix.", AttrInput | AttrHidden)),
                   grouped("targets", scalar("weight", "tw", 1.0, "Its weight.")),
                   matrix("pivotMatrix", "pvm", "The frame the pivot is in: the start joint's parent's world matrix.", AttrInput | AttrHidden),
                   vec3("pivot", "pvt", glm::dvec3(0.0), "The start joint's position in pivotMatrix.", kInHidden),
                   matrix("spaceMatrix", "spm", "Turns the world vector into the space the pole vector is read in.", kInHidden),
                   vec3("offset", "o", glm::dvec3(0.0), "Added to the result."),
                   vec3("restTranslate", "rt", glm::dvec3(0.0), "The pole vector while every weight is 0.", kInHidden),
                   output(vec3("constraintTranslate", "ct", glm::dvec3(0.0), "Drives the handle's poleVector."))},
                  poleVectorConstraint});
}
} // namespace lila::scene::mathnodes
