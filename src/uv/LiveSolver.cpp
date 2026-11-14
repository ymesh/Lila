#include "uv/LiveSolver.h"

#include "core/Profile.h"
#include "uv/FinalOperator.h"
#include "uv/OptimizeMath.h"
#include "uv/Unwrap.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>

namespace lila::uv
{
namespace
{
using detail::Jac2;
using detail::SpMat;
using detail::Svd2;
using detail::Triplet;
using detail::svd2;
using detail::compose;
using detail::TriFrame;
using detail::triFrame;

constexpr double kPinWeight = 1e6;    // times the mean diagonal
constexpr double kTargetWeight = 1e3; // soft pins (drags)
/** Relaxing the export, a step stops before it squeezes one of the island's own triangles under this share of its rest area. */
constexpr double kCageFloor = 0.2;

double cross2(glm::dvec2 a, glm::dvec2 b)
{
    return a.x * b.y - a.y * b.x;
}

/** Smallest t in (0, inf) where det(J0 + t Jd) reaches the floor (zero: the triangle flips), or infinity. */
double flipRoot(const Jac2& j0, const Jac2& jd, double floor = 0.0)
{
    double qa = jd.det();
    double qb = j0.a * jd.d + j0.d * jd.a - j0.b * jd.c - j0.c * jd.b;
    double qc = j0.det() - floor;
    double root = std::numeric_limits<double>::infinity();
    double disc = qb * qb - 4.0 * qa * qc;
    if (disc < 0.0)
        return root;
    // Cancellation-free form: both roots from q, so a nearly linear det(t) still gives an accurate root.
    double q = -0.5 * (qb + (qb < 0.0 ? -1.0 : 1.0) * std::sqrt(disc));
    double r1 = qa != 0.0 ? q / qa : std::numeric_limits<double>::infinity();
    double r2 = q != 0.0 ? qc / q : std::numeric_limits<double>::infinity();
    if (r1 > 0.0)
        root = std::min(root, r1);
    if (r2 > 0.0)
        root = std::min(root, r2);
    return root;
}
} // namespace

struct LiveIsland::Factor
{
    SpMat a;
    Eigen::SimplicialLDLT<SpMat> ldlt;
    std::vector<int> triSlot;  // 9 per triangle
    std::vector<int> diagSlot; // per vertex
    std::vector<int> bndSlot;  // 9 per boundary vertex
    double mean = 1.0;         // mean diagonal of the elastic part
    double prox = 0.0;
    double hold = 0.0;         // the cage hold's factor (exported-mesh mode)
    bool ok = false;
    // Exported-mesh mode: the exported corners' stiffness matrix, and the system's factor (complex when weights rotate).
    SpMat s;
    detail::UvFactor uv;
    Eigen::VectorXd so, sov; // s times the exported offsets, u and v
};

std::string_view liveBrushName(LiveBrush b)
{
    switch (b)
    {
    case LiveBrush::Relax: return "relax";
    case LiveBrush::Flatten: return "flatten";
    case LiveBrush::SmoothBoundary: return "smoothBoundary";
    case LiveBrush::Pin: return "pin";
    case LiveBrush::Unpin: return "unpin";
    case LiveBrush::Straighten: return "straighten";
    case LiveBrush::Pinch: return "pinch";
    case LiveBrush::Grab: return "grab";
    case LiveBrush::Stiffen: return "stiffen";
    case LiveBrush::Soften: return "soften";
    }
    return "relax";
}

bool liveBrushFromName(std::string_view name, LiveBrush& out)
{
    for (int i = 0; i <= int(LiveBrush::Soften); ++i)
        if (liveBrushName(LiveBrush(i)) == name)
        {
            out = LiveBrush(i);
            return true;
        }
    return false;
}

double liveFalloff(double t)
{
    if (t >= 1.0)
        return 0.0;
    t = std::max(0.0, t);
    double s = 1.0 - t * t;
    return s * s;
}

LiveIsland::LiveIsland(Chart chart, const LiveOptions& opts, std::vector<float> stiffness)
    : mChart(std::move(chart)), mOpts(opts), mStiff(std::move(stiffness))
{
    prepare();
}

void LiveIsland::prepare()
{
    LILA_ZONE();

    const Index V = mChart.vertexCount();
    const size_t T = mChart.tris.size();
    if (mChart.pinned.size() != V)
        mChart.pinned.assign(V, 0);
    if (mStiff.size() != V)
        mStiff.assign(V, 1.0f);
    for (float& k : mStiff)
        k = k > 0.0f ? std::clamp(k, 0.05f, 20.0f) : 1.0f;

    double a3 = mChart.area3d(), au = mChart.areaUv();
    mSign = au < 0.0 ? -1.0 : 1.0;
    mScale = mOpts.keepScale && a3 > 0.0 && std::abs(au) > 0.0 ? std::sqrt(std::abs(au) / a3) : 1.0;

    mFrames.assign(T, {});
    double restArea = 0.0;
    for (size_t i = 0; i < T; ++i)
    {
        const auto& t = mChart.tris[i];
        glm::dvec3 a = mChart.pos[t[0]] * mScale, b = mChart.pos[t[1]] * mScale, c = mChart.pos[t[2]] * mScale;
        glm::dvec3 e1 = b - a, e2 = c - a;
        double l1 = glm::length(e1);
        if (l1 <= 0.0)
            continue;
        glm::dvec3 x = e1 / l1;
        double px = glm::dot(e2, x);
        double py = glm::length(e2 - px * x);
        double area = 0.5 * l1 * py;
        // Slivers below this carry no shape and only hurt the conditioning.
        if (area <= 1e-14 * l1 * l1)
            continue;
        glm::dvec2 p[3] = {{0.0, 0.0}, {l1, 0.0}, {px, py}};
        Frame& f = mFrames[i];
        f.area = area;
        for (int j = 0; j < 3; ++j)
        {
            glm::dvec2 e = p[(j + 2) % 3] - p[(j + 1) % 3];
            f.g[j] = glm::dvec2(-e.y, e.x) / (2.0 * area);
        }
        restArea += area;
    }
    mLength = std::sqrt(std::max(restArea, 1e-30));

    mX.resize(V);
    for (Index v = 0; v < V; ++v)
        mX[v] = toInternal(mChart.uv[v]);
    mPinAt = mX;
    mTarget.assign(V, glm::dvec2(0.0));
    mTargetW.assign(V, 0.0);
    mTargetCount = 0;

    mBoundary.clear();
    mBoundaryNb.clear();
    mOnBoundary.assign(V, 0);
    for (const std::vector<Index>& loop : mChart.boundaryLoops())
    {
        if (loop.size() < 3)
            continue;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            mBoundary.push_back(loop[i]);
            mBoundaryNb.push_back({loop[(i + loop.size() - 1) % loop.size()], loop[(i + 1) % loop.size()]});
            mOnBoundary[loop[i]] = 1;
        }
    }

    std::vector<uint64_t> edges;
    edges.reserve(T * 3);
    for (const auto& t : mChart.tris)
        for (int i = 0; i < 3; ++i)
        {
            Index a = t[i], b = t[(i + 1) % 3];
            if (a != b)
                edges.push_back((uint64_t(std::min(a, b)) << 32) | std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    mAdjStart.assign(V + 1, 0);
    for (uint64_t e : edges)
    {
        ++mAdjStart[Index(e >> 32) + 1];
        ++mAdjStart[Index(e & 0xffffffffu) + 1];
    }
    for (Index v = 0; v < V; ++v)
        mAdjStart[v + 1] += mAdjStart[v];
    mAdj.assign(mAdjStart[V], 0);
    std::vector<Index> fill(mAdjStart.begin(), mAdjStart.end() - 1);
    for (uint64_t e : edges)
    {
        Index a = Index(e >> 32), b = Index(e & 0xffffffffu);
        mAdj[fill[a]++] = b;
        mAdj[fill[b]++] = a;
    }

    mLo3 = glm::dvec3(std::numeric_limits<double>::max());
    mHi3 = -mLo3;
    for (const glm::dvec3& p : mChart.pos)
    {
        mLo3 = glm::min(mLo3, p);
        mHi3 = glm::max(mHi3, p);
    }

    if (mFinal)
        prepareFinal();
    mFactor.reset();
    mMatrixDirty = true;
    mSettled = true;
    mPolished = true;
    mIters = 0;
}

void LiveIsland::setFinal(std::shared_ptr<const FinalIsland> island)
{
    mFinal = island && !island->tris.empty() ? std::move(island) : nullptr;
    mFinalFrames.clear();
    mFinalFill.reset();
    mFinalOp.reset();
    mCageHold.reset();
    if (mFinal)
        prepareFinal();
    mFactor.reset();
    mMatrixDirty = true;
    touched();
}

void LiveIsland::prepareFinal()
{
    auto fill = std::make_shared<const UninfluencedFill>(mChart, *mFinal);
    if (!fill->empty())
    {
        mFinal = std::make_shared<const FinalIsland>(fill->fold(*mFinal));
        mFinalFill = std::move(fill);
    }
    const FinalIsland& isl = *mFinal;
    mFinalOp = std::make_shared<const detail::FinalOperator>(isl, size_t(vertexCount()), mSign < 0.0);
    mCageHold = std::make_shared<const detail::CageHold>(mChart, isl, mX);
    mFinalFrames.assign(isl.tris.size(), {});
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        const auto& t = isl.tris[i];
        const TriFrame tf = triFrame(isl.pos[t[0]] * mScale, isl.pos[t[1]] * mScale, isl.pos[t[2]] * mScale);
        const double l1 = glm::length(isl.pos[t[1]] - isl.pos[t[0]]) * mScale;
        if (tf.area <= 1e-14 * l1 * l1)
            continue;
        Frame& f = mFinalFrames[i];
        f.area = tf.area;
        for (int j = 0; j < 3; ++j)
            f.g[j] = tf.g[j];
    }
}

std::vector<glm::dvec2> LiveIsland::finalUvs(const std::vector<glm::dvec2>& x, bool offsets) const
{
    std::vector<glm::dvec2> y(mFinalOp->corners());
    mFinalOp->apply(x, y, offsets ? glm::dvec2(mSign, 1.0) : glm::dvec2(0.0));
    return y;
}

void LiveIsland::factorFinal()
{
    LILA_ZONE();

    const FinalIsland& isl = *mFinal;
    const Index V = vertexCount();
    const size_t C = isl.pos.size();
    auto f = std::make_shared<Factor>();
    std::vector<Triplet> trips;
    std::vector<double> cornerStiff(C, 1.0);
    for (size_t c = 0; c < C; ++c)
    {
        double w = 0.0, k = 0.0;
        for (Index e = isl.rowStart[c]; e < isl.rowStart[c + 1]; ++e)
        {
            w += std::abs(isl.weights[e]);
            k += std::abs(isl.weights[e]) * mStiff[isl.cols[e]];
        }
        if (w > 0.0)
            cornerStiff[c] = k / w;
    }
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        const Frame& fr = mFinalFrames[i];
        if (fr.area <= 0.0)
            continue;
        const auto& t = isl.tris[i];
        const double w = fr.area * (cornerStiff[t[0]] + cornerStiff[t[1]] + cornerStiff[t[2]]) / 3.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                trips.emplace_back(int(t[r]), int(t[c]), w * glm::dot(fr.g[r], fr.g[c]));
    }
    f->s.resize(Eigen::Index(C), Eigen::Index(C));
    f->s.setFromTriplets(trips.begin(), trips.end());
    {
        const std::vector<glm::dvec2> o = finalUvs(std::vector<glm::dvec2>(mX.size(), glm::dvec2(0.0)), true);
        Eigen::VectorXd ou(static_cast<Eigen::Index>(C)), ov(static_cast<Eigen::Index>(C));
        for (size_t c = 0; c < C; ++c)
        {
            ou[Eigen::Index(c)] = o[c].x;
            ov[Eigen::Index(c)] = o[c].y;
        }
        f->so = f->s * ou;
        f->sov = f->s * ov;
    }
    const auto [k, ki] = mFinalOp->gram(f->s);
    double trace = 0.0;
    for (Index v = 0; v < V; ++v)
        trace += k.coeff(v, v);
    f->mean = V > 0 && trace > 0.0 ? trace / double(V) : 1.0;
    f->prox = 1e-7 * f->mean;
    trips.clear();
    if (mCageHold)
    {
        f->hold = mCageHold->scaleFor(detail::kCageHold, f->mean);
        for (const Triplet& t : mCageHold->laplacian)
            trips.emplace_back(t.row(), t.col(), f->hold * t.value());
    }
    for (Index v = 0; v < V; ++v)
    {
        double d = f->prox;
        if (mChart.pinned[v])
            d += kPinWeight * f->mean;
        else if (mTargetW[v] > 0.0)
            d += kTargetWeight * f->mean * mTargetW[v];
        trips.emplace_back(int(v), int(v), d);
    }
    if (mOpts.boundarySmooth > 0.0)
    {
        const double w = mOpts.boundarySmooth * f->mean;
        const double c[3] = {1.0, -2.0, 1.0};
        for (size_t i = 0; i < mBoundary.size(); ++i)
        {
            const Index ids[3] = {mBoundaryNb[i][0], mBoundary[i], mBoundaryNb[i][1]};
            for (int r = 0; r < 3; ++r)
                for (int q = 0; q < 3; ++q)
                    trips.emplace_back(int(ids[r]), int(ids[q]), w * c[r] * c[q]);
        }
    }
    SpMat d(V, V);
    d.setFromTriplets(trips.begin(), trips.end());
    f->a = k + d;
    f->ok = f->uv.compute(f->a, ki);
    mFactor = std::move(f);
    ++mFactorizations;
    mMatrixDirty = false;
}

double LiveIsland::finalStep(std::vector<double>& bu, std::vector<double>& bv) const
{
    const FinalIsland& isl = *mFinal;
    const Factor& f = *mFactor;
    const size_t T = isl.tris.size(), C = isl.pos.size();
    const std::vector<glm::dvec2> y = finalUvs(mX, true);
    const double gs = std::max(0.0, mOpts.balance) * 0.95, gc = std::max(0.0, -mOpts.balance) * 0.95;
    std::vector<double> local(T * 6, 0.0), triEnergy(T, 0.0);
    tbb::parallel_for(size_t(0), T, [&](size_t i) {
        const Frame& fr = mFinalFrames[i];
        if (fr.area <= 0.0)
            return;
        const auto& t = isl.tris[i];
        glm::dvec2 gu(0.0), gv(0.0);
        for (int j = 0; j < 3; ++j)
        {
            gu += y[t[j]].x * fr.g[j];
            gv += y[t[j]].y * fr.g[j];
        }
        const Jac2 jm{gu.x, gu.y, gv.x, gv.y};
        const Svd2 sv = svd2(jm);
        double sig[2] = {sv.s1, sv.s2}, tau[2], e = 0.0;
        for (int k = 0; k < 2; ++k)
        {
            const double give = sig[k] > 1.0 ? gs : gc;
            tau[k] = std::max(0.05, 1.0 + give * (sig[k] - 1.0));
            if (give > 0.0)
                e += (1.0 / give - 1.0) * (tau[k] - 1.0) * (tau[k] - 1.0);
        }
        const Jac2 tm = compose(sv, tau[0], tau[1]);
        double k = 0.0, w = 0.0;
        for (int j = 0; j < 3; ++j)
            for (Index q = isl.rowStart[t[j]]; q < isl.rowStart[t[j] + 1]; ++q)
            {
                k += std::abs(isl.weights[q]) * mStiff[isl.cols[q]];
                w += std::abs(isl.weights[q]);
            }
        k = fr.area * (w > 0.0 ? k / w : 1.0);
        for (int j = 0; j < 3; ++j)
        {
            local[i * 6 + j] = k * (fr.g[j].x * tm.a + fr.g[j].y * tm.b);
            local[i * 6 + 3 + j] = k * (fr.g[j].x * tm.c + fr.g[j].y * tm.d);
        }
        const double d0 = jm.a - tm.a, d1 = jm.b - tm.b, d2 = jm.c - tm.c, d3 = jm.d - tm.d;
        triEnergy[i] = k * (d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3 + e);
    });
    const auto nC = Eigen::Index(C);
    Eigen::VectorXd eu = Eigen::VectorXd::Zero(nC), ev = Eigen::VectorXd::Zero(nC);
    double energy = 0.0;
    for (size_t i = 0; i < T; ++i)
    {
        const auto& t = isl.tris[i];
        for (int j = 0; j < 3; ++j)
        {
            eu[t[j]] += local[i * 6 + j];
            ev[t[j]] += local[i * 6 + 3 + j];
        }
        energy += triEnergy[i];
    }
    const Eigen::VectorXd gu = eu - f.so, gv = ev - f.sov;
    std::vector<glm::dvec2> g(C), r(mX.size());
    for (size_t c = 0; c < C; ++c)
        g[c] = glm::dvec2(gu[Eigen::Index(c)], gv[Eigen::Index(c)]);
    mFinalOp->applyTransposed(g, r);
    bu.resize(r.size());
    bv.resize(r.size());
    for (size_t v = 0; v < r.size(); ++v)
    {
        bu[v] = r[v].x;
        bv[v] = r[v].y;
    }
    if (mCageHold && f.hold > 0.0)
    {
        for (size_t v = 0; v < r.size(); ++v)
        {
            bu[v] += f.hold * mCageHold->rest[v].x;
            bv[v] += f.hold * mCageHold->rest[v].y;
        }
        energy += f.hold * mCageHold->energy(mX);
    }
    return energy;
}


void LiveIsland::setOptions(const LiveOptions& opts)
{
    bool rescale = opts.keepScale != mOpts.keepScale;
    if (opts.boundarySmooth != mOpts.boundarySmooth)
        markDirty();
    mOpts = opts;
    if (rescale)
    {
        // Rebuild the rest shape at the new scale, keeping uvs, pins, targets and stiffness.
        std::vector<glm::dvec2> pinAt = mPinAt, target = mTarget;
        std::vector<double> targetW = mTargetW;
        size_t count = mTargetCount;
        prepare();
        mPinAt = pinAt;
        mTarget = target;
        mTargetW = targetW;
        mTargetCount = count;
    }
}

void LiveIsland::touched()
{
    ++mVersion;
}

void LiveIsland::pushToChart()
{
    for (Index v = 0; v < mChart.vertexCount(); ++v)
        mChart.uv[v] = glm::dvec2(mX[v].x * mSign, mX[v].y);
    touched();
}

void LiveIsland::setPinned(Index v, bool on)
{
    if (v >= vertexCount() || (mChart.pinned[v] != 0) == on)
        return;
    mChart.pinned[v] = on ? 1 : 0;
    if (on)
        mPinAt[v] = mX[v];
    markDirty();
    touched();
}

void LiveIsland::setStiffness(Index v, double k)
{
    if (v >= vertexCount())
        return;
    float kk = float(std::clamp(k, 0.05, 20.0));
    if (kk == mStiff[v])
        return;
    mStiff[v] = kk;
    markDirty();
    touched();
}

void LiveIsland::setTarget(Index v, glm::dvec2 at, double weight)
{
    if (v >= vertexCount())
        return;
    weight = std::max(0.0, weight);
    if (weight != mTargetW[v])
    {
        mTargetCount += (weight > 0.0) - (mTargetW[v] > 0.0);
        mTargetW[v] = weight;
        markDirty();
    }
    mTarget[v] = toInternal(at);
}

void LiveIsland::clearTargets()
{
    if (mTargetCount == 0)
        return;
    std::fill(mTargetW.begin(), mTargetW.end(), 0.0);
    mTargetCount = 0;
    markDirty();
}

void LiveIsland::setUv(Index v, glm::dvec2 uv)
{
    if (v >= vertexCount())
        return;
    mX[v] = toInternal(uv);
    if (mChart.pinned[v])
        mPinAt[v] = mX[v];
    mChart.uv[v] = uv;
    touched();
}

void LiveIsland::wake()
{
    mSettled = false;
    mPolished = false;
    mIters = 0;
    mHold = centroid();
    mHoldValid = true;
}

glm::dvec2 LiveIsland::centroid() const
{
    glm::dvec2 c(0.0);
    for (const glm::dvec2& p : mX)
        c += p;
    return mX.empty() ? c : c / double(mX.size());
}

void LiveIsland::restoreCentroid(glm::dvec2 target)
{
    glm::dvec2 d = target - centroid();
    for (glm::dvec2& p : mX)
        p += d;
}

void LiveIsland::alignTo(const std::vector<glm::dvec2>& before)
{
    if (before.size() != mX.size() || mX.empty())
        return;
    glm::dvec2 cb(0.0), cx = centroid();
    for (const glm::dvec2& p : before)
        cb += p;
    cb /= double(before.size());
    double sd = 0.0, sc = 0.0;
    for (size_t i = 0; i < mX.size(); ++i)
    {
        glm::dvec2 p = mX[i] - cx, q = before[i] - cb;
        sd += glm::dot(p, q);
        sc += cross2(p, q);
    }
    double ang = std::atan2(sc, sd), c = std::cos(ang), s = std::sin(ang);
    for (glm::dvec2& p : mX)
    {
        glm::dvec2 d = p - cx;
        p = cb + glm::dvec2(d.x * c - d.y * s, d.x * s + d.y * c);
    }
}

void LiveIsland::buildPattern()
{
    LILA_ZONE();

    const Index V = vertexCount();
    auto f = std::make_shared<Factor>();
    std::vector<Triplet> trips;
    trips.reserve(mChart.tris.size() * 9 + V + mBoundary.size() * 9);
    for (const auto& t : mChart.tris)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                trips.emplace_back(int(t[r]), int(t[c]), 0.0);
    for (Index v = 0; v < V; ++v)
        trips.emplace_back(int(v), int(v), 0.0);
    for (size_t i = 0; i < mBoundary.size(); ++i)
    {
        Index ids[3] = {mBoundaryNb[i][0], mBoundary[i], mBoundaryNb[i][1]};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                trips.emplace_back(int(ids[r]), int(ids[c]), 0.0);
    }
    f->a.resize(V, V);
    f->a.setFromTriplets(trips.begin(), trips.end());
    f->a.makeCompressed();
    auto slot = [&](Index row, Index col) {
        const int* inner = f->a.innerIndexPtr();
        int lo = f->a.outerIndexPtr()[col], hi = f->a.outerIndexPtr()[col + 1];
        return int(std::lower_bound(inner + lo, inner + hi, int(row)) - inner);
    };
    f->triSlot.resize(mChart.tris.size() * 9);
    tbb::parallel_for(size_t(0), mChart.tris.size(), [&](size_t i) {
        const auto& t = mChart.tris[i];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                f->triSlot[i * 9 + r * 3 + c] = slot(t[r], t[c]);
    });
    f->diagSlot.resize(V);
    for (Index v = 0; v < V; ++v)
        f->diagSlot[v] = slot(v, v);
    f->bndSlot.resize(mBoundary.size() * 9);
    for (size_t i = 0; i < mBoundary.size(); ++i)
    {
        Index ids[3] = {mBoundaryNb[i][0], mBoundary[i], mBoundaryNb[i][1]};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                f->bndSlot[i * 9 + r * 3 + c] = slot(ids[r], ids[c]);
    }
    f->ldlt.analyzePattern(f->a);
    mFactor = std::move(f);
}

void LiveIsland::ensureFactor()
{
    if (mFinal)
    {
        if (!mFactor || mMatrixDirty)
            factorFinal();
        return;
    }
    if (!mFactor)
    {
        buildPattern();
        mMatrixDirty = true;
    }
    if (!mMatrixDirty)
        return;
    LILA_ZONE();

    Factor& f = *mFactor;
    const Index V = vertexCount();
    double* val = f.a.valuePtr();
    std::fill(val, val + f.a.nonZeros(), 0.0);
    for (size_t i = 0; i < mChart.tris.size(); ++i)
    {
        const Frame& fr = mFrames[i];
        if (fr.area <= 0.0)
            continue;
        const auto& t = mChart.tris[i];
        double w = fr.area * (double(mStiff[t[0]]) + mStiff[t[1]] + mStiff[t[2]]) / 3.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                val[f.triSlot[i * 9 + r * 3 + c]] += w * glm::dot(fr.g[r], fr.g[c]);
    }
    double trace = 0.0;
    for (Index v = 0; v < V; ++v)
        trace += val[f.diagSlot[v]];
    f.mean = V > 0 && trace > 0.0 ? trace / double(V) : 1.0;
    // Proximal term: removes the translation null space of a free island without biasing its shape.
    f.prox = 1e-7 * f.mean;
    for (Index v = 0; v < V; ++v)
    {
        double d = f.prox;
        if (mChart.pinned[v])
            d += kPinWeight * f.mean;
        else if (mTargetW[v] > 0.0)
            d += kTargetWeight * f.mean * mTargetW[v];
        val[f.diagSlot[v]] += d;
    }
    if (mOpts.boundarySmooth > 0.0)
    {
        const double w = mOpts.boundarySmooth * f.mean;
        const double c[3] = {1.0, -2.0, 1.0};
        for (size_t i = 0; i < mBoundary.size(); ++i)
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k)
                    val[f.bndSlot[i * 9 + r * 3 + k]] += w * c[r] * c[k];
    }
    f.ldlt.factorize(f.a);
    f.ok = f.ldlt.info() == Eigen::Success;
    ++mFactorizations;
    mMatrixDirty = false;
}

double LiveIsland::maxStep(const std::vector<glm::dvec2>& dir) const
{
    double t = 1.0;
    if (mFinal)
    {
        const std::vector<glm::dvec2> y = finalUvs(mX, true), dy = finalUvs(dir, false);
        for (size_t i = 0; i < mFinal->tris.size(); ++i)
        {
            const Frame& f = mFinalFrames[i];
            if (f.area <= 0.0)
                continue;
            const auto& tr = mFinal->tris[i];
            glm::dvec2 gu(0.0), gv(0.0), du(0.0), dv(0.0);
            for (int j = 0; j < 3; ++j)
            {
                gu += y[tr[j]].x * f.g[j];
                gv += y[tr[j]].y * f.g[j];
                du += dy[tr[j]].x * f.g[j];
                dv += dy[tr[j]].y * f.g[j];
            }
            const Jac2 j0{gu.x, gu.y, gv.x, gv.y}, jd{du.x, du.y, dv.x, dv.y};
            if (j0.det() <= 0.0)
                continue;
            t = std::min(t, 0.9 * flipRoot(j0, jd));
        }
    }
    // The island's own triangles too when the export is what relaxes: its layout is what the UV Editor shows and edits.
    for (size_t i = 0; i < mChart.tris.size(); ++i)
    {
        const Frame& f = mFrames[i];
        if (f.area <= 0.0)
            continue;
        const auto& tr = mChart.tris[i];
        glm::dvec2 gu(0.0), gv(0.0), du(0.0), dv(0.0);
        for (int j = 0; j < 3; ++j)
        {
            gu += mX[tr[j]].x * f.g[j];
            gv += mX[tr[j]].y * f.g[j];
            du += dir[tr[j]].x * f.g[j];
            dv += dir[tr[j]].y * f.g[j];
        }
        Jac2 j0{gu.x, gu.y, gv.x, gv.y}, jd{du.x, du.y, dv.x, dv.y};
        const double det = j0.det();
        if (det <= 0.0)
            continue;
        t = std::min(t, 0.9 * flipRoot(j0, jd, mFinal ? std::min(kCageFloor, 0.5 * det) : 0.0));
    }
    return std::max(0.0, t);
}

void LiveIsland::step(std::span<const double> weights)
{
    LILA_ZONE();

    const Index V = vertexCount();
    const size_t T = mChart.tris.size();
    if (V < 3 || T == 0)
    {
        mSettled = true;
        return;
    }
    ensureFactor();
    if (!mFactor->ok)
    {
        mSettled = true;
        return;
    }
    const Factor& f = *mFactor;
    Eigen::VectorXd bu = Eigen::VectorXd::Zero(V), bv = Eigen::VectorXd::Zero(V);
    double energy = 0.0;
    if (mFinal)
    {
        std::vector<double> fu, fv;
        energy = finalStep(fu, fv);
        for (Index v = 0; v < V; ++v)
        {
            bu[v] = fu[v];
            bv[v] = fv[v];
        }
    }
    else
    {
    // Local step: each triangle's target is its nearest rotation, with the singular values only partly pulled back to 1
    // on the side the balance tolerates.
    const double gs = std::max(0.0, mOpts.balance) * 0.95, gc = std::max(0.0, -mOpts.balance) * 0.95;
    std::vector<double> local(T * 6, 0.0);
    std::vector<double> triEnergy(T, 0.0);
    tbb::parallel_for(size_t(0), T, [&](size_t i) {
        const Frame& fr = mFrames[i];
        if (fr.area <= 0.0)
            return;
        const auto& t = mChart.tris[i];
        glm::dvec2 gu(0.0), gv(0.0);
        for (int j = 0; j < 3; ++j)
        {
            gu += mX[t[j]].x * fr.g[j];
            gv += mX[t[j]].y * fr.g[j];
        }
        Jac2 jm{gu.x, gu.y, gv.x, gv.y};
        Svd2 s = svd2(jm);
        double sig[2] = {s.s1, s.s2}, tau[2], e = 0.0;
        for (int k = 0; k < 2; ++k)
        {
            double give = sig[k] > 1.0 ? gs : gc;
            tau[k] = std::max(0.05, 1.0 + give * (sig[k] - 1.0));
            if (give > 0.0)
                e += (1.0 / give - 1.0) * (tau[k] - 1.0) * (tau[k] - 1.0);
        }
        double cp = std::cos(s.phi), sp = std::sin(s.phi), ct = std::cos(s.theta), st = std::sin(s.theta);
        // T = Rot(phi) diag(tau) Rot(theta)
        double m00 = cp * tau[0], m01 = -sp * tau[1], m10 = sp * tau[0], m11 = cp * tau[1];
        double t00 = m00 * ct + m01 * st, t01 = -m00 * st + m01 * ct;
        double t10 = m10 * ct + m11 * st, t11 = -m10 * st + m11 * ct;
        double k = fr.area * (double(mStiff[t[0]]) + mStiff[t[1]] + mStiff[t[2]]) / 3.0;
        for (int j = 0; j < 3; ++j)
        {
            local[i * 6 + j] = k * (fr.g[j].x * t00 + fr.g[j].y * t01);
            local[i * 6 + 3 + j] = k * (fr.g[j].x * t10 + fr.g[j].y * t11);
        }
        double d0 = jm.a - t00, d1 = jm.b - t01, d2 = jm.c - t10, d3 = jm.d - t11;
        triEnergy[i] = k * (d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3 + e);
    });

    for (size_t i = 0; i < T; ++i)
    {
        const auto& t = mChart.tris[i];
        for (int j = 0; j < 3; ++j)
        {
            bu[t[j]] += local[i * 6 + j];
            bv[t[j]] += local[i * 6 + 3 + j];
        }
        energy += triEnergy[i];
    }
    }
    mEnergy = energy / (mLength * mLength);
    for (Index v = 0; v < V; ++v)
    {
        glm::dvec2 r = f.prox * mX[v];
        if (mChart.pinned[v])
            r += kPinWeight * f.mean * mPinAt[v];
        else if (mTargetW[v] > 0.0)
            r += kTargetWeight * f.mean * mTargetW[v] * mTarget[v];
        bu[v] += r.x;
        bv[v] += r.y;
    }

    // Global step.
    Eigen::VectorXd su, sv;
    bool solved = false;
    if (mFinal)
        solved = f.uv.solve(bu, bv, su, sv);
    else
    {
        su = f.ldlt.solve(bu);
        sv = f.ldlt.solve(bv);
        solved = su.allFinite() && sv.allFinite();
    }
    if (!solved)
    {
        mSettled = true;
        return;
    }
    std::vector<glm::dvec2> dir(V);
    for (Index v = 0; v < V; ++v)
    {
        dir[v] = mChart.pinned[v] ? glm::dvec2(0.0) : glm::dvec2(su[v], sv[v]) - mX[v];
        if (!weights.empty())
            dir[v] *= v < weights.size() ? std::clamp(weights[v], 0.0, 1.0) : 0.0;
    }
    double t = mOpts.injective ? maxStep(dir) : 1.0;
    double move = 0.0;
    for (Index v = 0; v < V; ++v)
    {
        glm::dvec2 d = t * dir[v];
        mX[v] += d;
        move = std::max(move, std::abs(d.x) + std::abs(d.y));
    }
    for (Index v = 0; v < V; ++v)
        if (mChart.pinned[v])
            mX[v] = mPinAt[v];
    if (mFinalFill)
        mFinalFill->apply(mX);
    bool anchored = mTargetCount > 0;
    for (uint8_t p : mChart.pinned)
        anchored |= p != 0;
    if (!anchored && weights.empty() && mHoldValid)
        restoreCentroid(mHold);
    pushToChart();

    if (!weights.empty())
        return;
    ++mIters;
    if (move < mOpts.tolerance * mLength || t < 1e-9 || mIters >= mOpts.maxIterations)
    {
        mSettled = true;
        if (!mPolished && mOpts.slimSteps > 0 && mTargetCount == 0 && !mFinal)
            slimPolish();
        mPolished = true;
    }
}

int LiveIsland::solve(int maxIterations)
{
    if (maxIterations < 0)
        maxIterations = mOpts.maxIterations;
    wake();
    int n = 0;
    while (!mSettled && n < maxIterations)
    {
        step();
        ++n;
    }
    return n;
}

void LiveIsland::slimPolish()
{
    LILA_ZONE();

    Chart c = mChart;
    for (Index v = 0; v < c.vertexCount(); ++v)
        c.uv[v] = mX[v];
    bool pinned = std::any_of(c.pinned.begin(), c.pinned.end(), [](uint8_t p) { return p != 0; });
    if (slim(c, mOpts.slimSteps, true) < 0)
        return;
    std::vector<glm::dvec2> before = mX;
    for (Index v = 0; v < c.vertexCount(); ++v)
        mX[v] = c.uv[v] * (pinned ? 1.0 : mScale);
    if (!pinned)
        alignTo(before);
    pushToChart();
}

bool LiveIsland::flatten()
{
    LILA_ZONE();

    Chart c = mChart;
    for (Index v = 0; v < c.vertexCount(); ++v)
        c.uv[v] = mX[v];
    size_t pins = 0;
    Index onePin = kInvalid;
    for (Index v = 0; v < c.vertexCount(); ++v)
        if (c.pinned[v])
        {
            ++pins;
            onePin = v;
        }
    UnwrapOptions o;
    o.iterations = 20;
    if (!unwrapChart(c, o).ok)
        return false;
    std::vector<glm::dvec2> before = mX;
    for (Index v = 0; v < c.vertexCount(); ++v)
        mX[v] = pins >= 2 ? c.uv[v] : c.uv[v] * mScale;
    if (pins < 2)
    {
        alignTo(before);
        if (pins == 1)
        {
            glm::dvec2 d = mPinAt[onePin] - mX[onePin];
            for (glm::dvec2& p : mX)
                p += d;
        }
    }
    for (Index v = 0; v < vertexCount(); ++v)
        if (mChart.pinned[v])
            mX[v] = mPinAt[v];
    pushToChart();
    return true;
}

size_t LiveIsland::folds() const
{
    size_t n = 0;
    for (size_t i = 0; i < mChart.tris.size(); ++i)
    {
        if (mFrames[i].area <= 0.0)
            continue;
        const auto& t = mChart.tris[i];
        if (cross2(mX[t[1]] - mX[t[0]], mX[t[2]] - mX[t[0]]) <= 0.0)
            ++n;
    }
    return n;
}

std::vector<double> LiveIsland::dabWeights(const LiveDab& dab, glm::dvec2& centerUv, glm::dvec2& fromUv) const
{
    const Index V = vertexCount();
    std::vector<double> w;
    if (dab.radius <= 0.0 || V == 0)
        return w;
    if (dab.surface)
    {
        glm::dvec3 r(dab.radius);
        if (glm::any(glm::lessThan(dab.center3 + r, mLo3)) || glm::any(glm::greaterThan(dab.center3 - r, mHi3)))
            return w;
        w.assign(V, 0.0);
        double best = std::numeric_limits<double>::max(), bestFrom = best;
        bool any = false;
        for (Index v = 0; v < V; ++v)
        {
            double d = glm::length(mChart.pos[v] - dab.center3);
            if (d < best)
            {
                best = d;
                centerUv = mX[v];
            }
            double df = glm::length(mChart.pos[v] - dab.lineFrom3);
            if (df < bestFrom)
            {
                bestFrom = df;
                fromUv = mX[v];
            }
            if (d < dab.radius)
            {
                w[v] = dab.strength * liveFalloff(d / dab.radius);
                any = true;
            }
        }
        if (!any)
            w.clear();
        return w;
    }
    centerUv = toInternal(dab.center);
    fromUv = toInternal(dab.lineFrom);
    glm::dvec2 lo = mX.front(), hi = lo;
    for (const glm::dvec2& p : mX)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    if (centerUv.x + dab.radius < lo.x || centerUv.y + dab.radius < lo.y || centerUv.x - dab.radius > hi.x || centerUv.y - dab.radius > hi.y)
        return w;
    w.assign(V, 0.0);
    bool any = false;
    for (Index v = 0; v < V; ++v)
    {
        double d = glm::length(mX[v] - centerUv);
        if (d < dab.radius)
        {
            w[v] = dab.strength * liveFalloff(d / dab.radius);
            any = true;
        }
    }
    if (!any)
        w.clear();
    return w;
}

bool LiveIsland::applyDab(const LiveDab& dab)
{
    LILA_ZONE();

    glm::dvec2 c(0.0), from(0.0);
    std::vector<double> w = dabWeights(dab, c, from);
    const Index V = vertexCount();
    if (dab.first)
    {
        mStrokeFrom = dab.surface ? from : toInternal(dab.lineFrom);
        if (dab.brush == LiveBrush::Grab)
            clearTargets();
    }
    if (w.empty())
        return false;

    // Moves one vertex unless that would fold one of its triangles.
    std::vector<Index> triStart, triOf;
    auto moveSafely = [&](Index v, glm::dvec2 to) {
        if (mChart.pinned[v])
            return false;
        if (triStart.empty())
        {
            triStart.assign(V + 1, 0);
            for (const auto& t : mChart.tris)
                for (Index k : t)
                    ++triStart[k + 1];
            for (Index i = 0; i < V; ++i)
                triStart[i + 1] += triStart[i];
            triOf.assign(triStart[V], 0);
            std::vector<Index> fill(triStart.begin(), triStart.end() - 1);
            for (Index i = 0; i < mChart.tris.size(); ++i)
                for (Index k : mChart.tris[i])
                    triOf[fill[k]++] = i;
        }
        glm::dvec2 old = mX[v];
        for (Index k = triStart[v]; k < triStart[v + 1]; ++k)
        {
            const auto& t = mChart.tris[triOf[k]];
            if (mFrames[triOf[k]].area <= 0.0)
                continue;
            double before = cross2(mX[t[1]] - mX[t[0]], mX[t[2]] - mX[t[0]]);
            mX[v] = to;
            double after = cross2(mX[t[1]] - mX[t[0]], mX[t[2]] - mX[t[0]]);
            mX[v] = old;
            if (before > 0.0 && after <= 0.0)
                return false;
        }
        mX[v] = to;
        return true;
    };

    LiveBrush b = dab.brush;
    if (dab.invert)
    {
        if (b == LiveBrush::Pin)
            b = LiveBrush::Unpin;
        else if (b == LiveBrush::Unpin)
            b = LiveBrush::Pin;
        else if (b == LiveBrush::Stiffen)
            b = LiveBrush::Soften;
        else if (b == LiveBrush::Soften)
            b = LiveBrush::Stiffen;
    }
    switch (b)
    {
    case LiveBrush::Pin:
    case LiveBrush::Unpin:
        for (Index v = 0; v < V; ++v)
            if (w[v] > 0.0)
                setPinned(v, b == LiveBrush::Pin);
        break;
    case LiveBrush::Stiffen:
    case LiveBrush::Soften:
        for (Index v = 0; v < V; ++v)
            if (w[v] > 0.0)
                setStiffness(v, mStiff[v] * std::exp((b == LiveBrush::Stiffen ? 0.35 : -0.35) * w[v]));
        break;
    case LiveBrush::Relax: step(w); break;
    case LiveBrush::Flatten:
    {
        for (double& x : w)
            x = x > 0.0 ? 1.0 : 0.0;
        double balance = mOpts.balance;
        mOpts.balance = 0.0;
        int n = 2 + int(std::lround(dab.strength * 6.0));
        for (int i = 0; i < n; ++i)
            step(w);
        mOpts.balance = balance;
        break;
    }
    case LiveBrush::SmoothBoundary:
        for (size_t i = 0; i < mBoundary.size(); ++i)
        {
            Index v = mBoundary[i];
            if (w[v] > 0.0)
            {
                glm::dvec2 mid = (mX[mBoundaryNb[i][0]] + mX[mBoundaryNb[i][1]]) * 0.5;
                moveSafely(v, mX[v] + (mid - mX[v]) * (0.5 * w[v]));
            }
        }
        break;
    case LiveBrush::Straighten:
    {
        glm::dvec2 d = c - mStrokeFrom;
        double len = glm::length(d);
        if (len <= 1e-12)
            break;
        d /= len;
        for (Index v = 0; v < V; ++v)
            if (w[v] > 0.0)
            {
                glm::dvec2 p = mStrokeFrom + d * glm::dot(mX[v] - mStrokeFrom, d);
                moveSafely(v, mX[v] + (p - mX[v]) * (0.5 * w[v]));
            }
        break;
    }
    case LiveBrush::Pinch:
        for (Index v = 0; v < V; ++v)
            if (w[v] > 0.0)
                moveSafely(v, mX[v] + (c - mX[v]) * ((dab.invert ? -0.15 : 0.15) * w[v]));
        break;
    case LiveBrush::Grab:
    {
        glm::dvec2 delta = toInternal(dab.delta);
        if (dab.first)
        {
            for (Index v = 0; v < V; ++v)
                if (w[v] > 0.02 && !mChart.pinned[v])
                {
                    mTargetCount += mTargetW[v] <= 0.0;
                    mTargetW[v] = w[v] / std::max(dab.strength, 1e-9);
                    mTarget[v] = mX[v];
                }
            markDirty();
        }
        for (Index v = 0; v < V; ++v)
            if (mTargetW[v] > 0.0)
            {
                mTarget[v] += delta * mTargetW[v];
                moveSafely(v, mX[v] + delta * mTargetW[v]);
            }
        break;
    }
    }
    pushToChart();
    return true;
}

std::vector<uint8_t> seamFlags(const geo::Mesh& mesh)
{
    std::vector<uint8_t> out(mesh.edgeCapacity(), 0);
    std::span<const bool> seam = mesh.edgeAttrs().get<bool>(geo::attr::kSeam);
    for (size_t e = 0; e < seam.size() && e < out.size(); ++e)
        out[e] = seam[e] ? 1 : 0;
    return out;
}

std::vector<LiveIsland> liveIslands(const geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const LiveOptions& opts)
{
    LILA_ZONE();

    std::vector<uint8_t> seams = seamFlags(mesh);
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    co.extraSeams = seams;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    std::span<const float> stiff = mesh.cornerAttrs().get<float>("uvStiffness");
    std::vector<LiveIsland> out(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), [&](size_t i) {
        Chart& ch = charts[i];
        std::vector<float> k(ch.vertexCount(), 1.0f);
        if (!stiff.empty())
            for (size_t j = 0; j < ch.corners.size(); ++j)
                if (float s = stiff[ch.corners[j]]; s > 0.0f)
                    k[ch.cornerVertex[j]] = s;
        out[i] = LiveIsland(std::move(ch), opts, std::move(k));
    });
    return out;
}

FinalMap attachFinal(std::span<LiveIsland> islands, const geo::Mesh& cage, std::string_view uvSet, const Stack& stack)
{
    LILA_ZONE();

    FinalMap map = buildFinalMap(cage, uvSet, stack);
    if (!map.active())
        return map;
    std::vector<Chart> charts;
    charts.reserve(islands.size());
    for (const LiveIsland& is : islands)
        charts.push_back(is.chart());
    std::vector<FinalIsland> final = finalIslands(map, cage, charts, uvSet);
    for (size_t i = 0; i < islands.size(); ++i)
        islands[i].setFinal(std::make_shared<const FinalIsland>(std::move(final[i])));
    return map;
}

void writeLiveIslands(geo::Mesh& mesh, std::span<const LiveIsland> islands, std::string_view uvSet, bool pinsAndStiffness)
{
    LILA_ZONE();

    std::span<glm::vec2> uv = mesh.cornerAttrs().getOrAdd<glm::vec2>(uvSet);
    bool anyPin = false, anyStiff = false;
    for (const LiveIsland& is : islands)
    {
        const Chart& ch = is.chart();
        for (size_t j = 0; j < ch.corners.size(); ++j)
            uv[ch.corners[j]] = glm::vec2(ch.uv[ch.cornerVertex[j]]);
        for (uint8_t p : ch.pinned)
            anyPin |= p != 0;
        for (float k : is.stiffness())
            anyStiff |= k != 1.0f;
    }
    if (!pinsAndStiffness)
        return;
    if (anyPin || !mesh.cornerAttrs().get<bool>("uvPin").empty())
    {
        std::span<bool> pin = mesh.cornerAttrs().getOrAdd<bool>("uvPin");
        for (const LiveIsland& is : islands)
            for (size_t j = 0; j < is.chart().corners.size(); ++j)
                pin[is.chart().corners[j]] = is.chart().pinned[is.chart().cornerVertex[j]] != 0;
    }
    if (anyStiff || !mesh.cornerAttrs().get<float>("uvStiffness").empty())
    {
        std::span<float> st = mesh.cornerAttrs().getOrAdd<float>("uvStiffness");
        for (const LiveIsland& is : islands)
            for (size_t j = 0; j < is.chart().corners.size(); ++j)
                st[is.chart().corners[j]] = is.stiffness()[is.chart().cornerVertex[j]];
    }
}

// ---------------------------------------------------------------------------------------------------------------------

LiveSolver::LiveSolver()
{
    mFront = std::make_shared<Frame>();
    mThread = std::thread([this] { run(); });
}

LiveSolver::~LiveSolver()
{
    {
        std::lock_guard lk(mMutex);
        mStop = true;
    }
    mWake.notify_all();
    if (mThread.joinable())
        mThread.join();
}

void LiveSolver::load(std::vector<LiveIsland> islands)
{
    auto shared = std::make_shared<std::vector<LiveIsland>>(std::move(islands));
    edit([this, shared](std::vector<LiveIsland>& is) {
        is = std::move(*shared);
        mIterations = 0;
        ++mEpoch;
    });
}

void LiveSolver::edit(Edit fn)
{
    {
        std::lock_guard lk(mMutex);
        mQueue.push_back(std::move(fn));
    }
    mWake.notify_all();
}

void LiveSolver::sync(const Edit& fn)
{
    auto ticket = std::make_shared<std::atomic<uint64_t>>(0);
    edit([this, fn, ticket](std::vector<LiveIsland>& is) {
        if (fn)
            fn(is);
        ticket->store(mPass);
    });
    std::unique_lock lk(mMutex);
    mIdleCv.wait(lk, [&] {
        uint64_t t = ticket->load();
        return mStop || (t != 0 && mFinished >= t);
    });
}

void LiveSolver::setPaused(bool on)
{
    {
        std::lock_guard lk(mMutex);
        mPaused = on;
    }
    mWake.notify_all();
    mIdleCv.notify_all();
}

bool LiveSolver::paused() const
{
    std::lock_guard lk(mMutex);
    return mPaused;
}

std::shared_ptr<const LiveSolver::Frame> LiveSolver::frame() const
{
    std::lock_guard lk(mMutex);
    return mFront;
}

uint64_t LiveSolver::generation() const
{
    std::lock_guard lk(mMutex);
    return mFront->generation;
}

bool LiveSolver::idle() const
{
    std::lock_guard lk(mMutex);
    return mQueue.empty() && !mBusy && (!mAwake || mPaused);
}

bool LiveSolver::waitIdle(int milliseconds)
{
    std::unique_lock lk(mMutex);
    return mIdleCv.wait_for(lk, std::chrono::milliseconds(milliseconds), [&] { return mQueue.empty() && !mBusy && (!mAwake || mPaused); });
}

void LiveSolver::publish(bool idle)
{
    LILA_ZONE();

    std::shared_ptr<Frame> f;
    {
        std::lock_guard lk(mMutex);
        if (mBack && mBack.use_count() == 1)
            f = std::move(mBack);
    }
    if (!f)
        f = std::make_shared<Frame>();
    const Frame& prev = *mFront; // only this thread replaces mFront
    f->epoch = mEpoch;
    f->generation = prev.generation + 1;
    f->idle = idle;
    f->iterations = mIterations;
    f->islands.resize(mIslands.size());
    for (size_t i = 0; i < mIslands.size(); ++i)
    {
        const LiveIsland& is = mIslands[i];
        if (prev.epoch == mEpoch && i < prev.islands.size() && prev.islands[i] && prev.islands[i]->version == is.version() &&
            prev.islands[i]->settled == is.settled())
        {
            f->islands[i] = prev.islands[i];
            continue;
        }
        auto s = std::make_shared<IslandState>();
        s->version = is.version();
        s->uv = is.chart().uv;
        s->pinned = is.chart().pinned;
        s->stiffness.assign(is.stiffness().begin(), is.stiffness().end());
        s->settled = is.settled();
        f->islands[i] = std::move(s);
    }
    std::lock_guard lk(mMutex);
    mBack = std::const_pointer_cast<Frame>(mFront);
    mFront = std::move(f);
}

void LiveSolver::run()
{
    using Clock = std::chrono::steady_clock;
    auto lastPublish = Clock::now() - std::chrono::seconds(1);
    std::unique_lock lk(mMutex);
    for (;;)
    {
        mWake.wait(lk, [&] { return mStop || !mQueue.empty() || (!mPaused && mAwake); });
        if (mStop)
            break;
        std::vector<Edit> queue = std::move(mQueue);
        mQueue.clear();
        bool paused = mPaused;
        mBusy = true;
        ++mPass;
        lk.unlock();

        bool changed = !queue.empty();
        for (Edit& e : queue)
            e(mIslands);
        bool awake = false;
        if (!paused)
        {
            // Iterate for a few ms before looking at the queue again, so a stream of edits does not starve the solve.
            auto t0 = Clock::now();
            std::vector<size_t> active;
            for (;;)
            {
                active.clear();
                for (size_t i = 0; i < mIslands.size(); ++i)
                    if (!mIslands[i].settled())
                        active.push_back(i);
                if (active.empty())
                    break;
                tbb::parallel_for(size_t(0), active.size(), size_t(1), [&](size_t k) { mIslands[active[k]].step(); });
                ++mIterations;
                changed = true;
                if (Clock::now() - t0 > std::chrono::milliseconds(6))
                    break;
                std::lock_guard peek(mMutex);
                if (!mQueue.empty() || mStop)
                    break;
            }
        }
        for (const LiveIsland& is : mIslands)
            awake |= !is.settled();
        bool idleNow = !awake || paused;
        if (changed && (idleNow || !queue.empty() || Clock::now() - lastPublish > std::chrono::milliseconds(15)))
        {
            publish(idleNow);
            lastPublish = Clock::now();
        }
        else if (idleNow && !mFront->idle)
        {
            publish(true);
            lastPublish = Clock::now();
        }

        lk.lock();
        mBusy = false;
        mAwake = awake;
        mFinished = mPass;
        mIdleCv.notify_all();
    }
}
} // namespace lila::uv
