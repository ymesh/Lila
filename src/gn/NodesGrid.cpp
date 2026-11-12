#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "gn/Volume.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <climits>
#include <optional>

namespace lila::gn::nodes
{
namespace grid_nodes
{
using GridPtr = std::shared_ptr<const Volume>;
using V3 = glm::vec3;

constexpr size_t kMaxGridPoints = size_t(50) << 20;

GridPtr gridOf(const ExecContext& c, std::string_view id)
{
    GridPtr v = c.geometry(id)->volume;
    return v && !v->empty() ? v : nullptr;
}

GridPtr vectorGridOf(const ExecContext& c, std::string_view id)
{
    GridPtr v = gridOf(c, id);
    if (v && !v->isVector())
        throw ExecError(c.node.name + ": needs a vector grid (Grid Gradient, Field to Grid with type vector), not a " +
                        (v->isSdf() ? "signed distance" : "density"));
    return v;
}

/** Lengths in values brought in line after a vector grid's vectors were edited in place. */
void syncLengths(Volume& v)
{
    for (size_t n = 0; n < v.vectors.size(); ++n)
        v.values[n] = glm::length(v.vectors[n]);
}

GeometryPtr replaceGrid(const GeometryPtr& g, Volume v)
{
    Geometry out = *g;
    out.volume = std::make_shared<const Volume>(std::move(v));
    return makeGeometry(std::move(out));
}

/** Runs fn(i, j, k) over every voxel, slices in parallel. */
template <class Fn> void eachVoxel(const glm::ivec3& dims, Fn&& fn)
{
    tbb::parallel_for(0, dims.z, [&](int k) {
        for (int j = 0; j < dims.y; ++j)
            for (int i = 0; i < dims.x; ++i)
                fn(i, j, k);
    });
}

glm::ivec3 clampVoxel(const Volume& v, glm::ivec3 p) { return glm::clamp(p, glm::ivec3(0), v.dims - 1); }
float clampedAt(const Volume& v, glm::ivec3 p)
{
    p = clampVoxel(v, p);
    return v.values[v.index(p.x, p.y, p.z)];
}
V3 clampedVectorAt(const Volume& v, glm::ivec3 p)
{
    p = clampVoxel(v, p);
    return v.vectors[v.index(p.x, p.y, p.z)];
}

/** Central difference along a voxel axis, one-sided at the grid's edge. */
template <class Read> auto indexDerivative(const Volume& v, glm::ivec3 p, int axis, Read&& read)
{
    glm::ivec3 lo = p, hi = p;
    lo[axis] = std::max(p[axis] - 1, 0);
    hi[axis] = std::min(p[axis] + 1, v.dims[axis] - 1);
    const int span = hi[axis] - lo[axis];
    using R = decltype(read(p));
    return span > 0 ? (read(hi) - read(lo)) / float(span) : R(0.0f);
}

/** Turns derivatives along the voxel axes into world-space ones: d/dworld = A^-T d/dindex. */
glm::dmat3 worldFromIndexDerivatives(const Volume& v) { return glm::transpose(glm::inverse(v.axes())); }

/** The world-space Jacobian of a vector grid at a voxel: column b is d(vector)/d(world b). */
glm::dmat3 jacobian(const Volume& v, const glm::dmat3& inverseAxes, glm::ivec3 p)
{
    auto read = [&](glm::ivec3 q) { return clampedVectorAt(v, q); };
    glm::dmat3 index;
    for (int a = 0; a < 3; ++a)
        index[a] = glm::dvec3(indexDerivative(v, p, a, read));
    return index * inverseAxes;
}

Volume scalarLike(const Volume& src, Volume::Kind kind, std::string name)
{
    Volume out;
    out.name = std::move(name);
    out.kind = kind;
    out.dims = src.dims;
    out.indexToWorld = src.indexToWorld;
    out.background = 0.0f;
    out.values.assign(src.size(), 0.0f);
    return out;
}

/** Voxel centres as loose points, the context Field to Grid evaluates its field on. */
geo::Mesh voxelCentres(const Volume& v)
{
    geo::Mesh centres;
    centres.reserve(v.size(), 0, 0, 0);
    for (int k = 0; k < v.dims.z; ++k)
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
                centres.addPoint(v.worldOf({i, j, k}));
    return centres;
}

SocketDecl gridVoxelSizeIn() { return bounded(in("voxelSize", T::Float, 5.0, "Edge length of a voxel."), 0.01); }

SocketDecl gridOut() { return geoOut("grid"); }

void meshToVolume()
{
    registerDecl({
        .type = "MeshToVolume",
        .label = "Mesh to Volume",
        .category = "Volume",
        .doc = "A density grid (fog) filling a closed mesh. With fill off, only voxels within the interior band of the surface hold the "
               "density, a hollow shell.",
        .sockets = {.inputs = {geoIn("mesh"), in("density", T::Float, 1.0), gridVoxelSizeIn(),
                               bounded(in("interiorBand", T::Float, 10.0, "Depth of the shell when fill is off."), 0.0),
                               in("fill", T::Bool, true, "Fill the whole interior.")},
                    .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("mesh");
                std::shared_ptr<const geo::Mesh> m = g->instances ? realize(*g).mesh : g->mesh;
                if (!m || m->numFaces() == 0)
                {
                    c.out("grid", makeGeometry({}));
                    return;
                }
                const double voxel = std::max(c.f("voxelSize"), 0.01), band = std::max(c.f("interiorBand"), 0.0);
                const bool fill = c.b("fill");
                // Inside is decided by parity, so a filled fog only needs the distances one voxel deep.
                const int voxels = fill ? 1 : std::clamp(int(std::ceil(band / voxel)) + 1, 1, 1000);
                c.out("grid", volumeGeometry(sdfToFog(meshToSdf(*m, voxel, voxels), c.f("density"), band, fill)));
            },
    });
}

void pointsToVolume()
{
    registerDecl({
        .type = "PointsToVolume",
        .label = "Points to Volume",
        .category = "Volume",
        .doc = "A density grid (fog) holding density inside a sphere around each point (the points component, or the mesh's vertices "
               "when there are none).",
        .sockets = {.inputs = {geoIn("points"), in("density", T::Float, 1.0), bounded(fin("radius", T::Float, 10.0), 0.0), gridVoxelSizeIn()},
                    .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("points");
                std::shared_ptr<const geo::Mesh> m = g->points;
                FieldContext ctx;
                if (m)
                    ctx = FieldContext::forPoints(*m);
                else
                {
                    m = g->instances ? realize(*g).mesh : g->mesh;
                    if (m)
                        ctx = FieldContext::forMesh(*m, Domain::Point);
                }
                if (!m || m->numPoints() == 0)
                {
                    c.out("grid", makeGeometry({}));
                    return;
                }
                std::vector<D3> pts;
                pts.reserve(m->numPoints());
                for (Index p = 0; p < m->pointCapacity(); ++p)
                    if (m->pointValid(p))
                        pts.push_back(m->position(p));
                std::vector<double> radii = evaluateAll<double>(c.field("radius"), ctx);
                if (radii.size() != pts.size())
                    radii.resize(1, radii.empty() ? 0.0 : radii[0]);
                Volume sdf = pointsToSdf(pts, radii, std::max(c.f("voxelSize"), 0.01), 1);
                c.out("grid", volumeGeometry(sdfToFog(sdf, c.f("density"), 0.0, true)));
            },
    });
}

void fieldToGrid()
{
    registerDecl({
        .type = "FieldToGrid",
        .label = "Field to Grid",
        .category = "Volume",
        .doc = "A grid on the topology grid's voxels holding a field, read with Position as each voxel's centre and Index as its voxel. "
               "Float keeps a signed distance topology's kind and makes a density otherwise.",
        .props = {prop("type", {"float", "vector"}, "float")},
        .sockets = {.inputs = {geoIn("topology", "The grid whose voxels are filled."), fin("value", T::Float, 0.0),
                               fin("vector", T::Vector, vec(0, 0, 0), Implicit::None, "Read when type is vector."),
                               in("background", T::Float, 0.0, "Value outside the grid, for float.")},
                    .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr topo = gridOf(c, "topology");
                if (!topo)
                {
                    c.out("grid", c.geometry("topology"));
                    return;
                }
                const geo::Mesh centres = voxelCentres(*topo);
                const FieldContext ctx = FieldContext::forPoints(centres);
                Volume out = scalarLike(*topo, topo->isSdf() ? Volume::Kind::Sdf : Volume::Kind::Density, topo->name);
                if (c.prop("type") == "vector")
                {
                    std::vector<D3> v = evaluateAll<D3>(c.field("vector"), ctx);
                    std::vector<V3> vectors(out.size());
                    for (size_t n = 0; n < vectors.size(); ++n)
                        vectors[n] = V3(v[std::min(n, v.size() - 1)]);
                    out.name = "vector";
                    out.setVectors(std::move(vectors));
                }
                else
                {
                    std::vector<double> v = evaluateAll<double>(c.field("value"), ctx);
                    for (size_t n = 0; n < out.size(); ++n)
                        out.values[n] = float(v[std::min(n, v.size() - 1)]);
                    out.background = float(c.f("background"));
                }
                c.out("grid", replaceGrid(c.geometry("topology"), std::move(out)));
            },
    });
}

void gridToPoints()
{
    registerDecl({
        .type = "GridToPoints",
        .label = "Grid to Points",
        .category = "Volume",
        .doc = "A point at the centre of every voxel whose value differs from the background by more than threshold (a vector's length "
               "above it), carrying the voxel's value.",
        .sockets = {.inputs = {geoIn("grid"), bounded(in("threshold", T::Float, 0.0), 0.0)},
                    .outputs = {geoOut("points"), fout("value", T::Float), fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                const std::string valueName = c.anonymous("value"), vectorName = c.anonymous("vector");
                c.out("value", attributeField(valueName, T::Float));
                c.out("vector", attributeField(vectorName, T::Vector));
                geo::Mesh pts;
                if (!v)
                {
                    c.out("points", pointGeometry(std::move(pts)));
                    return;
                }
                const float threshold = float(std::max(c.f("threshold"), 0.0));
                std::vector<size_t> active;
                for (size_t n = 0; n < v->size(); ++n)
                    if (std::abs(v->values[n] - v->background) > threshold)
                        active.push_back(n);
                if (active.size() > kMaxGridPoints)
                    throw ExecError(c.node.name + ": over 50 million points; raise the threshold");
                const size_t sx = size_t(v->dims.x), sxy = sx * size_t(v->dims.y);
                for (size_t n : active)
                    pts.addPoint(v->worldOf({double(n % sx), double((n / sx) % size_t(v->dims.y)), double(n / sxy)}));
                auto values = pts.pointAttrs().getOrAdd<float>(valueName);
                auto vectors = pts.pointAttrs().getOrAdd<V3>(vectorName);
                for (size_t p = 0; p < active.size(); ++p)
                {
                    values[p] = v->values[active[p]];
                    vectors[p] = v->isVector() ? v->vectors[active[p]] : V3(0.0f);
                }
                c.out("points", pointGeometry(std::move(pts)));
            },
    });
}

class GridSampleField final : public Field
{
public:
    GridSampleField(GridPtr v, FieldPtr position, bool nearest, bool vector)
        : Field(vector ? SocketType::Vector : SocketType::Float), mGrid(std::move(v)), mPosition(std::move(position)), mNearest(nearest), mVector(vector)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        std::vector<D3> pos = evaluateAll<D3>(mPosition, ctx);
        if (mVector)
        {
            std::vector<D3> out(pos.size());
            tbb::parallel_for(size_t(0), pos.size(), [&](size_t k) { out[k] = mNearest ? mGrid->sampleVectorNearest(pos[k]) : mGrid->sampleVector(pos[k]); });
            return Column::of(std::move(out));
        }
        std::vector<double> out(pos.size());
        tbb::parallel_for(size_t(0), pos.size(), [&](size_t k) { out[k] = mNearest ? mGrid->sampleNearest(pos[k]) : mGrid->sample(pos[k]); });
        return Column::of(std::move(out));
    }

private:
    GridPtr mGrid;
    FieldPtr mPosition;
    bool mNearest, mVector;
};

void sampleGrid()
{
    registerDecl({
        .type = "SampleGrid",
        .label = "Sample Grid",
        .category = "Volume",
        .doc = "A grid's value at each position: value is the float (a vector grid's length), vector the vector (zero on a float grid). "
               "Outside the grid it reads the background.",
        .props = {prop("interpolation", {"trilinear", "nearest"}, "trilinear")},
        .sockets = {.inputs = {geoIn("grid"), fin("position", T::Vector, nullptr, Implicit::Position)},
                    .outputs = {fout("value", T::Float), fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("value", constantFloat(0.0));
                    c.out("vector", constantVector(D3(0.0)));
                    return;
                }
                const bool nearest = c.prop("interpolation") == "nearest";
                c.out("value", std::make_shared<GridSampleField>(v, c.field("position"), nearest, false));
                c.out("vector", std::make_shared<GridSampleField>(v, c.field("position"), nearest, true));
            },
    });
}

class GridIndexField final : public Field
{
public:
    GridIndexField(GridPtr v, std::array<FieldPtr, 3> coords, bool vector)
        : Field(vector ? SocketType::Vector : SocketType::Float), mGrid(std::move(v)), mCoords(std::move(coords)), mVector(vector)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        std::array<std::vector<int64_t>, 3> ijk;
        size_t n = 0;
        for (int a = 0; a < 3; ++a)
        {
            ijk[size_t(a)] = evaluateAll<int64_t>(mCoords[size_t(a)], ctx);
            n = std::max(n, ijk[size_t(a)].size());
        }
        auto at = [&](int a, size_t k) { const auto& col = ijk[size_t(a)]; return int(col[std::min(k, col.size() - 1)]); };
        if (mVector)
        {
            std::vector<D3> out(n);
            for (size_t k = 0; k < n; ++k)
                out[k] = D3(mGrid->vectorAt(at(0, k), at(1, k), at(2, k)));
            return Column::of(std::move(out));
        }
        std::vector<double> out(n);
        for (size_t k = 0; k < n; ++k)
            out[k] = mGrid->at(at(0, k), at(1, k), at(2, k));
        return Column::of(std::move(out));
    }

private:
    GridPtr mGrid;
    std::array<FieldPtr, 3> mCoords;
    bool mVector;
};

void sampleGridIndex()
{
    registerDecl({
        .type = "SampleGridIndex",
        .label = "Sample Grid Index",
        .category = "Volume",
        .doc = "The value of voxel (x, y, z), counted from the grid's first voxel; outside the grid it reads the background.",
        .sockets = {.inputs = {geoIn("grid"), fin("x", T::Int, 0), fin("y", T::Int, 0), fin("z", T::Int, 0)},
                    .outputs = {fout("value", T::Float), fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("value", constantFloat(0.0));
                    c.out("vector", constantVector(D3(0.0)));
                    return;
                }
                std::array<FieldPtr, 3> coords = {c.field("x"), c.field("y"), c.field("z")};
                c.out("value", std::make_shared<GridIndexField>(v, coords, false));
                c.out("vector", std::make_shared<GridIndexField>(v, coords, true));
            },
    });
}

void gridGradient()
{
    registerDecl({
        .type = "GridGradient",
        .label = "Grid Gradient",
        .category = "Volume",
        .doc = "A vector grid of the direction and rate the value rises fastest, per world unit. On a signed distance it is the outward "
               "normal; a vector grid uses its lengths.",
        .sockets = {.inputs = {geoIn("grid")}, .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const glm::dmat3 toWorld = worldFromIndexDerivatives(*v);
                std::vector<V3> g(v->size());
                auto read = [&](glm::ivec3 q) { return clampedAt(*v, q); };
                eachVoxel(v->dims, [&](int i, int j, int k) {
                    const glm::ivec3 p(i, j, k);
                    glm::dvec3 d(indexDerivative(*v, p, 0, read), indexDerivative(*v, p, 1, read), indexDerivative(*v, p, 2, read));
                    g[v->index(i, j, k)] = V3(toWorld * d);
                });
                Volume out = scalarLike(*v, Volume::Kind::Vector, "gradient");
                out.setVectors(std::move(g));
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(out)));
            },
    });
}

void gridCurl()
{
    registerDecl({
        .type = "GridCurl",
        .label = "Grid Curl",
        .category = "Volume",
        .doc = "A vector grid of how a vector grid swirls around each voxel: the axis of rotation, its length the rate.",
        .sockets = {.inputs = {geoIn("grid")}, .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = vectorGridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const glm::dmat3 inverseAxes = glm::inverse(v->axes());
                std::vector<V3> curl(v->size());
                eachVoxel(v->dims, [&](int i, int j, int k) {
                    const glm::dmat3 J = jacobian(*v, inverseAxes, {i, j, k});
                    curl[v->index(i, j, k)] = V3(J[1][2] - J[2][1], J[2][0] - J[0][2], J[0][1] - J[1][0]);
                });
                Volume out = scalarLike(*v, Volume::Kind::Vector, "curl");
                out.setVectors(std::move(curl));
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(out)));
            },
    });
}

void gridDivergence()
{
    registerDecl({
        .type = "GridDivergence",
        .label = "Grid Divergence",
        .category = "Volume",
        .doc = "A density grid of how much a vector grid flows out of each voxel: positive where it spreads, negative where it gathers.",
        .sockets = {.inputs = {geoIn("grid")}, .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = vectorGridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const glm::dmat3 inverseAxes = glm::inverse(v->axes());
                Volume out = scalarLike(*v, Volume::Kind::Density, "divergence");
                eachVoxel(v->dims, [&](int i, int j, int k) {
                    const glm::dmat3 J = jacobian(*v, inverseAxes, {i, j, k});
                    out.values[v->index(i, j, k)] = float(J[0][0] + J[1][1] + J[2][2]);
                });
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(out)));
            },
    });
}

void gridLaplacian()
{
    registerDecl({
        .type = "GridLaplacian",
        .label = "Grid Laplacian",
        .category = "Volume",
        .doc = "A density grid of how much each voxel differs from the average around it, per world unit squared: the divergence of the "
               "gradient. A vector grid uses its lengths.",
        .sockets = {.inputs = {geoIn("grid")}, .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                // Laplacian = trace(A^-T H A^-1) = sum of metric[a][b] * H[a][b] over the index Hessian H.
                const glm::dmat3 inverseAxes = glm::inverse(v->axes());
                const glm::dmat3 metric = inverseAxes * glm::transpose(inverseAxes);
                Volume out = scalarLike(*v, Volume::Kind::Density, "laplacian");
                eachVoxel(v->dims, [&](int i, int j, int k) {
                    const glm::ivec3 p(i, j, k);
                    const double centre = clampedAt(*v, p);
                    double sum = 0.0;
                    for (int a = 0; a < 3; ++a)
                        for (int b = a; b < 3; ++b)
                        {
                            const double m = a == b ? metric[a][a] : metric[a][b] + metric[b][a];
                            if (std::abs(m) < 1e-15)
                                continue;
                            glm::ivec3 ea(0), eb(0);
                            ea[a] = 1;
                            eb[b] = 1;
                            double h;
                            if (a == b)
                                h = clampedAt(*v, p + ea) - 2.0 * centre + clampedAt(*v, p - ea);
                            else
                                h = 0.25 * (clampedAt(*v, p + ea + eb) - clampedAt(*v, p + ea - eb) - clampedAt(*v, p - ea + eb) + clampedAt(*v, p - ea - eb));
                            sum += m * h;
                        }
                    out.values[v->index(i, j, k)] = float(sum);
                });
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(out)));
            },
    });
}

/** Where a particle at p now was dt ago, carried by the velocity grid. */
glm::dvec3 backtrace(const Volume& velocity, const glm::dvec3& p, double dt, int order)
{
    auto vel = [&](const glm::dvec3& x) { return velocity.sampleVector(x); };
    if (order == 1)
        return p - dt * vel(p);
    if (order == 2)
        return p - dt * vel(p - 0.5 * dt * vel(p));
    const glm::dvec3 k1 = vel(p), k2 = vel(p - 0.5 * dt * k1), k3 = vel(p - 0.5 * dt * k2), k4 = vel(p - dt * k3);
    return p - dt / 6.0 * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
}

void advectGrid()
{
    registerDecl({
        .type = "AdvectGrid",
        .label = "Advect Grid",
        .category = "Volume",
        .doc = "Carries a grid's values along a velocity grid (units per second) for time seconds, each voxel reading where its value "
               "came from. Higher integration orders follow curving flow more closely.",
        .props = {prop("integration", {"euler", "midpoint", "rk4"}, "midpoint")},
        .sockets = {.inputs = {geoIn("grid"), geoIn("velocity", "A vector grid; its own voxels need not match the grid's."),
                               in("time", T::Float, 1.0 / 24.0, "Seconds to advect for."), bounded(in("substeps", T::Int, 1), 1, 64)},
                    .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                GridPtr vel = vectorGridOf(c, "velocity");
                if (!v || !vel)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const std::string mode = c.prop("integration");
                const int order = mode == "euler" ? 1 : mode == "midpoint" ? 2 : 4;
                const int steps = int(std::clamp<int64_t>(c.i("substeps"), 1, 64));
                const double dt = c.f("time") / steps;
                Volume cur = *v;
                for (int s = 0; s < steps; ++s)
                {
                    Volume next = cur;
                    std::vector<V3> vectors(cur.isVector() ? cur.size() : 0);
                    eachVoxel(cur.dims, [&](int i, int j, int k) {
                        const glm::dvec3 from = backtrace(*vel, cur.worldOf({i, j, k}), dt, order);
                        const size_t n = cur.index(i, j, k);
                        if (cur.isVector())
                            vectors[n] = V3(cur.sampleVector(from));
                        else
                            next.values[n] = float(cur.sample(from));
                    });
                    if (cur.isVector())
                        next.setVectors(std::move(vectors));
                    cur = std::move(next);
                }
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(cur)));
            },
    });
}

/** Neighbour offsets: 6 sharing a face, 18 with edges, 26 with corners. */
std::vector<glm::ivec3> neighbourhood(const std::string& connectivity)
{
    const int limit = connectivity == "face" ? 1 : connectivity == "edge" ? 2 : 3;
    std::vector<glm::ivec3> out;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                if (int n = std::abs(x) + std::abs(y) + std::abs(z); n > 0 && n <= limit)
                    out.push_back({x, y, z});
    return out;
}

/**
 * Grows (or shrinks) what a grid holds by one voxel per step: a density or vector spreads its strongest neighbour, a signed
 * distance its most inside one. Erode is the opposite.
 */
Volume morph(const Volume& src, int steps, const std::vector<glm::ivec3>& offsets, bool grow)
{
    // Larger key wins when growing, so a signed distance ranks by its negation (more inside is larger).
    const float sign = src.isSdf() ? -1.0f : 1.0f;
    Volume cur = src;
    for (int s = 0; s < steps; ++s)
    {
        Volume next = cur;
        std::vector<V3> vectors = cur.vectors;
        eachVoxel(cur.dims, [&](int i, int j, int k) {
            const glm::ivec3 p(i, j, k);
            const size_t n = cur.index(i, j, k);
            glm::ivec3 best = p;
            float bestKey = sign * cur.values[n];
            for (const glm::ivec3& o : offsets)
            {
                const glm::ivec3 q = p + o;
                const float key = sign * (cur.inside(q.x, q.y, q.z) ? cur.values[cur.index(q.x, q.y, q.z)] : cur.background);
                if (grow ? key > bestKey : key < bestKey)
                {
                    bestKey = key;
                    best = q;
                }
            }
            next.values[n] = sign * bestKey;
            if (cur.isVector())
                vectors[n] = cur.vectorAt(best.x, best.y, best.z);
        });
        if (cur.isVector())
            next.setVectors(std::move(vectors));
        cur = std::move(next);
    }
    return cur;
}

void morphNode(const char* type, const char* label, bool grow)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Volume",
        .doc = grow ? "Grows what a grid holds by a voxel per step: densities and vectors spread their strongest neighbour, a signed "
                      "distance surface moves outward."
                    : "Shrinks what a grid holds by a voxel per step: densities and vectors take their weakest neighbour, a signed "
                      "distance surface moves inward.",
        .props = {prop("connectivity", {"face", "edge", "vertex"}, "face", "Neighbours that count: 6 sharing a face, 18 with edges, 26 with corners.")},
        .sockets = {.inputs = {geoIn("grid"), bounded(in("steps", T::Int, 1), 0, 256)}, .outputs = {gridOut()}},
        .exec =
            [grow](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const int steps = int(std::clamp<int64_t>(c.i("steps"), 0, 256));
                c.out("grid", replaceGrid(c.geometry("grid"), morph(*v, steps, neighbourhood(c.prop("connectivity")), grow)));
            },
    });
}

/** A box mean along one voxel axis, edges clamped; vectors per component. */
void meanAxis(Volume& v, int axis, int r)
{
    const std::vector<float> src = v.values;
    const std::vector<V3> srcVectors = v.vectors;
    const float scale = 1.0f / float(2 * r + 1);
    eachVoxel(v.dims, [&](int i, int j, int k) {
        glm::ivec3 at(i, j, k);
        float sum = 0.0f;
        V3 sumVector(0.0f);
        for (int o = -r; o <= r; ++o)
        {
            glm::ivec3 q = at;
            q[axis] = std::clamp(q[axis] + o, 0, v.dims[axis] - 1);
            const size_t n = v.index(q.x, q.y, q.z);
            if (srcVectors.empty())
                sum += src[n];
            else
                sumVector += srcVectors[n];
        }
        const size_t n = v.index(i, j, k);
        if (srcVectors.empty())
            v.values[n] = sum * scale;
        else
            v.vectors[n] = sumVector * scale;
    });
}

void filterNode(const char* type, const char* label, bool median)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Volume",
        .doc = median ? "Each voxel takes the median of the cube width voxels around it, removing specks while keeping edges; vectors per "
                        "component."
                      : "Each voxel takes the average of the cube width voxels around it, softening the grid; vectors per component.",
        .sockets = {.inputs = {geoIn("grid"), bounded(in("width", T::Int, 1, "Reach of the filter in voxels."), 1, median ? 4 : 32),
                               bounded(in("iterations", T::Int, 1), 0, 100)},
                    .outputs = {gridOut()}},
        .exec =
            [median](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const int r = int(std::clamp<int64_t>(c.i("width"), 1, median ? 4 : 32));
                Volume cur = *v;
                for (int64_t it = std::clamp<int64_t>(c.i("iterations"), 0, 100); it > 0; --it)
                {
                    if (!median)
                    {
                        for (int axis = 0; axis < 3; ++axis)
                            meanAxis(cur, axis, r);
                        syncLengths(cur);
                        continue;
                    }
                    Volume next = cur;
                    const int channels = cur.isVector() ? 3 : 1;
                    eachVoxel(cur.dims, [&](int i, int j, int k) {
                        std::vector<float> window;
                        window.reserve(size_t((2 * r + 1) * (2 * r + 1) * (2 * r + 1)));
                        const size_t n = cur.index(i, j, k);
                        for (int ch = 0; ch < channels; ++ch)
                        {
                            window.clear();
                            for (int z = -r; z <= r; ++z)
                                for (int y = -r; y <= r; ++y)
                                    for (int x = -r; x <= r; ++x)
                                    {
                                        const glm::ivec3 q = clampVoxel(cur, {i + x, j + y, k + z});
                                        const size_t m = cur.index(q.x, q.y, q.z);
                                        window.push_back(channels == 1 ? cur.values[m] : cur.vectors[m][ch]);
                                    }
                            std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
                            const float mid = window[window.size() / 2];
                            if (channels == 1)
                                next.values[n] = mid;
                            else
                                next.vectors[n][ch] = mid;
                        }
                    });
                    syncLengths(next);
                    cur = std::move(next);
                }
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(cur)));
            },
    });
}

void voxelizeGrid()
{
    registerDecl({
        .type = "VoxelizeGrid",
        .label = "Voxelize Grid",
        .category = "Volume",
        .doc = "The grid resampled onto axis-aligned voxels of a new size over the same bounds: coarser to save memory, finer to work in "
               "more detail. A rotated grid comes out axis-aligned.",
        .sockets = {.inputs = {geoIn("grid"), gridVoxelSizeIn()}, .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                Volume out;
                try
                {
                    out = Volume::grid(v->bounds(), std::max(c.f("voxelSize"), 0.01), v->isSdf() ? Volume::Kind::Sdf : Volume::Kind::Density, v->background);
                }
                catch (const std::exception& e)
                {
                    throw ExecError(c.node.name + ": " + e.what());
                }
                out.name = v->name;
                if (v->isVector())
                    out.setVectors(v->resampledVectors(out));
                else
                    out.values = v->resampled(out);
                c.out("grid", replaceGrid(c.geometry("grid"), std::move(out)));
            },
    });
}

/** The grid cut down to the voxels keep(n) accepts, plus a voxel of margin; null when none are. */
std::optional<Volume> cropped(const Volume& v, const std::function<bool(size_t)>& keep)
{
    glm::ivec3 lo(INT_MAX), hi(INT_MIN);
    for (int k = 0; k < v.dims.z; ++k)
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
                if (keep(v.index(i, j, k)))
                {
                    lo = glm::min(lo, glm::ivec3(i, j, k));
                    hi = glm::max(hi, glm::ivec3(i, j, k));
                }
    if (lo.x > hi.x)
        return std::nullopt;
    lo = glm::max(lo - 1, glm::ivec3(0));
    hi = glm::min(hi + 1, v.dims - 1);
    Volume out;
    out.name = v.name;
    out.kind = v.kind;
    out.background = v.background;
    out.dims = hi - lo + 1;
    out.indexToWorld = v.indexToWorld;
    out.indexToWorld[3] = glm::dvec4(v.worldOf(glm::dvec3(lo)), 1.0);
    const size_t count = size_t(out.dims.x) * size_t(out.dims.y) * size_t(out.dims.z);
    out.values.resize(count);
    if (v.isVector())
        out.vectors.resize(count);
    for (int k = 0; k < out.dims.z; ++k)
        for (int j = 0; j < out.dims.y; ++j)
            for (int i = 0; i < out.dims.x; ++i)
            {
                const size_t from = v.index(lo.x + i, lo.y + j, lo.z + k), to = out.index(i, j, k);
                out.values[to] = v.values[from];
                if (v.isVector())
                    out.vectors[to] = v.vectors[from];
            }
    return out;
}

void pruneGrid()
{
    registerDecl({
        .type = "PruneGrid",
        .label = "Prune Grid",
        .category = "Volume",
        .doc = "Crops the grid to the voxels that matter, keeping a voxel of margin. Inactive keeps voxels that differ from the background, "
               "threshold those that differ by more than threshold; sdf first clamps a signed distance to its band, then keeps the band.",
        .props = {prop("mode", {"inactive", "threshold", "sdf"}, "inactive")},
        .sockets = {.inputs = {geoIn("grid"), bounded(in("threshold", T::Float, 0.01), 0.0)}, .outputs = {gridOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GridPtr v = gridOf(c, "grid");
                if (!v)
                {
                    c.out("grid", c.geometry("grid"));
                    return;
                }
                const std::string mode = c.prop("mode");
                Volume src = *v;
                std::function<bool(size_t)> keep;
                if (mode == "sdf" && src.isSdf())
                {
                    const float band = std::abs(src.background);
                    for (float& x : src.values)
                        x = std::clamp(x, -band, band);
                    keep = [&src, band](size_t n) { return std::abs(src.values[n]) < band; };
                }
                else
                {
                    const float t = mode == "threshold" ? float(std::max(c.f("threshold"), 0.0)) : 0.0f;
                    keep = [&src, t](size_t n) { return std::abs(src.values[n] - src.background) > t; };
                }
                std::optional<Volume> out = cropped(src, keep);
                c.out("grid", out ? replaceGrid(c.geometry("grid"), std::move(*out)) : [&] {
                    Geometry rest = *c.geometry("grid");
                    rest.volume = nullptr;
                    return makeGeometry(std::move(rest));
                }());
            },
    });
}

void gridInfo()
{
    registerDecl({
        .type = "GridInfo",
        .label = "Grid Info",
        .category = "Volume",
        .doc = "A grid's voxel-to-world transform (voxel size, rotation and the first voxel's centre), background value, name, kind "
               "(density, sdf or vector) and the voxels differing from the background. Empty without a grid.",
        .sockets = {.inputs = {geoIn("grid")},
                    .outputs = {out("transform", T::Matrix), out("background", T::Float), out("name", T::String), out("kind", T::String),
                                out("activeVoxels", T::Int)}},
        .exec =
            [](ExecContext& c) {
                GridPtr v = gridOf(c, "grid");
                c.out("transform", constantMatrix(v ? v->indexToWorld : glm::dmat4(1.0)));
                c.out("background", constantFloat(v ? v->background : 0.0));
                c.outString("name", v ? v->name : std::string());
                c.outString("kind", !v ? "" : v->isSdf() ? "sdf" : v->isVector() ? "vector" : "density");
                int64_t active = 0;
                if (v)
                    for (float x : v->values)
                        active += x != v->background;
                c.out("activeVoxels", constantInt(active));
            },
    });
}
} // namespace grid_nodes

void registerGridNodes()
{
    using namespace grid_nodes;
    meshToVolume();
    pointsToVolume();
    fieldToGrid();
    gridToPoints();
    sampleGrid();
    sampleGridIndex();
    gridGradient();
    gridCurl();
    gridDivergence();
    gridLaplacian();
    advectGrid();
    morphNode("GridDilate", "Grid Dilate", true);
    morphNode("GridErode", "Grid Erode", false);
    filterNode("GridMean", "Grid Mean", false);
    filterNode("GridMedian", "Grid Median", true);
    voxelizeGrid();
    pruneGrid();
    gridInfo();
}
} // namespace lila::gn::nodes
