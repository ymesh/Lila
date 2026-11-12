#include "gn/NodesCommon.h"
#include "gn/Volume.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>

namespace lila::gn::nodes::instances2
{
/** Static kd-tree over points: nearest neighbour, optionally skipping one point. */
class NearestTree
{
public:
    NearestTree(const std::vector<D3>& all, std::vector<Index> ids) : mAll(all), mIds(std::move(ids)) { build(0, mIds.size(), 0); }

    /** The nearest of the tree's points other than skip, kInvalid when there is none. */
    Index nearest(const D3& q, Index skip) const
    {
        Index best = geo::kInvalid;
        double bestD = std::numeric_limits<double>::max();
        search(0, mIds.size(), 0, q, skip, best, bestD);
        return best;
    }

private:
    void build(size_t lo, size_t hi, int axis)
    {
        if (hi - lo <= 1)
            return;
        size_t mid = (lo + hi) / 2;
        std::nth_element(mIds.begin() + ptrdiff_t(lo), mIds.begin() + ptrdiff_t(mid), mIds.begin() + ptrdiff_t(hi),
                         [&](Index a, Index b) { return mAll[a][axis] < mAll[b][axis]; });
        build(lo, mid, (axis + 1) % 3);
        build(mid + 1, hi, (axis + 1) % 3);
    }

    void search(size_t lo, size_t hi, int axis, const D3& q, Index skip, Index& best, double& bestD) const
    {
        if (lo >= hi)
            return;
        size_t mid = (lo + hi) / 2;
        Index i = mIds[mid];
        D3 d = mAll[i] - q;
        double dd = glm::dot(d, d);
        if (i != skip && (dd < bestD || (dd == bestD && i < best)))
        {
            bestD = dd;
            best = i;
        }
        double diff = q[axis] - mAll[i][axis];
        int next = (axis + 1) % 3;
        size_t nearLo = diff < 0.0 ? lo : mid + 1, nearHi = diff < 0.0 ? mid : hi;
        size_t farLo = diff < 0.0 ? mid + 1 : lo, farHi = diff < 0.0 ? hi : mid;
        search(nearLo, nearHi, next, q, skip, best, bestD);
        if (diff * diff <= bestD)
            search(farLo, farHi, next, q, skip, best, bestD);
    }

    const std::vector<D3>& mAll;
    std::vector<Index> mIds;
};

class NearestIndexField final : public Field
{
public:
    NearestIndexField(FieldPtr position, FieldPtr group, bool has) : Field(has ? SocketType::Bool : SocketType::Int), mPos(std::move(position)), mGroup(std::move(group)), mHas(has) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        std::vector<D3> pos = evaluateAll<D3>(mPos, ctx);
        std::vector<int64_t> group = evaluateAll<int64_t>(mGroup, ctx);
        std::map<int64_t, std::vector<Index>> members;
        for (size_t k = 0; k < pos.size(); ++k)
            members[group[k]].push_back(Index(k));
        std::vector<int64_t> idx(pos.size(), -1);
        for (auto& [g, ids] : members)
        {
            NearestTree tree(pos, ids);
            const std::vector<Index>& list = ids;
            auto body = [&](size_t k) {
                Index i = list[k];
                Index n = tree.nearest(pos[i], i);
                idx[i] = n == geo::kInvalid ? -1 : int64_t(n);
            };
            if (list.size() > 2048)
                tbb::parallel_for(size_t(0), list.size(), body);
            else
                for (size_t k = 0; k < list.size(); ++k)
                    body(k);
        }
        if (!mHas)
            return Column::of(std::move(idx));
        std::vector<uint8_t> has(idx.size());
        for (size_t k = 0; k < idx.size(); ++k)
            has[k] = idx[k] >= 0;
        return Column::of(std::move(has));
    }

private:
    FieldPtr mPos, mGroup;
    bool mHas;
};

geo::Bounds boundsOf(const Geometry& g)
{
    Geometry r = realize(g);
    geo::Bounds b;
    if (r.mesh)
        b.extend(r.mesh->bounds());
    if (r.points)
        b.extend(r.points->bounds());
    if (r.curves)
        for (const D3& p : r.curves->positions)
            b.extend(p);
    if (r.heightfield && !r.heightfield->empty())
        b.extend(geo::heightfieldToMesh(*r.heightfield).bounds());
    if (r.volume && !r.volume->empty())
        b.extend(r.volume->bounds());
    return b;
}

class InstanceBoundsField final : public Field
{
public:
    InstanceBoundsField(bool max, FieldPtr local) : Field(SocketType::Vector), mMax(max), mLocal(std::move(local)) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Instances)
            return Column::of(D3(0.0));
        const Instances& in = *ctx.instances;
        std::vector<geo::Bounds> refBounds(in.references.size());
        for (size_t r = 0; r < refBounds.size(); ++r)
            if (in.references[r])
                refBounds[r] = boundsOf(*in.references[r]);
        std::vector<uint8_t> local = evaluateAll<uint8_t>(mLocal, ctx);
        std::vector<D3> out(in.size(), D3(0.0));
        for (size_t k = 0; k < in.size(); ++k)
        {
            int r = in.reference[k];
            if (r < 0 || size_t(r) >= refBounds.size() || !refBounds[size_t(r)].valid())
                continue;
            const geo::Bounds& b = refBounds[size_t(r)];
            if (local[k])
            {
                out[k] = mMax ? b.max : b.min;
                continue;
            }
            geo::Bounds w;
            for (int corner = 0; corner < 8; ++corner)
            {
                D3 p((corner & 1) ? b.max.x : b.min.x, (corner & 2) ? b.max.y : b.min.y, (corner & 4) ? b.max.z : b.min.z);
                w.extend(D3(in.transforms[k] * glm::dvec4(p, 1.0)));
            }
            out[k] = mMax ? w.max : w.min;
        }
        return Column::of(std::move(out));
    }

private:
    bool mMax;
    FieldPtr mLocal;
};

geo::Mesh pointSubset(const geo::Mesh& m, const std::vector<Index>& ids)
{
    geo::Mesh out;
    out.pointAttrs().mergeLayout(m.pointAttrs());
    for (Index p : ids)
    {
        Index np = out.addPoint(m.position(p));
        out.pointAttrs().copyFrom(m.pointAttrs(), np, p);
    }
    return out;
}

CurveSet curveSubset(const CurveSet& cs, const std::vector<Index>& curves)
{
    CurveSet out;
    out.pointAttrs.mergeLayout(cs.pointAttrs);
    out.curveAttrs.mergeLayout(cs.curveAttrs);
    for (Index cu : curves)
    {
        size_t first = out.numPoints();
        out.addCurve(cs.curvePoints(cu), cs.cyclic[cu] != 0);
        for (size_t k = 0; k < cs.curveSize(cu); ++k)
            out.pointAttrs.copyFrom(cs.pointAttrs, first + k, cs.offsets[cu] + Index(k));
        out.curveAttrs.copyFrom(cs.curveAttrs, out.numCurves() - 1, cu);
        if (cu < cs.knots.size() && !cs.knots[cu].empty())
        {
            out.knots.resize(out.numCurves());
            out.knots.back() = cs.knots[cu];
        }
    }
    return out;
}

Instances instanceSubset(const Instances& in, const std::vector<Index>& ids)
{
    Instances out;
    out.references = in.references;
    out.attrs.mergeLayout(in.attrs);
    for (Index k : ids)
    {
        out.add(in.transforms[k], in.reference[k]);
        out.attrs.copyFrom(in.attrs, out.size() - 1, k);
    }
    return out;
}

/** Selected element indices bucketed by group id, groups in ascending id. */
std::map<int64_t, std::vector<Index>> groupsOf(ExecContext& c, const FieldContext& ctx)
{
    std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
    std::vector<int64_t> id = evaluateAll<int64_t>(c.field("groupId"), ctx);
    std::map<int64_t, std::vector<Index>> out;
    for (size_t k = 0; k < ctx.size; ++k)
        if (sel[k])
            out[id[k]].push_back(Index(k));
    return out;
}

void splitToInstances()
{
    registerDecl({
        .type = "SplitToInstances",
        .label = "Split to Instances",
        .category = "Instances",
        .doc = "One instance per group id of the selected elements, at the origin, each holding its group's part: faces of the mesh, "
               "points (mesh points become a point cloud), curves or instances. Groups go in order of id.",
        .props = {prop("domain", {"point", "face", "curve", "instance"}, "face")},
        .sockets = {.inputs = {geoIn(), selectionIn(), fin("groupId", T::Int, 0)}, .outputs = {geoOut("instances"), fout("groupId", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("geometry");
                Domain d = domainOf(c.prop("domain"));
                std::map<int64_t, std::vector<GeometryPtr>> parts;
                if (d == Domain::Face && g->mesh)
                {
                    const geo::Mesh& m = *g->mesh;
                    for (auto& [id, faces] : groupsOf(c, FieldContext::forMesh(m, Domain::Face)))
                    {
                        std::vector<uint8_t> keep(m.faceCapacity(), 0);
                        for (Index f : faces)
                            keep[f] = 1;
                        std::vector<Index> drop;
                        for (Index f = 0; f < m.faceCapacity(); ++f)
                            if (m.faceValid(f) && !keep[f])
                                drop.push_back(f);
                        geo::Mesh part = m;
                        geo::deleteFaces(part, drop);
                        part.compact();
                        parts[id].push_back(meshGeometry(std::move(part)));
                    }
                }
                if (d == Domain::Point)
                {
                    for (const geo::Mesh* m : {g->mesh.get(), g->points.get()})
                        if (m)
                            for (auto& [id, pts] : groupsOf(c, m == g->points.get() ? FieldContext::forPoints(*m) : FieldContext::forMesh(*m, Domain::Point)))
                                parts[id].push_back(pointGeometry(pointSubset(*m, pts)));
                }
                if (d == Domain::Curve && g->curves)
                    for (auto& [id, curves] : groupsOf(c, FieldContext::forCurves(*g->curves, Domain::Curve)))
                        parts[id].push_back(curveGeometry(curveSubset(*g->curves, curves)));
                if (d == Domain::Instance && g->instances)
                    for (auto& [id, ids] : groupsOf(c, FieldContext::forInstances(*g->instances)))
                    {
                        Geometry sub;
                        sub.instances = std::make_shared<const Instances>(instanceSubset(*g->instances, ids));
                        parts[id].push_back(makeGeometry(std::move(sub)));
                    }
                Instances out;
                std::vector<int64_t> ids;
                for (auto& [id, list] : parts)
                {
                    out.references.push_back(list.size() == 1 ? list[0] : makeGeometry(join(list)));
                    out.add(glm::dmat4(1.0), int(out.references.size()) - 1);
                    ids.push_back(id);
                }
                const std::string attr = c.anonymous("groupId");
                writeAttribute(out, attr, Column::of(std::move(ids)), T::Int);
                Geometry result;
                result.instances = std::make_shared<const Instances>(std::move(out));
                c.out("instances", makeGeometry(std::move(result)));
                c.out("groupId", attributeField(attr, T::Int));
            },
    });
}

void instanceBounds()
{
    registerDecl({
        .type = "InstanceBounds",
        .label = "Instance Bounds",
        .category = "Instances",
        .doc = "Corners of each instance's axis-aligned bounding box: around the placed instance, or in its own space when local.",
        .sockets = {.inputs = {fin("local", T::Bool, false)}, .outputs = {fout("min", T::Vector), fout("max", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                c.out("min", std::make_shared<InstanceBoundsField>(false, c.field("local")));
                c.out("max", std::make_shared<InstanceBoundsField>(true, c.field("local")));
            },
    });
}

void setId()
{
    registerDecl({
        .type = "SetID",
        .label = "Set ID",
        .category = "Geometry",
        .doc = "Writes the id attribute that random values and the ID input read: on the points of meshes, point clouds and curves, and on instances.",
        .sockets = {.inputs = {geoIn(), selectionIn(), fin("id", T::Int, nullptr, Implicit::Index)}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                auto values = [&](const FieldContext& ctx) {
                    std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                    std::vector<int64_t> id = evaluateAll<int64_t>(c.field("id"), ctx);
                    std::vector<int64_t> old = evaluateAll<int64_t>(idField(), ctx);
                    for (size_t k = 0; k < id.size(); ++k)
                        if (!sel[k])
                            id[k] = old[k];
                    return Column::of(std::move(id));
                };
                c.out("geometry", editComponents(c.geometry("geometry"),
                                                 {.mesh = [&](geo::Mesh& m) { writeAttribute(m, "id", Domain::Point, values(FieldContext::forMesh(m, Domain::Point)), T::Int); },
                                                  .points = [&](geo::Mesh& m) { writeAttribute(m, "id", Domain::Point, values(FieldContext::forPoints(m)), T::Int); },
                                                  .curves = [&](CurveSet& cs) { writeAttribute(cs, "id", Domain::Point, values(FieldContext::forCurves(cs, Domain::Point)), T::Int); },
                                                  .instances = [&](Instances& in) { writeAttribute(in, "id", values(FieldContext::forInstances(in)), T::Int); }}));
            },
    });
}

void indexOfNearest()
{
    registerDecl({
        .type = "IndexOfNearest",
        .label = "Index of Nearest",
        .category = "Geometry",
        .doc = "The index of the closest other element with the same group id, -1 when it is alone in its group.",
        .sockets = {.inputs = {fin("position", T::Vector, nullptr, Implicit::Position), fin("groupId", T::Int, 0)},
                    .outputs = {fout("index", T::Int), fout("hasNeighbor", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                c.out("index", std::make_shared<NearestIndexField>(c.field("position"), c.field("groupId"), false));
                c.out("hasNeighbor", std::make_shared<NearestIndexField>(c.field("position"), c.field("groupId"), true));
            },
    });
}

void geometryName()
{
    registerDecl({
        .type = "SetGeometryName",
        .label = "Set Geometry Name",
        .category = "Geometry",
        .doc = "Labels the geometry. Joins keep the first part's name; nodes that edit geometry keep it.",
        .sockets = {.inputs = {geoIn(), in("name", T::String, "")}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                Geometry g = *c.geometry("geometry");
                g.name = c.s("name");
                c.out("geometry", makeGeometry(std::move(g)));
            },
    });
    registerDecl({
        .type = "GeometryName",
        .label = "Geometry Name",
        .category = "Geometry",
        .doc = "The geometry's name, empty when it has none.",
        .sockets = {.inputs = {geoIn()}, .outputs = {out("name", T::String)}},
        .exec = [](ExecContext& c) { c.outString("name", c.geometry("geometry")->name); },
    });
}

void distributeInGrid()
{
    registerDecl({
        .type = "DistributePointsInGrid",
        .label = "Distribute Points in Grid",
        .category = "Volume",
        .doc = "Points on the volume's own voxels where it is solid (a density above threshold, a signed distance below 0). Centers puts "
               "one at each such voxel's centre; random scatters density points per voxel, scaled by a density's value, within the "
               "voxel. Each point gets the voxel's value as \"value\".",
        .props = {prop("mode", {"centers", "random"}, "centers")},
        .sockets = {.inputs = {geoIn("volume"), in("threshold", T::Float, 0.1), bounded(in("density", T::Float, 1.0, "Random: points per voxel."), 0.0), in("seed", T::Int, 0)},
                    .outputs = {geoOut("points")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                std::shared_ptr<const Volume> v = c.geometry("volume")->volume;
                geo::Mesh pts;
                std::vector<double> value;
                if (v && !v->empty())
                {
                    const bool random = c.prop("mode") == "random";
                    const double threshold = c.f("threshold"), density = c.f("density");
                    const uint32_t seed = uint32_t(c.i("seed"));
                    for (int k = 0; k < v->dims.z; ++k)
                        for (int j = 0; j < v->dims.y; ++j)
                            for (int i = 0; i < v->dims.x; ++i)
                            {
                                const double x = v->at(i, j, k);
                                if (v->isSdf() ? x >= 0.0 : x <= threshold)
                                    continue;
                                if (!random)
                                {
                                    pts.addPoint(v->worldOf(D3(i, j, k)));
                                    value.push_back(x);
                                    continue;
                                }
                                const uint32_t cell = hash32(uint32_t(v->index(i, j, k)), seed);
                                const double want = density * (v->isSdf() ? 1.0 : x);
                                int n = int(std::floor(want));
                                if (hashToUnit(hash32(cell, 1)) < want - n)
                                    ++n;
                                if (pts.numPoints() + Index(n) > 50000000)
                                    throw ExecError(c.node.name + ": over 50 million points; lower the density");
                                for (int s = 0; s < n; ++s)
                                {
                                    D3 r(hashToUnit(hash32(cell, uint32_t(s), 2)), hashToUnit(hash32(cell, uint32_t(s), 3)), hashToUnit(hash32(cell, uint32_t(s), 4)));
                                    pts.addPoint(v->worldOf(D3(i, j, k) + r - 0.5));
                                    value.push_back(x);
                                }
                            }
                }
                writeAttribute(pts, "value", Domain::Point, Column::of(std::move(value)), T::Float);
                c.out("points", pointGeometry(std::move(pts)));
            },
    });
}

/** Positions to cluster: the point cloud, else the mesh's points. */
const geo::Mesh* clusterSource(const Geometry& g) { return g.points ? g.points.get() : g.mesh.get(); }

/** k-means with k-means++ seeding: a cluster per point and the cluster centres. */
void kMeans(const std::vector<D3>& p, int k, uint32_t seed, int iterations, std::vector<int64_t>& label, std::vector<D3>& centres)
{
    const size_t n = p.size();
    label.assign(n, 0);
    centres.clear();
    if (n == 0 || k <= 0)
        return;
    k = int(std::min<size_t>(size_t(k), n));
    centres.push_back(p[size_t(hashToUnit(hash32(seed, 0, 11)) * double(n)) % n]);
    std::vector<double> d2(n, std::numeric_limits<double>::max());
    while (int(centres.size()) < k)
    {
        double total = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            D3 d = p[i] - centres.back();
            d2[i] = std::min(d2[i], glm::dot(d, d));
            total += d2[i];
        }
        double pick = hashToUnit(hash32(seed, uint32_t(centres.size()), 12)) * total;
        size_t chosen = n - 1;
        for (size_t i = 0; i < n; ++i)
        {
            pick -= d2[i];
            if (pick <= 0.0)
            {
                chosen = i;
                break;
            }
        }
        if (total <= 0.0)
            chosen = centres.size() % n;
        centres.push_back(p[chosen]);
    }
    for (int it = 0; it < std::max(1, iterations); ++it)
    {
        auto assign = [&](size_t i) {
            double best = std::numeric_limits<double>::max();
            for (size_t c = 0; c < centres.size(); ++c)
            {
                D3 d = p[i] - centres[c];
                double dd = glm::dot(d, d);
                if (dd < best)
                {
                    best = dd;
                    label[i] = int64_t(c);
                }
            }
        };
        tbb::parallel_for(size_t(0), n, assign);
        std::vector<D3> sum(centres.size(), D3(0.0));
        std::vector<size_t> count(centres.size(), 0);
        for (size_t i = 0; i < n; ++i)
        {
            sum[size_t(label[i])] += p[i];
            ++count[size_t(label[i])];
        }
        bool moved = false;
        for (size_t c = 0; c < centres.size(); ++c)
            if (count[c])
            {
                D3 next = sum[c] / double(count[c]);
                moved = moved || glm::distance(next, centres[c]) > 1e-9;
                centres[c] = next;
            }
        if (!moved)
            break;
    }
}

void clusterPoints()
{
    registerDecl({
        .type = "ClusterPoints",
        .label = "Cluster Points",
        .category = "Point",
        .doc = "Groups the points (of the point cloud, else the mesh) into count clusters of nearby points (k-means). Gives each point "
               "its cluster, and a point at each cluster's centre carrying \"cluster\" and its point count as \"size\".",
        .sockets = {.inputs = {geoIn(), bounded(in("count", T::Int, 8), 1, 100000), in("seed", T::Int, 0), bounded(in("iterations", T::Int, 20), 1, 1000)},
                    .outputs = {geoOut(), fout("cluster", T::Int), geoOut("centroids")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("geometry");
                const std::string attr = c.anonymous("cluster");
                const geo::Mesh* src = clusterSource(*g);
                geo::Mesh cents;
                if (src)
                {
                    std::vector<D3> p(src->positions().begin(), src->positions().end());
                    std::vector<int64_t> label;
                    std::vector<D3> centres;
                    kMeans(p, int(c.i("count")), uint32_t(c.i("seed")), int(c.i("iterations")), label, centres);
                    std::vector<int64_t> ids(centres.size()), size(centres.size(), 0);
                    for (int64_t l : label)
                        ++size[size_t(l)];
                    for (size_t k = 0; k < centres.size(); ++k)
                    {
                        cents.addPoint(centres[k]);
                        ids[k] = int64_t(k);
                    }
                    writeAttribute(cents, "cluster", Domain::Point, Column::of(std::move(ids)), T::Int);
                    writeAttribute(cents, "size", Domain::Point, Column::of(std::move(size)), T::Int);
                    Column col = Column::of(std::move(label));
                    if (g->points)
                        g = editComponents(g, {.points = [&](geo::Mesh& m) { writeAttribute(m, attr, Domain::Point, col, T::Int); }});
                    else
                        g = editComponents(g, {.mesh = [&](geo::Mesh& m) { writeAttribute(m, attr, Domain::Point, col, T::Int); }});
                }
                c.out("geometry", g);
                c.out("cluster", attributeField(attr, T::Int));
                c.out("centroids", pointGeometry(std::move(cents)));
            },
    });
}

/** Accepted boxes in a uniform hash grid, for overlap tests. */
class BoxGrid
{
public:
    explicit BoxGrid(double cell) : mCell(std::max(cell, 1e-6)) {}

    bool overlaps(const geo::Bounds& b) const
    {
        bool hit = false;
        forCells(b, [&](int64_t key) {
            auto it = mCells.find(key);
            if (it == mCells.end())
                return;
            for (size_t i : it->second)
            {
                const geo::Bounds& o = mBoxes[i];
                if (b.min.x < o.max.x && b.max.x > o.min.x && b.min.y < o.max.y && b.max.y > o.min.y && b.min.z < o.max.z && b.max.z > o.min.z)
                {
                    if (!mSpheres || glm::distance(b.center(), o.center()) < 0.5 * (b.size().x + o.size().x))
                        hit = true;
                }
            }
        });
        return hit;
    }
    void add(const geo::Bounds& b)
    {
        size_t i = mBoxes.size();
        mBoxes.push_back(b);
        forCells(b, [&](int64_t key) { mCells[key].push_back(i); });
    }
    void spheres(bool s) { mSpheres = s; }

private:
    template <class Fn> void forCells(const geo::Bounds& b, Fn&& fn) const
    {
        glm::vec<3, int64_t> lo(glm::floor(b.min / mCell)), hi(glm::floor(b.max / mCell));
        for (int64_t z = lo.z; z <= hi.z; ++z)
            for (int64_t y = lo.y; y <= hi.y; ++y)
                for (int64_t x = lo.x; x <= hi.x; ++x)
                    fn((x * 73856093) ^ (y * 19349663) ^ (z * 83492791));
    }

    double mCell;
    bool mSpheres = false;
    std::vector<geo::Bounds> mBoxes;
    std::unordered_map<int64_t, std::vector<size_t>> mCells;
};

/** Which elements to keep: in priority order, each one whose bounds overlap none kept before it. Unselected elements are kept and block. */
std::vector<uint8_t> pruneKeep(ExecContext& c, const FieldContext& ctx, std::span<const D3> pos)
{
    const size_t n = pos.size();
    const bool box = c.prop("shape") == "box";
    const std::string order = c.prop("priority");
    std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
    std::vector<double> radius = evaluateAll<double>(c.field("radius"), ctx);
    std::vector<D3> size = evaluateAll<D3>(c.field("size"), ctx);
    std::vector<double> weight = evaluateAll<double>(c.field("weight"), ctx);
    const uint32_t seed = uint32_t(c.i("seed"));
    std::vector<geo::Bounds> bounds(n);
    double largest = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        D3 half = box ? glm::abs(size[k]) * 0.5 : D3(std::abs(radius[k]));
        bounds[k].min = pos[k] - half;
        bounds[k].max = pos[k] + half;
        largest = std::max({largest, 2.0 * half.x, 2.0 * half.y, 2.0 * half.z});
    }
    std::vector<double> key(n);
    for (size_t k = 0; k < n; ++k)
    {
        D3 s = bounds[k].size();
        key[k] = order == "random" ? hashToUnit(hash32(uint32_t(k), seed)) : order == "largestFirst" ? s.x * s.y * s.z : weight[k];
    }
    std::vector<size_t> rank(n);
    std::iota(rank.begin(), rank.end(), size_t(0));
    std::stable_sort(rank.begin(), rank.end(), [&](size_t a, size_t b) {
        if (sel[a] != sel[b])
            return sel[a] < sel[b];
        return key[a] > key[b];
    });
    BoxGrid grid(largest > 0.0 ? largest : 1.0);
    grid.spheres(!box);
    std::vector<uint8_t> keep(n, 1);
    for (size_t k : rank)
    {
        if (sel[k] && grid.overlaps(bounds[k]))
        {
            keep[k] = 0;
            continue;
        }
        grid.add(bounds[k]);
    }
    return keep;
}

std::vector<Index> keptIds(const std::vector<uint8_t>& keep)
{
    std::vector<Index> ids;
    for (size_t k = 0; k < keep.size(); ++k)
        if (keep[k])
            ids.push_back(Index(k));
    return ids;
}

void selfPrune()
{
    registerDecl({
        .type = "SelfPrune",
        .label = "Self Prune",
        .category = "Point",
        .doc = "Removes points and instances that overlap ones kept before them, given each a sphere of radius or a box of size around "
               "its position. Priority sets who goes first: higher weight, a seeded random order, or the largest first. Unselected "
               "elements are kept and still block others.",
        .props = {prop("shape", {"sphere", "box"}, "sphere"), prop("priority", {"weight", "random", "largestFirst"}, "weight")},
        .sockets = {.inputs = {geoIn(), selectionIn(), bounded(fin("radius", T::Float, 10.0), 0.0), fin("size", T::Vector, vec(20, 20, 20), Implicit::None, "Box: full extents."),
                               fin("weight", T::Float, 0.0, Implicit::None, "Higher is kept first."), in("seed", T::Int, 0)},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                c.out("geometry", editComponents(c.geometry("geometry"), {.points =
                                                                              [&](geo::Mesh& m) {
                                                                                  FieldContext ctx = FieldContext::forPoints(m);
                                                                                  std::vector<D3> pos(m.positions().begin(), m.positions().end());
                                                                                  m = pointSubset(m, keptIds(pruneKeep(c, ctx, pos)));
                                                                              },
                                                                          .instances =
                                                                              [&](Instances& in) {
                                                                                  FieldContext ctx = FieldContext::forInstances(in);
                                                                                  std::vector<D3> pos(in.size());
                                                                                  for (size_t k = 0; k < pos.size(); ++k)
                                                                                      pos[k] = D3(in.transforms[k][3]);
                                                                                  in = instanceSubset(in, keptIds(pruneKeep(c, ctx, pos)));
                                                                              }}));
            },
    });
}
} // namespace lila::gn::nodes::instances2

namespace lila::gn::nodes
{
void registerInstanceNodes2()
{
    instances2::splitToInstances();
    instances2::instanceBounds();
    instances2::setId();
    instances2::indexOfNearest();
    instances2::geometryName();
    instances2::distributeInGrid();
    instances2::clusterPoints();
    instances2::selfPrune();
}
} // namespace lila::gn::nodes
