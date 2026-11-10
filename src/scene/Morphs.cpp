#include "scene/Morphs.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/DeformerUtil.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "rigmath/Easing.h"

#include <Eigen/Dense>
#include <glm/gtc/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace lila::scene
{
using namespace rigutil;
using namespace deformutil;

namespace
{
using morphs::kDeltas;
using morphs::kEditMesh;
using morphs::kEditWeight;
using morphs::kFitShapes;
using morphs::kFitTolerance;
using morphs::kInBetweens;
using morphs::kInterpolation;
using morphs::kLink;
using morphs::kMorphName;
using morphs::kPivot;
using morphs::kPointWeights;
using morphs::kPrevious;
using morphs::kResponse;
using morphs::kTargetMesh;
using morphs::kWeight;
using morphs::Interpolation;

constexpr double kShapeEps = 1e-6;

/** One shape of a target as evaluation sees it: sparse offsets, or a whole mesh to diff against the input. */
struct ShapeRef
{
    double weight = 1.0;
    const std::vector<uint32_t>* indices = nullptr;
    const std::vector<glm::dvec3>* deltas = nullptr;
    const geo::Mesh* mesh = nullptr;
};

/** The full shape first, then the in-betweens by weight; an edit mesh stands in for the shape it edits. */
std::vector<ShapeRef> shapesOf(const DataPtr& deltas, const MeshPtr& live, const DataPtr& inBetweens, const MeshPtr& edit, double editWeight)
{
    std::vector<ShapeRef> out;
    const auto* full = dynamic_cast<const MorphDeltas*>(deltas.get());
    out.push_back({1.0, full ? &full->indices : nullptr, full ? &full->deltas : nullptr, live.get()});
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(inBetweens.get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            out.push_back({s.weight, &s.indices, &s.deltas, nullptr});
    if (edit)
        for (ShapeRef& s : out)
            if (std::abs(s.weight - editWeight) < kShapeEps)
                s.mesh = edit.get();
    return out;
}

// ------------------------------------------------------------------------------------------------ paths

/**
 * Every shape of a target over the points any of them moves, for paths that need each point's shapes together. Knots
 * run by weight and include the rest shape (all zero) at 0.
 */
struct ShapeTable
{
    std::vector<uint32_t> points;
    std::vector<double> knots;
    std::vector<glm::dvec3> offsets; // knot-major: offsets[k * points.size() + i]
    const glm::dvec3* at(size_t knot) const { return offsets.data() + knot * points.size(); }
};

/** in is needed only for shapes given as meshes. */
std::shared_ptr<const ShapeTable> buildTable(const std::vector<ShapeRef>& shapes, const geo::Mesh* in)
{
    auto t = std::make_shared<ShapeTable>();
    std::vector<size_t> order(shapes.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return shapes[a].weight < shapes[b].weight; });

    const size_t cap = in ? in->pointCapacity() : 0;
    std::vector<int32_t> slot;
    auto touch = [&](uint32_t p) {
        if (p >= slot.size())
            slot.resize(size_t(p) + 1, -1);
        if (slot[p] < 0)
        {
            slot[p] = int32_t(t->points.size());
            t->points.push_back(p);
        }
    };
    for (const ShapeRef& s : shapes)
    {
        if (s.mesh && in && s.mesh->pointCapacity() == cap)
        {
            for (geo::Index p = 0; p < cap; ++p)
                if (in->pointValid(p) && glm::length(s.mesh->position(p) - in->position(p)) > 1e-9)
                    touch(uint32_t(p));
        }
        else if (s.indices && s.deltas)
            for (size_t k = 0; k < std::min(s.indices->size(), s.deltas->size()); ++k)
                if (!in || (*s.indices)[k] < cap)
                    touch((*s.indices)[k]);
    }

    const size_t np = t->points.size();
    bool restPlaced = false;
    auto addKnot = [&](double w) {
        t->knots.push_back(w);
        t->offsets.resize(t->knots.size() * np, glm::dvec3(0.0));
    };
    for (size_t o : order)
    {
        const ShapeRef& s = shapes[o];
        if (!restPlaced && s.weight > 0.0)
        {
            addKnot(0.0);
            restPlaced = true;
        }
        addKnot(s.weight);
        glm::dvec3* dst = t->offsets.data() + (t->knots.size() - 1) * np;
        if (s.mesh && in && s.mesh->pointCapacity() == cap)
        {
            for (size_t i = 0; i < np; ++i)
                dst[i] = s.mesh->position(t->points[i]) - in->position(t->points[i]);
        }
        else if (s.indices && s.deltas)
            for (size_t k = 0; k < std::min(s.indices->size(), s.deltas->size()); ++k)
                if (uint32_t p = (*s.indices)[k]; p < slot.size() && slot[p] >= 0)
                    dst[slot[p]] = (*s.deltas)[k];
    }
    if (!restPlaced)
        addKnot(0.0);
    return t;
}

/** The segment of sorted knots t[0..n) that w falls in (the end ones extended), and where along it, 0 to 1 inside. */
std::pair<size_t, double> segmentOf(const double* t, size_t n, double w)
{
    size_t seg = 0;
    while (seg + 2 < n && w > t[seg + 1])
        ++seg;
    const double span = t[seg + 1] - t[seg];
    return {seg, std::abs(span) < 1e-12 ? 1.0 : (w - t[seg]) / span};
}

/** Coefficients of each knot (rest included) at w: linear, or a cubic Hermite with Catmull-Rom tangents. */
std::vector<double> knotCoefficients(const std::vector<double>& t, double w, bool smooth)
{
    const size_t n = t.size();
    std::vector<double> c(n, 0.0);
    if (n < 2)
        return c;
    const auto [seg, s] = segmentOf(t.data(), n, w);
    if (!smooth || n == 2)
    {
        c[seg] += 1.0 - s;
        c[seg + 1] += s;
        return c;
    }
    auto tangent = [&](size_t j, double scale) {
        const size_t a = j == 0 ? 0 : j - 1;
        const size_t b = j + 1 == n ? j : j + 1;
        const double span = t[b] - t[a];
        if (std::abs(span) < 1e-12)
            return;
        c[b] += scale / span;
        c[a] -= scale / span;
    };
    if (s < 0.0 && seg == 0)
    {
        c[0] += 1.0;
        tangent(0, w - t[0]);
        return c;
    }
    if (s > 1.0 && seg + 2 == n)
    {
        c[n - 1] += 1.0;
        tangent(n - 1, w - t[n - 1]);
        return c;
    }
    const double h = t[seg + 1] - t[seg];
    const double s2 = s * s, s3 = s2 * s;
    c[seg] += 2.0 * s3 - 3.0 * s2 + 1.0;
    c[seg + 1] += -2.0 * s3 + 3.0 * s2;
    tangent(seg, (s3 - 2.0 * s2 + s) * h);
    tangent(seg + 1, (s3 - s2) * h);
    return c;
}

/** A circle through three points in order, a then b then c, with b's and c's angles from a in the travel direction. */
struct Circle
{
    glm::dvec3 center{0.0}, u{0.0}, v{0.0};
    double radius = 0.0, angleB = 0.0, angleC = 0.0;
    glm::dvec3 at(double angle) const { return center + radius * (std::cos(angle) * u + std::sin(angle) * v); }
};

std::optional<Circle> circleThrough(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 ab = b - a, ac = c - a;
    const glm::dvec3 n = glm::cross(ab, ac);
    const double nn = glm::dot(n, n);
    const double scale = glm::dot(ab, ab) * glm::dot(ac, ac);
    // Nearly collinear points make a huge circle that is a line in all but rounding.
    if (scale < 1e-24 || nn < 1e-8 * scale)
        return std::nullopt;
    Circle out;
    out.center = a + (glm::cross(n, ab) * glm::dot(ac, ac) + glm::cross(ac, n) * glm::dot(ab, ab)) / (2.0 * nn);
    out.radius = glm::length(a - out.center);
    out.u = (a - out.center) / out.radius;
    out.v = glm::normalize(glm::cross(n, out.u));
    auto angle = [&](const glm::dvec3& p) {
        const glm::dvec3 d = p - out.center;
        double t = std::atan2(glm::dot(d, out.v), glm::dot(d, out.u));
        return t < 0.0 ? t + 2.0 * math::kPi : t;
    };
    out.angleB = angle(b);
    out.angleC = angle(c);
    if (out.angleC < out.angleB)
        out.angleC += 2.0 * math::kPi;
    return out;
}

/**
 * Circular blending: inside a segment, the point on the circle through the segment and its previous knot, and on the
 * circle through it and its next knot, blended across the segment. One circle at the ends; straight where points line up.
 */
glm::dvec3 arcPoint(const double* t, const glm::dvec3* P, size_t n, double w)
{
    const auto [i, s] = segmentOf(t, n, w);
    const glm::dvec3 straight = P[i] + (P[i + 1] - P[i]) * s;
    std::optional<glm::dvec3> back, ahead;
    if (i >= 1)
        if (auto c = circleThrough(P[i - 1], P[i], P[i + 1]))
            back = c->at(c->angleB + (c->angleC - c->angleB) * s);
    if (i + 2 < n)
        if (auto c = circleThrough(P[i], P[i + 1], P[i + 2]))
            ahead = c->at(c->angleB * s);
    const bool hasBack = i >= 1, hasAhead = i + 2 < n;
    if (!hasBack && !hasAhead)
        return straight;
    const glm::dvec3 b = back.value_or(straight), a = ahead.value_or(straight);
    if (!hasAhead)
        return b;
    if (!hasBack)
        return a;
    const double k = std::clamp(s, 0.0, 1.0);
    return b * (1.0 - k) + a * k;
}

/** Each segment swings about the pivot: direction turning on the great circle, distance to the pivot blending. */
glm::dvec3 pivotPoint(const double* t, const glm::dvec3* P, size_t n, double w, const glm::dvec3& base, const glm::dvec3& pivot)
{
    const auto [i, s] = segmentOf(t, n, w);
    const glm::dvec3 a = base + P[i] - pivot, b = base + P[i + 1] - pivot;
    const double la = glm::length(a), lb = glm::length(b);
    const glm::dvec3 straight = P[i] + (P[i + 1] - P[i]) * s;
    if (la < 1e-9 || lb < 1e-9)
        return straight;
    const glm::dvec3 axis = glm::cross(a, b);
    const double sinT = glm::length(axis) / (la * lb), cosT = glm::dot(a, b) / (la * lb);
    if (sinT < 1e-9 && cosT < 0.0)
        return straight;
    const double radius = la + (lb - la) * s;
    glm::dvec3 dir = a / la;
    if (sinT >= 1e-9)
    {
        const double angle = std::atan2(sinT, cosT) * s;
        const glm::dvec3 k = glm::normalize(axis);
        dir = dir * std::cos(angle) + glm::cross(k, dir) * std::sin(angle) + k * glm::dot(k, dir) * (1.0 - std::cos(angle));
    }
    return pivot + dir * radius - base;
}

/** Point i's offset at w (already through the response curve). */
glm::dvec3 pathPoint(const ShapeTable& tab, size_t i, Interpolation mode, double w, const glm::dvec3& base, const glm::dvec3& pivot)
{
    const size_t n = tab.knots.size();
    const size_t np = tab.points.size();
    glm::dvec3 P[16];
    std::vector<glm::dvec3> heap;
    glm::dvec3* p = P;
    if (n > 16)
    {
        heap.resize(n);
        p = heap.data();
    }
    for (size_t k = 0; k < n; ++k)
        p[k] = tab.offsets[k * np + i];
    switch (mode)
    {
    case Interpolation::Arc: return arcPoint(tab.knots.data(), p, n, w);
    case Interpolation::Pivot: return pivotPoint(tab.knots.data(), p, n, w, base, pivot);
    default:
    {
        std::vector<double> c = knotCoefficients(tab.knots, w, mode == Interpolation::Smooth);
        glm::dvec3 out(0.0);
        for (size_t k = 0; k < n; ++k)
            out += p[k] * c[k];
        return out;
    }
    }
}

Interpolation interpolationOf(const Node& t)
{
    return Interpolation(std::clamp<int64_t>(t.as<int64_t>(kInterpolation), 0, int64_t(Interpolation::Pivot)));
}

const std::vector<std::string>& interpolationNames()
{
    static const std::vector<std::string> names{"linear", "smooth", "arc", "pivot"};
    return names;
}

/** Control Rig's easing types, in its order, so the response attr's index is its ERigVMAnimEasingType. */
const std::vector<std::string>& responseNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        const std::string all = rigmath::easing::rigVmNames();
        for (size_t at = 0; at <= all.size();)
        {
            const size_t bar = std::min(all.find('|', at), all.size());
            out.push_back(all.substr(at, bar - at));
            at = bar + 1;
        }
        return out;
    }();
    return names;
}

std::string enumName(const std::vector<std::string>& names, int64_t i) { return i >= 0 && size_t(i) < names.size() ? names[size_t(i)] : std::to_string(i); }

int64_t enumIndex(const std::vector<std::string>& names, const std::string& name, const char* cmd)
{
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return int64_t(i);
    std::string all;
    for (const std::string& n : names)
        all += (all.empty() ? "" : " | ") + n;
    throw CommandError(std::format("{}: {} isn't one of {}", cmd, name, all));
}

/** What one morphTarget passes down its chain. */
struct MorphLink final : Data
{
    DataPtr prev;
    double weight = 0.0;
    DataPtr deltas;
    MeshPtr live;
    DataPtr inBetweens;
    MeshPtr edit;
    double editWeight = 1.0;
    DataPtr pointWeights;
    Interpolation interpolation = Interpolation::Linear;
    glm::dvec3 pivot{0.0};
    int64_t response = 0;
    std::shared_ptr<const ShapeTable> table; // for compiled paths with stored shapes only
    const char* kind() const override { return "morphLink"; }
    json toJson() const override { return {{"kind", kind()}}; }
};

std::vector<double> knotsOf(const std::vector<ShapeRef>& shapes)
{
    std::vector<double> k;
    for (const ShapeRef& s : shapes)
        k.push_back(s.weight);
    return k;
}

/** shapeCoefficients for shapes in any order. */
std::vector<double> coefficients(const std::vector<double>& weights, double w, bool smooth = false)
{
    std::vector<size_t> order(weights.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weights[a] < weights[b]; });
    std::vector<double> sorted;
    for (size_t i : order)
        sorted.push_back(weights[i]);
    std::vector<double> c = morphs::shapeCoefficients(sorted, w, smooth);
    std::vector<double> out(weights.size(), 0.0);
    for (size_t k = 0; k < order.size(); ++k)
        out[order[k]] = c[k];
    return out;
}

/** Adds a target's Linear or Smooth shapes at weight w. */
void addShapes(const geo::Mesh& in, std::vector<glm::dvec3>& points, const std::vector<ShapeRef>& shapes, double w, bool smooth,
               const PointWeights* pw)
{
    std::vector<double> c = coefficients(knotsOf(shapes), w, smooth);
    auto mask = [pw](size_t p) { return pw && p < pw->values.size() ? double(pw->values[p]) : 1.0; };
    for (size_t s = 0; s < shapes.size(); ++s)
    {
        const double k = c[s];
        if (k == 0.0)
            continue;
        if (const geo::Mesh* m = shapes[s].mesh)
        {
            if (m->pointCapacity() != in.pointCapacity())
                throw std::runtime_error("a live or edited target's point count differs from the mesh");
            parallelPoints(points.size(), [&](size_t p) { points[p] += (m->position(geo::Index(p)) - in.position(geo::Index(p))) * (k * mask(p)); });
            continue;
        }
        if (!shapes[s].indices || !shapes[s].deltas)
            continue;
        const std::vector<uint32_t>& idx = *shapes[s].indices;
        const std::vector<glm::dvec3>& del = *shapes[s].deltas;
        parallelPoints(std::min(idx.size(), del.size()), [&](size_t i) {
            uint32_t p = idx[i];
            if (p < points.size())
                points[p] += del[i] * (k * mask(p));
        });
    }
}

class MorphNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        for (DataPtr d = as<DataPtr>(attr("targets")); d;)
        {
            const auto* link = dynamic_cast<const MorphLink*>(d.get());
            if (!link)
                break;
            d = link->prev;
            if (link->weight == 0.0)
                continue;
            const double w = morphs::respond(link->response, link->weight);
            std::vector<ShapeRef> shapes = shapesOf(link->deltas, link->live, link->inBetweens, link->edit, link->editWeight);
            const auto* pw = dynamic_cast<const PointWeights*>(link->pointWeights.get());
            if (!morphs::compiles(link->interpolation))
            {
                addShapes(in, points, shapes, w, link->interpolation == Interpolation::Smooth, pw);
                continue;
            }
            std::shared_ptr<const ShapeTable> tab = link->table && !link->live && !link->edit ? link->table : buildTable(shapes, &in);
            parallelPoints(tab->points.size(), [&](size_t i) {
                const uint32_t p = tab->points[i];
                if (p >= points.size())
                    return;
                const double m = pw && p < pw->values.size() ? double(pw->values[p]) : 1.0;
                points[p] += pathPoint(*tab, i, link->interpolation, w, in.position(geo::Index(p)), link->pivot) * m;
            });
        }
    }
};

/** A compiled target's exported shapes: the full shape, then shapes fit to what its path adds, over its touched points. */
struct Compiled
{
    std::shared_ptr<const ShapeTable> table;
    std::vector<glm::dvec3> base; // the morph input's positions at the table's points
    Interpolation mode = Interpolation::Linear;
    glm::dvec3 pivot{0.0};
    std::vector<std::vector<glm::dvec3>> shapes; // per table point
    std::vector<double> norm2;
    morphs::Fit fit;

    std::vector<glm::dvec3> path(double w) const
    {
        std::vector<glm::dvec3> out(table->points.size());
        parallelPoints(out.size(), [&](size_t i) { out[i] = pathPoint(*table, i, mode, w, base[i], pivot); });
        return out;
    }
    /** Shape weights at an already responded weight; the shapes are orthogonal, so each is its own projection. */
    std::vector<double> weightsAt(double w) const
    {
        std::vector<glm::dvec3> x = path(w);
        std::vector<double> out(shapes.size(), 0.0);
        for (size_t k = 0; k < shapes.size(); ++k)
        {
            if (norm2[k] <= 0.0)
                continue;
            double dot = 0.0;
            for (size_t i = 0; i < x.size(); ++i)
                dot += glm::dot(x[i], shapes[k][i]);
            out[k] = dot / norm2[k];
        }
        return out;
    }
};

class MorphTargetNode : public Node
{
public:
    void compute(const EvalContext&) override
    {
        auto link = std::make_shared<MorphLink>();
        link->prev = as<DataPtr>(kPrevious);
        link->weight = as<double>(kWeight);
        link->deltas = as<DataPtr>(kDeltas);
        link->live = as<MeshPtr>(kTargetMesh);
        link->inBetweens = as<DataPtr>(kInBetweens);
        link->edit = as<MeshPtr>(kEditMesh);
        link->editWeight = as<double>(kEditWeight);
        link->pointWeights = as<DataPtr>(kPointWeights);
        link->interpolation = interpolationOf(*this);
        link->pivot = as<glm::dvec3>(kPivot);
        link->response = as<int64_t>(kResponse);
        if (morphs::compiles(link->interpolation) && !link->live && !link->edit)
            link->table = storedTable(link->deltas, link->inBetweens);
        setOutput(kLink, DataPtr(std::move(link)));
    }

    /** What a compile was made from; a change to any of it compiles again. */
    struct CompileKey
    {
        DataPtr deltas, inBetweens;
        MeshPtr live, edit, base;
        double editWeight = 1.0;
        int64_t mode = 0, response = 0, fitShapes = 0;
        glm::dvec3 pivot{0.0};
        double tolerance = 0.0, lo = 0.0, hi = 0.0;
        bool operator==(const CompileKey&) const = default;
    };
    std::mutex compileLock;
    CompileKey compileKey;
    std::shared_ptr<const Compiled> compiled;

private:
    /** The table of stored shapes, kept while they stay the same so animating the weight doesn't rebuild it. */
    std::shared_ptr<const ShapeTable> storedTable(const DataPtr& deltas, const DataPtr& inBetweens)
    {
        std::lock_guard lock(tableLock);
        if (!table || tableDeltas != deltas || tableInBetweens != inBetweens)
        {
            table = buildTable(shapesOf(deltas, nullptr, inBetweens, nullptr, 1.0), nullptr);
            tableDeltas = deltas;
            tableInBetweens = inBetweens;
        }
        return table;
    }
    std::mutex tableLock;
    DataPtr tableDeltas, tableInBetweens;
    std::shared_ptr<const ShapeTable> table;
};

// ------------------------------------------------------------------------------------------------ helpers

/** A morph, its mesh, or one of its targets, to the morph. */
NodeId resolveMorph(Graph& graph, const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (n.type().isA("morph"))
        return n.id();
    if (morphs::isTarget(n))
        if (NodeId m = morphs::morphOf(graph, n.id()); m != kNoNode)
            return m;
    if (n.type().isA("mesh"))
        if (NodeId m = morphs::find(graph, n.id()); m != kNoNode)
            return m;
    throw CommandError(std::string(cmd) + ": " + name + " has no morph");
}

Node& targetNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!morphs::isTarget(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a morphTarget");
    return n;
}

std::shared_ptr<MorphDeltas> deltasBetween(const geo::Mesh& base, const geo::Mesh& target)
{
    auto d = std::make_shared<MorphDeltas>();
    for (geo::Index p = 0; p < base.pointCapacity() && p < target.pointCapacity(); ++p)
    {
        if (!base.pointValid(p))
            continue;
        glm::dvec3 delta = target.position(p) - base.position(p);
        if (glm::length(delta) > 1e-7)
        {
            d->indices.push_back(uint32_t(p));
            d->deltas.push_back(delta);
        }
    }
    return d;
}

std::shared_ptr<MorphDeltas> sparse(const std::vector<glm::dvec3>& dense)
{
    auto d = std::make_shared<MorphDeltas>();
    for (size_t p = 0; p < dense.size(); ++p)
        if (glm::length(dense[p]) > 1e-7)
        {
            d->indices.push_back(uint32_t(p));
            d->deltas.push_back(dense[p]);
        }
    return d;
}

std::vector<glm::dvec3> dense(const std::vector<uint32_t>& indices, const std::vector<glm::dvec3>& deltas, size_t n)
{
    std::vector<glm::dvec3> out(n, glm::dvec3(0.0));
    for (size_t k = 0; k < std::min(indices.size(), deltas.size()); ++k)
        if (indices[k] < n)
            out[indices[k]] = deltas[k];
    return out;
}

MeshPtr morphInput(Graph& graph, NodeId morph)
{
    graph.evaluate();
    return std::get<MeshPtr>(graph.value({morph, DeformerNode::kInMesh, -1}));
}

/** A target's shape at shapeWeight (1 the full shape, else an in-between) as dense offsets from the morph's input. */
std::vector<glm::dvec3> shapeOffsets(Graph& graph, NodeId target, double shapeWeight, const geo::Mesh& base)
{
    Node* t = graph.node(target);
    const size_t n = base.pointCapacity();
    auto fromMesh = [&](const MeshPtr& m) {
        std::vector<glm::dvec3> out(n, glm::dvec3(0.0));
        if (m && m->pointCapacity() == n)
            for (geo::Index p = 0; p < n; ++p)
                out[p] = m->position(p) - base.position(p);
        return out;
    };
    MeshPtr edit = t->as<MeshPtr>(kEditMesh);
    if (edit && std::abs(t->as<double>(kEditWeight) - shapeWeight) < kShapeEps)
        return fromMesh(edit);
    if (std::abs(shapeWeight - 1.0) < kShapeEps)
    {
        if (MeshPtr live = t->as<MeshPtr>(kTargetMesh))
            return fromMesh(live);
        if (const auto* d = dynamic_cast<const MorphDeltas*>(t->as<DataPtr>(kDeltas).get()))
            return dense(d->indices, d->deltas, n);
        return std::vector<glm::dvec3>(n, glm::dvec3(0.0));
    }
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(t->as<DataPtr>(kInBetweens).get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            if (std::abs(s.weight - shapeWeight) < kShapeEps)
                return dense(s.indices, s.deltas, n);
    throw CommandError(std::format("{} has no in-between at {}", t->name(), shapeWeight));
}

/** Shape weights of a target: 1, then its in-betweens. */
std::vector<double> knotWeightsOf(const Node& t)
{
    std::vector<double> out{1.0};
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(t.as<DataPtr>(kInBetweens).get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            out.push_back(s.weight);
    return out;
}

std::vector<ShapeRef> shapesOfNode(const Node& t)
{
    return shapesOf(t.as<DataPtr>(kDeltas), t.as<MeshPtr>(kTargetMesh), t.as<DataPtr>(kInBetweens), t.as<MeshPtr>(kEditMesh), t.as<double>(kEditWeight));
}

/** The shape at shapeWeight on the target's path between its other shapes, the way evaluation reaches that weight now. */
std::vector<glm::dvec3> interpolatedOffsets(Graph& graph, NodeId target, double w, const geo::Mesh& base)
{
    const Node& t = *graph.node(target);
    std::shared_ptr<const ShapeTable> tab = buildTable(shapesOfNode(t), &base);
    const Interpolation mode = interpolationOf(t);
    const glm::dvec3 pivot = t.as<glm::dvec3>(kPivot);
    std::vector<glm::dvec3> out(base.pointCapacity(), glm::dvec3(0.0));
    for (size_t i = 0; i < tab->points.size(); ++i)
        if (uint32_t p = tab->points[i]; p < out.size())
            out[p] = pathPoint(*tab, i, mode, w, base.position(geo::Index(p)), pivot);
    return out;
}

void setShape(Graph& graph, NodeId target, double shapeWeight, std::shared_ptr<MorphDeltas> d)
{
    if (std::abs(shapeWeight - 1.0) < kShapeEps)
    {
        if (graph.source({target, kTargetMesh, -1}))
            graph.disconnect({target, kTargetMesh, -1});
        graph.setValue({target, kDeltas, -1}, DataPtr(std::move(d)));
    }
    else
        morphs::setInBetween(graph, target, shapeWeight, std::move(d));
}

/** Hooks a target node onto the end of the morph's chain. */
void appendTarget(Graph& graph, NodeId morph, NodeId t)
{
    PlugRef targets{morph, graph.node(morph)->type().attrIndex("targets"), -1};
    if (auto last = graph.source(targets))
    {
        graph.disconnect(targets);
        graph.connect(*last, {t, kPrevious, -1});
    }
    graph.connect({t, kLink, -1}, targets);
}

/** morphName made unique among the morph's targets. */
std::string uniqueMorphName(Graph& graph, NodeId morph, const std::string& wanted, NodeId except = kNoNode)
{
    auto taken = [&](const std::string& s) {
        for (NodeId t : morphs::targetNodes(graph, morph))
            if (t != except && graph.node(t)->as<std::string>(kMorphName) == s)
                return true;
        return false;
    };
    if (!taken(wanted))
        return wanted;
    for (int i = 2;; ++i)
        if (std::string s = wanted + std::to_string(i); !taken(s))
            return s;
}

NodeId newTarget(Graph& graph, NodeId morph, const std::string& morphName)
{
    std::string name = uniqueMorphName(graph, morph, morphName);
    Node* t = guarded([&] { return graph.createNode(*types::find("morphTarget"), name + "Target", kNoNode, true); });
    graph.setValue({t->id(), kMorphName, -1}, name);
    appendTarget(graph, morph, t->id());
    return t->id();
}

NodeId addTargetFromMesh(Graph& graph, NodeId morph, NodeId targetMesh, const std::string& morphName, bool live)
{
    MeshPtr base = morphInput(graph, morph);
    MeshPtr target = graph.node(targetMesh)->as<MeshPtr>(MeshNode::kOutMesh);
    if (!base || !target)
        throw CommandError("morph: " + graph.node(targetMesh)->name() + " or the base has no geometry");
    if (base->pointCapacity() != target->pointCapacity())
        throw CommandError(std::format("morph: {} has {} points, the base {}", graph.node(targetMesh)->name(), target->pointCapacity(),
                                       base->pointCapacity()));
    NodeId t = newTarget(graph, morph, morphName);
    if (live)
        graph.connect({targetMesh, MeshNode::kOutMesh, -1}, {t, kTargetMesh, -1});
    else
        graph.setValue({t, kDeltas, -1}, DataPtr(deltasBetween(*base, *target)));
    return t;
}

void removeTarget(Graph& graph, NodeId t)
{
    auto prev = graph.source({t, kPrevious, -1});
    std::vector<PlugRef> dsts = graph.destinations({t, kLink, -1});
    for (const PlugRef& d : dsts)
        graph.disconnect(d);
    if (prev)
        for (const PlugRef& d : dsts)
            graph.connect(*prev, d);
    graph.deleteNode(t);
}

json targetInfo(Graph& graph, NodeId t)
{
    Node* n = graph.node(t);
    json ib = json::array();
    for (double w : knotWeightsOf(*n))
        if (std::abs(w - 1.0) >= kShapeEps)
            ib.push_back(w);
    auto src = graph.source({t, kWeight, -1});
    MeshPtr edit = n->as<MeshPtr>(kEditMesh);
    auto editSrc = graph.source({t, kEditMesh, -1});
    return {{"node", n->name()},
            {"name", n->as<std::string>(kMorphName)},
            {"weight", std::get<double>(graph.value({t, kWeight, -1}))},
            {"sliderMin", n->as<double>(morphs::kSliderMin)},
            {"sliderMax", n->as<double>(morphs::kSliderMax)},
            {"live", graph.source({t, kTargetMesh, -1}).has_value()},
            {"inBetweens", ib},
            {"interpolation", interpolationNames()[size_t(interpolationOf(*n))]},
            {"response", enumName(responseNames(), n->as<int64_t>(kResponse))},
            {"driver", src ? json(graph.plugName(*src)) : json(nullptr)},
            {"editing", editSrc ? json(graph.node(editSrc->node)->name()) : json(nullptr)}};
}

/** A posed mesh's evaluated points, object space. */
std::vector<glm::dvec3> evaluatedPoints(Graph& graph, NodeId mesh)
{
    graph.evaluate();
    MeshPtr m = graph.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        throw CommandError(graph.node(mesh)->name() + " has no geometry");
    return {m->positions().begin(), m->positions().end()};
}

/** Copies a mesh node's evaluated shape into a new mesh at the same world placement. */
Node* duplicateShape(Graph& graph, NodeId mesh, const MeshPtr& shape, const std::string& name)
{
    auto copy = std::make_shared<geo::Mesh>(*shape);
    copy->pointAttrs().remove(modifiers::kBypassLayer);
    Node* m = guarded([&] { return graph.createNode(*types::find("mesh"), name, kNoNode, true); });
    graph.setValue({m->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(copy)));
    Transform::setLocalMatrix(graph, m->id(), worldOf(*graph.node(mesh)));
    return m;
}

std::string mirroredName(const std::string& name)
{
    static const std::pair<const char*, const char*> kPairs[] = {{"Left", "Right"}, {"left", "right"}, {"L", "R"}, {"l", "r"}};
    for (auto [a, b] : kPairs)
    {
        if (std::string m = rig::mirrorName(name, a, b); m != name)
            return m;
        if (std::string m = rig::mirrorName(name, b, a); m != name)
            return m;
    }
    return name + "_mirror";
}

/** The offsets reflected across the symmetry plane: each point takes its partner's offset, mirrored. */
std::vector<glm::dvec3> reflectOffsets(const std::vector<glm::dvec3>& in, const geo::SymmetryMap& map, int axis, int keepSide)
{
    std::vector<glm::dvec3> out = in;
    for (size_t p = 0; p < in.size(); ++p)
    {
        // keepSide 0 flips every point; otherwise only points off that side take their partner's offset.
        if (keepSide != 0 && map.side(geo::Index(p)) == keepSide)
            continue;
        if (map.onPlane(geo::Index(p)))
        {
            out[p] = in[p];
            out[p][axis] = keepSide != 0 ? 0.0 : -in[p][axis];
            continue;
        }
        geo::Index m = map.mirrorPoint(geo::Index(p));
        if (m == geo::kInvalid || m >= in.size())
            continue;
        glm::dvec3 d = in[m];
        d[axis] = -d[axis];
        out[p] = d;
    }
    return out;
}

/** Posed influences of the mesh's skin, most rotated first, for picking a corrective's driver. */
std::vector<NodeId> posedInfluences(Graph& graph, NodeId mesh)
{
    std::vector<std::pair<double, NodeId>> posed;
    NodeId s = skin::find(graph, mesh);
    if (s == kNoNode)
        return {};
    for (const skin::Influence& inf : skin::influences(graph, s))
    {
        if (inf.joint == kNoNode)
            continue;
        Node* j = graph.node(inf.joint);
        glm::dvec3 r = j->as<glm::dvec3>(Transform::kRotate);
        glm::dmat3 rot(eulerToMatrix(r, RotateOrder(std::clamp<int64_t>(j->as<int64_t>(Transform::kRotateOrder), 0, 5))));
        double angle = glm::degrees(2.0 * std::acos(std::clamp(std::abs(glm::quat_cast(rot).w), 0.0, 1.0)));
        if (angle > 5.0)
            posed.push_back({angle, inf.joint});
    }
    std::sort(posed.begin(), posed.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<NodeId> out;
    for (auto& p : posed)
        out.push_back(p.second);
    return out;
}

/** "Body_corrective" or "Body_corrective2" back to "Body". */
std::string baseOfSculpt(const std::string& sculpt)
{
    std::string s = sculpt;
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back())))
        s.pop_back();
    const std::string suffix = "_corrective";
    if (s.size() > suffix.size() && s.ends_with(suffix))
        return s.substr(0, s.size() - suffix.size());
    return {};
}

/** The weights a compile samples the path at: the slider's range (0 to 1 at least) through the response curve, and the knots. */
std::vector<double> compileSamples(const Node& t, const ShapeTable& tab)
{
    const double lo = std::min(t.as<double>(morphs::kSliderMin), 0.0);
    const double hi = std::max(t.as<double>(morphs::kSliderMax), 1.0);
    const int64_t response = t.as<int64_t>(kResponse);
    const int steps = std::clamp(int(std::ceil((hi - lo) * 16.0)), 16, 256);
    std::vector<double> out;
    for (int j = 0; j <= steps; ++j)
        out.push_back(morphs::respond(response, lo + (hi - lo) * double(j) / double(steps)));
    for (double k : tab.knots)
        if (k != 0.0)
            out.push_back(k);
    return out;
}

/**
 * Compiles an Arc or Pivot target for export: the full shape first, so the exported shape of the target's name looks
 * right on its own, then principal components of what the path adds to it until the export is within fitTolerance.
 */
std::shared_ptr<Compiled> compile(const Node& t, const geo::Mesh& base)
{
    LILA_ZONE();

    auto c = std::make_shared<Compiled>();
    c->table = buildTable(shapesOfNode(t), &base);
    c->mode = interpolationOf(t);
    c->pivot = t.as<glm::dvec3>(kPivot);
    const ShapeTable& tab = *c->table;
    const size_t np = tab.points.size();
    for (uint32_t p : tab.points)
        c->base.push_back(base.position(geo::Index(p)));

    std::vector<glm::dvec3> full = c->path(1.0);
    double fullNorm = 0.0;
    for (const glm::dvec3& d : full)
        fullNorm += glm::dot(d, d);
    c->shapes.push_back(full);
    c->norm2.push_back(fullNorm);

    const std::vector<double> samples = compileSamples(t, tab);
    const Eigen::Index S = Eigen::Index(samples.size()), D = Eigen::Index(np * 3);
    Eigen::MatrixXd R(S, D);
    for (Eigen::Index s = 0; s < S; ++s)
    {
        std::vector<glm::dvec3> x = c->path(samples[size_t(s)]);
        double along = 0.0;
        if (fullNorm > 0.0)
        {
            for (size_t i = 0; i < np; ++i)
                along += glm::dot(x[i], full[i]);
            along /= fullNorm;
        }
        for (size_t i = 0; i < np; ++i)
        {
            const glm::dvec3 r = x[i] - full[i] * along;
            R(s, Eigen::Index(i * 3)) = r.x;
            R(s, Eigen::Index(i * 3 + 1)) = r.y;
            R(s, Eigen::Index(i * 3 + 2)) = r.z;
        }
    }
    auto worst = [&] {
        double e = 0.0;
        for (Eigen::Index s = 0; s < S; ++s)
            for (Eigen::Index i = 0; i < D; i += 3)
                e = std::max(e, std::sqrt(R(s, i) * R(s, i) + R(s, i + 1) * R(s, i + 1) + R(s, i + 2) * R(s, i + 2)));
        return e;
    };

    const double tolerance = std::max(t.as<double>(kFitTolerance), 0.0);
    const int most = int(std::clamp<int64_t>(t.as<int64_t>(kFitShapes), 1, 32));
    double error = worst();
    if (error > tolerance && most > 1 && np > 0)
    {
        const Eigen::MatrixXd gram = R * R.transpose();
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(gram);
        const double biggest = eig.eigenvalues().maxCoeff();
        for (Eigen::Index k = S - 1; k >= 0 && int(c->shapes.size()) < most && error > tolerance; --k)
        {
            const double lambda = eig.eigenvalues()(k);
            if (lambda <= 0.0 || lambda <= biggest * 1e-14)
                break;
            const Eigen::VectorXd v = R.transpose() * eig.eigenvectors().col(k) / std::sqrt(lambda);
            const Eigen::VectorXd coef = R * v;
            R -= coef * v.transpose();
            error = worst();
            // Scaled so the shape's weight stays within -1..1 over the range, its larger swing positive.
            Eigen::Index at = 0;
            coef.cwiseAbs().maxCoeff(&at);
            const double scale = coef(at);
            std::vector<glm::dvec3> shape(np);
            for (size_t i = 0; i < np; ++i)
                shape[i] = glm::dvec3(v(Eigen::Index(i * 3)), v(Eigen::Index(i * 3 + 1)), v(Eigen::Index(i * 3 + 2))) * scale;
            c->shapes.push_back(std::move(shape));
            c->norm2.push_back(scale * scale);
        }
    }
    c->fit.shapes = int(c->shapes.size());
    c->fit.error = error;
    c->fit.capped = error > tolerance;
    if (c->fit.capped)
        log::warn("{}: its {} exported shapes stay up to {:.4g} cm off its {} path; raise fitShapes or fitTolerance", t.name(), c->fit.shapes,
                  error, c->mode == Interpolation::Arc ? "arc" : "pivot");
    return c;
}

/** The target's compile, made again only when what it was made from changes. Evaluates the graph. */
std::shared_ptr<const Compiled> compiledOf(Graph& graph, NodeId target)
{
    auto* node = dynamic_cast<MorphTargetNode*>(graph.node(target));
    const NodeId morph = node ? morphs::morphOf(graph, target) : kNoNode;
    MeshPtr base = morph != kNoNode ? morphInput(graph, morph) : nullptr;
    if (!base)
        return nullptr;
    MorphTargetNode::CompileKey key{node->as<DataPtr>(kDeltas),
                                    node->as<DataPtr>(kInBetweens),
                                    node->as<MeshPtr>(kTargetMesh),
                                    node->as<MeshPtr>(kEditMesh),
                                    base,
                                    node->as<double>(kEditWeight),
                                    node->as<int64_t>(kInterpolation),
                                    node->as<int64_t>(kResponse),
                                    node->as<int64_t>(kFitShapes),
                                    node->as<glm::dvec3>(kPivot),
                                    node->as<double>(kFitTolerance),
                                    node->as<double>(morphs::kSliderMin),
                                    node->as<double>(morphs::kSliderMax)};
    std::lock_guard lock(node->compileLock);
    if (!node->compiled || !(node->compileKey == key))
    {
        node->compiled = compile(*node, *base);
        node->compileKey = std::move(key);
    }
    return node->compiled;
}
} // namespace

// ------------------------------------------------------------------------------------------------ data kinds

json MorphDeltas::toJson() const { return {{"kind", kind()}, {"indices", indices}, {"deltas", flatten(deltas)}}; }

DataPtr MorphDeltas::fromJson(const json& j)
{
    auto d = std::make_shared<MorphDeltas>();
    readArray(j, "indices", d->indices);
    std::vector<double> f;
    readArray(j, "deltas", f);
    d->deltas = unflatten(f);
    d->indices.resize(std::min(d->indices.size(), d->deltas.size()));
    d->deltas.resize(d->indices.size());
    return d;
}

size_t MorphInBetweens::bytes() const
{
    size_t n = 0;
    for (const Shape& s : shapes)
        n += s.indices.size() * sizeof(uint32_t) + s.deltas.size() * sizeof(glm::dvec3);
    return n;
}

json MorphInBetweens::toJson() const
{
    json list = json::array();
    for (const Shape& s : shapes)
        list.push_back({{"weight", s.weight}, {"indices", s.indices}, {"deltas", flatten(s.deltas)}});
    return {{"kind", kind()}, {"shapes", list}};
}

DataPtr MorphInBetweens::fromJson(const json& j)
{
    auto d = std::make_shared<MorphInBetweens>();
    if (j.contains("shapes") && j["shapes"].is_array())
        for (const json& s : j["shapes"])
        {
            Shape shape;
            shape.weight = s.value("weight", 0.5);
            readArray(s, "indices", shape.indices);
            std::vector<double> f;
            readArray(s, "deltas", f);
            shape.deltas = unflatten(f);
            shape.indices.resize(std::min(shape.indices.size(), shape.deltas.size()));
            shape.deltas.resize(shape.indices.size());
            d->shapes.push_back(std::move(shape));
        }
    std::sort(d->shapes.begin(), d->shapes.end(), [](const Shape& a, const Shape& b) { return a.weight < b.weight; });
    return d;
}

// ------------------------------------------------------------------------------------------------ api

namespace morphs
{
bool isTarget(const Node& n) { return n.type().isA("morphTarget"); }

NodeId find(Graph& graph, NodeId mesh)
{
    for (NodeId id : modifiers::stack(graph, mesh))
        if (graph.node(id)->type().isA("morph"))
            return id;
    return kNoNode;
}

NodeId morphOf(Graph& graph, NodeId target)
{
    NodeId cur = target;
    for (int guard = 0; guard < 100000; ++guard)
    {
        std::vector<PlugRef> dsts = graph.destinations({cur, kLink, -1});
        if (dsts.empty())
            return kNoNode;
        cur = dsts[0].node;
        Node* n = graph.node(cur);
        if (n->type().isA("morph"))
            return cur;
        if (!isTarget(*n))
            return kNoNode;
    }
    return kNoNode;
}

std::vector<NodeId> targetNodes(Graph& graph, NodeId morph)
{
    std::vector<NodeId> out;
    Node* m = graph.node(morph);
    if (!m)
        return out;
    PlugRef plug{morph, m->type().attrIndex("targets"), -1};
    while (auto src = graph.source(plug))
    {
        Node* t = graph.node(src->node);
        if (!isTarget(*t) || out.size() > 100000)
            break;
        out.push_back(t->id());
        plug = {t->id(), kPrevious, -1};
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<double> shapeCoefficients(const std::vector<double>& knots, double w, bool smooth)
{
    std::vector<double> c(knots.size(), 0.0);
    if (knots.empty() || w == 0.0)
        return c;
    // Knot list with the empty shape at 0 (index -1).
    std::vector<std::pair<double, int>> k{{0.0, -1}};
    for (size_t i = 0; i < knots.size(); ++i)
        k.push_back({knots[i], int(i)});
    std::stable_sort(k.begin(), k.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<double> t;
    for (const auto& kw : k)
        t.push_back(kw.first);
    std::vector<double> kc = knotCoefficients(t, w, smooth);
    for (size_t j = 0; j < k.size(); ++j)
        if (k[j].second >= 0)
            c[size_t(k[j].second)] += kc[j];
    return c;
}

double respond(int64_t type, double w)
{
    if (type <= 0)
        return w;
    const double a = std::abs(w);
    const double v = a <= 1.0 ? rigmath::easing::rigVm(type, a) : a;
    return w < 0.0 ? -v : v;
}

bool mapped(const Node& t) { return knotWeightsOf(t).size() > 1 || t.as<int64_t>(kResponse) != 0 || compiles(interpolationOf(t)); }

std::vector<std::string> shapeNames(Graph& graph, NodeId target)
{
    const Node* n = graph.node(target);
    std::vector<std::string> out;
    if (!n)
        return out;
    std::string name = n->as<std::string>(kMorphName);
    if (name.empty())
        name = n->name();
    if (compiles(interpolationOf(*n)))
    {
        std::shared_ptr<const Compiled> c = compiledOf(graph, target);
        const size_t count = c ? c->shapes.size() : 1;
        for (size_t k = 0; k < count; ++k)
            out.push_back(k == 0 ? name : std::format("{}_p{}", name, k + 1));
        return out;
    }
    for (double w : knotWeightsOf(*n))
        out.push_back(std::abs(w - 1.0) < kShapeEps ? name : std::format("{}_{}", name, int(std::lround(w * 100.0))));
    return out;
}

std::vector<double> shapeWeights(Graph& graph, NodeId target, double w)
{
    const Node* n = graph.node(target);
    if (!n)
        return {};
    const double r = respond(n->as<int64_t>(kResponse), w);
    const Interpolation mode = interpolationOf(*n);
    if (compiles(mode))
    {
        std::shared_ptr<const Compiled> c = compiledOf(graph, target);
        return c ? c->weightsAt(r) : std::vector<double>(1, r);
    }
    return coefficients(knotWeightsOf(*n), r, mode == Interpolation::Smooth);
}

Fit fit(Graph& graph, NodeId target)
{
    const Node* n = graph.node(target);
    if (!n || !compiles(interpolationOf(*n)))
        return {};
    std::shared_ptr<const Compiled> c = compiledOf(graph, target);
    return c ? c->fit : Fit{};
}

std::vector<std::vector<glm::dvec3>> paths(Graph& graph, NodeId target, int steps, size_t maxPaths)
{
    std::vector<std::vector<glm::dvec3>> out;
    const Node* n = graph.node(target);
    const NodeId morph = n ? morphOf(graph, target) : kNoNode;
    MeshPtr base = morph != kNoNode ? morphInput(graph, morph) : nullptr;
    if (!base || steps < 1)
        return out;
    std::shared_ptr<const ShapeTable> tab = buildTable(shapesOfNode(*n), base.get());
    const Interpolation mode = interpolationOf(*n);
    const glm::dvec3 pivot = n->as<glm::dvec3>(kPivot);
    const int64_t response = n->as<int64_t>(kResponse);
    const double lo = n->as<double>(kSliderMin), hi = n->as<double>(kSliderMax);
    const auto* pw = dynamic_cast<const PointWeights*>(n->as<DataPtr>(kPointWeights).get());
    std::vector<std::vector<glm::dvec3>> all(tab->points.size());
    parallelPoints(all.size(), [&](size_t i) {
        const uint32_t p = tab->points[i];
        const glm::dvec3 at = base->position(geo::Index(p));
        const double m = pw && p < pw->values.size() ? double(pw->values[p]) : 1.0;
        all[i].reserve(size_t(steps) + 1);
        for (int j = 0; j <= steps; ++j)
        {
            const double w = respond(response, lo + (hi - lo) * double(j) / double(steps));
            all[i].push_back(at + pathPoint(*tab, i, mode, w, at, pivot) * m);
        }
    });
    std::vector<std::pair<double, size_t>> reach;
    for (size_t i = 0; i < all.size(); ++i)
    {
        double far = 0.0;
        for (const glm::dvec3& q : all[i])
            far = std::max(far, glm::length(q - all[i].front()));
        if (far > 1e-6)
            reach.push_back({far, i});
    }
    std::sort(reach.begin(), reach.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t k = 0; k < reach.size() && k < maxPaths; ++k)
        out.push_back(std::move(all[reach[k].second]));
    return out;
}

std::vector<Target> targets(Graph& graph, NodeId morph)
{
    std::vector<Target> out;
    Node* m = graph.node(morph);
    if (!m)
        return out;
    MeshPtr base = morphInput(graph, morph);
    const double envelope = m->as<double>(DeformerNode::kEnvelope);
    for (NodeId t : targetNodes(graph, morph))
    {
        Node* n = graph.node(t);
        const std::vector<std::string> names = shapeNames(graph, t);
        const std::vector<double> c = shapeWeights(graph, t, n->as<double>(kWeight));
        const auto* pw = dynamic_cast<const PointWeights*>(n->as<DataPtr>(kPointWeights).get());
        auto mask = [pw](size_t p) { return pw && p < pw->values.size() ? double(pw->values[p]) : 1.0; };
        std::shared_ptr<const Compiled> comp = compiles(interpolationOf(*n)) ? compiledOf(graph, t) : nullptr;
        const std::vector<double> ws = knotWeightsOf(*n);
        for (size_t s = 0; s < names.size(); ++s)
        {
            Target e;
            e.node = t;
            e.name = names[s];
            e.weight = (s < c.size() ? c[s] : 0.0) * envelope;
            if (comp && s < comp->shapes.size())
            {
                for (size_t i = 0; i < comp->table->points.size(); ++i)
                {
                    const uint32_t p = comp->table->points[i];
                    const glm::dvec3 d = comp->shapes[s][i] * mask(p);
                    if (glm::length(d) > 1e-7)
                    {
                        e.indices.push_back(p);
                        e.deltas.push_back(d);
                    }
                }
            }
            else if (base && !comp && s < ws.size())
            {
                std::vector<glm::dvec3> offsets = shapeOffsets(graph, t, ws[s], *base);
                for (size_t p = 0; p < offsets.size(); ++p)
                    offsets[p] *= mask(p);
                std::shared_ptr<MorphDeltas> d = sparse(offsets);
                e.indices = std::move(d->indices);
                e.deltas = std::move(d->deltas);
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<double> weights(Graph& graph, NodeId morph)
{
    graph.evaluate();
    std::vector<double> out;
    Node* m = graph.node(morph);
    if (!m)
        return out;
    const double envelope = m->as<double>(DeformerNode::kEnvelope);
    for (NodeId t : targetNodes(graph, morph))
        for (double c : shapeWeights(graph, t, graph.node(t)->as<double>(kWeight)))
            out.push_back(c * envelope);
    return out;
}

NodeId create(Graph& graph, NodeId mesh, const std::string& name)
{
    return deformers::addDeformer(graph, "morph", {mesh}, name, true).front();
}

NodeId addTarget(Graph& graph, NodeId morph, const std::string& morphName, std::shared_ptr<const MorphDeltas> deltas, double weight)
{
    NodeId t = newTarget(graph, morph, morphName);
    graph.setValue({t, kDeltas, -1}, DataPtr(std::move(deltas)));
    graph.setValue({t, kWeight, -1}, weight);
    return t;
}

void setInBetween(Graph& graph, NodeId target, double weight, std::shared_ptr<const MorphDeltas> deltas)
{
    if (std::abs(weight) < kShapeEps || std::abs(weight - 1.0) < kShapeEps)
        throw CommandError("an in-between's weight can't be 0 or 1");
    Node* t = graph.node(target);
    auto next = std::make_shared<MorphInBetweens>();
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(t->as<DataPtr>(kInBetweens).get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            if (std::abs(s.weight - weight) >= kShapeEps)
                next->shapes.push_back(s);
    MorphInBetweens::Shape s;
    s.weight = weight;
    if (deltas)
    {
        s.indices = deltas->indices;
        s.deltas = deltas->deltas;
    }
    next->shapes.push_back(std::move(s));
    std::sort(next->shapes.begin(), next->shapes.end(), [](auto& a, auto& b) { return a.weight < b.weight; });
    graph.setValue({target, kInBetweens, -1}, DataPtr(std::move(next)));
}

void setPointWeights(Graph& graph, NodeId target, DataPtr weights) { graph.setValue({target, kPointWeights, -1}, std::move(weights)); }

std::shared_ptr<MorphDeltas> invertSculpt(Graph& graph, NodeId target, const std::vector<glm::dvec3>& sculpt, double* residual)
{
    LILA_ZONE();

    const NodeId morph = morphOf(graph, target);
    const NodeId mesh = morph != kNoNode ? modifiers::meshOf(graph, morph) : kNoNode;
    if (mesh == kNoNode)
        throw CommandError(graph.node(target)->name() + " is not on a mesh's morph");
    MeshPtr base = morphInput(graph, morph);
    if (!base)
        throw CommandError(graph.node(mesh)->name() + " has no geometry");
    const size_t n = base->pointCapacity();
    if (sculpt.size() != n)
        throw CommandError(std::format("the sculpt has {} points, {} has {}", sculpt.size(), graph.node(mesh)->name(), n));

    UndoStack& undo = UndoStack::get();
    const bool wasRecording = undo.enabled();
    const PlugRef deltasPlug{target, kDeltas, -1};
    const DataPtr saved = std::get<DataPtr>(graph.storedValue(deltasPlug));
    const auto liveSrc = graph.source({target, kTargetMesh, -1});
    struct Restore
    {
        Graph& graph;
        PlugRef plug;
        DataPtr saved;
        UndoStack& undo;
        bool was;
        std::optional<PlugRef> live;
        NodeId target;
        ~Restore()
        {
            try
            {
                graph.setValue(plug, saved);
                if (live)
                    graph.connect(*live, {target, kTargetMesh, -1});
                graph.evaluate();
            }
            catch (const std::exception& e)
            {
                log::error("invertSculpt: {}", e.what());
            }
            undo.setEnabled(was);
        }
    };
    undo.setEnabled(false);
    Restore restore{graph, deltasPlug, saved, undo, wasRecording, liveSrc, target};
    if (liveSrc)
        graph.disconnect({target, kTargetMesh, -1});

    std::vector<uint32_t> all(n);
    for (size_t p = 0; p < n; ++p)
        all[p] = uint32_t(p);
    auto evalWith = [&](const std::vector<glm::dvec3>& f) {
        auto d = std::make_shared<MorphDeltas>();
        d->indices = all;
        d->deltas = f;
        graph.setValue(deltasPlug, DataPtr(std::move(d)));
        std::vector<glm::dvec3> out = evaluatedPoints(graph, mesh);
        if (out.size() != n)
            throw CommandError(graph.node(mesh)->name() + ": a modifier above the morph changes the point count; correctives need point-wise deformers");
        return out;
    };

    std::vector<glm::dvec3> f(n, glm::dvec3(0.0));
    std::vector<glm::dvec3> p0 = evalWith(f);
    // The skin is affine in each rest point, so a unit step gives the exact per-point Jacobian.
    std::vector<glm::dmat3> jac(n, glm::dmat3(0.0));
    for (int k = 0; k < 3; ++k)
    {
        std::vector<glm::dvec3> step(n, glm::dvec3(0.0));
        for (glm::dvec3& s : step)
            s[k] = 1.0;
        std::vector<glm::dvec3> pk = evalWith(step);
        for (size_t p = 0; p < n; ++p)
            jac[p][k] = pk[p] - p0[p];
    }
    std::vector<glm::dmat3> inv(n);
    bool any = false;
    for (size_t p = 0; p < n; ++p)
    {
        double det = glm::determinant(jac[p]);
        any = any || std::abs(det) > 1e-12;
        inv[p] = std::abs(det) > 1e-12 ? glm::inverse(jac[p]) : glm::dmat3(1.0);
    }
    if (!any)
        throw CommandError(graph.node(target)->name() + ": its weight is 0 at this pose, so no offset can reach the sculpt");

    std::vector<glm::dvec3> cur = p0;
    double worst = 0.0;
    for (int iter = 0; iter < 12; ++iter)
    {
        worst = 0.0;
        for (size_t p = 0; p < n; ++p)
        {
            if (!base->pointValid(geo::Index(p)))
                continue;
            glm::dvec3 r = sculpt[p] - cur[p];
            worst = std::max(worst, glm::length(r));
            f[p] += inv[p] * r;
        }
        if (worst < 1e-9)
            break;
        cur = evalWith(f);
    }
    worst = 0.0;
    for (size_t p = 0; p < n; ++p)
        if (base->pointValid(geo::Index(p)))
            worst = std::max(worst, glm::length(sculpt[p] - cur[p]));
    if (residual)
        *residual = worst;
    return sparse(f);
}

void registerTypes()
{
    if (types::find("morph"))
        return;
    data::registerKind("morphDeltas", &MorphDeltas::fromJson);
    data::registerKind("morphInBetweens", &MorphInBetweens::fromJson);

    deformers::registerDeformer({"morph", "mood-smile",
                                 "Morph targets: blends the mesh toward target shapes by their weights, with in-betweens and pose driven "
                                 "correctives. Sits under any skinCluster. Exported to Unreal as morph targets with their curves.",
                                 {connected("targets", PlugType::Data, DataPtr(), "The last morphTarget in the chain.")},
                                 [] { return std::make_unique<MorphNode>(); }});

    NodeType target;
    target.name = "morphTarget";
    target.icon = "mood-smile";
    target.doc = "One morph target: its weight, its full shape as offsets or a live target mesh, and any in-between shapes.";
    target.ownAttrs = {
        keyable("weight", PlugType::Float, 0.0, "How much of the target applies, usually 0 to 1."),
        stored("morphName", PlugType::String, std::string(), "The morph target's name in exports."),
        hiddenStored("deltas", PlugType::Data, DataPtr(), "Full shape offsets from the base, object space."),
        connected("targetMesh", PlugType::Mesh, MeshPtr(), "A live target's outMesh; overrides the stored offsets."),
        connected("previous", PlugType::Data, DataPtr(), "The target before this one."),
        {.name = "link", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "This target plus everything before it."},
        hiddenStored("inBetweens", PlugType::Data, DataPtr(), "In-between shapes, each with the weight it is reached at."),
        connected("editMesh", PlugType::Mesh, MeshPtr(), "The mesh being sculpted in edit mode; stands in for the shape it edits."),
        hiddenStored("editWeight", PlugType::Float, 1.0, "Which shape the edit mesh edits: 1 the full shape, else an in-between's weight."),
        hiddenStored("pointWeights", PlugType::Data, DataPtr(), "Per-point weights scaling every shape of this target; points past the end weigh 1."),
        {.name = "sliderMin", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable,
         .doc = "The weight at the left end of the Morph Targets panel's slider; negative weights push the shape the other way.",
         .category = "Slider"},
        {.name = "sliderMax", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrInput | AttrStorable,
         .doc = "The weight at the right end of the Morph Targets panel's slider; past 1 overshoots the shape.", .category = "Slider"},
        {.name = "interpolation", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .enumNames = interpolationNames(),
         .enumDocs = {"Straight from shape to shape.", "A smooth curve through the in-betweens.",
                      "Circular arcs through each point's in-betweens: lids over eyes, lips round teeth. Needs an in-between.",
                      "Swings about the pivot, the distance to it blending: works without in-betweens."},
         .doc = "How points travel between the target's shapes. Arc and Pivot export as the full shape plus fitted shapes.",
         .category = "Path"},
        {.name = "pivot", .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0), .flags = AttrInput | AttrStorable,
         .childNames = {"pivotX", "pivotY", "pivotZ"}, .childShort = {"pvx", "pvy", "pvz"},
         .doc = "What Pivot interpolation swings about, in the mesh's object space. morphPath pivot= sets it from a node.", .category = "Path"},
        {.name = "response", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .enumNames = responseNames(), .doc = "Eases the weight before the path: Control Rig's easing curves.", .category = "Path"},
        {.name = "fitShapes", .type = PlugType::Int, .defaultValue = int64_t(4), .flags = AttrInput | AttrStorable,
         .doc = "Arc and Pivot: the most shapes an export may use for the path, the full shape included.", .minValue = 1.0,
         .maxValue = 32.0, .category = "Export"},
        {.name = "fitTolerance", .type = PlugType::Float, .defaultValue = 0.01, .flags = AttrInput | AttrStorable,
         .doc = "Arc and Pivot: how far, in cm, the exported shapes may stray from the path before another is added.",
         .minValue = 0.0, .category = "Export"},
    };
    target.create = [] { return std::make_unique<MorphTargetNode>(); };
    types::registerType(std::move(target));

    // Older scene files name the morph types and their offsets this way.
    types::addFileAlias("blendShape", "morph");
    types::addFileAlias("blendTarget", "morphTarget");
    data::registerKind("blendDeltas", &MorphDeltas::fromJson);
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "morph",
        .doc = "Add a morph deformer (morph targets) to the base mesh, the last argument, with a target per other mesh. Targets need "
               "the base's point count. It sits under any skinCluster, so morphs apply before skinning, as in Unreal. Returns the "
               "morph.",
        .positionalDoc = "targets..., base",
        .flags = {{"live", "", T::Bool, false, "Keep targets connected so editing a target mesh updates the morph."},
                  {"name", "", T::String, nullptr, "Name for the morph."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("morph: give targets and the base mesh last");
            NodeId base = meshNamed(objs.back(), "morph").id();
            objs.pop_back();
            if (find(graph, base) != kNoNode)
                throw CommandError("morph: " + graph.node(base)->name() + " already has a morph; use addMorphTarget");
            NodeId m = create(graph, base, a.has("name") ? a.get<std::string>("name") : "Morph");
            for (const std::string& name : objs)
                addTargetFromMesh(graph, m, meshNamed(name, "morph").id(), name, a.get<bool>("live"));
            return graph.node(m)->name();
        },
    });

    ops::registerCommand({
        .name = "addMorphTarget",
        .doc = "Add targets to a morph (or the morph on a mesh, made if missing): the first argument, then target meshes. With no "
               "target meshes it adds one empty target named by name, to sculpt with editMorphTarget. Returns the new target nodes, "
               "whose weight attrs drive the morphs.",
        .positionalDoc = "morph or mesh, targets...",
        .flags = {{"live", "", T::Bool, false, "Keep the targets connected so editing them updates the morphs."},
                  {"name", "n", T::String, nullptr, "Export name for a single or empty target. Default: the target mesh's name."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("addMorphTarget: give the morph (or its mesh), then the targets");
            Node& first = nodeNamed(objs.front(), "addMorphTarget");
            NodeId m = kNoNode;
            if (first.type().isA("mesh") && find(graph, first.id()) == kNoNode && !first.type().isA("lattice"))
                m = create(graph, first.id(), "Morph");
            else
                m = resolveMorph(graph, objs.front(), "addMorphTarget");
            json out = json::array();
            if (objs.size() == 1)
            {
                out.push_back(graph.node(newTarget(graph, m, a.has("name") ? a.get<std::string>("name") : "Target"))->name());
                return out;
            }
            for (size_t i = 1; i < objs.size(); ++i)
            {
                NodeId t = meshNamed(objs[i], "addMorphTarget").id();
                std::string name = a.has("name") && objs.size() == 2 ? a.get<std::string>("name") : objs[i];
                out.push_back(graph.node(addTargetFromMesh(graph, m, t, name, a.get<bool>("live")))->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "removeMorphTarget",
        .doc = "Remove morph targets (morphTarget nodes) from their morph. Pose drivers feeding them stay.",
        .positionalDoc = "targets",
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            for (const std::string& name : a.objects())
            {
                Node& n = targetNamed(name, "removeMorphTarget");
                if (auto e = graph.source({n.id(), kEditMesh, -1}))
                    graph.deleteNode(e->node);
                removeTarget(graph, n.id());
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "morphTargets",
        .doc = "A morph's targets in order: [{node, name, weight, sliderMin, sliderMax, live, inBetweens, driver, editing}]. "
               "driver is the plug setting the weight (a pose driver for a corrective), editing the edit mesh in edit mode, "
               "sliderMin and sliderMax the panel slider's range. Set a weight with setAttr(node + \".weight\", w).",
        .positionalDoc = "morph, mesh or target",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("morphTargets: no morph");
            json out = json::array();
            for (NodeId t : targetNodes(graph, resolveMorph(graph, objs.back(), "morphTargets")))
                out.push_back(targetInfo(graph, t));
            return out;
        },
    });

    ops::registerCommand({
        .name = "renameMorphTarget",
        .doc = "Rename a morph target: its export name (made unique on the morph) and its node, which becomes <name>Target. "
               "Returns the node's new name.",
        .positionalDoc = "target, newName",
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            if (a.positional.size() < 2)
                throw CommandError("renameMorphTarget: give the target and the new name");
            Node& t = targetNamed(a.positional[0].get<std::string>(), "renameMorphTarget");
            std::string wanted = a.positional[1].get<std::string>();
            if (wanted.empty())
                throw CommandError("renameMorphTarget: the name is empty");
            NodeId m = morphOf(graph, t.id());
            std::string name = m != kNoNode ? uniqueMorphName(graph, m, wanted, t.id()) : wanted;
            graph.setValue({t.id(), kMorphName, -1}, name);
            return guarded([&] { return graph.rename(t.id(), name + "Target"); });
        },
    });

    ops::registerCommand({
        .name = "extractMorphTarget",
        .doc = "Build a new mesh in a target's shape (base plus offsets): the full shape, or the in-between at inBetween=. "
               "Sculpt it and put it back with setMorphTarget, or use editMorphTarget. Returns the new mesh.",
        .positionalDoc = "target",
        .flags = {{"inBetween", "", T::Float, nullptr, "The in-between's weight; default the full shape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("extractMorphTarget: no target");
            Node& n = targetNamed(objs.back(), "extractMorphTarget");
            NodeId m = morphOf(graph, n.id());
            if (m == kNoNode)
                throw CommandError("extractMorphTarget: " + n.name() + " is not connected to a morph");
            MeshPtr base = morphInput(graph, m);
            if (!base)
                throw CommandError("extractMorphTarget: the base has no geometry");
            const double w = a.has("inBetween") ? a.get<double>("inBetween") : 1.0;
            std::vector<glm::dvec3> off = guarded([&] { return shapeOffsets(graph, n.id(), w, *base); });
            auto shape = std::make_shared<geo::Mesh>(*base);
            for (size_t p = 0; p < off.size(); ++p)
                shape->setPosition(geo::Index(p), shape->position(geo::Index(p)) + off[p]);
            std::string morph = n.as<std::string>(kMorphName);
            NodeId baseMesh = modifiers::meshOf(graph, m);
            Node* mesh = duplicateShape(graph, baseMesh, shape, morph.empty() ? n.name() : morph);
            return mesh->name();
        },
    });

    ops::registerCommand({
        .name = "setMorphTarget",
        .doc = "Replace a target's shape with a mesh's (same point count as the base): the full shape, or the in-between at "
               "inBetween= (added if missing). A live target becomes stored offsets.",
        .positionalDoc = "target, mesh",
        .flags = {{"inBetween", "", T::Float, nullptr, "The in-between's weight; default the full shape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("setMorphTarget: give the target, then the mesh");
            Node& t = targetNamed(objs[0], "setMorphTarget");
            Node& mesh = meshNamed(objs[1], "setMorphTarget");
            NodeId m = morphOf(graph, t.id());
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            MeshPtr shape = mesh.as<MeshPtr>(MeshNode::kOutMesh);
            if (!base || !shape || base->pointCapacity() != shape->pointCapacity())
                throw CommandError("setMorphTarget: " + mesh.name() + " doesn't match the base's points");
            guarded([&] {
                setShape(graph, t.id(), a.has("inBetween") ? a.get<double>("inBetween") : 1.0, deltasBetween(*base, *shape));
                return 0;
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "addInBetween",
        .doc = "Add an in-between shape to a target, reached at weight (say 0.5) on the way to the full shape at 1: from mesh when "
               "given, else the shape the target has at that weight now, ready to edit with editMorphTarget(inBetween=).",
        .positionalDoc = "target, mesh?",
        .flags = {{"weight", "w", T::Float, 0.5, "Where the in-between is reached; not 0 or 1."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("addInBetween: no target");
            Node& t = targetNamed(objs[0], "addInBetween");
            NodeId m = morphOf(graph, t.id());
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            if (!base)
                throw CommandError("addInBetween: " + t.name() + " is not on a morph with geometry");
            const double w = a.get<double>("weight");
            std::shared_ptr<MorphDeltas> d;
            if (objs.size() > 1)
            {
                MeshPtr shape = meshNamed(objs[1], "addInBetween").as<MeshPtr>(MeshNode::kOutMesh);
                if (!shape || shape->pointCapacity() != base->pointCapacity())
                    throw CommandError("addInBetween: " + objs[1] + " doesn't match the base's points");
                d = deltasBetween(*base, *shape);
            }
            else
                d = sparse(interpolatedOffsets(graph, t.id(), w, *base));
            guarded([&] {
                setInBetween(graph, t.id(), w, d);
                return 0;
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "removeInBetween",
        .doc = "Remove a target's in-between at weight.",
        .positionalDoc = "target",
        .flags = {{"weight", "w", T::Float, 0.5, "The in-between's weight."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("removeInBetween: no target");
            Node& t = targetNamed(objs[0], "removeInBetween");
            const double w = a.get<double>("weight");
            const auto* ib = dynamic_cast<const MorphInBetweens*>(t.as<DataPtr>(kInBetweens).get());
            auto next = std::make_shared<MorphInBetweens>();
            bool found = false;
            for (const MorphInBetweens::Shape& s : ib ? ib->shapes : std::vector<MorphInBetweens::Shape>{})
            {
                if (std::abs(s.weight - w) < kShapeEps)
                    found = true;
                else
                    next->shapes.push_back(s);
            }
            if (!found)
                throw CommandError(std::format("removeInBetween: {} has no in-between at {}", t.name(), w));
            graph.setValue({t.id(), kInBetweens, -1}, next->shapes.empty() ? DataPtr() : DataPtr(std::move(next)));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "morphPath",
        .doc = "Set or query how a target's points travel to its shapes. interpolation linear, smooth (a curve through the "
               "in-betweens), arc (circular arcs through each point's in-betweens) or pivot (a swing about pivot=, no in-between "
               "needed); response eases the weight first, with Control Rig's easing curves. Returns {interpolation, response, "
               "pivot, shapes, fitError, capped}: shapes are the exported shape names, fitError how far arc and pivot exports "
               "stray from the path, in cm.",
        .positionalDoc = "target",
        .flags = {{"interpolation", "i", T::String, nullptr, "linear | smooth | arc | pivot", {"linear", "smooth", "arc", "pivot"}},
                  {"response", "r", T::String, nullptr, "An easing name, as the response attr lists them; linear for none."},
                  {"pivot", "p", T::Any, nullptr, "A node whose position becomes the pivot, or [x, y, z] in the mesh's object space."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("morphPath: no target");
            Node& t = targetNamed(objs[0], "morphPath");
            if (a.has("interpolation"))
                graph.setValue({t.id(), kInterpolation, -1}, enumIndex(interpolationNames(), a.get<std::string>("interpolation"), "morphPath"));
            if (a.has("response"))
                graph.setValue({t.id(), kResponse, -1}, enumIndex(responseNames(), a.get<std::string>("response"), "morphPath"));
            if (a.has("pivot"))
            {
                const json& p = a.flags["pivot"];
                glm::dvec3 at(0.0);
                if (p.is_array() && p.size() == 3)
                    at = {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
                else if (p.is_string())
                {
                    const NodeId m = morphOf(graph, t.id());
                    const NodeId mesh = m != kNoNode ? modifiers::meshOf(graph, m) : kNoNode;
                    if (mesh == kNoNode)
                        throw CommandError("morphPath: " + t.name() + " is not on a mesh's morph");
                    graph.evaluate();
                    const glm::dmat4 world = worldOf(nodeNamed(p.get<std::string>(), "morphPath"));
                    at = glm::dvec3(glm::inverse(worldOf(*graph.node(mesh))) * world[3]);
                }
                else
                    throw CommandError("morphPath: pivot is a node name or [x, y, z]");
                graph.setValue({t.id(), kPivot, -1}, at);
            }
            const Fit f = fit(graph, t.id());
            const glm::dvec3 pv = t.as<glm::dvec3>(kPivot);
            return {{"interpolation", interpolationNames()[size_t(interpolationOf(t))]},
                    {"response", enumName(responseNames(), t.as<int64_t>(kResponse))},
                    {"pivot", {pv.x, pv.y, pv.z}},
                    {"shapes", shapeNames(graph, t.id())},
                    {"fitError", f.error},
                    {"capped", f.capped}};
        },
    });

    ops::registerCommand({
        .name = "mirrorMorphTarget",
        .doc = "Mirror a target through the base's symmetry (points matched across the axis plane in object space). mode flip "
               "makes a new target with the shape on the other side, named with left and right swapped; symmetrize copies one "
               "side onto the other in place. In-betweens go along. Returns the target written.",
        .positionalDoc = "target",
        .flags = {{"axis", "", T::String, "x", "Mirror axis, object space.", {"x", "y", "z"}},
                  {"mode", "", T::String, "flip", "flip | symmetrize", {"flip", "symmetrize"}},
                  {"direction", "", T::Int, 1, "symmetrize: 1 copies the positive side onto the negative, -1 the reverse.", {}, {}, -1.0, 1.0},
                  {"tolerance", "", T::Float, 1e-3, "How far a point may be from its partner's reflection, cm.", {}, {}, 0.0},
                  {"name", "", T::String, nullptr, "flip: the new target's name."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("mirrorMorphTarget: no target");
            Node& t = targetNamed(objs[0], "mirrorMorphTarget");
            NodeId m = morphOf(graph, t.id());
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            if (!base)
                throw CommandError("mirrorMorphTarget: " + t.name() + " is not on a morph with geometry");
            const int axis = axisIndex(a.get<std::string>("axis"), "mirrorMorphTarget");
            geo::SymmetryMap map = geo::SymmetryMap::build(*base, geo::axisPlane(axis), a.get<double>("tolerance"));
            if (map.unmatchedCount() > 0)
                log::warn("mirrorMorphTarget: {} points of the base have no mirror partner and keep their offsets", map.unmatchedCount());
            const bool flip = a.get<std::string>("mode") == "flip";
            const int keep = flip ? 0 : (a.get<int>("direction") >= 0 ? 1 : -1);
            NodeId out = t.id();
            if (flip)
            {
                std::string name = a.has("name") ? a.get<std::string>("name") : mirroredName(t.as<std::string>(kMorphName));
                out = newTarget(graph, m, name);
            }
            for (double w : knotWeightsOf(t))
            {
                std::vector<glm::dvec3> off = shapeOffsets(graph, t.id(), w, *base);
                setShape(graph, out, w, sparse(reflectOffsets(off, map, axis, keep)));
            }
            return graph.node(out)->name();
        },
    });

    ops::registerCommand({
        .name = "editMorphTarget",
        .doc = "Edit mode for sculpting a target: on makes an edit mesh in the target's shape (or the in-between at inBetween=) "
               "where the base is, hides the base and selects the edit mesh; the morph follows it live. off bakes the edit "
               "mesh into the target, deletes it and shows the base. Returns the edit mesh while on.",
        .positionalDoc = "target",
        .flags = {{"on", "", T::Bool, nullptr, "Default: toggle."}, {"inBetween", "", T::Float, nullptr, "Edit this in-between; default the full shape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("editMorphTarget: no target");
            Node& t = targetNamed(objs[0], "editMorphTarget");
            NodeId m = morphOf(graph, t.id());
            NodeId baseMesh = m != kNoNode ? modifiers::meshOf(graph, m) : kNoNode;
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            if (!base || baseMesh == kNoNode)
                throw CommandError("editMorphTarget: " + t.name() + " is not on a mesh's morph");
            auto editing = graph.source({t.id(), kEditMesh, -1});
            const bool on = a.has("on") ? a.get<bool>("on") : !editing;
            if (on)
            {
                if (editing)
                    return graph.node(editing->node)->name();
                const double w = a.has("inBetween") ? a.get<double>("inBetween") : 1.0;
                std::vector<glm::dvec3> off = guarded([&] { return shapeOffsets(graph, t.id(), w, *base); });
                auto shape = std::make_shared<geo::Mesh>(*base);
                for (size_t p = 0; p < off.size(); ++p)
                    shape->setPosition(geo::Index(p), shape->position(geo::Index(p)) + off[p]);
                Node* edit = duplicateShape(graph, baseMesh, shape, t.as<std::string>(kMorphName) + "_edit");
                graph.setValue({t.id(), kEditWeight, -1}, w);
                graph.connect({edit->id(), MeshNode::kOutMesh, -1}, {t.id(), kEditMesh, -1});
                setVisible(graph, baseMesh, false);
                graph.select({edit->id()});
                return edit->name();
            }
            if (!editing)
                return nullptr;
            const NodeId editNode = editing->node;
            graph.evaluate();
            MeshPtr edited = graph.node(editNode)->as<MeshPtr>(MeshNode::kOutMesh);
            const double w = t.as<double>(kEditWeight);
            graph.disconnect({t.id(), kEditMesh, -1});
            if (edited && edited->pointCapacity() == base->pointCapacity())
                setShape(graph, t.id(), w, deltasBetween(*base, *edited));
            else
                log::warn("editMorphTarget: the edit mesh's points no longer match the base; the target is unchanged");
            graph.deleteNode(editNode);
            setVisible(graph, baseMesh, true);
            graph.select({baseMesh});
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sculptCorrective",
        .doc = "Start a corrective: pose the rig, then this copies the mesh as it looks now into <mesh>_corrective, in place, and "
               "hides the mesh. Sculpt the fix on the copy, then run addCorrective. Returns the sculpt mesh.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("sculptCorrective: no mesh");
            Node& mesh = meshNamed(objs.back(), "sculptCorrective");
            graph.evaluate();
            MeshPtr posed = mesh.as<MeshPtr>(MeshNode::kOutMesh);
            if (!posed)
                throw CommandError("sculptCorrective: " + mesh.name() + " has no geometry");
            Node* sculpt = duplicateShape(graph, mesh.id(), posed, mesh.name() + "_corrective");
            setVisible(graph, mesh.id(), false);
            graph.select({sculpt->id()});
            return sculpt->name();
        },
    });

    ops::registerCommand({
        .name = "addCorrective",
        .doc = "Turn a sculpt of the posed mesh into a corrective morph target. The fix is taken back through the skin into rest "
               "space (pose-space inversion) so it applies before skinning and matches the sculpt at this pose, and a pose driver "
               "reading each driver joint against its rest drives the weight: 1 at this pose, 0 at rest. Several drivers "
               "multiply (a combination corrective). Give the sculpt then the base (the base can be left out for a sculpt made "
               "by sculptCorrective). Returns {target, drivers, residual}.",
        .positionalDoc = "sculpt, base?",
        .flags = {{"driver", "d", T::Any, nullptr, "Joint name or list of joints. Default: the skin's most rotated influence."},
                  {"name", "n", T::String, nullptr, "The target's name. Default: <driver>_corrective."},
                  {"axis", "", T::String, "x", "Each driver's twist axis (its bone axis).", {"x", "y", "z"}},
                  {"radius", "", T::Float, 0.0, "Degrees from the pose where the weight reaches 0; 0 uses the rest-to-pose angle.", {}, {}, 0.0, 180.0},
                  {"twistWeight", "", T::Float, 0.0, "How much twist about the axis counts, 0 ignoring it.", {}, {}, 0.0},
                  {"falloff", "", T::String, "smooth", "linear | smooth | smoother", {"linear", "smooth", "smoother"}},
                  {"keep", "", T::Bool, false, "Keep the sculpt mesh instead of deleting it."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("addCorrective: give the sculpt, then the base");
            Node& sculpt = meshNamed(objs[0], "addCorrective");
            std::string baseName = objs.size() > 1 ? objs[1] : baseOfSculpt(sculpt.name());
            if (baseName.empty())
                throw CommandError("addCorrective: give the base mesh after the sculpt");
            Node& base = meshNamed(baseName, "addCorrective");

            std::vector<NodeId> driverJoints;
            if (a.has("driver"))
            {
                const json& d = a.flags["driver"];
                for (const json& j : d.is_array() ? d : json::array({d}))
                    driverJoints.push_back(nodeNamed(j.get<std::string>(), "addCorrective").id());
            }
            else
            {
                std::vector<NodeId> posed = posedInfluences(graph, base.id());
                if (posed.empty())
                    throw CommandError("addCorrective: no posed joint found on " + base.name() + "'s skin; give driver=");
                driverJoints.push_back(posed.front());
            }

            graph.evaluate();
            MeshPtr sculptMesh = sculpt.as<MeshPtr>(MeshNode::kOutMesh);
            if (!sculptMesh)
                throw CommandError("addCorrective: " + sculpt.name() + " has no geometry");
            const glm::dmat4 toBase = glm::inverse(worldOf(base)) * worldOf(sculpt);
            std::vector<glm::dvec3> target(sculptMesh->pointCapacity());
            for (geo::Index p = 0; p < sculptMesh->pointCapacity(); ++p)
                target[p] = glm::dvec3(toBase * glm::dvec4(sculptMesh->position(p), 1.0));

            NodeId m = find(graph, base.id());
            if (m == kNoNode)
                m = create(graph, base.id(), "Morph");
            std::string name = a.has("name") ? a.get<std::string>("name") : graph.node(driverJoints.front())->name() + "_corrective";
            NodeId t = newTarget(graph, m, name);

            const int axis = axisIndex(a.get<std::string>("axis"), "addCorrective");
            const std::string fall = a.get<std::string>("falloff");
            const drivers::Falloff falloff = fall == "linear" ? drivers::Falloff::Linear
                                             : fall == "smoother" ? drivers::Falloff::Smoother
                                                                  : drivers::Falloff::Smooth;
            json made = json::array();
            NodeId prev = kNoNode;
            for (NodeId j : driverJoints)
            {
                NodeId d = drivers::createPoseDriver(graph, j, graph.node(t)->as<std::string>(kMorphName) + "_" + graph.node(j)->name() + "Driver",
                                                     axis, a.get<double>("radius"), a.get<double>("twistWeight"), falloff);
                if (prev != kNoNode)
                    graph.connect({prev, drivers::kOutput, -1}, {d, drivers::kInWeight, -1});
                prev = d;
                made.push_back(graph.node(d)->name());
            }
            graph.connect({prev, drivers::kOutput, -1}, {t, kWeight, -1});

            double residual = 0.0;
            std::shared_ptr<MorphDeltas> deltas = invertSculpt(graph, t, target, &residual);
            graph.setValue({t, kDeltas, -1}, DataPtr(std::move(deltas)));
            if (!a.get<bool>("keep"))
                graph.deleteNode(sculpt.id());
            setVisible(graph, base.id(), true);
            graph.select({base.id()});
            return {{"target", graph.node(t)->name()}, {"drivers", made}, {"residual", residual}};
        },
    });
}
} // namespace morphs
} // namespace lila::scene
