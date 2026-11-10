#include "scene/Deformers.h"

#include "ops/Command.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "scene/DeformEval.h"
#include "scene/DeformWeights.h"
#include "scene/DeformerUtil.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/BendBonesDeformer.h"
#include "scene/Jiggle.h"
#include "scene/MeshCache.h"
#include "scene/MeshNodes.h"
#include "scene/Morphs.h"
#include "scene/RigUtil.h"
#include "scene/ShapeDeformers.h"
#include "scene/Skin.h"
#include "scene/SkinBake.h"
#include "scene/SurfaceDeformers.h"
#include "scene/TextureDeformers.h"
#include "scene/SoftSelect.h"

#include <tbb/parallel_for.h>

#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>

namespace lila::scene
{
using namespace rigutil;
using namespace deformutil;

namespace
{
// ------------------------------------------------------------------------------------------------ data kinds

/** Points captured at bind time (delta mush rest shape). */
struct RestPoints final : Data
{
    std::vector<glm::dvec3> points;
    const char* kind() const override { return "restPoints"; }
    size_t bytes() const override { return points.size() * sizeof(glm::dvec3); }
    json toJson() const override { return {{"kind", kind()}, {"points", flatten(points)}}; }
    static DataPtr fromJson(const json& j)
    {
        auto r = std::make_shared<RestPoints>();
        std::vector<double> f;
        readArray(j, "points", f);
        r->points = unflatten(f);
        return r;
    }
};

/** Dense mean value coordinates of every target point against the cage as it was at bind time. */
struct CageData final : Data
{
    std::vector<glm::dvec3> rest; // cage points, world
    std::vector<float> weights;   // points x rest.size()
    uint32_t points = 0;
    const char* kind() const override { return "cageBinding"; }
    size_t bytes() const override { return rest.size() * sizeof(glm::dvec3) + weights.size() * sizeof(float); }
    json toJson() const override { return {{"kind", kind()}, {"rest", flatten(rest)}, {"weights", weights}, {"points", points}}; }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<CageData>();
        std::vector<double> f;
        readArray(j, "rest", f);
        d->rest = unflatten(f);
        readArray(j, "weights", d->weights);
        d->points = j.value("points", 0u);
        if (d->weights.size() != size_t(d->points) * d->rest.size())
            d->points = d->rest.empty() ? 0 : uint32_t(d->weights.size() / d->rest.size());
        return d;
    }
};

// ------------------------------------------------------------------------------------------------ attr helpers

AttrDef boundsAttr(const char* name, double def, const char* doc) { return keyable(name, PlugType::Float, def, doc); }

AttrDef falloffRadiusAttr()
{
    return keyable("falloffRadius", PlugType::Float, 0.0,
                   "Spherical falloff around the handle, in handle units: points beyond it are untouched. 0 turns it off.", 0.0);
}
AttrDef falloffSoftnessAttr()
{
    return keyable("falloffSoftness", PlugType::Float, 0.5, "How much of the falloff radius eases out, 0 a hard edge to 1 all soft.", 0.0, 1.0);
}

// ------------------------------------------------------------------------------------------------ nodes

enum class NonlinearKind
{
    Bend,
    Twist,
    Taper,
    Squash,
    Sine,
    Wave,
};

class NonlinearNode : public DeformerNode
{
public:
    explicit NonlinearNode(NonlinearKind k) : mKind(k) {}

    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 handle = matrix("handleMatrix");
        const glm::dmat4 toHandle = safeInverse(handle, "the handle") * geom;
        const glm::dmat4 back = safeInverse(geom, "the mesh") * handle;
        const double radius = num("falloffRadius"), softness = num("falloffSoftness");
        std::function<glm::dvec3(const glm::dvec3&)> f;
        switch (mKind)
        {
        case NonlinearKind::Bend:
            f = [o = deform::BendParams{num("angle"), num("lowBound"), num("highBound")}](const glm::dvec3& p) { return deform::bend(p, o); };
            break;
        case NonlinearKind::Twist:
            f = [o = deform::TwistParams{num("startAngle"), num("endAngle"), num("lowBound"), num("highBound")}](const glm::dvec3& p) {
                return deform::twist(p, o);
            };
            break;
        case NonlinearKind::Taper:
            f = [o = deform::TaperParams{num("startScale"), num("endScale"), num("curve"), num("lowBound"), num("highBound")}](const glm::dvec3& p) {
                return deform::taper(p, o);
            };
            break;
        case NonlinearKind::Squash:
            f = [o = deform::SquashParams{num("factor"), num("expand"), num("maxExpandPos"), num("lowBound"), num("highBound")}](
                    const glm::dvec3& p) { return deform::squash(p, o); };
            break;
        case NonlinearKind::Sine:
            f = [o = deform::SineParams{num("amplitude"), num("wavelength"), num("offset"), num("dropoff"), num("lowBound"), num("highBound")}](
                    const glm::dvec3& p) { return deform::sine(p, o); };
            break;
        case NonlinearKind::Wave:
            f = [o = deform::WaveParams{num("amplitude"), num("wavelength"), num("offset"), num("dropoff"), num("minRadius"), num("maxRadius")}](
                    const glm::dvec3& p) { return deform::wave(p, o); };
            break;
        }
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 h = xform(toHandle, points[i]);
            double w = deform::sphericalFalloff(glm::length(h), radius, softness);
            if (w <= 0.0)
                return;
            points[i] = xform(back, glm::mix(h, f(h), w));
        });
    }

    bool gpuProgram(const geo::Mesh&, deformEval::Program& out) const override
    {
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 handle = matrix("handleMatrix");
        out.kernel = deformEval::Kernel::Nonlinear;
        out.push(safeInverse(handle, "the handle") * geom);
        out.push(safeInverse(geom, "the mesh") * handle);
        out.push(float(num("falloffRadius")));
        out.push(float(num("falloffSoftness")));
        out.push(uint32_t(mKind));
        auto push = [&](std::initializer_list<const char*> names) {
            for (const char* a : names)
                out.push(float(num(a)));
        };
        switch (mKind)
        {
        case NonlinearKind::Bend:
            push({"angle", "lowBound", "highBound"});
            break;
        case NonlinearKind::Twist:
            push({"startAngle", "endAngle", "lowBound", "highBound"});
            break;
        case NonlinearKind::Taper:
            push({"startScale", "endScale", "curve", "lowBound", "highBound"});
            break;
        case NonlinearKind::Squash:
            push({"factor", "expand", "maxExpandPos", "lowBound", "highBound"});
            break;
        case NonlinearKind::Sine:
        case NonlinearKind::Wave:
            push({"amplitude", "wavelength"});
            // Whole waves of phase change nothing, and dropping them keeps the GPU's float sine accurate.
            out.push(float(num("offset") - std::floor(num("offset"))));
            push({"dropoff"});
            if (mKind == NonlinearKind::Sine)
                push({"lowBound", "highBound"});
            else
                push({"minRadius", "maxRadius"});
            break;
        }
        return true;
    }

private:
    NonlinearKind mKind;
};

class FfdNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        glm::ivec3 n;
        const std::vector<glm::dvec3> disp = displacements(n);
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const glm::dmat4 baseInv = safeInverse(matrix("baseMatrix"), "the lattice base");
        const glm::dmat4 lat = matrix("latticeMatrix");
        const auto basis = deform::FfdBasis(std::clamp<int64_t>(integer("interpolation"), 0, 2));
        const double outside = num("outsideFalloff");
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 w = xform(geom, points[i]);
            glm::dvec3 q = xform(baseInv, w);
            double dist = glm::length(glm::max(glm::abs(q) - glm::dvec3(0.5), glm::dvec3(0.0)));
            double fall = outside > 0.0 ? deform::sphericalFalloff(dist, outside, 1.0) : (dist > 1e-9 ? 0.0 : 1.0);
            if (fall <= 0.0)
                return;
            glm::dvec3 stu = glm::clamp(q + glm::dvec3(0.5), glm::dvec3(0.0), glm::dvec3(1.0));
            glm::dvec3 moved = xform(lat, q + deform::ffdDisplacement(n, disp, stu, basis));
            points[i] = xform(geomInv, glm::mix(w, moved, fall));
        });
    }

    bool gpuProgram(const geo::Mesh&, deformEval::Program& out) const override
    {
        glm::ivec3 n;
        const std::vector<glm::dvec3> disp = displacements(n);
        if (n.x > 32 || n.y > 32 || n.z > 32)
            return false;
        const glm::dmat4 geom = matrix("geomMatrix");
        out.kernel = deformEval::Kernel::Ffd;
        out.push(safeInverse(matrix("baseMatrix"), "the lattice base") * geom);
        out.push(safeInverse(geom, "the mesh") * matrix("latticeMatrix"));
        for (int k = 0; k < 3; ++k)
            out.push(uint32_t(n[k]));
        out.push(uint32_t(std::clamp<int64_t>(integer("interpolation"), 0, 2)));
        out.push(float(num("outsideFalloff")));
        out.push(std::span<const glm::dvec3>(disp));
        return true;
    }

private:
    /** Each lattice point's offset from its rest place, lattice units, and the lattice's points per axis. */
    std::vector<glm::dvec3> displacements(glm::ivec3& n) const
    {
        const MeshPtr& lattice = as<MeshPtr>(attr("lattice"));
        if (!lattice)
            throw std::runtime_error("no lattice connected");
        glm::dvec3 dv = as<glm::dvec3>(attr("divisions"));
        n = glm::ivec3(std::max(2, int(dv.x)), std::max(2, int(dv.y)), std::max(2, int(dv.z)));
        size_t count = size_t(n.x) * size_t(n.y) * size_t(n.z);
        if (lattice->pointCapacity() != count)
            throw std::runtime_error("the lattice's points no longer match its divisions");
        std::vector<glm::dvec3> disp(count);
        for (int k = 0; k < n.z; ++k)
            for (int j = 0; j < n.y; ++j)
                for (int i = 0; i < n.x; ++i)
                {
                    size_t idx = deform::latticeIndex(n, i, j, k);
                    disp[idx] = lattice->position(geo::Index(idx)) - deform::latticeRest(n, i, j, k);
                }
        return disp;
    }
};

/** A delta mush binding and the adjacency it smooths over, shared through the bind cache. */
struct DeltaMushBound
{
    deform::Adjacency adj;
    deform::DeltaMushBind bind;
    /** deform.slang's copies: neighbour and corner CSR, border flags, rest deltas. Built on first GPU use. */
    mutable std::once_flag gpuOnce;
    mutable std::vector<uint32_t> gpuNeighbors, gpuCorners, gpuBorder;
    mutable std::vector<float> gpuLocal;

    size_t bytes() const { return (adj.offsets.size() + adj.neighbors.size()) * 4 + adj.boundary.size() + bind.local.size() * sizeof(glm::dvec3); }

    void buildGpu(const geo::Mesh& topology) const
    {
        LILA_ZONE();

        const size_t n = adj.offsets.size() - 1;
        gpuNeighbors.assign(adj.offsets.begin(), adj.offsets.end());
        gpuNeighbors.insert(gpuNeighbors.end(), adj.neighbors.begin(), adj.neighbors.end());
        gpuBorder.assign(adj.boundary.begin(), adj.boundary.end());
        gpuCorners.assign(n + 1, 0);
        std::vector<uint32_t> pairs;
        for (size_t p = 0; p < n; ++p)
        {
            if (p < topology.pointCapacity() && topology.pointValid(geo::Index(p)))
                topology.forEachPointCorner(geo::Index(p), [&](geo::Index c) {
                    pairs.push_back(uint32_t(topology.cornerNextPoint(c)));
                    pairs.push_back(uint32_t(topology.cornerPoint(topology.cornerPrev(c))));
                });
            gpuCorners[p + 1] = uint32_t(pairs.size() / 2);
        }
        gpuCorners.insert(gpuCorners.end(), pairs.begin(), pairs.end());
        gpuLocal.resize(bind.local.size() * 3);
        for (size_t p = 0; p < bind.local.size(); ++p)
            for (int k = 0; k < 3; ++k)
                gpuLocal[p * 3 + k] = float(bind.local[p][k]);
    }
};

class DeltaMushNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        std::shared_ptr<const DeltaMushBound> b = bound(in, points.size());
        points = deform::deltaMushApply(in, points, b->adj, b->bind, num("displacement"));
    }

    bool gpuProgram(const geo::Mesh& in, deformEval::Program& out) const override
    {
        std::shared_ptr<const DeltaMushBound> b = bound(in, in.pointCapacity());
        if (b->bind.local.size() != in.pointCapacity() || b->adj.offsets.size() != size_t(in.pointCapacity()) + 1)
            return false;
        std::call_once(b->gpuOnce, [&] { b->buildGpu(in); });
        out.kernel = deformEval::Kernel::DeltaMush;
        out.iterations = uint32_t(b->bind.iterations);
        out.push(float(b->bind.step));
        out.push(uint32_t(b->bind.pinBoundary));
        out.push(float(num("displacement")));
        out.push(uint32_t(b->adj.offsets.size() - 1));
        out.blobs = {deformEval::blob(mKey ^ 1, b, b->gpuNeighbors), deformEval::blob(mKey ^ 2, b, b->gpuCorners),
                     deformEval::blob(mKey ^ 3, b, b->gpuBorder), deformEval::blob(mKey ^ 4, b, b->gpuLocal)};
        return true;
    }

private:
    /** The binding for this input from the bind cache, by topology, rest shape and settings: binding is the costly half. */
    std::shared_ptr<const DeltaMushBound> bound(const geo::Mesh& in, size_t points) const
    {
        DataPtr restData = as<DataPtr>(attr("rest"));
        const auto* rest = dynamic_cast<const RestPoints*>(restData.get());
        if (!rest)
            throw Unbound();
        if (rest->points.size() != points)
            throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
        const int iterations = int(std::clamp<int64_t>(integer("iterations"), 0, 1000));
        const double step = std::clamp(num("smoothStep"), 0.0, 1.0);
        const bool pin = as<bool>(attr("pinBorders"));
        if (mRestKey != restData)
        {
            mRestHash = deformEval::hashBytes(rest->points.data(), rest->points.size() * sizeof(glm::dvec3));
            mRestKey = restData;
        }
        uint64_t key = deformEval::hashCombine(deformEval::topologyHash(in), mRestHash);
        key = deformEval::hashCombine(key, uint64_t(iterations) << 1 | uint64_t(pin));
        key = deformEval::hashCombine(key, std::bit_cast<uint64_t>(step));
        if (!mBound || key != mKey)
        {
            mBound = deformEval::cachedBind<DeltaMushBound>(key, [&] {
                auto b = std::make_shared<DeltaMushBound>();
                b->adj = deform::pointAdjacency(in);
                b->bind = deform::deltaMushBind(in, rest->points, b->adj, iterations, step, pin);
                return std::shared_ptr<const DeltaMushBound>(std::move(b));
            });
            mKey = key;
        }
        return mBound;
    }

    mutable DataPtr mRestKey;
    mutable uint64_t mRestHash = 0;
    mutable uint64_t mKey = 0;
    mutable std::shared_ptr<const DeltaMushBound> mBound;
};

class CageNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        if (deformers::cageUsesCoordinates(*this))
            return deformers::cageDeform(*this, points, mCoordinates);
        const auto* b = dynamic_cast<const CageData*>(as<DataPtr>(attr("binding")).get());
        const MeshPtr& cage = as<MeshPtr>(attr("driver"));
        if (!b)
            throw Unbound();
        if (!cage)
            throw std::runtime_error("no cage connected");
        const size_t nv = b->rest.size();
        if (cage->pointCapacity() != nv)
            throw std::runtime_error("the cage's points changed since binding: run rebindDeformer");
        if (b->points != points.size())
            throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
        const glm::dmat4 dm = matrix("driverMatrix"), geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dvec3> disp(nv);
        for (size_t j = 0; j < nv; ++j)
            disp[j] = xform(dm, cage->position(geo::Index(j))) - b->rest[j];
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 d(0.0);
            const float* row = &b->weights[i * nv];
            for (size_t j = 0; j < nv; ++j)
                d += disp[j] * double(row[j]);
            points[i] = xform(geomInv, xform(geom, points[i]) + d);
        });
    }

    bool gpuProgram(const geo::Mesh& in, deformEval::Program& out) const override
    {
        if (deformers::cageUsesCoordinates(*this))
            return deformers::cageGpuProgram(*this, in.pointCapacity(), out, mCoordinates);
        DataPtr bindData = as<DataPtr>(attr("binding"));
        const auto* b = dynamic_cast<const CageData*>(bindData.get());
        const MeshPtr& cage = as<MeshPtr>(attr("driver"));
        if (!b)
            throw Unbound();
        if (!cage)
            throw std::runtime_error("no cage connected");
        const size_t nv = b->rest.size();
        if (cage->pointCapacity() != nv)
            throw std::runtime_error("the cage's points changed since binding: run rebindDeformer");
        if (b->points != in.pointCapacity())
            throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
        if (b->weights.size() != size_t(b->points) * nv || b->weights.size() >= size_t(UINT32_MAX))
            return false;
        const glm::dmat4 dm = matrix("driverMatrix");
        out.kernel = deformEval::Kernel::CageLinear;
        out.push(glm::dmat3(safeInverse(matrix("geomMatrix"), "the mesh")));
        out.push(uint32_t(nv));
        out.push(uint32_t(0));
        for (size_t j = 0; j < nv; ++j)
            out.push(xform(dm, cage->position(geo::Index(j))) - b->rest[j]);
        out.blobs[0] = deformEval::blob(deformEval::identityKey(bindData), bindData, b->weights);
        return true;
    }

private:
    mutable std::shared_ptr<deformers::CageCache> mCoordinates;
};

class SoftModNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const glm::dmat4 move = matrix("handleMatrix") * safeInverse(matrix("bindMatrix"), "the bind matrix");
        const glm::dvec3 center = as<glm::dvec3>(attr("falloffCenter"));
        const double radius = num("falloffRadius");
        const std::string& curveText = as<std::string>(attr("falloffCurve"));
        softSelect::Curve curve;
        if (!curveText.empty())
            curve = softSelect::Curve::fromJson(json::parse(curveText, nullptr, false));
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 w = xform(geom, points[i]);
            double f = 1.0;
            if (radius > 0.0)
            {
                double d = glm::length(w - center) / radius;
                f = d >= 1.0 ? 0.0 : curve.evaluate(d);
            }
            if (f <= 0.0)
                return;
            points[i] = xform(geomInv, glm::mix(w, xform(move, w), f));
        });
    }
};

// ------------------------------------------------------------------------------------------------ registration

NodeType deformerType(deformers::DeformerSpec s)
{
    if (!s.make)
        throw std::invalid_argument("deformer '" + s.name + "' has no make");
    NodeType t;
    t.name = s.name;
    t.icon = s.icon;
    t.doc = s.doc;
    t.retiredAttrs = std::move(s.retired);
    t.timeDependent = s.timeDependent;
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        keyable("enabled", PlugType::Bool, true, "Turn the deformer off without removing it."),
        stored("exportEnabled", PlugType::Bool, true, "Apply the deformer when exporting."),
        keyable("envelope", PlugType::Float, 1.0, "How much of the deformation applies: 0 none, 1 all.", 0.0, 1.0),
        hiddenStored("weights", PlugType::Data, DataPtr(), "Per-point weight map; points past its end weigh 1."),
        {.name = "field", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden | AttrResetOnDisconnect,
         .doc = "A field's outField (connectField): shapes where the deformer acts."},
        enumAttr("fieldMode", 0, {"multiply", "replace"},
                 "How a connected field combines with the weight map: multiply scales the painted weights by it, replace uses the "
                 "field instead of them."),
        hiddenStored("membership", PlugType::Data, DataPtr(),
                     "The points the deformer may move (setDeformerMembership); unset means every point."),
        {.name = "evalTime", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput,
         .doc = "How long the last evaluation took, milliseconds."},
    };
    for (AttrDef& p : s.params)
    {
        if (p.type == PlugType::Vec3)
            for (int k = 0; k < 3; ++k)
                p.childNames[k] = p.name + char('X' + k);
        t.ownAttrs.push_back(std::move(p));
    }
    t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden});
    const int out = int(t.ownAttrs.size()) - 1;
    auto make = s.make;
    t.create = [make, out]() -> std::unique_ptr<Node> {
        std::unique_ptr<DeformerNode> n = make();
        n->inAttr = DeformerNode::kInMesh;
        n->enabledAttr = DeformerNode::kEnabled;
        n->outAttr = out;
        n->apply = [](const geo::Mesh& m, const Node& self) { return static_cast<const DeformerNode&>(self).run(m); };
        return n;
    };
    return t;
}

std::vector<AttrDef> nonlinearCommon(std::vector<AttrDef> own)
{
    std::vector<AttrDef> a = {connected("handleMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's worldMatrix."), geomMatrixAttr(),
                              falloffRadiusAttr(), falloffSoftnessAttr()};
    a.insert(a.end(), own.begin(), own.end());
    return a;
}

AttrDef lowBound(double def = -1.0) { return boundsAttr("lowBound", def, "Where the effect starts along the handle's Y axis, handle units."); }
AttrDef highBound(double def = 1.0) { return boundsAttr("highBound", def, "Where the effect ends along the handle's Y axis, handle units."); }

template <NonlinearKind K> std::unique_ptr<DeformerNode> makeNonlinear() { return std::make_unique<NonlinearNode>(K); }

// ------------------------------------------------------------------------------------------------ command helpers

/** Splices the deformer into the mesh's stack: at the top, or just under the skinCluster when belowSkin. */
void insertDeformer(Graph& graph, NodeId mesh, DeformerNode& d, bool belowSkin)
{
    PlugRef top{mesh, MeshNode::kInMesh, -1};
    PlugRef at = top;
    if (belowSkin)
        if (NodeId s = skin::find(graph, mesh); s != kNoNode)
            at = {s, SkinClusterNode::kInMesh, -1};
    PlugRef in{d.id(), DeformerNode::kInMesh, -1};
    if (auto src = graph.source(at))
    {
        graph.disconnect(at);
        graph.connect(*src, in);
    }
    else
    {
        Node* source = graph.createNode(*types::find("meshSource"), {}, kNoNode, true);
        graph.setValue({source->id(), MeshSourceNode::kMesh, -1}, graph.storedValue(top));
        graph.setValue(top, MeshPtr());
        graph.connect({source->id(), MeshSourceNode::kOutMesh, -1}, in);
    }
    graph.connect({d.id(), d.outAttr, -1}, at);
}

DeformerNode& createDeformer(Graph& graph, const std::string& type, NodeId mesh, const std::string& name, bool belowSkin)
{
    const NodeType* t = types::find(type);
    Node* n = guarded([&] { return graph.createNode(*t, name, kNoNode, true); });
    auto& d = static_cast<DeformerNode&>(*n);
    insertDeformer(graph, mesh, d, belowSkin);
    if (int a = d.attr("geomMatrix"); a >= 0)
        graph.setValue({d.id(), a, -1}, worldOf(*graph.node(mesh)));
    return d;
}

NodeId meshOfDeformer(Graph& graph, NodeId d) { return modifiers::meshOf(graph, d); }

/** The deformer's input points in world space, evaluated now. */
std::vector<glm::dvec3> inputWorld(Graph& graph, DeformerNode& d, MeshPtr* meshOut = nullptr)
{
    graph.evaluate();
    MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
    if (!in)
        throw CommandError(d.name() + ": the mesh has no geometry");
    glm::dmat4 geom = d.as<glm::dmat4>(d.attr("geomMatrix"));
    std::vector<glm::dvec3> out(in->pointCapacity());
    for (geo::Index p = 0; p < in->pointCapacity(); ++p)
        out[p] = xform(geom, in->position(p));
    if (meshOut)
        *meshOut = in;
    return out;
}

/** The driver mesh feeding a wrap or cage, with its points in world space. */
std::pair<MeshPtr, glm::dmat4> driverOf(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr m = std::get<MeshPtr>(graph.value({d.id(), d.attr("driver"), -1}));
    if (!m)
        throw CommandError(d.name() + ": no driver mesh connected");
    return {m, std::get<glm::dmat4>(graph.value({d.id(), d.attr("driverMatrix"), -1}))};
}

void bindCage(Graph& graph, DeformerNode& d)
{
    if (d.integer("coordinates") != 0)
        return deformers::bindCageCoordinates(graph, d);
    auto [cage, dm] = driverOf(graph, d);
    auto data = std::make_shared<CageData>();
    data->rest.resize(cage->pointCapacity());
    for (geo::Index p = 0; p < cage->pointCapacity(); ++p)
        data->rest[p] = xform(dm, cage->position(p));
    std::vector<uint32_t> tris = deform::trianglePoints(*cage);
    if (tris.empty())
        throw CommandError(d.name() + ": the cage has no faces");
    std::vector<glm::dvec3> targets = inputWorld(graph, d);
    data->points = uint32_t(targets.size());
    data->weights = deform::meanValueCoordinates(data->rest, tris, targets);
    graph.setValue({d.id(), d.attr("binding"), -1}, DataPtr(std::move(data)));
}

void bindDeltaMush(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr rest;
    NodeId mesh = meshOfDeformer(graph, d.id());
    // Above a skinCluster the rest shape is the bind shape, not whatever pose the joints are in now.
    if (NodeId s = mesh != kNoNode ? skin::find(graph, mesh) : kNoNode; s != kNoNode)
    {
        std::vector<NodeId> st = modifiers::stack(graph, mesh);
        if (std::find(st.begin(), st.end(), s) < std::find(st.begin(), st.end(), d.id()))
            rest = skin::inputMesh(graph, s);
    }
    MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
    if (!rest || !in || rest->pointCapacity() != in->pointCapacity())
        rest = in;
    if (!rest)
        throw CommandError(d.name() + ": the mesh has no geometry");
    auto data = std::make_shared<RestPoints>();
    data->points.assign(rest->positions().begin(), rest->positions().end());
    graph.setValue({d.id(), d.attr("rest"), -1}, DataPtr(std::move(data)));
}

/** Locator handle sized to the bounds along `axis` (0 x, 1 y, 2 z); its +Y is the deformation axis. */
Node* makeHandle(Graph& graph, const std::string& name, const geo::Bounds& b, int axis)
{
    Node* h = guarded([&] { return graph.createNode(*types::find("locator"), name, kNoNode, true); });
    glm::dvec3 size = b.size();
    double extent = size[axis] > 1e-9 ? size[axis] : std::max({size.x, size.y, size.z, 1.0});
    double s = extent * 0.5;
    graph.setValue({h->id(), Transform::kTranslate, -1}, b.center());
    glm::dvec3 rot(0.0);
    if (axis == 0)
        rot.z = -90.0;
    else if (axis == 2)
        rot.x = 90.0;
    graph.setValue({h->id(), Transform::kRotate, -1}, rot);
    graph.setValue({h->id(), Transform::kScale, -1}, glm::dvec3(s));
    return h;
}

} // namespace

// ------------------------------------------------------------------------------------------------ data kinds

json PointWeights::toJson() const { return {{"kind", kind()}, {"values", values}}; }

DataPtr PointWeights::fromJson(const json& j)
{
    auto w = std::make_shared<PointWeights>();
    readArray(j, "values", w->values);
    return w;
}

// ------------------------------------------------------------------------------------------------ DeformerNode

double DeformerNode::num(const char* name) const
{
    int a = attr(name);
    if (a < 0)
        return 0.0;
    const PlugValue& v = value(a);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

int64_t DeformerNode::integer(const char* name) const { return int64_t(std::llround(num(name))); }

glm::dmat4 DeformerNode::matrix(const char* name) const
{
    int a = attr(name);
    return a < 0 ? glm::dmat4(1.0) : as<glm::dmat4>(a);
}

geo::Mesh DeformerNode::run(const geo::Mesh& in) const { return run(in, nullptr); }

geo::Mesh DeformerNode::run(const geo::Mesh& in, bool* ranOnGpu) const
{
    LILA_ZONE();

    std::vector<glm::dvec3> points(in.positions().begin(), in.positions().end());
    const bool gpu = deformEval::tryGpu(*this, in, points);
    if (!gpu)
        deform(in, points);
    if (ranOnGpu)
        *ranOnGpu = gpu;
    const std::vector<float> weight = deformWeights::effective(*this, in);
    const double envelope = as<double>(kEnvelope);
    geo::Mesh out = in;
    parallelPoints(points.size(), [&](size_t p) {
        if (!in.pointValid(geo::Index(p)))
            return;
        double w = envelope * double(weight[p]);
        const glm::dvec3& rest = in.position(geo::Index(p));
        glm::dvec3 moved = rest + (points[p] - rest) * w;
        if (std::isfinite(moved.x) && std::isfinite(moved.y) && std::isfinite(moved.z))
            out.setPosition(geo::Index(p), moved);
    });
    if (out.cornerAttrs().has(geo::attr::kNormal))
        geo::updateCornerNormals(out);
    if (out.cornerAttrs().has(geo::attr::kTangent))
        geo::updateTangents(out);
    return out;
}

void DeformerNode::compute(const EvalContext&)
{
    LILA_ZONE();

    // compute reads nothing but this node's inputs, so the same inputs give the same output.
    const bool canCache = cacheable() && !type().timeDependent;
    const std::vector<AttrDef>& attrs = type().attrs;
    if (canCache && mLastInputs.size() == attrs.size() && mLastOut)
    {
        bool same = true;
        for (size_t a = 0; a < attrs.size() && same; ++a)
            same = !(attrs[a].flags & AttrInput) || (mLastInputs[a] == value(int(a)) && mLastElements[a] == elements(int(a)));
        if (same)
        {
            deformEval::countCompute(true);
            setOutput(outAttr, mLastOut);
            setOutput(kEvalTime, 0.0);
            return;
        }
    }
    deformEval::countCompute(false);
    mLastInputs.clear();
    mLastOut.reset();
    mRanOnGpu = false;

    mError.clear();
    setOutput(kEvalTime, 0.0);
    const MeshPtr& in = as<MeshPtr>(kInMesh);
    if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in) || as<double>(kEnvelope) == 0.0)
    {
        setOutput(outAttr, in);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    try
    {
        setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(run(*in, &mRanOnGpu))));
        setOutput(kEvalTime, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    catch (const Unbound& e)
    {
        mError = e.what();
        setOutput(outAttr, in);
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        mError = e.what();
        setOutput(outAttr, in);
    }
    if (canCache)
    {
        mLastInputs.assign(attrs.size(), PlugValue());
        mLastElements.assign(attrs.size(), {});
        for (size_t a = 0; a < attrs.size(); ++a)
            if (attrs[a].flags & AttrInput)
            {
                mLastInputs[a] = value(int(a));
                mLastElements[a] = elements(int(a));
            }
        mLastOut = as<MeshPtr>(outAttr);
    }
}

// ------------------------------------------------------------------------------------------------ api

namespace deformers
{
bool isDeformer(const Node& n) { return (n.traits() & Node::kDeformer) != 0; }

const NodeType& registerDeformer(DeformerSpec spec)
{
    return types::registerType(deformerType(std::move(spec)));
}

const NodeType* registerDeformer(ext::Context& ctx, DeformerSpec spec)
{
    try
    {
        return types::registerType(ctx, deformerType(std::move(spec)));
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

std::vector<NodeId> addDeformer(Graph& graph, const std::string& type, const std::vector<NodeId>& meshes, const std::string& name, bool belowSkin)
{
    const NodeType* t = types::find(type);
    if (!t || !t->create || !(t->create()->traits() & Node::kDeformer))
        throw CommandError("'" + type + "' is not a deformer type");
    std::vector<NodeId> out;
    for (NodeId mesh : meshes)
        out.push_back(createDeformer(graph, type, mesh, name.empty() ? type : name, belowSkin).id());
    return out;
}

std::vector<NodeId> list(Graph& graph, NodeId mesh)
{
    std::vector<NodeId> out;
    for (NodeId id : modifiers::stack(graph, mesh))
        if (isDeformer(*graph.node(id)))
            out.push_back(id);
    return out;
}

std::vector<float> pointWeights(Graph& graph, NodeId deformer, size_t n)
{
    std::vector<float> out(n, 1.0f);
    const auto* w = dynamic_cast<const PointWeights*>(std::get<DataPtr>(graph.storedValue({deformer, DeformerNode::kWeights, -1})).get());
    if (w)
        for (size_t i = 0; i < std::min(n, w->values.size()); ++i)
            out[i] = w->values[i];
    return out;
}

void registerTypes()
{
    if (types::find("lattice"))
        return;
    data::registerKind("pointWeights", &PointWeights::fromJson);
    data::registerKind("restPoints", &RestPoints::fromJson);
    data::registerKind("cageBinding", &CageData::fromJson);

    registerDeformer({"bendDeformer", "rainbow",
                      "Bends the mesh along an arc around its handle's Y axis, rigid past the bounds. Use it for curling, drooping and "
                      "arching shapes.",
                      nonlinearCommon({keyable("angle", PlugType::Float, 90.0, "Degrees bent across the bounds."), lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Bend>});
    registerDeformer({"twistDeformer", "rotate-clockwise",
                      "Twists the mesh around its handle's Y axis, the angle ramping from the low bound to the high one.",
                      nonlinearCommon({keyable("startAngle", PlugType::Float, 0.0, "Twist at the low bound, degrees."),
                                       keyable("endAngle", PlugType::Float, 90.0, "Twist at the high bound, degrees."), lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Twist>});
    registerDeformer({"taperDeformer", "triangle-inverted",
                      "Scales the mesh across its handle's Y axis, ramping from the low bound to the high one, with an optional bulge "
                      "(flare) in between.",
                      nonlinearCommon({keyable("startScale", PlugType::Float, 1.0, "Cross-section scale at the low bound.", 0.0),
                                       keyable("endScale", PlugType::Float, 0.5, "Cross-section scale at the high bound.", 0.0),
                                       keyable("curve", PlugType::Float, 0.0, "Bulge added midway, in scale units (negative pinches)."),
                                       lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Taper>});
    registerDeformer({"squashDeformer", "arrows-vertical",
                      "Squashes or stretches the mesh along its handle's Y axis while the sides bulge to keep the volume. The cartoon "
                      "squash and stretch.",
                      nonlinearCommon({keyable("factor", PlugType::Float, -0.5, "Length change along Y: -0.5 halves it, 1 doubles it.", -1.0),
                                       keyable("expand", PlugType::Float, 1.0, "How much the sides bulge to keep the volume, 0 not at all.", 0.0),
                                       keyable("maxExpandPos", PlugType::Float, 0.5, "Where between the bounds the bulge peaks, 0 to 1.", 0.0, 1.0),
                                       lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Squash>});
    registerDeformer({"sineDeformer", "wave-sine",
                      "Offsets the mesh along the handle's X axis by a sine wave running along its Y axis. Use it for flags, tails and "
                      "wiggles.",
                      nonlinearCommon({keyable("amplitude", PlugType::Float, 0.2, "Wave height, handle units."),
                                       keyable("wavelength", PlugType::Float, 1.0, "Length of one wave, handle units.", 1e-3),
                                       keyable("offset", PlugType::Float, 0.0, "Phase, in waves; key it to animate the wave."),
                                       keyable("dropoff", PlugType::Float, 0.0, "-1 to 1: fades the amplitude toward the ends (positive) or the middle (negative).", -1.0, 1.0),
                                       lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Sine>});
    registerDeformer({"waveDeformer", "ripple",
                      "Ripples the mesh along the handle's Y axis in rings spreading from it. Use it for water rings and shock waves.",
                      nonlinearCommon({keyable("amplitude", PlugType::Float, 0.1, "Ripple height, handle units."),
                                       keyable("wavelength", PlugType::Float, 0.5, "Distance between rings, handle units.", 1e-3),
                                       keyable("offset", PlugType::Float, 0.0, "Phase, in waves; key it to make the rings travel."),
                                       keyable("dropoff", PlugType::Float, 0.0, "-1 to 1: fades the rings outward (positive) or inward (negative).", -1.0, 1.0),
                                       keyable("minRadius", PlugType::Float, 0.0, "Rings start this far from the axis, handle units.", 0.0),
                                       keyable("maxRadius", PlugType::Float, 1.0, "Rings stop this far from the axis, handle units.", 0.0)}),
                      &makeNonlinear<NonlinearKind::Wave>});

    AttrDef divisions = stored("divisions", PlugType::Vec3, glm::dvec3(4.0), "Lattice points per axis, set when the lattice is made.", 2.0, 32.0);
    registerDeformer({"ffd", "grid-dots",
                      "Free-form deformation: the mesh follows a lattice whose points are moved with the usual component tools. Use it "
                      "for broad shape changes and squash and stretch.",
                      {connected("lattice", PlugType::Mesh, MeshPtr(), "The lattice's outMesh."),
                       connected("latticeMatrix", PlugType::Matrix, glm::dmat4(1.0), "The lattice's worldMatrix."),
                       hiddenStored("baseMatrix", PlugType::Matrix, glm::dmat4(1.0), "The lattice's rest box: its world matrix when bound."),
                       geomMatrixAttr(), divisions,
                       enumAttr("interpolation", 1, {"trilinear", "bernstein", "bspline"},
                                "Trilinear is exact per cell, Bernstein smooth and global, bspline smooth and local (best for dense lattices)."),
                       keyable("outsideFalloff", PlugType::Float, 0.0,
                               "Points outside the lattice box are affected up to this far (lattice units), fading out. 0: only points inside.", 0.0)},
                      [] { return std::make_unique<FfdNode>(); }});

    registerDeformer({"deltaMush", "droplet",
                      "Smooths away deformation artefacts while keeping the surface detail of the rest shape. Put it on top of a "
                      "skinCluster to fix collapsing elbows and candy-wrapped twists.",
                      {keyable("iterations", PlugType::Int, int64_t(10), "Smoothing passes; more spreads the correction further.", 0.0, 1000.0),
                       keyable("smoothStep", PlugType::Float, 0.5, "How far each pass moves a point toward its neighbours, 0 to 1.", 0.0, 1.0),
                       keyable("displacement", PlugType::Float, 1.0, "How much of the rest detail is put back: 0 gives the smoothed mesh."),
                       stored("pinBorders", PlugType::Bool, true, "Keep open borders from shrinking while smoothing."),
                       hiddenStored("rest", PlugType::Data, DataPtr(), "The rest shape captured at bind time.")},
                      [] { return std::make_unique<DeltaMushNode>(); }});

    std::vector<AttrDef> cageAttrs = {connected("driver", PlugType::Mesh, MeshPtr(), "The cage's outMesh."),
                                      connected("driverMatrix", PlugType::Matrix, glm::dmat4(1.0), "The cage's worldMatrix."), geomMatrixAttr(),
                                      hiddenStored("binding", PlugType::Data, DataPtr(), "Each point's coordinates against the rest cage.")};
    for (AttrDef& a : cageParams())
        cageAttrs.push_back(std::move(a));
    registerDeformer({"cage", "box",
                      "Deforms the mesh by a closed low-poly cage around it (mean value, Green or Somigliana coordinates): edit the "
                      "cage, the mesh inside follows smoothly.",
                      std::move(cageAttrs), [] { return std::make_unique<CageNode>(); }});

    registerDeformer({"softMod", "focus-2",
                      "Moves points with a handle, fading out with distance through the soft select falloff curve. Radius 0 moves "
                      "every point (weighted by the weight map), like a cluster.",
                      {connected("handleMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's worldMatrix."),
                       hiddenStored("bindMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's world matrix when bound."), geomMatrixAttr(),
                       keyable("falloffCenter", PlugType::Vec3, glm::dvec3(0.0), "Centre of the falloff, world space."),
                       keyable("falloffRadius", PlugType::Float, 10.0, "Falloff radius, cm. 0 affects every point fully.", 0.0),
                       stored("falloffCurve", PlugType::String, std::string(),
                              "Falloff shape as soft select curve JSON ([[pos, value, interp], ...]); empty for the default smooth curve.")},
                      [] { return std::make_unique<SoftModNode>(); }});

    // The lattice is a mesh, so its points are edited with every component tool; the viewport draws it as wire.
    NodeType lattice;
    lattice.name = "lattice";
    lattice.icon = "grid-dots";
    lattice.base = types::find("mesh");
    lattice.dag = true;
    lattice.doc = "The control lattice of an ffd deformer. Move its points (vertex mode) to reshape the meshes it deforms.";
    lattice.create = [] { return std::make_unique<MeshNode>(); };
    types::registerType(std::move(lattice));

    registerSurfaceTypes();
    morphs::registerTypes();
    jiggle::registerTypes();
    bendbones::registerTypes();
    meshCache::registerTypes();
    registerShapeTypes();
    registerTextureTypes();
    fields::registerTypes();
    deformWeights::registerTypes();
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "deformers",
        .doc = "A mesh's deformers (morph, ffd, deltaMush, ...), applied first first.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            for (NodeId mesh : meshesOf(a.objects(), "deformers"))
                for (NodeId id : deformers::list(Graph::current(), mesh))
                    out.push_back(Graph::current().node(id)->name());
            return out;
        },
    });

    ops::registerCommand({
        .name = "deformer",
        .doc = "Add a deformer of any registered type, an extension's included, to the top of each mesh's stack. Returns the "
               "new deformers.",
        .positionalDoc = "meshes",
        .flags = {{"type", "", T::String, nullptr, "The deformer node type, e.g. deltaMush."},
                  {"params", "", T::Any, nullptr, "{param: value} to set on each deformer."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            if (!a.has("type"))
                throw CommandError("deformer: give a type");
            Graph& graph = Graph::current();
            std::vector<NodeId> made =
                deformers::addDeformer(graph, a.get<std::string>("type"), meshesOf(a.objects(), "deformer"), a.get<std::string>("name"));
            json out = json::array();
            for (NodeId id : made)
            {
                if (a.has("params"))
                    applyParams(graph, *graph.node(id), a.flags["params"]);
                out.push_back(graph.node(id)->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "nonlinear",
        .doc = "Add a nonlinear deformer (bend, twist, taper, squash, sine or wave) to the meshes, driven by a new handle sized to "
               "them. Move, rotate and scale the handle to place the effect. Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"type", "", T::String, "bend", "bend | twist | taper | squash | sine | wave", {"bend", "twist", "taper", "squash", "sine", "wave"}},
                  {"axis", "", T::String, "y", "World axis the handle's deformation axis lies along: x, y or z.", {"x", "y", "z"}},
                  {"params", "", T::Any, nullptr, "{param: value} to set on each deformer, e.g. {\"angle\": 45}."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "nonlinear");
            std::string kind = a.get<std::string>("type");
            const NodeType* t = types::find(kind + "Deformer");
            if (!t || !t->isA(kind + "Deformer") || kind.empty())
                throw CommandError("nonlinear: unknown type '" + kind + "'");
            std::string label = std::string(1, char(std::toupper(static_cast<unsigned char>(kind[0])))) + kind.substr(1);
            Node* h = makeHandle(graph, label + "Handle", worldBounds(graph, meshes), axisIndex(a.get<std::string>("axis"), "nonlinear"));
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = createDeformer(graph, kind + "Deformer", mesh, a.has("name") ? a.get<std::string>("name") : label, false);
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                if (a.has("params"))
                    applyParams(graph, d, a.flags["params"]);
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    ops::registerCommand({
        .name = "lattice",
        .doc = "Add an ffd deformer to the meshes with a new lattice around them. Edit the lattice's points in vertex mode (soft select "
               "and symmetry work) or transform the whole lattice. Returns [lattice, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"divisions", "", T::Vec3, json::array({4, 4, 4}), "Lattice points along x, y and z, each 2 to 32."},
                  {"interpolation", "", T::String, "bernstein", "trilinear | bernstein | bspline", {"trilinear", "bernstein", "bspline"}},
                  {"name", "", T::String, nullptr, "Name for the lattice."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "lattice");
            glm::dvec3 div = vec3Of(a.flags["divisions"]);
            glm::ivec3 n(std::clamp(int(div.x), 2, 32), std::clamp(int(div.y), 2, 32), std::clamp(int(div.z), 2, 32));
            std::string interp = a.get<std::string>("interpolation");
            int64_t basis = interp == "trilinear" ? 0 : interp == "bspline" ? 2 : interp == "bernstein" ? 1 : -1;
            if (basis < 0)
                throw CommandError("lattice: interpolation must be trilinear, bernstein or bspline");
            geo::Bounds b = worldBounds(graph, meshes);
            glm::dvec3 size = b.size();
            double pad = std::max(glm::length(size) * 0.005, 1e-3);
            size = glm::max(size + glm::dvec3(pad * 2.0), glm::dvec3(pad));

            Node* lat = guarded([&] { return graph.createNode(*types::find("lattice"), a.has("name") ? a.get<std::string>("name") : "Lattice", kNoNode, true); });
            graph.setValue({lat->id(), Transform::kTranslate, -1}, b.center());
            graph.setValue({lat->id(), Transform::kScale, -1}, size);
            graph.setValue({lat->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(deform::latticeMesh(n))));
            graph.evaluate();
            glm::dmat4 latW = worldOf(*lat);
            json out = json::array({lat->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = createDeformer(graph, "ffd", mesh, "Ffd", false);
                setPlug(graph, d.id(), "divisions", glm::dvec3(n));
                setPlug(graph, d.id(), "interpolation", basis);
                setPlug(graph, d.id(), "baseMatrix", latW);
                connectPlug(graph, lat->id(), "outMesh", d.id(), "lattice");
                connectPlug(graph, lat->id(), "worldMatrix", d.id(), "latticeMatrix");
                out.push_back(d.name());
            }
            graph.select({lat->id()});
            return out;
        },
    });

    ops::registerCommand({
        .name = "deltaMush",
        .doc = "Add a delta mush to the meshes: smooths out deformation artefacts (from skinning or other deformers) while keeping "
               "the rest shape's detail. Above a skinCluster the rest shape is the bind shape. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"iterations", "", T::Int, 10, "Smoothing passes.", {}, {}, 0.0, 1000.0},
                  {"smoothStep", "", T::Float, 0.5, "How far each pass moves a point, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            json out = json::array();
            for (NodeId mesh : meshesOf(a.objects(), "deltaMush"))
            {
                DeformerNode& d = createDeformer(graph, "deltaMush", mesh, a.has("name") ? a.get<std::string>("name") : "DeltaMush", false);
                setPlug(graph, d.id(), "iterations", int64_t(a.get<int>("iterations")));
                setPlug(graph, d.id(), "smoothStep", a.get<double>("smoothStep"));
                bindDeltaMush(graph, d);
                out.push_back(d.name());
            }
            return out;
        },
    });

    auto driverCommand = [](const char* cmd, const char* type, const char* label) {
        return [cmd, type, label](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError(std::string(cmd) + ": give the meshes to deform, then the driver last");
            Node& driver = meshNamed(objs.back(), cmd);
            objs.pop_back();
            json out = json::array();
            for (NodeId mesh : meshesOf(objs, cmd))
            {
                if (mesh == driver.id())
                    throw CommandError(std::string(cmd) + ": a mesh can't drive itself");
                DeformerNode& d = createDeformer(graph, type, mesh, a.has("name") ? a.get<std::string>("name") : label, false);
                guarded([&] {
                    connectPlug(graph, driver.id(), "outMesh", d.id(), "driver");
                    connectPlug(graph, driver.id(), "worldMatrix", d.id(), "driverMatrix");
                    return 0;
                });
                if (a.has("coordinates"))
                    applyParams(graph, d, {{"coordinates", a.get<std::string>("coordinates")}});
                bindCage(graph, d);
                out.push_back(d.name());
            }
            return out;
        };
    };

    ops::registerCommand({
        .name = "cage",
        .doc = "Deform meshes by a closed cage mesh around them (mean value coordinates). Give the meshes first and the cage last; "
               "then edit the cage's points. Returns the deformers.",
        .positionalDoc = "meshes..., cage",
        .flags = {{"coordinates", "", T::String, nullptr, "meanValue (default) | green | somigliana | harmonic", {"meanValue", "green", "somigliana", "harmonic"}},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = driverCommand("cage", "cage", "Cage"),
    });

    ops::registerCommand({
        .name = "softMod",
        .doc = "Add a soft modification handle: moving, rotating or scaling the handle moves nearby points, fading out over the "
               "radius with the soft select curve. Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"position", "", T::Vec3, nullptr, "Handle and falloff centre, world space. Default: the meshes' centre."},
                  {"radius", "", T::Float, nullptr, "Falloff radius, cm. Default: a quarter of the meshes' size.", {}, {}, 0.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "softMod");
            geo::Bounds b = worldBounds(graph, meshes);
            glm::dvec3 pos = a.has("position") ? vec3Of(a.flags["position"]) : b.center();
            double radius = a.has("radius") ? a.get<double>("radius") : std::max(glm::length(b.size()) * 0.25, 1e-3);
            Node* h = guarded([&] { return graph.createNode(*types::find("locator"), "SoftModHandle", kNoNode, true); });
            graph.setValue({h->id(), Transform::kTranslate, -1}, pos);
            graph.evaluate();
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = createDeformer(graph, "softMod", mesh, a.has("name") ? a.get<std::string>("name") : "SoftMod", false);
                setPlug(graph, d.id(), "bindMatrix", worldOf(*h));
                setPlug(graph, d.id(), "falloffCenter", pos);
                setPlug(graph, d.id(), "falloffRadius", radius);
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    ops::registerCommand({
        .name = "rebindDeformer",
        .doc = "Re-capture a deformer's bind-time state from the scene as it is now: the mesh's placement, the delta mush rest "
               "shape, wrap and cage bindings, the softMod handle's rest, or the lattice's rest box.",
        .positionalDoc = "deformers",
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            for (const std::string& name : a.objects())
            {
                DeformerNode& d = deformerNamed(name, "rebindDeformer");
                NodeId mesh = meshOfDeformer(graph, d.id());
                graph.evaluate();
                if (int ga = d.attr("geomMatrix"); ga >= 0 && mesh != kNoNode)
                    graph.setValue({d.id(), ga, -1}, worldOf(*graph.node(mesh)));
                if (rebindSurface(graph, d) || rebindShape(graph, d) || rebindTexture(graph, d))
                    continue;
                const std::string& t = d.type().name;
                if (t == "deltaMush")
                    bindDeltaMush(graph, d);
                else if (t == "cage")
                    bindCage(graph, d);
                else if (t == "softMod")
                {
                    if (auto src = graph.source({d.id(), d.attr("handleMatrix"), -1}))
                    {
                        glm::dmat4 hw = worldOf(*graph.node(src->node));
                        graph.setValue({d.id(), d.attr("bindMatrix"), -1}, hw);
                        graph.setValue({d.id(), d.attr("falloffCenter"), -1}, glm::dvec3(hw[3]));
                    }
                }
                else if (t == "ffd")
                {
                    if (auto src = graph.source({d.id(), d.attr("latticeMatrix"), -1}))
                        graph.setValue({d.id(), d.attr("baseMatrix"), -1}, worldOf(*graph.node(src->node)));
                }
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setDeformerWeights",
        .doc = "Set a deformer's per-point weight map: weights= the whole map (one value per point), or points= ids with value=. "
               "Weights scale the envelope per point; 1 is full effect.",
        .positionalDoc = "deformer",
        .flags = {{"weights", "", T::List, nullptr, "One weight per point."},
                  {"points", "", T::List, nullptr, "Point ids to set to value."},
                  {"value", "", T::Float, 1.0, "Weight for points=."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("setDeformerWeights: no deformer");
            DeformerNode& d = deformerNamed(objs.back(), "setDeformerWeights");
            graph.evaluate();
            MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
            size_t n = in ? in->pointCapacity() : 0;
            auto w = std::make_shared<PointWeights>();
            w->values = deformers::pointWeights(graph, d.id(), n);
            if (a.has("weights"))
            {
                w->values.clear();
                for (const json& v : a.flags["weights"])
                    w->values.push_back(v.get<float>());
            }
            if (a.has("points"))
                for (const json& p : a.flags["points"])
                {
                    size_t i = p.get<size_t>();
                    if (i >= w->values.size())
                        w->values.resize(i + 1, 1.0f);
                    w->values[i] = float(a.get<double>("value"));
                }
            graph.setValue({d.id(), DeformerNode::kWeights, -1}, DataPtr(std::move(w)));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "deformerWeights",
        .doc = "A deformer's per-point weight map, one value per point of its input (1 where unpainted).",
        .positionalDoc = "deformer",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("deformerWeights: no deformer");
            DeformerNode& d = deformerNamed(objs.back(), "deformerWeights");
            graph.evaluate();
            MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
            return deformers::pointWeights(graph, d.id(), in ? in->pointCapacity() : 0);
        },
    });

    registerSurfaceCommands();
    morphs::registerCommands();
    jiggle::registerCommands();
    bendbones::registerCommands();
    meshCache::registerCommands();
    skinbake::registerCommands();
    registerShapeCommands();
    registerTextureCommands();
    fields::registerCommands();
    deformWeights::registerCommands();
}
} // namespace deformers
} // namespace lila::scene
