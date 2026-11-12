#pragma once

#include "gn/Exec.h"
#include "gn/NodesCommon.h"

#include <doctest/doctest.h>

#include <vector>

/** Running small node graphs on geometry built in the test, and reading their outputs back. */
namespace nodes2test
{
using namespace lila;
using json = nlohmann::json;
using N = gn::GNode;
using L = gn::GLink;
using D3 = glm::dvec3;

struct Run
{
    gn::GeometryPtr geometry;
    std::vector<gn::Value> values; // the extra group outputs, in order
};

/** The nodes between the group's input (the given geometry) and output; outputs are the extra group outputs. */
inline Run run(std::vector<N> nodes, std::vector<L> links, std::vector<gn::GOutput> outputs = {}, gn::GeometryPtr input = nullptr, gn::ExecEnv env = {})
{
    gn::Group g = gn::Group::makeDefault("T");
    g.outputs = std::move(outputs);
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    if (input)
        env.input = std::move(input);
    std::vector<gn::Value> out = gn::executeOutputs(*plan, env);
    Run r;
    r.geometry = out[0].geometry;
    r.values.assign(out.begin() + 1, out.end());
    return r;
}

inline gn::GeometryPtr curves(const std::vector<std::vector<D3>>& pts, const std::vector<bool>& closed = {})
{
    gn::CurveSet cs;
    for (size_t k = 0; k < pts.size(); ++k)
        cs.addCurve(pts[k], k < closed.size() && closed[k]);
    return gn::curveGeometry(std::move(cs));
}

inline gn::GeometryPtr points(const std::vector<D3>& pts)
{
    geo::Mesh m;
    for (const D3& p : pts)
        m.addPoint(p);
    return gn::pointGeometry(std::move(m));
}

template <class T> std::vector<T> onCurves(const gn::Value& v, const gn::CurveSet& cs, gn::Domain d = gn::Domain::Point)
{
    return gn::evaluateAll<T>(v.field, gn::FieldContext::forCurves(cs, d));
}

template <class T> std::vector<T> onPoints(const gn::Value& v, const geo::Mesh& m) { return gn::evaluateAll<T>(v.field, gn::FieldContext::forPoints(m)); }

template <class T> T single(const gn::Value& v) { return gn::evaluateSingle<T>(v.field); }

inline bool near(const D3& a, const D3& b, double eps = 1e-6) { return glm::length(a - b) < eps; }
} // namespace nodes2test
