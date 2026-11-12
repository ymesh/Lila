#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "gn/Texture.h"

#include <tbb/parallel_for.h>

#include <numeric>
#include <sstream>

/** Deformers and attribute passes: lattice, point deform, attribute transfer, edge damage and flowmaps. */
namespace lila::gn::nodes
{
namespace meshops
{
geo::Mesh subdivideLinear(const geo::Mesh& m);
}

namespace deform2
{
/** Static kd-tree answering k-nearest queries within a radius. */
class KnnTree
{
public:
    explicit KnnTree(std::vector<D3> pts) : mPts(std::move(pts)), mOrder(mPts.size())
    {
        std::iota(mOrder.begin(), mOrder.end(), Index(0));
        build(0, mOrder.size(), 0);
    }

    bool empty() const { return mPts.empty(); }
    const D3& point(Index i) const { return mPts[i]; }

    /** Up to k nearest points within radius, as (squared distance, index), nearest first. */
    void nearest(const D3& q, size_t k, double radius, std::vector<std::pair<double, Index>>& out) const
    {
        out.clear();
        if (k == 0)
            return;
        double r2 = radius * radius;
        search(0, mOrder.size(), 0, q, k, r2, out);
        std::sort(out.begin(), out.end());
    }

private:
    void build(size_t lo, size_t hi, int axis)
    {
        if (hi - lo <= 1)
            return;
        size_t mid = (lo + hi) / 2;
        std::nth_element(mOrder.begin() + lo, mOrder.begin() + mid, mOrder.begin() + hi, [&](Index a, Index b) { return mPts[a][axis] < mPts[b][axis]; });
        build(lo, mid, (axis + 1) % 3);
        build(mid + 1, hi, (axis + 1) % 3);
    }

    /** out is kept as a max-heap on distance while searching. */
    void search(size_t lo, size_t hi, int axis, const D3& q, size_t k, double& r2, std::vector<std::pair<double, Index>>& out) const
    {
        if (lo >= hi)
            return;
        size_t mid = (lo + hi) / 2;
        Index i = mOrder[mid];
        double d = glm::dot(mPts[i] - q, mPts[i] - q);
        if (d <= r2)
        {
            out.push_back({d, i});
            std::push_heap(out.begin(), out.end());
            if (out.size() > k)
            {
                std::pop_heap(out.begin(), out.end());
                out.pop_back();
            }
            if (out.size() == k)
                r2 = std::min(r2, out.front().first);
        }
        double diff = q[axis] - mPts[i][axis];
        int next = (axis + 1) % 3;
        size_t nearLo = diff < 0.0 ? lo : mid + 1, nearHi = diff < 0.0 ? mid : hi;
        size_t farLo = diff < 0.0 ? mid + 1 : lo, farHi = diff < 0.0 ? hi : mid;
        search(nearLo, nearHi, next, q, k, r2, out);
        if (diff * diff <= r2)
            search(farLo, farHi, next, q, k, r2, out);
    }

    std::vector<D3> mPts;
    std::vector<Index> mOrder;
};

/** The points a proxy offers: its mesh's, else its point cloud's, else its curves'. */
std::vector<D3> proxyPoints(const Geometry& g)
{
    if (g.mesh && !g.mesh->empty())
        return {g.mesh->positions().begin(), g.mesh->positions().end()};
    if (g.points && !g.points->empty())
        return {g.points->positions().begin(), g.points->positions().end()};
    if (g.curves)
        return g.curves->positions;
    return {};
}

/** Every point of the mesh, point cloud and curves moved by fn (Bezier handles too). */
GeometryPtr movePoints(const GeometryPtr& g, const std::function<D3(const D3&)>& fn)
{
    auto meshFn = [&](geo::Mesh& m) {
        std::span<D3> pos = m.positions();
        tbb::parallel_for(size_t(0), pos.size(), [&](size_t k) { pos[k] = fn(pos[k]); });
        m.cornerAttrs().remove(geo::attr::kNormal);
    };
    return editComponents(g, {.mesh = meshFn,
                              .points = meshFn,
                              .curves = [&](CurveSet& cs) {
                                  tbb::parallel_for(size_t(0), cs.positions.size(), [&](size_t k) { cs.positions[k] = fn(cs.positions[k]); });
                                  for (const char* h : {"handle_left", "handle_right"})
                                      if (geo::Attribute* a = cs.pointAttrs.find(h); a && a->type() == geo::AttrType::Float3)
                                          for (glm::vec3& v : a->values<glm::vec3>())
                                              v = glm::vec3(fn(D3(v)));
                              }});
}

double smooth01(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// ------------------------------------------------------------------------------------------------ lattice

void latticeNode()
{
    registerDecl({
        .type = "Lattice",
        .label = "Lattice",
        .category = "Deform",
        .doc = "Deforms geometry with a lattice cage. The rest lattice spans the geometry's bounding box (plus padding) at the given "
               "points per axis, or the bounds of a Rest grid. Its points move by the Offset field (evaluated on the lattice, so it can "
               "read Position) or take the positions of a Cage of the same points. Points outside the box fade out over Falloff cm. "
               "The lattice output is the moved cage, to view or to edit and feed back in as Cage.",
        .props = {prop("basis", {"bspline", "trilinear", "bernstein"}, "bspline", "Bspline is smooth and local, bernstein global.")},
        .sockets = {.inputs = {geoIn(), geoIn("cage", "Moved lattice points, in the lattice output's order."), geoIn("rest", "A grid whose bounds set the rest lattice."),
                               in("resolution", T::Vector, vec(3, 3, 3), "Points per axis, 2 or more."), bounded(in("padding", T::Float, 1.0, "cm."), 0.0),
                               fin("offset", T::Vector, vec(0, 0, 0), Implicit::None, "Movement of each lattice point, cm."), bounded(in("falloff", T::Float, 0.0, "cm."), 0.0)},
                    .outputs = {geoOut(), geoOut("lattice")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("geometry");
                D3 r = glm::round(c.v("resolution"));
                glm::ivec3 n(std::clamp(int(r.x), 2, 64), std::clamp(int(r.y), 2, 64), std::clamp(int(r.z), 2, 64));
                std::vector<D3> restPts = proxyPoints(*c.geometry("rest"));
                geo::Bounds box;
                for (const D3& p : restPts.empty() ? proxyPoints(*g) : restPts)
                    box.extend(p);
                if (g->curves)
                    for (const D3& p : g->curves->positions)
                        if (restPts.empty())
                            box.extend(p);
                if (!box.valid())
                {
                    c.out("geometry", g);
                    c.out("lattice", makeGeometry({}));
                    return;
                }
                D3 pad(restPts.empty() ? std::max(0.0, c.f("padding")) : 0.0);
                box.min -= pad;
                box.max += pad;
                D3 size = glm::max(box.max - box.min, D3(1e-3)), centre = (box.min + box.max) * 0.5;

                geo::Mesh lattice = deform::latticeMesh(n);
                for (Index p = 0; p < lattice.pointCapacity(); ++p)
                    lattice.setPosition(p, centre + lattice.position(p) * size);
                std::vector<D3> rest(lattice.positions().begin(), lattice.positions().end());
                std::vector<D3> cage = proxyPoints(*c.geometry("cage"));
                if (!cage.empty() && cage.size() != rest.size())
                    throw ExecError("Lattice: the cage has " + std::to_string(cage.size()) + " points, the lattice " + std::to_string(rest.size()));
                if (cage.empty())
                {
                    std::vector<D3> off = evaluateAll<D3>(c.field("offset"), FieldContext::forMesh(lattice, Domain::Point));
                    cage = rest;
                    for (size_t k = 0; k < cage.size(); ++k)
                        cage[k] += off[k];
                }
                std::vector<D3> disp(rest.size());
                for (size_t k = 0; k < rest.size(); ++k)
                {
                    disp[k] = cage[k] - rest[k];
                    lattice.setPosition(Index(k), cage[k]);
                }
                deform::FfdBasis basis = c.prop("basis") == "trilinear" ? deform::FfdBasis::Trilinear
                                         : c.prop("basis") == "bernstein" ? deform::FfdBasis::Bernstein
                                                                          : deform::FfdBasis::BSpline;
                double falloff = std::max(0.0, c.f("falloff"));
                c.out("geometry", movePoints(g, [&](const D3& p) {
                          D3 stu = (p - box.min) / size;
                          D3 clamped = glm::clamp(stu, D3(0.0), D3(1.0));
                          double outside = glm::length((stu - clamped) * size);
                          double w = outside <= 1e-9 ? 1.0 : falloff > 0.0 ? 1.0 - smooth01(outside / falloff) : 0.0;
                          return w > 0.0 ? p + deform::ffdDisplacement(n, disp, clamped, basis) * w : p;
                      }));
                c.out("lattice", meshGeometry(std::move(lattice)));
            },
    });
}

// ------------------------------------------------------------------------------------------------ point deform

/** Best rotation taking rest offsets onto deformed ones (polar part of their covariance); identity when degenerate. */
glm::dmat3 fitRotation(const glm::dmat3& cov)
{
    if (std::abs(glm::determinant(cov)) < 1e-12)
        return glm::dmat3(1.0);
    glm::dmat3 r = cov;
    for (int it = 0; it < 30; ++it)
    {
        glm::dmat3 inv = glm::inverse(glm::transpose(r));
        glm::dmat3 next = (r + inv) * 0.5;
        double diff = 0.0;
        for (int a = 0; a < 3; ++a)
            diff += glm::length(next[a] - r[a]);
        r = next;
        if (diff < 1e-12)
            break;
    }
    return glm::determinant(r) > 0.0 ? r : glm::dmat3(1.0);
}

/**
 * The rest and deformed normals of a neighbourhood as one more covariance term, so a flat (planar) neighbourhood still
 * fixes a full rotation. Zero when the neighbours lie on a line.
 */
glm::dmat3 neighbourhoodNormals(const std::vector<D3>& rest, const std::vector<D3>& moved, size_t i, const std::vector<std::pair<double, Index>>& nb)
{
    Index a = geo::kInvalid, b = geo::kInvalid;
    double best = 0.0;
    for (auto [d, j] : nb)
        if (d > best)
        {
            best = d;
            a = j;
        }
    if (a == geo::kInvalid)
        return glm::dmat3(0.0);
    D3 ra = rest[a] - rest[i];
    double bestArea = 0.0;
    for (auto [d, j] : nb)
        if (double area = glm::length(glm::cross(ra, rest[j] - rest[i])); area > bestArea)
        {
            bestArea = area;
            b = j;
        }
    if (b == geo::kInvalid || bestArea < 1e-9 * best)
        return glm::dmat3(0.0);
    D3 nr = glm::cross(ra, rest[b] - rest[i]), nd = glm::cross(moved[a] - moved[i], moved[b] - moved[i]);
    if (glm::length(nd) < 1e-12)
        return glm::dmat3(0.0);
    return glm::outerProduct(glm::normalize(nd), glm::normalize(nr)) * best;
}

void pointDeformNode()
{
    registerDecl({
        .type = "PointDeform",
        .label = "Point Deform",
        .category = "Deform",
        .doc = "Makes geometry follow a proxy: each point captures the proxy's rest points within the capture radius (up to Max "
               "Points, weighted by distance) and moves as they move in the deformed proxy. Rigid also turns it with its neighbourhood. "
               "Rest and Deformed need the same points in the same order; points with no rest point in reach stay put.",
        .props = {prop("mode", {"rigid", "translate"}, "rigid")},
        .sockets = {.inputs = {geoIn(), geoIn("rest"), geoIn("deformed"), bounded(in("radius", T::Float, 50.0, "Capture radius, cm."), 0.001),
                               bounded(in("maxPoints", T::Int, 8), 1, 256)},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("geometry");
                std::vector<D3> rest = proxyPoints(*c.geometry("rest")), moved = proxyPoints(*c.geometry("deformed"));
                if (rest.empty())
                {
                    c.out("geometry", g);
                    return;
                }
                if (rest.size() != moved.size())
                    throw ExecError("Point Deform: rest has " + std::to_string(rest.size()) + " points, deformed " + std::to_string(moved.size()));
                KnnTree tree(rest);
                double radius = std::max(0.001, c.f("radius"));
                size_t k = size_t(std::clamp<int64_t>(c.i("maxPoints"), 1, 256));
                bool rigid = c.prop("mode") == "rigid";
                std::vector<glm::dmat3> rot(rest.size(), glm::dmat3(1.0));
                if (rigid)
                    tbb::parallel_for(size_t(0), rest.size(), [&](size_t i) {
                        std::vector<std::pair<double, Index>> nb;
                        tree.nearest(rest[i], std::max<size_t>(k, 6), radius, nb);
                        glm::dmat3 cov(0.0);
                        for (auto [d, j] : nb)
                            cov += glm::outerProduct(moved[j] - moved[i], rest[j] - rest[i]);
                        cov += neighbourhoodNormals(rest, moved, i, nb);
                        rot[i] = fitRotation(cov);
                    });
                c.out("geometry", movePoints(g, [&](const D3& p) {
                          thread_local std::vector<std::pair<double, Index>> nb;
                          tree.nearest(p, k, radius, nb);
                          if (nb.empty())
                              return p;
                          D3 sum(0.0);
                          double wsum = 0.0;
                          for (auto [d2, j] : nb)
                          {
                              double t = 1.0 - std::sqrt(d2) / radius;
                              double w = std::max(t * t, 1e-9);
                              sum += (moved[j] + rot[j] * (p - rest[j])) * w;
                              wsum += w;
                          }
                          return sum / wsum;
                      }));
            },
    });
}

// ------------------------------------------------------------------------------------------------ attribute transfer

std::vector<std::string> attributeNames(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s + ",")
    {
        if (ch == ',' || ch == ' ' || ch == ';')
        {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        }
        else
            cur += ch;
    }
    return out;
}

/** Element positions on a domain: points, or face centroids. */
std::vector<D3> domainPositions(const geo::Mesh& m, Domain d)
{
    std::vector<D3> out;
    if (d == Domain::Face)
    {
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                out.push_back(m.faceCentroid(f));
        return out;
    }
    return {m.positions().begin(), m.positions().end()};
}

/** One transfer source: its element positions and the column of each attribute on them. */
struct TransferSource
{
    std::vector<D3> positions;
    std::map<std::string, Column> columns;
};

template <class V> V blendValues(const std::vector<V>& src, const std::vector<std::pair<double, Index>>& nb, const std::vector<double>& w, V own, double amount)
{
    if constexpr (std::is_same_v<V, int64_t> || std::is_same_v<V, uint8_t> || std::is_same_v<V, glm::dmat4>)
        return amount >= 0.5 ? src[nb.front().second] : own;
    else
    {
        V sum{};
        for (size_t i = 0; i < nb.size(); ++i)
            sum += src[nb[i].second] * w[i];
        return own + (sum - own) * amount;
    }
}

void attributeTransferNode()
{
    registerDecl({
        .type = "AttributeTransfer",
        .label = "Attribute Transfer",
        .category = "Attribute",
        .doc = "Copies named attributes from the source onto the geometry by distance, on points or faces. Each element blends its "
               "Count nearest source elements (inverse distance squared) and mixes the result with its own value by Blend. Within "
               "Max Distance less Falloff the transfer is full; it fades to none at Max Distance (0 reaches everywhere). Integer "
               "and boolean attributes take the nearest value.",
        .props = {prop("domain", {"point", "face"}, "point")},
        .sockets = {.inputs = {geoIn(), geoIn("source"), in("attributes", T::String, "color", "Names, separated by commas or spaces."),
                               bounded(in("maxDistance", T::Float, 0.0, "cm; 0 is unlimited."), 0.0), bounded(in("falloff", T::Float, 0.0, "cm."), 0.0),
                               bounded(in("count", T::Int, 4), 1, 64), bounded(in("blend", T::Float, 1.0), 0.0, 1.0)},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("geometry"), sg = c.geometry("source");
                Domain d = domainOf(c.prop("domain"));
                const geo::Mesh* sm = sg->mesh && !sg->mesh->empty() ? sg->mesh.get() : sg->points && !sg->points->empty() ? sg->points.get() : nullptr;
                std::vector<std::string> names = attributeNames(c.s("attributes"));
                if (!sm || names.empty() || !g->mesh)
                {
                    c.out("geometry", g);
                    return;
                }
                Domain sd = sm == sg->points.get() ? Domain::Point : d;
                TransferSource src;
                src.positions = domainPositions(*sm, sd);
                FieldContext sctx = sm == sg->points.get() ? FieldContext::forPoints(*sm) : FieldContext::forMesh(*sm, sd);
                for (const std::string& name : names)
                {
                    Domain from = sd;
                    std::optional<Column> col = readAttribute(sctx, name, &from);
                    if (!col)
                        continue;
                    Column adapted = from == sd ? *col : adaptDomain(*sm, *col, from, sd);
                    std::visit([&](const auto& v) {
                        using V = typename std::decay_t<decltype(v)>::value_type;
                        src.columns[name] = Column::of(expand<V>(adapted, src.positions.size()));
                    },
                               adapted.data);
                }
                if (src.columns.empty())
                {
                    c.out("geometry", g);
                    return;
                }
                KnnTree tree(src.positions);
                double maxD = c.f("maxDistance"), falloff = std::max(0.0, c.f("falloff")), blend = std::clamp(c.f("blend"), 0.0, 1.0);
                size_t k = size_t(std::clamp<int64_t>(c.i("count"), 1, 64));
                double reach = maxD > 0.0 ? maxD : std::numeric_limits<double>::max();
                c.out("geometry", editComponents(g, {.mesh = [&](geo::Mesh& m) {
                                                         std::vector<D3> at = domainPositions(m, d);
                                                         std::vector<std::vector<std::pair<double, Index>>> nbs(at.size());
                                                         std::vector<std::vector<double>> weights(at.size());
                                                         std::vector<double> amount(at.size(), 0.0);
                                                         tbb::parallel_for(size_t(0), at.size(), [&](size_t e) {
                                                             tree.nearest(at[e], k, reach, nbs[e]);
                                                             if (nbs[e].empty())
                                                                 return;
                                                             double wsum = 0.0;
                                                             for (auto [d2, j] : nbs[e])
                                                             {
                                                                 weights[e].push_back(1.0 / (d2 + 1e-12));
                                                                 wsum += weights[e].back();
                                                             }
                                                             for (double& w : weights[e])
                                                                 w /= wsum;
                                                             double nearest = std::sqrt(nbs[e].front().first), fade = 1.0;
                                                             if (maxD > 0.0 && falloff > 0.0)
                                                                 fade = 1.0 - smooth01((nearest - (maxD - falloff)) / falloff);
                                                             amount[e] = blend * fade;
                                                         });
                                                         FieldContext tctx = FieldContext::forMesh(m, d);
                                                         for (const auto& [name, scol] : src.columns)
                                                         {
                                                             std::visit([&](const auto& sv) {
                                                                 using V = typename std::decay_t<decltype(sv)>::value_type;
                                                                 Domain from = d;
                                                                 std::optional<Column> own = readAttribute(tctx, name, &from);
                                                                 std::vector<V> mine(at.size(), V{});
                                                                 if (own && own->data.index() == scol.data.index())
                                                                     mine = expand<V>(from == d ? *own : adaptDomain(m, *own, from, d), at.size());
                                                                 for (size_t e = 0; e < at.size(); ++e)
                                                                     if (!nbs[e].empty())
                                                                         mine[e] = blendValues<V>(sv, nbs[e], weights[e], mine[e], amount[e]);
                                                                 writeAttribute(m, name, d, Column::of(std::move(mine)), socketOf<V>());
                                                             },
                                                                        scol.data);
                                                         }
                                                     }}));
            },
    });
}

// ------------------------------------------------------------------------------------------------ edge damage

struct SharpEdge
{
    D3 a, b, inward;
};

std::vector<SharpEdge> sharpEdges(const geo::Mesh& m, double degrees)
{
    std::vector<SharpEdge> out;
    double cosLimit = std::cos(degrees * kPi / 180.0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
        D3 n0 = m.faceNormal(m.cornerFace(c0)), n1 = m.faceNormal(m.cornerFace(c1));
        if (glm::dot(n0, n1) > cosLimit)
            continue;
        D3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        D3 mid = (a + b) * 0.5;
        // Only convex edges chip: the faces' centres sit behind each other's planes.
        D3 f1 = m.faceCentroid(m.cornerFace(c1));
        if (glm::dot(f1 - mid, n0) > 1e-9)
            continue;
        D3 in = n0 + n1;
        double len = glm::length(in);
        out.push_back({a, b, len > 1e-12 ? -in / len : -n0});
    }
    return out;
}

double segmentDistance(const D3& p, const D3& a, const D3& b)
{
    D3 ab = b - a;
    double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-24), 0.0, 1.0);
    return glm::length(p - (a + ab * t));
}

double damageNoise(const D3& p, double w)
{
    double sum = 0.0, amp = 0.5, freq = 1.0;
    for (int o = 0; o < 3; ++o)
    {
        sum += amp * tex::gradientNoise4(glm::dvec4(p * freq, w + o * 3.7));
        amp *= 0.5;
        freq *= 2.1;
    }
    return sum;
}

void edgeDamageNode()
{
    registerDecl({
        .type = "EdgeDamage",
        .label = "Edge Damage",
        .category = "Mesh",
        .doc = "Chips and wear along convex sharp edges without booleans: points within Size of an edge sharper than Angle are pushed "
               "in by up to Amount, in noisy patches set by Coverage. Subdivisions add points to carve into first; Cut chamfers the "
               "edges before chipping, for broken corners. Wear is how damaged each point is, 0 to 1, for masks.",
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("angle", T::Float, 30.0, "Degrees."), 0.0, 180.0), bounded(in("amount", T::Float, 1.5, "Depth, cm."), 0.0),
                               bounded(in("size", T::Float, 3.0, "Reach from the edge, cm."), 0.001), bounded(in("chipScale", T::Float, 4.0, "Size of the chips, cm."), 0.001),
                               bounded(in("coverage", T::Float, 0.6), 0.0, 1.0), in("seed", T::Int, 0), bounded(in("subdivisions", T::Int, 2), 0, 5),
                               in("cut", T::Bool, false)},
                    .outputs = {geoOut("mesh"), fout("wear", T::Float)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                double angle = std::clamp(c.f("angle"), 0.0, 180.0), amount = std::max(0.0, c.f("amount")), size = std::max(0.001, c.f("size"));
                double chip = std::max(0.001, c.f("chipScale")), coverage = std::clamp(c.f("coverage"), 0.0, 1.0);
                double w = hashToUnit(hash32(uint32_t(c.i("seed")), 91)) * 1000.0;
                int levels = int(std::clamp<int64_t>(c.i("subdivisions"), 0, 5));
                bool cut = c.b("cut");
                std::string wearName = c.anonymous("wear");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<SharpEdge> edges = sharpEdges(m, angle);
                                                                      if (cut && !edges.empty())
                                                                      {
                                                                          std::vector<Index> ids;
                                                                          double cosLimit = std::cos(angle * kPi / 180.0);
                                                                          for (Index e = 0; e < m.edgeCapacity(); ++e)
                                                                              if (m.edgeValid(e) && m.edgeFaceCount(e) == 2)
                                                                              {
                                                                                  Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
                                                                                  if (glm::dot(m.faceNormal(m.cornerFace(c0)), m.faceNormal(m.cornerFace(c1))) <= cosLimit)
                                                                                      ids.push_back(e);
                                                                              }
                                                                          geo::BevelOptions bo;
                                                                          bo.offset = size * 0.4;
                                                                          geo::bevelEdges(m, ids, bo);
                                                                          m.compact();
                                                                      }
                                                                      for (int l = 0; l < levels; ++l)
                                                                          m = meshops::subdivideLinear(m);
                                                                      if (cut)
                                                                          edges = sharpEdges(m, std::min(angle, 20.0));
                                                                      std::vector<D3> moved(m.positions().begin(), m.positions().end());
                                                                      std::vector<float> wear(m.pointCapacity(), 0.0f);
                                                                      tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
                                                                          if (!m.pointValid(p))
                                                                              return;
                                                                          D3 q = m.position(p);
                                                                          double best = size;
                                                                          const SharpEdge* near = nullptr;
                                                                          for (const SharpEdge& e : edges)
                                                                              if (double dist = segmentDistance(q, e.a, e.b); dist < best)
                                                                              {
                                                                                  best = dist;
                                                                                  near = &e;
                                                                              }
                                                                          if (!near)
                                                                              return;
                                                                          double reach = 1.0 - best / size;
                                                                          double n01 = 0.5 + damageNoise(q / chip, w);
                                                                          double mask = smooth01((n01 - (1.0 - coverage)) * 5.0 + 0.5);
                                                                          double depth = reach * reach * mask * (0.6 + 0.8 * (0.5 + damageNoise(q / (chip * 0.35), w + 50.0)));
                                                                          wear[p] = float(std::clamp(reach * mask, 0.0, 1.0));
                                                                          moved[p] = q + near->inward * amount * depth;
                                                                      });
                                                                      for (Index p = 0; p < m.pointCapacity(); ++p)
                                                                          if (m.pointValid(p))
                                                                              m.setPosition(p, moved[p]);
                                                                      auto attr = m.pointAttrs().getOrAdd<float>(wearName);
                                                                      std::copy(wear.begin(), wear.end(), attr.begin());
                                                                      m.cornerAttrs().remove(geo::attr::kNormal);
                                                                  }}));
                c.out("wear", attributeField(wearName, T::Float));
            },
    });
}

// ------------------------------------------------------------------------------------------------ flowmap

/** The flow direction in the face's UV space: w written as a * dP/du + b * dP/dv in least squares. */
glm::dvec2 toUvSpace(const geo::Mesh& m, Index f, std::span<const glm::vec2> uv, const D3& w)
{
    std::vector<Index> cs = m.faceCorners(f);
    D3 p0 = m.position(m.cornerPoint(cs[0]));
    glm::dvec2 t0(uv[cs[0]]);
    // dP/du and dP/dv from the face's corner fan, least squares over its triangles.
    glm::dmat2 ata(0.0);
    glm::dmat3x2 atb(0.0);
    for (size_t i = 1; i + 1 < cs.size(); ++i)
    {
        D3 e1 = m.position(m.cornerPoint(cs[i])) - p0, e2 = m.position(m.cornerPoint(cs[i + 1])) - p0;
        glm::dvec2 d1 = glm::dvec2(uv[cs[i]]) - t0, d2 = glm::dvec2(uv[cs[i + 1]]) - t0;
        for (auto [du, dp] : {std::pair{d1, e1}, std::pair{d2, e2}})
        {
            ata += glm::outerProduct(du, du);
            for (int a = 0; a < 3; ++a)
                atb[a] += du * dp[a];
        }
    }
    if (std::abs(glm::determinant(ata)) < 1e-18)
        return {w.x, w.z};
    glm::dmat2 inv = glm::inverse(ata);
    // Rows of the UV gradient (dP/du, dP/dv) as columns of a 3x2 map.
    D3 dPdu, dPdv;
    for (int a = 0; a < 3; ++a)
    {
        glm::dvec2 g = inv * atb[a];
        dPdu[a] = g.x;
        dPdv[a] = g.y;
    }
    glm::dmat2 gram(glm::dot(dPdu, dPdu), glm::dot(dPdu, dPdv), glm::dot(dPdu, dPdv), glm::dot(dPdv, dPdv));
    if (std::abs(glm::determinant(gram)) < 1e-18)
        return {w.x, w.z};
    glm::dvec2 ab = glm::inverse(gram) * glm::dvec2(glm::dot(w, dPdu), glm::dot(w, dPdv));
    double len = glm::length(ab);
    return len > 1e-12 ? ab / len * glm::length(w) : glm::dvec2(0.0);
}

/** Nearest point on any guide polyline and the guide's direction there. */
struct GuideHit
{
    double distance = std::numeric_limits<double>::max();
    D3 tangent{0.0};
};

GuideHit nearestGuide(const CurveSet& guides, const D3& p)
{
    GuideHit hit;
    for (size_t cu = 0; cu < guides.numCurves(); ++cu)
    {
        std::span<const D3> pts = guides.curvePoints(cu);
        size_t n = pts.size(), segs = guides.cyclic[cu] ? n : n - 1;
        for (size_t i = 0; n >= 2 && i < segs; ++i)
        {
            const D3 &a = pts[i], &b = pts[(i + 1) % n];
            if (double d = segmentDistance(p, a, b); d < hit.distance && glm::length(b - a) > 1e-12)
            {
                hit.distance = d;
                hit.tangent = glm::normalize(b - a);
            }
        }
    }
    return hit;
}

void flowmapNode()
{
    registerDecl({
        .type = "Flowmap",
        .label = "Flowmap",
        .category = "Attribute",
        .doc = "Writes a flow direction over the mesh for flowmap shaders (water, lava, moss growth), laid in the surface: from the "
               "Direction field, downhill (slope), or along the nearest guide curve within Radius (blending into Direction beyond). "
               "It is stored per corner as a 2D vector in the UV map's space under Name, and with Color also as vertex colour "
               "(0.5 is still, red along U, green along V), as Unreal flowmap materials read it. Flip V suits textures that count V down.",
        .props = {prop("mode", {"direction", "slope", "curves"}, "slope")},
        .sockets = {.inputs = {geoIn("mesh"), fin("direction", T::Vector, vec(1, 0, 0)), geoIn("guides", "Guide curves for curves mode."),
                               bounded(in("radius", T::Float, 200.0, "Guide reach, cm."), 0.001), bounded(in("strength", T::Float, 1.0, "Length of the stored vector."), 0.0, 1.0),
                               in("uvMap", T::String, "uv0"), in("name", T::String, "flow"), in("color", T::Bool, true), in("flipV", T::Bool, false)},
                    .outputs = {geoOut("mesh"), fout("flow", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                std::string mode = c.prop("mode"), name = c.s("name"), uvName = c.s("uvMap");
                if (name.empty())
                    name = "flow";
                GeometryPtr guidesG = c.geometry("guides");
                CurveSet guides = guidesG->curves ? guidesG->curves->evaluated() : CurveSet();
                double radius = std::max(0.001, c.f("radius")), strength = std::clamp(c.f("strength"), 0.0, 1.0);
                bool asColor = c.b("color"), flipV = c.b("flipV");
                FieldPtr dirField = c.field("direction");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<glm::vec3> normals = geo::pointNormals(m);
                                                                      std::vector<D3> dir = evaluateAll<D3>(dirField, FieldContext::forMesh(m, Domain::Point));
                                                                      std::vector<D3> world(m.pointCapacity(), D3(0.0));
                                                                      tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
                                                                          if (!m.pointValid(p))
                                                                              return;
                                                                          D3 n(normals[p]);
                                                                          auto inPlane = [&](const D3& v) { return v - n * glm::dot(v, n); };
                                                                          D3 base = inPlane(dir[p]);
                                                                          D3 w = base;
                                                                          if (mode == "slope")
                                                                          {
                                                                              D3 down = inPlane(D3(0, -1, 0));
                                                                              w = glm::length(down) > 1e-4 ? down : base;
                                                                          }
                                                                          else if (mode == "curves" && guides.numCurves())
                                                                          {
                                                                              GuideHit h = nearestGuide(guides, m.position(p));
                                                                              double t = 1.0 - smooth01(h.distance / radius);
                                                                              D3 along = inPlane(h.tangent);
                                                                              double bl = glm::length(base), al = glm::length(along);
                                                                              D3 b = bl > 1e-12 ? base / bl : D3(0.0), a = al > 1e-12 ? along / al : D3(0.0);
                                                                              w = b * (1.0 - t) + a * t;
                                                                          }
                                                                          double len = glm::length(w);
                                                                          world[p] = len > 1e-12 ? w / len * strength : D3(0.0);
                                                                      });
                                                                      m.cornerAttrs().getOrAdd<glm::vec2>(name);
                                                                      if (asColor)
                                                                          m.cornerAttrs().getOrAdd<glm::vec4>(geo::attr::kColor);
                                                                      // Spans are taken after every layer is added, since adding one can move the others.
                                                                      std::span<const glm::vec2> uv = std::as_const(m).cornerAttrs().get<glm::vec2>(uvName);
                                                                      std::span<glm::vec2> flow = m.cornerAttrs().get<glm::vec2>(name);
                                                                      std::span<glm::vec4> color = asColor ? m.cornerAttrs().get<glm::vec4>(geo::attr::kColor) : std::span<glm::vec4>{};
                                                                      for (Index f = 0; f < m.faceCapacity(); ++f)
                                                                      {
                                                                          if (!m.faceValid(f))
                                                                              continue;
                                                                          m.forEachFaceCorner(f, [&](Index cc) {
                                                                              D3 w = world[m.cornerPoint(cc)];
                                                                              glm::dvec2 v = uv.empty() ? glm::dvec2(w.x, w.z) : toUvSpace(m, f, uv, w);
                                                                              if (flipV)
                                                                                  v.y = -v.y;
                                                                              flow[cc] = glm::vec2(v);
                                                                              if (asColor)
                                                                                  color[cc] = glm::vec4(0.5f + 0.5f * float(v.x), 0.5f + 0.5f * float(v.y), 0.0f, 1.0f);
                                                                          });
                                                                      }
                                                                  }}));
                c.out("flow", attributeField(name, T::Vector));
            },
    });
}
} // namespace deform2

void registerDeform2Nodes()
{
    deform2::latticeNode();
    deform2::pointDeformNode();
    deform2::attributeTransferNode();
    deform2::edgeDamageNode();
    deform2::flowmapNode();
}
} // namespace lila::gn::nodes
