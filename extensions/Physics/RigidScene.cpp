// Jolt behind RigidScene. Jolt is built cross-platform deterministic; its results depend on the order of API calls, so
// every call here happens in a fixed order, and contacts gathered on Jolt's worker threads are folded so their order
// never shows.

#include "RigidScene.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <tbb/task_arena.h>
#include <tbb/task_group.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace lila::physics
{
namespace
{
constexpr double kM = 0.01; // cm to m
constexpr JPH::uint kMaxBodies = 1u << 16;
constexpr JPH::uint kMaxBodiesLimit = 1u << 18;

namespace layers
{
constexpr JPH::ObjectLayer kMoving = 0;
constexpr JPH::ObjectLayer kStatic = 1;
constexpr JPH::ObjectLayer kNone = 2; // collides with nothing
} // namespace layers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return JPH::BroadPhaseLayer(l == layers::kStatic ? 1 : 0); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override { return l == JPH::BroadPhaseLayer(1) ? "static" : "moving"; }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer l, JPH::BroadPhaseLayer bp) const override
    {
        return l != layers::kNone && (l == layers::kStatic ? bp == JPH::BroadPhaseLayer(0) : true);
    }
};

class LayerPairs final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        return a != layers::kNone && b != layers::kNone && (a == layers::kMoving || b == layers::kMoving);
    }
};

/**
 * Jolt's jobs on Lila's TBB pool. Jolt's results don't depend on how its jobs are scheduled, only on the order of API
 * calls, so this stays deterministic. A barrier counts the jobs added to it and waits on the task group until they ran.
 */
class TbbJobSystem final : public JPH::JobSystem
{
public:
    ~TbbJobSystem() noexcept override { mTasks->wait(); }

    int GetMaxConcurrency() const override { return tbb::this_task_arena::max_concurrency(); }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 dependencies = 0) override
    {
        Job* job = new Job(name, color, this, fn, dependencies);
        JobHandle handle(job);
        if (dependencies == 0)
            QueueJob(job);
        return handle;
    }

    Barrier* CreateBarrier() override { return new BarrierImpl(); }
    void DestroyBarrier(Barrier* barrier) override { delete static_cast<BarrierImpl*>(barrier); }

    void WaitForJobs(Barrier* barrier) override
    {
        auto* b = static_cast<BarrierImpl*>(barrier);
        while (b->pending.load(std::memory_order_acquire) > 0)
        {
            mTasks->wait();
            if (b->pending.load(std::memory_order_acquire) > 0)
                std::this_thread::yield();
        }
        std::lock_guard lock(b->mutex);
        b->jobs.clear();
    }

protected:
    class BarrierImpl final : public Barrier
    {
    public:
        void AddJob(const JobHandle& handle) override
        {
            if (!handle.GetPtr()->SetBarrier(this))
                return;
            pending.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard lock(mutex);
            jobs.push_back(handle);
        }
        void AddJobs(const JobHandle* handles, JPH::uint count) override
        {
            for (JPH::uint k = 0; k < count; ++k)
                AddJob(handles[k]);
        }

        std::atomic<int> pending{0};
        std::mutex mutex;
        std::vector<JobHandle> jobs; // kept alive until the wait is over

    protected:
        void OnJobFinished(Job*) override { pending.fetch_sub(1, std::memory_order_acq_rel); }
    };

    void QueueJob(Job* job) override
    {
        job->AddRef();
        mTasks->run([job] {
            job->Execute();
            job->Release();
        });
    }

    void QueueJobs(Job** jobs, JPH::uint count) override
    {
        for (JPH::uint k = 0; k < count; ++k)
            QueueJob(jobs[k]);
    }

    void FreeJob(Job* job) override { delete job; }

private:
    // Isolated: a bound context would take the first caller's task as its parent and its FP settings would outlive it.
    tbb::task_group_context mContext{tbb::task_group_context::isolated};
    std::unique_ptr<tbb::task_group> mTasks = std::make_unique<tbb::task_group>(mContext);
};

/** Built shapes by key, the least recently used dropped past the limit. Bodies still holding a dropped shape keep it alive. */
class ShapeCache
{
public:
    JPH::RefConst<JPH::Shape> find(uint64_t key)
    {
        std::lock_guard lock(mMutex);
        const auto it = mMap.find(key);
        if (it == mMap.end())
            return nullptr;
        mOrder.splice(mOrder.begin(), mOrder, it->second.second);
        return it->second.first;
    }

    void put(uint64_t key, JPH::RefConst<JPH::Shape> shape)
    {
        std::lock_guard lock(mMutex);
        if (mMap.count(key))
            return;
        mOrder.push_front(key);
        mMap.emplace(key, std::make_pair(std::move(shape), mOrder.begin()));
        while (mMap.size() > kLimit)
        {
            mMap.erase(mOrder.back());
            mOrder.pop_back();
        }
    }

    size_t size()
    {
        std::lock_guard lock(mMutex);
        return mMap.size();
    }

    void clear()
    {
        std::lock_guard lock(mMutex);
        mMap.clear();
        mOrder.clear();
    }

private:
    static constexpr size_t kLimit = 8192;
    std::mutex mMutex;
    std::list<uint64_t> mOrder;
    std::unordered_map<uint64_t, std::pair<JPH::RefConst<JPH::Shape>, std::list<uint64_t>::iterator>> mMap;
};

struct Runtime
{
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystem> jobs, serialJobs;
    ShapeCache shapes;
    std::atomic<bool> ready = false;
    ~Runtime()
    {
        if (!ready)
            return;
        shapes.clear();
        jobs.reset();
        serialJobs.reset();
        temp.reset();
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
} gRuntime;

JPH::Vec3 toJ(const glm::dvec3& v) { return JPH::Vec3(float(v.x), float(v.y), float(v.z)); }
JPH::Quat toJ(const glm::dquat& q) { return JPH::Quat(float(q.x), float(q.y), float(q.z), float(q.w)).Normalized(); }
glm::dvec3 fromJ(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::dquat fromJ(JPH::QuatArg q) { return glm::dquat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

JPH::RefConst<JPH::Shape> create(const JPH::ShapeSettings& s, const char* what)
{
    JPH::ShapeSettings::ShapeResult r = s.Create();
    if (r.HasError())
        throw std::invalid_argument(std::string(what) + ": " + r.GetError().c_str());
    return r.Get();
}

/** Points in metres, with their size: the diagonal of their bounds. */
JPH::Array<JPH::Vec3> metres(const std::vector<glm::dvec3>& points, double& size)
{
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const glm::dvec3& p : points)
    {
        pts.push_back(toJ(p * kM));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    size = points.empty() ? 0.0 : glm::length(hi - lo) * kM;
    return pts;
}

JPH::RefConst<JPH::Shape> sphere(double radiusM, double density)
{
    JPH::SphereShapeSettings ss(float(std::max(radiusM, 0.001)));
    ss.SetDensity(float(std::max(density, 1e-3)));
    return create(ss, "sphere");
}

JPH::RefConst<JPH::Shape> hull(const std::vector<glm::dvec3>& points, double rounding, double density)
{
    double size = 0.0;
    JPH::Array<JPH::Vec3> pts = metres(points, size);
    if (pts.size() >= 4)
    {
        JPH::ConvexHullShapeSettings hs(pts, float(std::min(rounding * kM, size * 0.05)));
        hs.SetDensity(float(std::max(density, 1e-3)));
        JPH::ShapeSettings::ShapeResult r = hs.Create();
        if (!r.HasError())
            return r.Get();
    }
    // Flat or empty: a small sphere keeps the body in the simulation.
    return sphere(std::max(size * 0.5, 0.005), density);
}

JPH::RefConst<JPH::Shape> mesh(const std::vector<glm::dvec3>& t)
{
    JPH::TriangleList tris;
    for (size_t i = 0; i + 2 < t.size(); i += 3)
    {
        const glm::dvec3 a = t[i] * kM, b = t[i + 1] * kM, c = t[i + 2] * kM;
        if (glm::length(glm::cross(b - a, c - a)) < 1e-12)
            continue;
        tris.push_back(JPH::Triangle(toJ(a), toJ(b), toJ(c)));
    }
    if (tris.empty())
        return sphere(0.001, 1000.0);
    return create(JPH::MeshShapeSettings(tris), "mesh");
}

/** The convex parts as one compound; a single part is just its hull. */
JPH::RefConst<JPH::Shape> compound(const Shape& s, double density)
{
    std::vector<JPH::RefConst<JPH::Shape>> hulls;
    for (const auto& part : s.parts)
        if (part && part->size() >= 4)
            hulls.push_back(hull(*part, s.rounding, density));
    if (hulls.empty())
        return hull(s.points ? *s.points : std::vector<glm::dvec3>{}, s.rounding, density);
    if (hulls.size() == 1)
        return hulls[0];
    JPH::StaticCompoundShapeSettings cs;
    for (const auto& h : hulls)
        cs.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), h.GetPtr());
    return create(cs, "compound");
}

/**
 * Jolt wants a square of samples whose side over the block size is a power of two; the grid sits in its corner and the
 * rest is padded with holes.
 */
JPH::RefConst<JPH::Shape> heightfield(const Shape& s)
{
    const int nx = s.samplesX, nz = s.samplesZ;
    if (!s.heights || nx < 2 || nz < 2 || s.heights->size() < size_t(nx) * size_t(nz))
        return sphere(0.001, 1000.0);
    constexpr JPH::uint32 kBlock = 4;
    JPH::uint32 side = kBlock * 2;
    while (side < JPH::uint32(std::max(nx, nz)))
        side *= 2;
    std::vector<float> samples(size_t(side) * side, JPH::HeightFieldShapeConstants::cNoCollisionValue);
    const std::vector<float>& h = *s.heights;
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
            samples[size_t(z) * side + size_t(x)] = float(double(h[size_t(z) * size_t(nx) + size_t(x)]) * kM);
    JPH::HeightFieldShapeSettings hs(samples.data(), JPH::Vec3(float(s.gridOrigin.x * kM), 0.0f, float(s.gridOrigin.y * kM)),
                                     JPH::Vec3(float(s.cell * kM), 1.0f, float(s.cell * kM)), side);
    hs.mBlockSize = kBlock;
    return create(hs, "heightfield");
}

JPH::RefConst<JPH::Shape> buildShape(const BodyDesc& b)
{
    static const std::vector<glm::dvec3> kNone;
    const Shape& s = b.shape;
    const std::vector<glm::dvec3>& pts = s.points ? *s.points : kNone;
    JPH::RefConst<JPH::Shape> inner;
    switch (s.kind)
    {
    case Shape::Kind::Mesh:
        if (b.role != Role::Dynamic)
            return mesh(pts);
        // Jolt can't simulate a triangle mesh; a dynamic one becomes its hull.
        return hull(pts, s.rounding, b.density);
    case Shape::Kind::Hull:
        return hull(pts, s.rounding, b.density);
    case Shape::Kind::Compound:
        inner = compound(s, b.density);
        break;
    case Shape::Kind::Heightfield:
        if (b.role == Role::Dynamic)
            throw std::invalid_argument("a heightfield body can't be dynamic");
        return heightfield(s);
    case Shape::Kind::Box:
    {
        const glm::dvec3 h = glm::max(s.halfExtents * kM, glm::dvec3(0.0005));
        const double radius = std::min(s.rounding * kM, std::min({h.x, h.y, h.z}) * 0.5);
        JPH::BoxShapeSettings bs(toJ(h), float(radius));
        bs.SetDensity(float(std::max(b.density, 1e-3)));
        inner = create(bs, "box");
        break;
    }
    case Shape::Kind::Sphere:
        inner = sphere(s.radius * kM, b.density);
        break;
    case Shape::Kind::Capsule:
    {
        JPH::CapsuleShapeSettings cs(float(std::max(s.halfHeight * kM, 0.0005)), float(std::max(s.radius * kM, 0.0005)));
        cs.SetDensity(float(std::max(b.density, 1e-3)));
        inner = create(cs, "capsule");
        break;
    }
    }
    if (s.center == glm::dvec3(0.0) && s.rotation == glm::dquat(1, 0, 0, 0))
        return inner;
    JPH::RotatedTranslatedShapeSettings rt(toJ(s.center * kM), toJ(s.rotation), inner);
    return create(rt, "offset shape");
}

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}

uint64_t bits(double d)
{
    uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

/** The body's shape, from the cache when the shape has a key. */
JPH::RefConst<JPH::Shape> shapeOf(const BodyDesc& b)
{
    const Shape& s = b.shape;
    if (s.key == 0)
        return buildShape(b);
    const bool dynamicMesh = s.kind == Shape::Kind::Mesh && b.role == Role::Dynamic;
    uint64_t key = mix(s.key, uint64_t(s.kind) * 2 + (dynamicMesh ? 1 : 0));
    for (double d : {s.rounding, s.center.x, s.center.y, s.center.z, s.halfExtents.x, s.halfExtents.y, s.halfExtents.z, s.radius, s.halfHeight, s.rotation.w, s.rotation.x, s.rotation.y, s.rotation.z})
        key = mix(key, bits(d));
    // Density lives in the convex shapes; static meshes and heightfields have none.
    if (s.kind != Shape::Kind::Heightfield && (s.kind != Shape::Kind::Mesh || dynamicMesh))
        key = mix(key, bits(b.density));
    if (JPH::RefConst<JPH::Shape> hit = gRuntime.shapes.find(key))
        return hit;
    JPH::RefConst<JPH::Shape> made = buildShape(b);
    gRuntime.shapes.put(key, made);
    return made;
}

/** A pair of bodies, the smaller index first; the ground is -1. */
using PairKey = std::pair<int, int>;

/** What one step saw of a pair, folded from every manifold so callback order never shows. */
struct StepTouch
{
    glm::dvec3 point{0.0}, normal{0.0, 1.0, 0.0};
    double depth = -1e300;
    double speed = 0.0;
    double slide = 0.0;
    double reducedMass = 0.0;
};
} // namespace

void initJolt()
{
    if (gRuntime.ready.load(std::memory_order_acquire))
        return;
    // Graph evaluation builds several physics users at once; a second init would free the job system the first one holds.
    static std::mutex initing;
    std::lock_guard lock(initing);
    if (gRuntime.ready.load(std::memory_order_relaxed))
        return;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    gRuntime.temp = std::make_unique<JPH::TempAllocatorImpl>(64 * 1024 * 1024);
    gRuntime.jobs = std::make_unique<TbbJobSystem>();
    gRuntime.serialJobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    gRuntime.ready.store(true, std::memory_order_release);
}

struct RigidScene::Impl final : public JPH::ContactListener
{
    JPH::JobSystem* jobs = nullptr;
    // The filters outlive the system that points at them.
    BroadPhaseLayers bp;
    ObjectVsBroadPhase objectVsBp;
    LayerPairs pairs;
    JPH::PhysicsSystem system;
    Settings settings;
    std::vector<BodyDesc> descs;
    std::vector<JPH::BodyID> ids;
    JPH::BodyID ground;
    JPH::Ref<JPH::GroupFilterTable> groups;
    std::vector<JPH::Ref<JPH::TwoBodyConstraint>> joints;
    std::vector<JointKind> jointKinds;
    JPH::uint maxBodies = kMaxBodies;
    int64_t stepCount = 0;
    bool addedSinceOptimize = false;
    double dt = 1.0 / 60.0;

    std::mutex touchMutex;
    std::map<PairKey, StepTouch> touches;
    std::map<PairKey, Contact> contacts;

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }
    const JPH::BodyInterface& bi() const { return system.GetBodyInterfaceNoLock(); }

    JPH::BodyID id(int body) const
    {
        if (body < 0 || size_t(body) >= ids.size())
            throw std::out_of_range("no body " + std::to_string(body));
        return ids[size_t(body)];
    }

    static int indexOf(const JPH::Body& b) { return int(int64_t(b.GetUserData()) - 1); }

    static double inverseMass(const JPH::Body& b) { return b.IsDynamic() ? double(b.GetMotionPropertiesUnchecked()->GetInverseMass()) : 0.0; }

    /** A world point (cm) in Jolt's space: metres from the scene's origin. */
    JPH::RVec3 toScene(const glm::dvec3& p) const { return JPH::RVec3(toJ((p - settings.origin) * kM)); }
    glm::dvec3 fromScene(JPH::RVec3Arg p) const { return fromJ(JPH::Vec3(p)) / kM + settings.origin; }

    /** Solves a contact between very different masses as if the heavier were only maxMassRatio times the lighter. */
    void balance(const JPH::Body& b1, const JPH::Body& b2, JPH::ContactSettings& cs) const
    {
        const double r = settings.maxMassRatio;
        if (r <= 1.0 || !b1.IsDynamic() || !b2.IsDynamic())
            return;
        const double i1 = inverseMass(b1), i2 = inverseMass(b2);
        if (i1 <= 0.0 || i2 <= 0.0)
            return;
        const double m1 = 1.0 / i1, m2 = 1.0 / i2;
        if (m1 > r * m2)
        {
            cs.mInvMassScale1 = float(m1 / (r * m2));
            cs.mInvInertiaScale1 = cs.mInvMassScale1;
        }
        else if (m2 > r * m1)
        {
            cs.mInvMassScale2 = float(m2 / (r * m1));
            cs.mInvInertiaScale2 = cs.mInvMassScale2;
        }
    }

    void touch(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m)
    {
        if (!settings.recordContacts)
            return;
        int a = indexOf(b1), b = indexOf(b2);
        JPH::Vec3 n = m.mWorldSpaceNormal;
        if (b >= 0 && (a < 0 || b < a))
        {
            std::swap(a, b);
            n = -n;
        }
        const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        const JPH::Vec3 rel = b1.GetPointVelocity(p) - b2.GetPointVelocity(p);
        const double normalSpeed = std::abs(double(rel.Dot(m.mWorldSpaceNormal)));
        const double tangential = double((rel - m.mWorldSpaceNormal * rel.Dot(m.mWorldSpaceNormal)).Length());
        const double inv = inverseMass(b1) + inverseMass(b2);

        std::lock_guard lock(touchMutex);
        StepTouch& t = touches[{a, b}];
        // The deepest manifold places the pair; peaks are maxima, so any callback order gives the same record.
        if (double(m.mPenetrationDepth) > t.depth)
        {
            t.depth = double(m.mPenetrationDepth);
            t.point = fromScene(p);
            t.normal = fromJ(n);
        }
        t.speed = std::max(t.speed, normalSpeed / kM);
        t.slide = std::max(t.slide, tangential / kM * dt);
        t.reducedMass = std::max(t.reducedMass, inv > 0.0 ? 1.0 / inv : 0.0);
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings& cs) override
    {
        balance(b1, b2, cs);
        touch(b1, b2, m);
    }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings& cs) override
    {
        balance(b1, b2, cs);
        touch(b1, b2, m);
    }

    bool sleeping(int body) const { return body < 0 || !bi().IsActive(ids[size_t(body)]); }

    /** Folds the step's touches into the contact records. Pairs Jolt stopped reporting because both slept stay resting. */
    void foldTouches()
    {
        for (auto& [key, c] : contacts)
            if (!touches.count(key))
                c.resting = c.resting && sleeping(key.first) && sleeping(key.second);
        for (const auto& [key, t] : touches)
        {
            Contact& c = contacts[key];
            c.a = key.first;
            c.b = key.second;
            c.point = t.point;
            c.normal = t.normal;
            c.peakImpulse = std::max(c.peakImpulse, t.speed * t.reducedMass);
            c.slide += t.slide;
            ++c.steps;
            c.resting = true;
        }
        touches.clear();
    }
};

RigidScene::RigidScene(const Settings& s) : mImpl(std::make_unique<Impl>())
{
    initJolt();
    Impl& w = *mImpl;
    const char* threads = std::getenv("LILA_PHYSICS_THREADS");
    w.jobs = s.serial || (threads && std::atoi(threads) == 1) ? gRuntime.serialJobs.get() : gRuntime.jobs.get();
    w.settings = s;
    w.maxBodies = s.maxBodies > 0 ? std::min(JPH::uint(s.maxBodies), kMaxBodiesLimit) : kMaxBodies;
    const JPH::uint maxPairs = s.maxPairs > 0 ? JPH::uint(s.maxPairs) : std::max(kMaxBodies, w.maxBodies);
    w.system.Init(w.maxBodies, 0, maxPairs, maxPairs, w.bp, w.objectVsBp, w.pairs);
    JPH::PhysicsSettings ps;
    ps.mDeterministicSimulation = true;
    ps.mAllowSleeping = s.sleep;
    ps.mPenetrationSlop = float(std::max(s.penetrationSlop, 0.0) * kM);
    ps.mSpeculativeContactDistance = float(std::max(s.speculativeDistance, 0.0) * kM);
    ps.mPointVelocitySleepThreshold = float(std::max(s.sleepSpeed, 0.0) * kM);
    if (s.velocitySteps > 0)
        ps.mNumVelocitySteps = JPH::uint(std::clamp(s.velocitySteps, 2, 64));
    if (s.positionSteps > 0)
        ps.mNumPositionSteps = JPH::uint(std::clamp(s.positionSteps, 1, 32));
    ps.mUseLargeIslandSplitter = s.largeIslandSplitter;
    w.system.SetPhysicsSettings(ps);
    if (s.pairFilter > 0)
        w.groups = new JPH::GroupFilterTable(JPH::uint(s.pairFilter));
    w.system.SetGravity(toJ(s.gravity * kM));
    w.system.SetContactListener(&w);
    if (s.ground)
    {
        const float extent = float(std::clamp(s.groundExtent, 100.0, 1e7) * kM);
        const JPH::Plane plane(JPH::Vec3(0, 1, 0), float(-(s.groundHeight - s.origin.y) * kM));
        JPH::BodyCreationSettings g(create(JPH::PlaneShapeSettings(plane, nullptr, extent), "ground"),
                                    JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kStatic);
        g.mFriction = float(s.groundFriction);
        g.mUserData = 0;
        w.ground = w.bi().CreateAndAddBody(g, JPH::EActivation::DontActivate);
    }
}

RigidScene::~RigidScene()
{
    if (!mImpl)
        return;
    Impl& w = *mImpl;
    w.system.SetContactListener(nullptr);
    for (auto& j : w.joints)
        w.system.RemoveConstraint(j);
    JPH::BodyInterface& bi = w.bi();
    auto drop = [&](JPH::BodyID id) {
        if (id.IsInvalid())
            return;
        bi.RemoveBody(id);
        bi.DestroyBody(id);
    };
    for (JPH::BodyID id : w.ids)
        drop(id);
    drop(w.ground);
}

int RigidScene::add(const BodyDesc& b)
{
    Impl& w = *mImpl;
    if (w.ids.size() + 1 >= w.maxBodies)
        throw std::invalid_argument("too many bodies (" + std::to_string(w.maxBodies) + " at most)");
    const bool fixed = b.role == Role::Static;
    const JPH::EMotionType motion = fixed ? JPH::EMotionType::Static : b.role == Role::Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings bs(shapeOf(b), w.toScene(b.position), toJ(b.rotation), motion,
                                 !b.collides ? layers::kNone : fixed ? layers::kStatic : layers::kMoving);
    // Anything may change role later: a pinned book let go, a shelf shaken by hand. setRole keeps meshes from going dynamic.
    bs.mAllowDynamicOrKinematic = true;
    bs.mLinearVelocity = toJ(b.velocity * kM);
    bs.mAngularVelocity = toJ(b.angularVelocity);
    bs.mFriction = float(b.friction);
    bs.mRestitution = float(b.bounce);
    bs.mLinearDamping = float(b.damping);
    bs.mAngularDamping = float(b.angularDamping < 0.0 ? b.damping : b.angularDamping);
    bs.mAllowSleeping = w.settings.sleep && b.canSleep;
    bs.mMotionQuality = b.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    if (b.mass > 0.0)
    {
        bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bs.mMassPropertiesOverride.mMass = float(b.mass);
    }
    if (w.groups && w.ids.size() < size_t(w.settings.pairFilter))
        bs.mCollisionGroup = JPH::CollisionGroup(w.groups, 0, JPH::CollisionGroup::SubGroupID(w.ids.size()));
    bs.mUserData = JPH::uint64(w.ids.size() + 1);
    w.ids.push_back(w.bi().CreateAndAddBody(bs, fixed ? JPH::EActivation::DontActivate : JPH::EActivation::Activate));
    w.descs.push_back(b);
    w.addedSinceOptimize = true;
    return int(w.ids.size() - 1);
}

size_t RigidScene::size() const { return mImpl->ids.size(); }

const BodyDesc& RigidScene::desc(int body) const
{
    mImpl->id(body);
    return mImpl->descs[size_t(body)];
}

void RigidScene::step(double dt)
{
    Impl& w = *mImpl;
    if (w.addedSinceOptimize)
    {
        w.system.OptimizeBroadPhase();
        w.addedSinceOptimize = false;
    }
    w.dt = dt;
    {
        // The temp allocator and job system are shared by every scene.
        static std::mutex stepping;
        std::lock_guard lock(stepping);
        w.system.Update(float(dt), std::max(1, w.settings.collisionSteps), gRuntime.temp.get(), w.jobs);
    }
    w.foldTouches();
    ++w.stepCount;
}

double RigidScene::settle(double seconds, double dt)
{
    double t = 0.0;
    while (t < seconds)
    {
        step(dt);
        t += dt;
        if (atRest())
            break;
    }
    return t;
}

SettleResult RigidScene::settleUntilRest(const SettleOptions& o)
{
    Impl& w = *mImpl;
    SettleResult r;
    const double dt = std::max(o.dt, 1e-4);
    double quiet = 0.0;
    while (r.seconds + dt * 0.5 < o.cap)
    {
        step(dt);
        r.seconds += dt;
        ++r.steps;
        if (atRest())
        {
            r.atRest = true;
            break;
        }
        bool still = o.quietTime > 0.0;
        for (size_t i = 0; still && i < w.ids.size(); ++i)
        {
            if (w.descs[i].role != Role::Dynamic || !w.bi().IsActive(w.ids[i]))
                continue;
            still = double(w.bi().GetLinearVelocity(w.ids[i]).Length()) / kM < o.quietSpeed &&
                    double(w.bi().GetAngularVelocity(w.ids[i]).Length()) < o.quietSpin;
        }
        quiet = still ? quiet + dt : 0.0;
        if (still && quiet >= o.quietTime)
        {
            r.atRest = true;
            break;
        }
    }
    r.capped = !r.atRest;
    return r;
}

int64_t RigidScene::steps() const { return mImpl->stepCount; }

const Settings& RigidScene::settings() const { return mImpl->settings; }

Pose RigidScene::pose(int body) const
{
    const JPH::BodyID id = mImpl->id(body);
    const JPH::BodyInterface& bi = mImpl->bi();
    return {mImpl->fromScene(bi.GetPosition(id)), fromJ(bi.GetRotation(id))};
}

glm::dvec3 RigidScene::velocity(int body) const { return fromJ(mImpl->bi().GetLinearVelocity(mImpl->id(body))) / kM; }

bool RigidScene::asleep(int body) const { return !mImpl->bi().IsActive(mImpl->id(body)); }

bool RigidScene::atRest() const
{
    const Impl& w = *mImpl;
    for (size_t i = 0; i < w.ids.size(); ++i)
        if (w.descs[i].role == Role::Dynamic && w.bi().IsActive(w.ids[i]))
            return false;
    return true;
}

void RigidScene::setRole(int body, Role role)
{
    Impl& w = *mImpl;
    const JPH::BodyID id = w.id(body);
    BodyDesc& d = w.descs[size_t(body)];
    if (d.role == role)
        return;
    JPH::BodyInterface& bi = w.bi();
    // A mesh body added as dynamic was built as its hull and may go dynamic again; one built as triangles can't.
    const JPH::EShapeSubType built = bi.GetShape(id)->GetSubType();
    if (role == Role::Dynamic && (built == JPH::EShapeSubType::Mesh || built == JPH::EShapeSubType::HeightField))
        throw std::invalid_argument("a triangle mesh or heightfield body can't be dynamic");
    bi.SetObjectLayer(id, !d.collides ? layers::kNone : role == Role::Static ? layers::kStatic : layers::kMoving);
    bi.SetMotionType(id,
                     role == Role::Static      ? JPH::EMotionType::Static
                     : role == Role::Kinematic ? JPH::EMotionType::Kinematic
                                               : JPH::EMotionType::Dynamic,
                     role == Role::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    d.role = role;
}

void RigidScene::moveTo(int body, const Pose& target, double dt)
{
    mImpl->bi().MoveKinematic(mImpl->id(body), mImpl->toScene(target.position), toJ(target.rotation), float(std::max(dt, 1e-6)));
}

void RigidScene::setPose(int body, const Pose& p)
{
    mImpl->bi().SetPositionAndRotation(mImpl->id(body), mImpl->toScene(p.position), toJ(p.rotation), JPH::EActivation::Activate);
}

void RigidScene::setVelocity(int body, const glm::dvec3& linear, const glm::dvec3& angular)
{
    mImpl->bi().SetLinearAndAngularVelocity(mImpl->id(body), toJ(linear * kM), toJ(angular));
}

void RigidScene::impulse(int body, const glm::dvec3& j, const glm::dvec3& at)
{
    const JPH::BodyID id = mImpl->id(body);
    if (mImpl->descs[size_t(body)].role != Role::Dynamic)
        return;
    mImpl->bi().AddImpulse(id, toJ(j * kM), mImpl->toScene(at));
}

void RigidScene::kick(int body, const glm::dvec3& v)
{
    const JPH::BodyID id = mImpl->id(body);
    if (mImpl->descs[size_t(body)].role != Role::Dynamic)
        return;
    mImpl->bi().AddLinearVelocity(id, toJ(v * kM));
}

void RigidScene::wake(int body) { mImpl->bi().ActivateBody(mImpl->id(body)); }

namespace
{
glm::dmat3 skew(const glm::dvec3& r) { return glm::dmat3(0, r.z, -r.y, -r.z, 0, r.x, r.y, -r.x, 0); }
} // namespace

void RigidScene::pull(int body, const glm::dvec3& local, const glm::dvec3& target, double frequency, double dt)
{
    Impl& w = *mImpl;
    const JPH::BodyID id = w.id(body);
    if (w.descs[size_t(body)].role != Role::Dynamic)
        return;
    JPH::BodyLockWrite lock(w.system.GetBodyLockInterfaceNoLock(), id);
    if (!lock.Succeeded())
        return;
    JPH::Body& b = lock.GetBody();
    const JPH::RMat44 xf = b.GetWorldTransform();
    const glm::dvec3 p = fromJ(JPH::Vec3(xf * toJ(local * kM)));
    const glm::dvec3 com = fromJ(JPH::Vec3(b.GetCenterOfMassPosition()));
    const glm::dvec3 r = p - com;
    const double omega = 2.0 * 3.14159265358979323846 * std::max(frequency, 0.01);
    const double blend = 1.0 - std::exp(-omega * dt);
    const glm::dvec3 v = fromJ(b.GetLinearVelocity()), wv = fromJ(b.GetAngularVelocity());
    const glm::dvec3 pointVel = v + glm::cross(wv, r);
    const glm::dvec3 want = ((target - w.settings.origin) * kM - p) * omega;
    const glm::dvec3 dv = (want - pointVel) * blend;

    // The impulse at p that changes p's velocity by dv: K = m^-1 I - [r] I^-1 [r].
    const double invM = double(b.GetMotionProperties()->GetInverseMass());
    const JPH::Mat44 ii = b.GetInverseInertia();
    glm::dmat3 invI;
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k)
            invI[c][k] = double(ii(k, c));
    const glm::dmat3 s = skew(r);
    const glm::dmat3 K = glm::dmat3(invM) - s * invI * s;
    if (std::abs(glm::determinant(K)) < 1e-18)
        return;
    const glm::dvec3 j = glm::inverse(K) * dv;
    b.AddImpulse(toJ(j), JPH::RVec3(toJ(p)));
    // A held thing would spin about the hand forever without some drag on its turning.
    b.SetAngularVelocity(b.GetAngularVelocity() * float(1.0 - 0.3 * blend));
    w.bi().ActivateBody(id);
}

double RigidScene::mass(int body) const
{
    const Impl& w = *mImpl;
    JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), w.id(body));
    if (!lock.Succeeded() || !lock.GetBody().IsDynamic())
        return 0.0;
    const float inv = lock.GetBody().GetMotionProperties()->GetInverseMass();
    return inv > 0.0f ? 1.0 / double(inv) : 0.0;
}

glm::dvec3 RigidScene::centerOfMass(int body) const { return mImpl->fromScene(mImpl->bi().GetCenterOfMassPosition(mImpl->id(body))); }

void RigidScene::bounds(int body, glm::dvec3& lo, glm::dvec3& hi) const
{
    const JPH::AABox box = mImpl->bi().GetTransformedShape(mImpl->id(body)).GetWorldSpaceBounds();
    lo = fromJ(box.mMin) / kM + mImpl->settings.origin;
    hi = fromJ(box.mMax) / kM + mImpl->settings.origin;
}

std::optional<RayHit> RigidScene::raycast(const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance) const
{
    const Impl& w = *mImpl;
    const double len = glm::length(direction);
    if (len < 1e-12 || maxDistance <= 0.0)
        return std::nullopt;
    const glm::dvec3 d = direction / len;
    const JPH::RRayCast ray(w.toScene(origin), toJ(d * maxDistance * kM));
    JPH::RayCastResult hit;
    if (!w.system.GetNarrowPhaseQueryNoLock().CastRay(ray, hit))
        return std::nullopt;
    RayHit out;
    out.distance = double(hit.mFraction) * maxDistance;
    out.point = origin + d * out.distance;
    JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), hit.mBodyID);
    if (lock.Succeeded())
    {
        const JPH::Body& b = lock.GetBody();
        out.body = Impl::indexOf(b);
        out.normal = fromJ(b.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, w.toScene(out.point)));
    }
    return out;
}

double RigidScene::penetration(int body) const
{
    const Impl& w = *mImpl;
    const JPH::BodyID id = w.id(body);
    const JPH::TransformedShape ts = w.bi().GetTransformedShape(id);
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = 0.0f;
    // A body sunk past the middle of a mesh or heightfield faces its triangles from behind, and is still inside.
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    JPH::IgnoreSingleBodyFilter self(id);
    w.system.GetNarrowPhaseQueryNoLock().CollideShape(ts.mShape, ts.GetShapeScale(), ts.GetCenterOfMassTransform(), settings, JPH::RVec3::sZero(), hits,
                                                      {}, {}, self);
    double deepest = 0.0;
    for (const JPH::CollideShapeResult& h : hits.mHits)
        deepest = std::max(deepest, double(h.mPenetrationDepth));
    return deepest / kM;
}

std::vector<Contact> RigidScene::contacts() const
{
    std::vector<Contact> out;
    out.reserve(mImpl->contacts.size());
    for (const auto& [key, c] : mImpl->contacts)
        out.push_back(c);
    return out;
}

void RigidScene::clearContacts() { mImpl->contacts.clear(); }

std::vector<uint8_t> RigidScene::save() const
{
    JPH::StateRecorderImpl rec;
    mImpl->system.SaveState(rec);
    const std::string data = rec.GetData();
    return std::vector<uint8_t>(data.begin(), data.end());
}

void RigidScene::restore(const std::vector<uint8_t>& state)
{
    JPH::StateRecorderImpl rec;
    rec.WriteBytes(state.data(), state.size());
    rec.Rewind();
    if (!mImpl->system.RestoreState(rec))
        throw std::runtime_error("the physics state couldn't be restored");
}
int RigidScene::addJoint(const JointDesc& j)
{
    Impl& w = *mImpl;
    const JPH::BodyID ia = w.id(j.a);
    JPH::Body* ba = nullptr;
    JPH::Body* bb = &JPH::Body::sFixedToWorld;
    {
        JPH::BodyLockWrite la(w.system.GetBodyLockInterfaceNoLock(), ia);
        ba = la.Succeeded() ? &la.GetBody() : nullptr;
    }
    if (j.b >= 0)
    {
        if (j.b == j.a)
            throw std::invalid_argument("a joint needs two different bodies");
        JPH::BodyLockWrite lb(w.system.GetBodyLockInterfaceNoLock(), w.id(j.b));
        bb = lb.Succeeded() ? &lb.GetBody() : nullptr;
    }
    if (!ba || !bb)
        throw std::invalid_argument("a joint's body is missing");
    const JPH::RVec3 p1 = w.toScene(j.pointA), p2 = w.toScene(j.pointB);
    JPH::Ref<JPH::TwoBodyConstraint> c;
    switch (j.kind)
    {
    case JointKind::Fixed:
    {
        JPH::FixedConstraintSettings fs;
        fs.mSpace = JPH::EConstraintSpace::WorldSpace;
        fs.mAutoDetectPoint = false;
        fs.mPoint1 = p1;
        fs.mPoint2 = p2;
        c = static_cast<JPH::TwoBodyConstraint*>(fs.Create(*ba, *bb));
        break;
    }
    case JointKind::Distance:
    {
        JPH::DistanceConstraintSettings ds;
        ds.mSpace = JPH::EConstraintSpace::WorldSpace;
        ds.mPoint1 = p1;
        ds.mPoint2 = p2;
        ds.mMinDistance = float(std::max(j.minDistance, 0.0) * kM);
        ds.mMaxDistance = float(std::max(j.maxDistance, j.minDistance) * kM);
        ds.mLimitsSpringSettings.mFrequency = float(std::max(j.frequency, 0.0));
        ds.mLimitsSpringSettings.mDamping = float(std::max(j.damping, 0.0));
        c = static_cast<JPH::TwoBodyConstraint*>(ds.Create(*ba, *bb));
        break;
    }
    case JointKind::SwingTwist:
    {
        JPH::SwingTwistConstraintSettings st;
        st.mSpace = JPH::EConstraintSpace::WorldSpace;
        st.mPosition1 = p1;
        st.mPosition2 = p2;
        st.mTwistAxis1 = toJ(j.twistAxisA).Normalized();
        st.mPlaneAxis1 = toJ(j.planeAxisA).Normalized();
        st.mTwistAxis2 = toJ(j.twistAxisB).Normalized();
        st.mPlaneAxis2 = toJ(j.planeAxisB).Normalized();
        st.mPlaneHalfConeAngle = float(j.planeHalfCone);
        st.mNormalHalfConeAngle = float(j.normalHalfCone);
        st.mTwistMinAngle = float(j.twistMin);
        st.mTwistMaxAngle = float(j.twistMax);
        c = static_cast<JPH::TwoBodyConstraint*>(st.Create(*ba, *bb));
        break;
    }
    }
    if (!c)
        throw std::invalid_argument("the joint couldn't be made");
    w.system.AddConstraint(c);
    w.joints.push_back(c);
    w.jointKinds.push_back(j.kind);
    return int(w.joints.size() - 1);
}

size_t RigidScene::joints() const { return mImpl->joints.size(); }

namespace
{
JPH::TwoBodyConstraint& jointAt(RigidScene::Impl& w, int joint)
{
    if (joint < 0 || size_t(joint) >= w.joints.size())
        throw std::out_of_range("no joint " + std::to_string(joint));
    return *w.joints[size_t(joint)];
}
} // namespace

void RigidScene::setJointEnabled(int joint, bool on) { jointAt(*mImpl, joint).SetEnabled(on); }

bool RigidScene::jointEnabled(int joint) const { return jointAt(*mImpl, joint).GetEnabled(); }

JointLoad RigidScene::jointLoad(int joint) const
{
    JPH::TwoBodyConstraint& c = jointAt(*mImpl, joint);
    JointLoad l;
    switch (mImpl->jointKinds[size_t(joint)])
    {
    case JointKind::Fixed:
    {
        const auto& f = static_cast<const JPH::FixedConstraint&>(c);
        l.linear = double(f.GetTotalLambdaPosition().Length());
        l.angular = double(f.GetTotalLambdaRotation().Length());
        break;
    }
    case JointKind::Distance: l.linear = std::abs(double(static_cast<const JPH::DistanceConstraint&>(c).GetTotalLambdaPosition())); break;
    case JointKind::SwingTwist:
    {
        const auto& s = static_cast<const JPH::SwingTwistConstraint&>(c);
        l.linear = double(s.GetTotalLambdaPosition().Length());
        const double t = s.GetTotalLambdaTwist(), y = s.GetTotalLambdaSwingY(), z = s.GetTotalLambdaSwingZ();
        l.angular = std::sqrt(t * t + y * y + z * z) + double(s.GetTotalLambdaMotor().Length());
        break;
    }
    }
    return l;
}

void RigidScene::setJointMotor(int joint, const glm::dquat& target, double frequency, double damping)
{
    if (mImpl->jointKinds.at(size_t(joint)) != JointKind::SwingTwist)
        throw std::invalid_argument("only swing-twist joints have motors");
    auto& s = static_cast<JPH::SwingTwistConstraint&>(jointAt(*mImpl, joint));
    const bool on = frequency > 0.0;
    const JPH::EMotorState state = on ? JPH::EMotorState::Position : JPH::EMotorState::Off;
    s.SetSwingMotorState(state);
    s.SetTwistMotorState(state);
    if (!on)
        return;
    s.SetTargetOrientationCS(toJ(target));
    const JPH::MotorSettings ms(float(frequency), float(std::max(damping, 0.0)));
    s.GetSwingMotorSettings() = ms;
    s.GetTwistMotorSettings() = ms;
}

std::vector<uint8_t> RigidScene::saveJoint(int joint) const
{
    JPH::StateRecorderImpl rec;
    jointAt(*mImpl, joint).SaveState(rec);
    const std::string data = rec.GetData();
    return std::vector<uint8_t>(data.begin(), data.end());
}

void RigidScene::restoreJoint(int joint, const std::vector<uint8_t>& state)
{
    JPH::StateRecorderImpl rec;
    rec.WriteBytes(state.data(), state.size());
    rec.Rewind();
    jointAt(*mImpl, joint).RestoreState(rec);
}

size_t RigidScene::jointStateSize(JointKind kind)
{
    static const std::array<size_t, 3> sizes = [] {
        initJolt();
        std::array<size_t, 3> out{};
        JPH::FixedConstraintSettings f;
        JPH::DistanceConstraintSettings d;
        JPH::SwingTwistConstraintSettings s;
        const JPH::TwoBodyConstraintSettings* all[3] = {&f, &d, &s};
        for (size_t k = 0; k < 3; ++k)
        {
            JPH::Ref<JPH::TwoBodyConstraint> c = all[k]->Create(JPH::Body::sFixedToWorld, JPH::Body::sFixedToWorld);
            JPH::StateRecorderImpl rec;
            c->SaveState(rec);
            out[k] = rec.GetData().size();
        }
        return out;
    }();
    return sizes[size_t(kind)];
}

void RigidScene::primeStep(double dt)
{
    JPH::StateRecorderImpl rec;
    rec.Write(JPH::EStateRecorderState::Global);
    rec.Write(float(dt));
    rec.Write(mImpl->system.GetGravity());
    rec.Rewind();
    mImpl->system.RestoreState(rec);
}

void RigidScene::setCollides(int a, int b, bool on)
{
    Impl& w = *mImpl;
    if (!w.groups || a < 0 || b < 0 || a >= w.settings.pairFilter || b >= w.settings.pairFilter)
        throw std::out_of_range("setCollides: bodies outside Settings::pairFilter");
    if (a == b)
        return;
    if (on)
        w.groups->EnableCollision(JPH::CollisionGroup::SubGroupID(a), JPH::CollisionGroup::SubGroupID(b));
    else
        w.groups->DisableCollision(JPH::CollisionGroup::SubGroupID(a), JPH::CollisionGroup::SubGroupID(b));
}

double RigidScene::overlap(int a, int b) const
{
    const JPH::BodyInterface& bi = mImpl->bi();
    const JPH::TransformedShape ta = bi.GetTransformedShape(mImpl->id(a));
    const JPH::TransformedShape tb = bi.GetTransformedShape(mImpl->id(b));
    if (!ta.GetWorldSpaceBounds().Overlaps(tb.GetWorldSpaceBounds()))
        return 0.0;
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    tb.CollideShape(ta.mShape, ta.GetShapeScale(), ta.GetCenterOfMassTransform(), settings, JPH::RVec3::sZero(), hits);
    double deepest = 0.0;
    for (const JPH::CollideShapeResult& h : hits.mHits)
        deepest = std::max(deepest, double(h.mPenetrationDepth));
    return deepest / kM;
}

glm::dvec3 RigidScene::angularVelocity(int body) const { return fromJ(mImpl->bi().GetAngularVelocity(mImpl->id(body))); }

glm::dvec3 RigidScene::pointVelocity(int body, const glm::dvec3& at) const
{
    return fromJ(mImpl->bi().GetPointVelocity(mImpl->id(body), mImpl->toScene(at))) / kM;
}

RawState RigidScene::raw(int body) const
{
    const JPH::BodyID id = mImpl->id(body);
    const JPH::BodyInterface& bi = mImpl->bi();
    const JPH::Vec3 p(bi.GetPosition(id)), v = bi.GetLinearVelocity(id), a = bi.GetAngularVelocity(id);
    const JPH::Quat q = bi.GetRotation(id);
    return {{p.GetX(), p.GetY(), p.GetZ()}, {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}, {v.GetX(), v.GetY(), v.GetZ()}, {a.GetX(), a.GetY(), a.GetZ()}};
}

void RigidScene::accelerate(int body, const glm::dvec3& linear, const glm::dvec3& angular)
{
    Impl& w = *mImpl;
    const JPH::BodyID id = w.id(body);
    JPH::Vec3 force, torque;
    {
        JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), id);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic())
            return;
        const JPH::Body& b = lock.GetBody();
        const JPH::MotionProperties* mp = b.GetMotionProperties();
        const double mass = 1.0 / std::max(double(mp->GetInverseMass()), 1e-12);
        force = toJ(linear * (kM * mass));
        torque = mp->GetInverseInertiaForRotation(JPH::Mat44::sRotation(b.GetRotation())).Inversed3x3().Multiply3x3(toJ(angular));
    }
    w.bi().AddForce(id, force, JPH::EActivation::Activate);
    if (angular != glm::dvec3(0.0))
        w.bi().AddTorque(id, torque, JPH::EActivation::Activate);
}

void RigidScene::push(int body, const glm::dvec3& force, const glm::dvec3& at)
{
    mImpl->bi().AddForce(mImpl->id(body), toJ(force * kM), mImpl->toScene(at), JPH::EActivation::Activate);
}

std::vector<glm::dvec3> hullTriangles(const std::vector<glm::dvec3>& points)
{
    std::vector<glm::dvec3> out;
    if (points.size() < 4)
        return out;
    initJolt();
    JPH::Array<JPH::Vec3> pts;
    for (const glm::dvec3& p : points)
        pts.push_back(toJ(p));
    JPH::ConvexHullShapeSettings settings(pts, 0.0f);
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (r.HasError())
        return out;
    const auto* hull = static_cast<const JPH::ConvexHullShape*>(r.Get().GetPtr());
    std::vector<JPH::uint> face(64);
    for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f)
    {
        face.resize(std::max<size_t>(face.size(), hull->GetNumVerticesInFace(f)));
        const JPH::uint count = hull->GetFaceVertices(f, JPH::uint(face.size()), face.data());
        for (JPH::uint k = 1; k + 1 < count; ++k)
            for (JPH::uint v : {face[0], face[k], face[k + 1]})
                out.push_back(fromJ(hull->GetPoint(v) + hull->GetCenterOfMass()));
    }
    return out;
}
int maxBodiesLimit() { return int(kMaxBodiesLimit) - 1; }

double hullVolume(const std::vector<glm::dvec3>& points)
{
    if (points.size() < 4)
        return 0.0;
    initJolt();
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : points)
        c += p;
    c /= double(points.size());
    // Centred, so far-off points keep their precision in floats.
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const glm::dvec3& p : points)
        pts.push_back(toJ(p - c));
    JPH::ConvexHullShapeSettings hs(pts, 0.0f);
    JPH::ShapeSettings::ShapeResult r = hs.Create();
    return r.HasError() ? 0.0 : double(r.Get()->GetVolume());
}

namespace
{
ConvexMesh hullMesh(const std::vector<glm::dvec3>& points)
{
    ConvexMesh out;
    if (points.size() < 4)
        return out;
    initJolt();
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : points)
        c += p;
    c /= double(points.size());
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const glm::dvec3& p : points)
        pts.push_back(toJ(p - c));
    JPH::ConvexHullShapeSettings hs(pts, 0.0f);
    JPH::ShapeSettings::ShapeResult r = hs.Create();
    if (r.HasError())
        return out;
    const auto* h = static_cast<const JPH::ConvexHullShape*>(r.Get().GetPtr());
    const glm::dvec3 com = fromJ(h->GetCenterOfMass());
    for (JPH::uint i = 0; i < h->GetNumPoints(); ++i)
        out.points.push_back(fromJ(h->GetPoint(i)) + com + c);
    std::vector<JPH::uint> face(h->GetNumPoints());
    for (JPH::uint f = 0; f < h->GetNumFaces(); ++f)
    {
        const JPH::uint n = h->GetFaceVertices(f, JPH::uint(face.size()), face.data());
        if (n >= 3)
            out.faces.emplace_back(face.begin(), face.begin() + n);
    }
    return out;
}

/** Points on a sphere of the radius about center, stretched into a capsule along Y when halfHeight > 0. */
std::vector<glm::dvec3> roundPoints(const glm::dvec3& center, double radius, double halfHeight)
{
    std::vector<glm::dvec3> out;
    constexpr int kRings = 8, kSides = 12;
    constexpr double kPi = 3.14159265358979323846;
    for (int r = 0; r <= kRings; ++r)
    {
        const double phi = kPi * r / kRings;
        const double y = std::cos(phi) * radius, ring = std::sin(phi) * radius;
        const double shift = r < kRings / 2 ? halfHeight : r > kRings / 2 ? -halfHeight : 0.0;
        for (int k = 0; k < (r == 0 || r == kRings ? 1 : kSides); ++k)
        {
            const double t = 2.0 * kPi * k / kSides;
            const glm::dvec3 p(std::cos(t) * ring, y + shift, std::sin(t) * ring);
            out.push_back(center + p);
            if (r == kRings / 2 && halfHeight > 0.0)
            {
                out.push_back(center + p + glm::dvec3(0, halfHeight, 0));
                out.push_back(center + p - glm::dvec3(0, halfHeight, 0));
            }
        }
    }
    return out;
}
} // namespace

std::vector<ConvexMesh> convexMeshes(const Shape& s)
{
    std::vector<ConvexMesh> out;
    auto add = [&](const std::vector<glm::dvec3>& pts) {
        ConvexMesh m = hullMesh(pts);
        if (!m.faces.empty())
            out.push_back(std::move(m));
    };
    switch (s.kind)
    {
    case Shape::Kind::Box:
    {
        std::vector<glm::dvec3> corners;
        for (int k = 0; k < 8; ++k)
            corners.push_back(s.center + s.halfExtents * glm::dvec3(k & 1 ? 1 : -1, k & 2 ? 1 : -1, k & 4 ? 1 : -1));
        add(corners);
        break;
    }
    case Shape::Kind::Hull:
        if (s.points)
            add(*s.points);
        break;
    case Shape::Kind::Compound:
        for (const auto& part : s.parts)
            if (part)
                add(*part);
        break;
    case Shape::Kind::Sphere:
        add(roundPoints(s.center, s.radius, 0.0));
        break;
    case Shape::Kind::Capsule:
        add(roundPoints(s.center, s.radius, s.halfHeight));
        break;
    default:
        break;
    }
    return out;
}

size_t shapeCacheSize() { return gRuntime.ready ? gRuntime.shapes.size() : 0; }

void clearShapeCache()
{
    if (gRuntime.ready)
        gRuntime.shapes.clear();
}
} // namespace lila::physics
