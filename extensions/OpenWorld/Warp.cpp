// The remix warp: rigid regions and pins blended into a velocity field, integrated in fold-checked steps.

#include "Warp.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace openworld
{
Warp::Warp(std::vector<Handle> regions, std::vector<Pin> pins, double falloff, int maxSteps, Vec2 lo, Vec2 hi)
    : mRegions(std::move(regions)), mPins(std::move(pins)), mFalloff(std::max(falloff, 1.0))
{
    LILA_ZONE();

    double longest = 0.0;
    for (const Handle& r : mRegions)
        for (Vec2 p : r.polygon)
            longest = std::max(longest, glm::length(r.motion.apply(p) - p));
    for (const Pin& p : mPins)
        longest = std::max(longest, glm::length(p.to - p.at));
    mIdentity = longest < 1e-9;
    if (mIdentity)
    {
        mSteps = 0;
        return;
    }

    // Probes: a grid over the map, plus rings just outside each region where neighbours squeeze hardest.
    std::vector<Vec2> probes;
    const int n = 96;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            probes.push_back(lo + (hi - lo) * Vec2(double(x) / n, double(z) / n));
    for (const Handle& r : mRegions)
    {
        const Vec2 c = centroid(r.polygon);
        for (size_t i = 0; i < r.polygon.size(); ++i)
        {
            const Vec2 a = r.polygon[i], b = r.polygon[(i + 1) % r.polygon.size()];
            for (double t : {0.0, 0.25, 0.5, 0.75})
            {
                const Vec2 p = a + (b - a) * t, out = glm::length(p - c) > 0.0 ? glm::normalize(p - c) : Vec2(1.0, 0.0);
                for (double d : {0.002, 0.01, 0.03, 0.1})
                    probes.push_back(p + out * (d * mFalloff));
            }
        }
    }
    for (const Pin& p : mPins)
        for (int k = 0; k < 8; ++k)
            for (double d : {0.01, 0.05})
                probes.push_back(p.at + rotated(Vec2(d * mFalloff, 0.0), 45.0 * k));

    const double h = std::max(glm::length(hi - lo), 1.0) * 2e-4;
    // Start where each step moves the fastest point a few percent of the falloff.
    int steps = std::clamp(int(std::ceil(longest / (0.05 * mFalloff))), 4, std::max(4, maxSteps));
    for (;;)
    {
        double minDet = 1.0;
        bool ok = tryRun(steps, probes, h, &minDet);
        mSteps = steps;
        mMinStepDet = minDet;
        // The probes can miss a thin squeeze; the composed map on remixCheck's own grid must not fold either.
        if (ok)
            ok = folds(lo, hi, kFoldGrid).folded == 0;
        if (ok || steps >= maxSteps)
            break;
        steps = std::min(maxSteps, steps * 2);
    }
}

Vec2 Warp::velocity(int step, Vec2 p) const
{
    const double t0 = double(step) / mSteps, t1 = double(step + 1) / mSteps;
    double wsum = 1.0 / (mFalloff * mFalloff);
    Vec2 v(0.0);
    for (const Handle& r : mRegions)
    {
        const Rigid m0 = r.motion.partial(t0), m1 = r.motion.partial(t1);
        const Vec2 back = m0.inverse(p);
        const Vec2 inc = m1.apply(back) - p;
        if (inside(r.polygon, back))
            return inc;
        const double d = std::max(distanceTo(r.polygon, back), 1e-6 * mFalloff);
        const double w = 1.0 / (d * d);
        wsum += w;
        v += inc * w;
    }
    for (const Pin& pin : mPins)
    {
        const Vec2 now = pin.at + (pin.to - pin.at) * t0;
        const Vec2 inc = (pin.to - pin.at) / double(mSteps);
        const double d2 = std::max(glm::dot(p - now, p - now), 1e-12 * mFalloff * mFalloff);
        const double w = 1.0 / d2;
        wsum += w;
        v += inc * w;
    }
    return v / wsum;
}

double Warp::stepJacobian(int step, Vec2 p, double h) const
{
    const Vec2 dx = (velocity(step, p + Vec2(h, 0.0)) - velocity(step, p - Vec2(h, 0.0))) / (2.0 * h);
    const Vec2 dz = (velocity(step, p + Vec2(0.0, h)) - velocity(step, p - Vec2(0.0, h))) / (2.0 * h);
    return (1.0 + dx.x) * (1.0 + dz.y) - dx.y * dz.x;
}

bool Warp::tryRun(int steps, const std::vector<Vec2>& probes, double h, double* minDet) const
{
    const_cast<Warp*>(this)->mSteps = steps;
    std::vector<Vec2> at = probes;
    std::atomic<bool> ok{true};
    std::mutex m;
    double worst = 1.0;
    for (int k = 0; k < steps && ok; ++k)
    {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, at.size(), 256), [&](const tbb::blocked_range<size_t>& r) {
            double local = 1.0;
            for (size_t i = r.begin(); i != r.end(); ++i)
            {
                local = std::min(local, stepJacobian(k, at[i], h));
                at[i] += velocity(k, at[i]);
            }
            std::lock_guard lock(m);
            worst = std::min(worst, local);
        });
        // Each step keeps at least this much of a cell's area, so the composition is far from folding.
        if (worst < 0.3)
            ok = false;
    }
    *minDet = worst;
    return ok;
}

Vec2 Warp::forward(Vec2 p) const
{
    if (mIdentity)
        return p;
    for (int k = 0; k < mSteps; ++k)
        p += velocity(k, p);
    return p;
}

Vec2 Warp::inverse(Vec2 q) const
{
    if (mIdentity)
        return q;
    const double tol = mFalloff * 1e-9;
    for (int k = mSteps - 1; k >= 0; --k)
    {
        // Each step moves points by a field whose gradient is well under 1, so the fixed point converges.
        Vec2 p = q - velocity(k, q);
        for (int it = 0; it < 12; ++it)
        {
            const Vec2 next = q - velocity(k, p);
            const bool done = glm::length(next - p) < tol;
            p = next;
            if (done)
                break;
        }
        q = p;
    }
    return q;
}

Warp::Folds Warp::folds(Vec2 lo, Vec2 hi, int res) const
{
    LILA_ZONE();

    Folds f;
    res = std::max(res, 2);
    f.cells = (res + 1) * (res + 1);
    if (mIdentity)
        return f;
    const Vec2 step = (hi - lo) / double(res);
    // The Jacobian of the composed map by central differences at each grid point: a coarse cell's corners can swing
    // past each other in a strong swirl without the map folding, so corners alone would misreport.
    const double h = std::max(1.0, 1e-3 * std::min(std::abs(step.x), std::abs(step.y)));
    std::vector<double> det(size_t(f.cells));
    tbb::parallel_for(0, res + 1, [&](int z) {
        for (int x = 0; x <= res; ++x)
        {
            const Vec2 p = lo + step * Vec2(x, z);
            const Vec2 dx = (forward(p + Vec2(h, 0.0)) - forward(p - Vec2(h, 0.0))) / (2.0 * h);
            const Vec2 dz = (forward(p + Vec2(0.0, h)) - forward(p - Vec2(0.0, h))) / (2.0 * h);
            det[size_t(z) * (res + 1) + x] = dx.x * dz.y - dx.y * dz.x;
        }
    });
    for (int z = 0; z <= res; ++z)
        for (int x = 0; x <= res; ++x)
        {
            const double j = det[size_t(z) * (res + 1) + x];
            f.minJacobian = std::min(f.minJacobian, j);
            if (j <= 0.0)
            {
                ++f.folded;
                if (f.where.size() < 64)
                    f.where.push_back(lo + step * Vec2(x, z));
            }
        }
    return f;
}

// ---------------------------------------------------------------------------------------------------- inverse grid

InverseGrid::InverseGrid(const Warp& w, Vec2 lo, Vec2 hi, int res, std::vector<Warp::Handle> rigid)
    : mWarp(&w), mLo(lo), mRes(std::max(res, 2)), mRigid(std::move(rigid))
{
    LILA_ZONE();

    if (w.identity())
        return;
    mStep = (hi - lo) / double(mRes - 1);
    mAt.resize(size_t(mRes) * mRes);
    tbb::parallel_for(0, mRes, [&](int z) {
        for (int x = 0; x < mRes; ++x)
        {
            const Vec2 q = lo + mStep * Vec2(x, z);
            mAt[size_t(z) * mRes + x] = w.inverse(q) - q;
        }
    });
}

Vec2 InverseGrid::operator()(Vec2 q) const
{
    if (!mWarp || mWarp->identity())
        return q;
    for (const Warp::Handle& r : mRigid)
        if (inside(r.polygon, q))
            return r.motion.inverse(q);
    const Vec2 g = (q - mLo) / mStep;
    if (g.x < 0.0 || g.y < 0.0 || g.x > mRes - 1 || g.y > mRes - 1)
        return mWarp->inverse(q);
    const int x0 = std::min(int(g.x), mRes - 2), z0 = std::min(int(g.y), mRes - 2);
    const double fx = g.x - x0, fz = g.y - z0;
    const auto at = [&](int x, int z) { return mAt[size_t(z) * mRes + x]; };
    const Vec2 d = (at(x0, z0) * (1 - fx) + at(x0 + 1, z0) * fx) * (1 - fz) + (at(x0, z0 + 1) * (1 - fx) + at(x0 + 1, z0 + 1) * fx) * fz;
    return q + d;
}
} // namespace openworld
