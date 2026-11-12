#include "gn/NodesCommon.h"

#include <algorithm>
#include <map>
#include <numeric>

namespace lila::gn::nodes::curveops
{
void controlFrames(const CurveSet& cs, int normalMode, std::vector<D3>& tangents, std::vector<D3>& normals);
}

namespace lila::gn::nodes::curve2
{
/** A field read off the curve being evaluated, per point or per curve, moved to the context's domain; fallback elsewhere. */
template <class V, class Fn> Column fromCurves(const FieldContext& ctx, Domain on, V fallback, Fn&& fn)
{
    if (ctx.component != Component::Curve)
        return Column::of(fallback);
    std::vector<V> v = fn(*ctx.curves);
    return adaptCurveDomain(*ctx.curves, Column::of(std::move(v)), on, ctx.domain);
}

class CyclicField final : public Field
{
public:
    CyclicField() : Field(SocketType::Bool) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        return fromCurves(ctx, Domain::Curve, uint8_t(0), [](const CurveSet& cs) { return cs.cyclic; });
    }
};

class HandleField final : public Field
{
public:
    HandleField(bool right, FieldPtr relative) : Field(SocketType::Vector), mRight(right), mRelative(std::move(relative)) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve)
            return Column::of(D3(0.0));
        const CurveSet& cs = *ctx.curves;
        std::span<const glm::vec3> h = cs.pointAttrs.get<glm::vec3>(mRight ? "handle_right" : "handle_left");
        std::vector<D3> v(cs.numPoints());
        for (size_t p = 0; p < v.size(); ++p)
            v[p] = h.empty() ? cs.positions[p] : D3(h[p]);
        Column all = adaptCurveDomain(cs, Column::of(std::move(v)), Domain::Point, ctx.domain);
        std::vector<uint8_t> rel = evaluateAll<uint8_t>(mRelative, ctx);
        std::vector<D3> out = expand<D3>(all, ctx.size);
        std::vector<D3> pos = evaluateAll<D3>(positionField(), ctx);
        for (size_t k = 0; k < out.size(); ++k)
            if (rel[k])
                out[k] -= pos[k];
        return Column::of(std::move(out));
    }

private:
    bool mRight;
    FieldPtr mRelative;
};

/** The curve each point is on. */
std::vector<size_t> curveOfPoints(const CurveSet& cs)
{
    std::vector<size_t> c(cs.numPoints());
    for (size_t cu = 0; cu < cs.numCurves(); ++cu)
        for (Index p = cs.offsets[cu]; p < cs.offsets[cu + 1]; ++p)
            c[p] = cu;
    return c;
}

class OffsetInCurveField final : public Field
{
public:
    OffsetInCurveField(FieldPtr point, FieldPtr offset, bool valid) : Field(valid ? SocketType::Bool : SocketType::Int), mPoint(std::move(point)), mOffset(std::move(offset)), mValid(valid) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve || ctx.curves->numPoints() == 0)
            return mValid ? Column::of(uint8_t(0)) : Column::of(int64_t(0));
        const CurveSet& cs = *ctx.curves;
        std::vector<size_t> curveOf = curveOfPoints(cs);
        std::vector<int64_t> pt = evaluateAll<int64_t>(mPoint, ctx), off = evaluateAll<int64_t>(mOffset, ctx);
        std::vector<int64_t> idx(ctx.size);
        std::vector<uint8_t> ok(ctx.size);
        for (size_t k = 0; k < ctx.size; ++k)
        {
            int64_t p = std::clamp<int64_t>(pt[k], 0, int64_t(cs.numPoints()) - 1);
            size_t cu = curveOf[size_t(p)];
            int64_t p0 = cs.offsets[cu], n = int64_t(cs.curveSize(cu));
            int64_t local = p - p0 + off[k];
            if (cs.cyclic[cu])
                local = ((local % n) + n) % n;
            ok[k] = local >= 0 && local < n;
            idx[k] = ok[k] ? p0 + local : p;
        }
        if (mValid)
            return Column::of(std::move(ok));
        return Column::of(std::move(idx));
    }

private:
    FieldPtr mPoint, mOffset;
    bool mValid;
};

std::vector<uint8_t> selectedCurves(const FieldPtr& sel, const CurveSet& cs) { return evaluateAll<uint8_t>(sel, FieldContext::forCurves(cs, Domain::Curve)); }

void cyclicNodes()
{
    registerDecl({
        .type = "SetSplineCyclic",
        .label = "Set Spline Cyclic",
        .category = "Curve",
        .doc = "Opens or closes curves. A NURBS curve whose cyclic state changes gets uniform knots.",
        .sockets = {.inputs = {geoIn("curve"), fin("selection", T::Bool, true, Implicit::None, "Curves to change."), fin("cyclic", T::Bool, false)},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        FieldContext ctx = FieldContext::forCurves(cs, Domain::Curve);
                                                                        std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                                        std::vector<uint8_t> cyc = evaluateAll<uint8_t>(c.field("cyclic"), ctx);
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                            if (sel[cu] && (cs.cyclic[cu] != 0) != (cyc[cu] != 0))
                                                                            {
                                                                                cs.cyclic[cu] = cyc[cu] ? 1 : 0;
                                                                                if (cu < cs.knots.size())
                                                                                    cs.knots[cu].clear();
                                                                            }
                                                                    }}));
            },
    });
    registerDecl({
        .type = "IsSplineCyclic",
        .label = "Is Spline Cyclic",
        .category = "Curve",
        .doc = "Whether the curve each element belongs to is closed.",
        .sockets = {.outputs = {fout("cyclic", T::Bool)}},
        .exec = [](ExecContext& c) { c.out("cyclic", std::make_shared<CyclicField>()); },
    });
}

void resolutionNodes()
{
    registerDecl({
        .type = "SetSplineResolution",
        .label = "Set Spline Resolution",
        .category = "Curve",
        .doc = "Segments per span when Bezier and NURBS curves are turned into points; 0 lets each span pick its own from how much it bends.",
        .sockets = {.inputs = {geoIn("curve"), fin("selection", T::Bool, true, Implicit::None, "Curves to change."), bounded(fin("resolution", T::Int, 12), 0, 512)},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        FieldContext ctx = FieldContext::forCurves(cs, Domain::Curve);
                                                                        std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                                        std::vector<int64_t> res = evaluateAll<int64_t>(c.field("resolution"), ctx);
                                                                        std::vector<int64_t> v = evaluateAll<int64_t>(attributeField("resolution", T::Int), ctx);
                                                                        for (size_t cu = 0; cu < v.size(); ++cu)
                                                                            if (sel[cu])
                                                                                v[cu] = std::clamp<int64_t>(res[cu], 0, 512);
                                                                        writeAttribute(cs, "resolution", Domain::Curve, Column::of(std::move(v)), T::Int);
                                                                    }}));
            },
    });
    registerDecl({
        .type = "SplineResolution",
        .label = "Spline Resolution",
        .category = "Curve",
        .doc = "Segments per span of the curve each element belongs to; 0 when its spans pick their own.",
        .sockets = {.outputs = {fout("resolution", T::Int)}},
        .exec = [](ExecContext& c) { c.out("resolution", attributeField("resolution", T::Int)); },
    });
}

void handleNodes()
{
    registerDecl({
        .type = "HandlePositions",
        .label = "Handle Positions",
        .category = "Curve",
        .doc = "Where each Bezier point's handles are, or their offset from the point when relative. Points without handles give their own position.",
        .sockets = {.inputs = {fin("relative", T::Bool, false)}, .outputs = {fout("left", T::Vector), fout("right", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                c.out("left", std::make_shared<HandleField>(false, c.field("relative")));
                c.out("right", std::make_shared<HandleField>(true, c.field("relative")));
            },
    });
    registerDecl({
        .type = "SetHandlePositions",
        .label = "Set Handle Positions",
        .category = "Curve",
        .doc = "Moves the left or right handles of Bezier points: to position when linked, then by offset. Auto and vector handles become "
               "free; an aligned handle opposite keeps its length and turns to stay in line.",
        .props = {prop("mode", {"left", "right"}, "left")},
        .sockets = {.inputs = {geoIn("curve"), selectionIn(), fin("position", T::Vector, vec(0, 0, 0), Implicit::None, "Unlinked, the handle stays where it is."),
                               fin("offset", T::Vector, vec(0, 0, 0))},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                const bool right = c.prop("mode") == "right";
                const bool absolute = !c.group.linksInto(c.node.name, "position").empty() || c.node.values.contains("position");
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        std::span<glm::vec3> hs = cs.pointAttrs.get<glm::vec3>(right ? "handle_right" : "handle_left");
                                                                        std::span<glm::vec3> ho = cs.pointAttrs.get<glm::vec3>(right ? "handle_left" : "handle_right");
                                                                        std::span<int32_t> ts = cs.pointAttrs.get<int32_t>(right ? "handle_type_right" : "handle_type_left");
                                                                        std::span<int32_t> to = cs.pointAttrs.get<int32_t>(right ? "handle_type_left" : "handle_type_right");
                                                                        if (hs.empty())
                                                                            return;
                                                                        FieldContext ctx = FieldContext::forCurves(cs, Domain::Point);
                                                                        std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                                        std::vector<D3> pos = evaluateAll<D3>(c.field("position"), ctx);
                                                                        std::vector<D3> off = evaluateAll<D3>(c.field("offset"), ctx);
                                                                        std::span<const int32_t> kind = cs.curveAttrs.get<int32_t>("curve_type");
                                                                        std::vector<size_t> curveOf = curveOfPoints(cs);
                                                                        for (size_t p = 0; p < cs.numPoints(); ++p)
                                                                        {
                                                                            if (!sel[p] || (!kind.empty() && kind[curveOf[p]] != int32_t(curve::Kind::Bezier)))
                                                                                continue;
                                                                            D3 h = (absolute ? pos[p] : D3(hs[p])) + off[p];
                                                                            hs[p] = glm::vec3(h);
                                                                            int32_t own = ts.empty() ? int32_t(curve::HandleType::Free) : ts[p];
                                                                            if (!ts.empty() && (own == int32_t(curve::HandleType::Auto) || own == int32_t(curve::HandleType::Vector)))
                                                                                ts[p] = int32_t(curve::HandleType::Free);
                                                                            if (own == int32_t(curve::HandleType::Aligned) && !to.empty() && to[p] == int32_t(curve::HandleType::Aligned) && !ho.empty())
                                                                            {
                                                                                D3 anchor = cs.positions[p], d = anchor - h;
                                                                                double len = glm::length(D3(ho[p]) - anchor);
                                                                                if (glm::length(d) > 1e-12)
                                                                                    ho[p] = glm::vec3(anchor + glm::normalize(d) * len);
                                                                            }
                                                                        }
                                                                    }}));
            },
    });
}

/** Signed angle about axis t turning a onto b, both square to t. */
double angleAbout(const D3& a, const D3& b, const D3& t) { return std::atan2(glm::dot(glm::cross(a, b), t), glm::dot(a, b)); }

void normalNode()
{
    registerDecl({
        .type = "SetCurveNormal",
        .label = "Set Curve Normal",
        .category = "Curve",
        .doc = "Sets how curve normals point by writing tilt: minimumTwist clears it, so normals turn as little as they can; yUp keeps them "
               "square to the up axis; free turns each one toward the normal input, projected square to the curve.",
        .props = {prop("mode", {"minimumTwist", "yUp", "free"}, "minimumTwist")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s{{geoIn("curve"), selectionIn()}, {geoOut("curve")}};
                if (findDecl("SetCurveNormal")->propValue(n, "mode") == "free")
                    s.inputs.push_back(fin("normal", T::Vector, vec(0, 1, 0)));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                const std::string mode = c.prop("mode");
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        FieldContext ctx = FieldContext::forCurves(cs, Domain::Point);
                                                                        std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                                        std::vector<double> tilt = evaluateAll<double>(attributeField("tilt", T::Float), ctx);
                                                                        CurveSet untilted = cs;
                                                                        untilted.pointAttrs.remove("tilt");
                                                                        std::vector<D3> t, n;
                                                                        curveops::controlFrames(untilted, 0, t, n);
                                                                        std::vector<D3> want;
                                                                        if (mode == "free")
                                                                            want = evaluateAll<D3>(c.field("normal"), ctx);
                                                                        for (size_t p = 0; p < tilt.size(); ++p)
                                                                        {
                                                                            if (!sel[p])
                                                                                continue;
                                                                            if (mode == "minimumTwist")
                                                                            {
                                                                                tilt[p] = 0.0;
                                                                                continue;
                                                                            }
                                                                            D3 target = mode == "yUp" ? D3(-t[p].z, 0.0, t[p].x) : want[p] - t[p] * glm::dot(want[p], t[p]);
                                                                            if (glm::length(target) < 1e-9)
                                                                                continue;
                                                                            tilt[p] = angleAbout(n[p], glm::normalize(target), t[p]);
                                                                        }
                                                                        writeAttribute(cs, "tilt", Domain::Point, Column::of(std::move(tilt)), T::Float);
                                                                    }}));
            },
    });
}

void offsetNode()
{
    registerDecl({
        .type = "OffsetPointInCurve",
        .label = "Offset Point in Curve",
        .category = "Curve",
        .doc = "The point offset steps along the same curve from a point, wrapping on closed curves, and whether that point exists.",
        .sockets = {.inputs = {fin("pointIndex", T::Int, nullptr, Implicit::Index), fin("offset", T::Int, 0)},
                    .outputs = {fout("isValidOffset", T::Bool), fout("pointIndex", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("isValidOffset", std::make_shared<OffsetInCurveField>(c.field("pointIndex"), c.field("offset"), true));
                c.out("pointIndex", std::make_shared<OffsetInCurveField>(c.field("pointIndex"), c.field("offset"), false));
            },
    });
}

void pointsToCurves()
{
    registerDecl({
        .type = "PointsToCurves",
        .label = "Points to Curves",
        .category = "Curve",
        .doc = "One poly curve per curve group id, through its points in order of weight (ties by index), groups in order of id. Point "
               "attributes carry over. Takes the point cloud, or the mesh's points when there is none.",
        .sockets = {.inputs = {geoIn("points"), fin("curveGroupId", T::Int, 0), fin("weight", T::Float, 0.0)}, .outputs = {geoOut("curves")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("points");
                const geo::Mesh* m = g->points ? g->points.get() : g->mesh.get();
                CurveSet out;
                if (m && m->numPoints() > 0)
                {
                    FieldContext ctx = g->points ? FieldContext::forPoints(*m) : FieldContext::forMesh(*m, Domain::Point);
                    std::vector<int64_t> group = evaluateAll<int64_t>(c.field("curveGroupId"), ctx);
                    std::vector<double> weight = evaluateAll<double>(c.field("weight"), ctx);
                    std::map<int64_t, std::vector<Index>> byGroup;
                    for (Index p = 0; p < m->numPoints(); ++p)
                        byGroup[group[p]].push_back(p);
                    out.pointAttrs.mergeLayout(m->pointAttrs());
                    for (auto& [id, pts] : byGroup)
                    {
                        std::stable_sort(pts.begin(), pts.end(), [&](Index a, Index b) { return weight[a] < weight[b]; });
                        std::vector<D3> pos(pts.size());
                        for (size_t k = 0; k < pts.size(); ++k)
                            pos[k] = m->position(pts[k]);
                        size_t q0 = out.numPoints();
                        out.addCurve(pos, false);
                        for (size_t k = 0; k < pts.size(); ++k)
                            out.pointAttrs.copyFrom(m->pointAttrs(), q0 + k, pts[k]);
                    }
                }
                c.out("curves", curveGeometry(std::move(out)));
            },
    });
}
} // namespace lila::gn::nodes::curve2

namespace lila::gn::nodes
{
void registerCurveNodes2()
{
    curve2::cyclicNodes();
    curve2::resolutionNodes();
    curve2::handleNodes();
    curve2::normalNode();
    curve2::offsetNode();
    curve2::pointsToCurves();
}
} // namespace lila::gn::nodes
