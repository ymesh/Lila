#include "gn/NodesCommon.h"

#include "core/Profile.h"

#include <algorithm>
#include <queue>

namespace lila::gn::nodes
{
namespace selectgrow
{
/** Neighbours of every element of a mesh domain: across edges or across corners (anything sharing a point or face). */
std::vector<std::vector<Index>> neighbours(const geo::Mesh& m, Domain d, bool corners)
{
    std::vector<std::vector<Index>> nb;
    auto addUnique = [](std::vector<Index>& v, Index x) {
        if (std::find(v.begin(), v.end(), x) == v.end())
            v.push_back(x);
    };
    if (d == Domain::Point)
    {
        nb.resize(m.pointCapacity());
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            if (corners)
                m.forEachPointCorner(p, [&](Index k) {
                    m.forEachFaceCorner(m.cornerFace(k), [&](Index o) {
                        if (m.cornerPoint(o) != p)
                            addUnique(nb[p], m.cornerPoint(o));
                    });
                });
            m.forEachPointEdge(p, [&](Index e) { addUnique(nb[p], m.edgeOtherPoint(e, p)); });
        }
    }
    else if (d == Domain::Edge)
    {
        nb.resize(m.edgeCapacity());
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            for (int s = 0; s < 2; ++s)
                m.forEachPointEdge(m.edgePoint(e, s), [&](Index x) {
                    if (x != e)
                        addUnique(nb[e], x);
                });
            if (corners)
                m.forEachEdgeCorner(e, [&](Index k) {
                    m.forEachFaceCorner(m.cornerFace(k), [&](Index o) {
                        if (m.cornerEdge(o) != e)
                            addUnique(nb[e], m.cornerEdge(o));
                    });
                });
        }
    }
    else
    {
        nb.resize(m.faceCapacity());
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            m.forEachFaceCorner(f, [&](Index k) {
                if (corners)
                    m.forEachPointCorner(m.cornerPoint(k), [&](Index o) {
                        if (m.cornerFace(o) != f)
                            addUnique(nb[f], m.cornerFace(o));
                    });
                else
                    m.forEachEdgeCorner(m.cornerEdge(k), [&](Index o) {
                        if (m.cornerFace(o) != f)
                            addUnique(nb[f], m.cornerFace(o));
                    });
            });
        }
    }
    return nb;
}

/** Element centres of a mesh domain, for distances between them. */
std::vector<D3> centres(const geo::Mesh& m, Domain d)
{
    if (d == Domain::Point)
        return std::vector<D3>(m.positions().begin(), m.positions().end());
    std::vector<D3> out;
    if (d == Domain::Edge)
    {
        out.resize(m.edgeCapacity(), D3(0.0));
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
                out[e] = (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5;
        return out;
    }
    out.resize(m.faceCapacity(), D3(0.0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out[f] = m.faceCentroid(f);
    return out;
}

struct Spread
{
    std::vector<double> distance; // infinity where not reached
};

/** Dijkstra out from the selected elements: by steps, or by distance between element centres. */
Spread spread(const geo::Mesh& m, Domain d, bool corners, const std::vector<uint8_t>& seed, bool byDistance, double limit)
{
    std::vector<std::vector<Index>> nb = neighbours(m, d, corners);
    std::vector<D3> at = byDistance ? centres(m, d) : std::vector<D3>{};
    Spread s;
    s.distance.assign(nb.size(), std::numeric_limits<double>::infinity());
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for (Index i = 0; i < Index(nb.size()); ++i)
        if (i < seed.size() && seed[i])
        {
            s.distance[i] = 0.0;
            queue.push({0.0, i});
        }
    while (!queue.empty())
    {
        auto [dist, i] = queue.top();
        queue.pop();
        if (dist > s.distance[i])
            continue;
        for (Index j : nb[i])
        {
            double nd = dist + (byDistance ? glm::length(at[j] - at[i]) : 1.0);
            if (nd <= limit + 1e-9 && nd < s.distance[j])
            {
                s.distance[j] = nd;
                queue.push({nd, j});
            }
        }
    }
    return s;
}

/** A field over the mesh worked out on one domain and moved to the one it is read on; other components read the selection itself. */
class SelectionField final : public Field
{
public:
    using Fn = std::function<Column(const geo::Mesh&, const std::vector<uint8_t>&)>;
    SelectionField(SocketType t, FieldPtr selection, Domain d, Fn fn) : Field(t), mSel(std::move(selection)), mDomain(d), mFn(std::move(fn)) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Mesh || !ctx.mesh)
            return gn::evaluate(mSel, ctx, type());
        const geo::Mesh& m = *ctx.mesh;
        std::vector<uint8_t> sel = evaluateAll<uint8_t>(mSel, FieldContext::forMesh(m, mDomain));
        return convert(adaptDomain(m, mFn(m, sel), mDomain, ctx.domain), type());
    }

private:
    FieldPtr mSel;
    Domain mDomain;
    Fn mFn;
};

PropDecl acrossProp()
{
    return prop("across", {"edges", "corners"}, "edges",
                "Edges: points joined by an edge, edges sharing a point, faces sharing an edge. Corners also takes anything sharing a face, or faces "
                "sharing only a point.");
}

void growShrink(const char* type, const char* label, bool grow)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Mesh Topology",
        .doc = grow ? "Adds the neighbours of a selection, a ring per step." : "Takes the outer ring off a selection per step: elements with an unselected neighbour drop out.",
        .props = {prop("domain", {"point", "edge", "face"}, "face"), acrossProp()},
        .sockets = {.inputs = {fin("selection", T::Bool, false), bounded(in("steps", T::Int, 1), 0, 100000)}, .outputs = {fout("selection", T::Bool)}},
        .exec =
            [grow](ExecContext& c) {
                Domain d = domainOf(c.prop("domain"));
                bool corners = c.prop("across") == "corners";
                int64_t steps = std::max<int64_t>(0, c.i("steps"));
                c.out("selection", std::make_shared<SelectionField>(T::Bool, c.field("selection"), d, [=](const geo::Mesh& m, const std::vector<uint8_t>& sel) {
                          std::vector<uint8_t> seed = sel;
                          if (!grow)
                              for (uint8_t& s : seed)
                                  s = s ? 0 : 1;
                          Spread s = spread(m, d, corners, seed, false, double(steps));
                          std::vector<uint8_t> out(s.distance.size(), 0);
                          for (size_t i = 0; i < out.size(); ++i)
                          {
                              bool reached = std::isfinite(s.distance[i]);
                              out[i] = grow ? (reached ? 1 : 0) : (sel[i] && !reached ? 1 : 0);
                          }
                          return Column::of(std::move(out));
                      }));
            },
    });
}

void expandNode()
{
    registerDecl({
        .type = "ExpandSelection",
        .label = "Expand Selection",
        .category = "Mesh Topology",
        .doc = "Spreads a selection over the mesh by a number of steps or a distance in cm, measured between element centres along the surface. Also "
               "gives how far each element is from the selection and a falloff from 1 at the selection to 0 at the limit.",
        .props = {prop("domain", {"point", "edge", "face"}, "point"), prop("mode", {"topology", "distance"}, "distance"), acrossProp()},
        .sockets = {.inputs = {fin("selection", T::Bool, false), bounded(in("steps", T::Int, 2, "Topology mode."), 0, 100000),
                               bounded(in("distance", T::Float, 10.0, "Distance mode, cm."), 0.0)},
                    .outputs = {fout("selection", T::Bool), fout("distance", T::Float, "Steps or cm from the selection; -1 beyond the limit."),
                                fout("falloff", T::Float)}},
        .exec =
            [](ExecContext& c) {
                Domain d = domainOf(c.prop("domain"));
                bool corners = c.prop("across") == "corners";
                bool byDistance = c.prop("mode") == "distance";
                double limit = byDistance ? std::max(0.0, c.f("distance")) : double(std::max<int64_t>(0, c.i("steps")));
                FieldPtr sel = c.field("selection");
                auto make = [=](int which) {
                    SocketType t = which == 0 ? T::Bool : T::Float;
                    return std::make_shared<SelectionField>(t, sel, d, [=](const geo::Mesh& m, const std::vector<uint8_t>& s) {
                        Spread sp = spread(m, d, corners, s, byDistance, limit);
                        std::vector<double> out(sp.distance.size(), 0.0);
                        for (size_t i = 0; i < out.size(); ++i)
                        {
                            double x = sp.distance[i];
                            bool in = std::isfinite(x);
                            out[i] = which == 0 ? (in ? 1.0 : 0.0) : which == 1 ? (in ? x : -1.0) : !in ? 0.0 : limit > 0.0 ? 1.0 - x / limit : 1.0;
                        }
                        if (which == 0)
                        {
                            std::vector<uint8_t> b(out.size());
                            for (size_t i = 0; i < out.size(); ++i)
                                b[i] = out[i] > 0.5 ? 1 : 0;
                            return Column::of(std::move(b));
                        }
                        return Column::of(std::move(out));
                    });
                };
                c.out("selection", make(0));
                c.out("distance", make(1));
                c.out("falloff", make(2));
            },
    });
}
} // namespace selectgrow

void registerSelectionNodes()
{
    selectgrow::growShrink("GrowSelection", "Grow Selection", true);
    selectgrow::growShrink("ShrinkSelection", "Shrink Selection", false);
    selectgrow::expandNode();
}
} // namespace lila::gn::nodes
