#include "brush/Stroke.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::brush
{
namespace
{
uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/** Deterministic 0..1 for a dab and a channel of variation. */
double random01(uint32_t seed, uint32_t index, uint32_t channel)
{
    return double(hash(seed * 0x9E3779B9u ^ hash(index * 4u + channel))) / 4294967296.0;
}

glm::dvec3 rotateAbout(const glm::dvec3& v, const glm::dvec3& axis, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0 - c);
}
} // namespace

// ------------------------------------------------------------------------------------------------ StrokeSampler

void StrokeSampler::begin(const BrushSettings& s, const StrokeInput& in, double radiusPx)
{
    mActive = true;
    mMode = s.mode;
    mSpacing = s.spacing;
    mLazy = s.lazyRadius;
    mSmoothing = s.smoothing;
    mJitter = s.jitter;
    mAngle = s.angle * std::numbers::pi / 180.0;
    mFollow = s.followStroke;
    mSeed = s.seed;
    setRadius(radiusPx);
    mStart = mCursor = mBrush = mLast = in.position;
    mPressure = mLastPressure = in.pressure;
    mDir = glm::dvec2(0.0);
    mDistance = 0.0;
    mIndex = 0;
    mOut.clear();
    mPath = {in.position};
    mPathPressure = {in.pressure};
    if (mMode == StrokeMode::Freehand)
        push(in.position, in.pressure, glm::dvec2(0.0));
}

void StrokeSampler::setRadius(double radiusPx)
{
    mRadius = std::max(radiusPx, 1e-6);
    mStep = std::max(1.0, mRadius * mSpacing);
}

void StrokeSampler::move(const StrokeInput& in)
{
    if (!mActive)
        return;
    mCursor = in.position;
    mPressure = in.pressure;
    if (mMode == StrokeMode::Line)
    {
        mBrush = mCursor;
        return;
    }
    if (mMode == StrokeMode::Curve)
    {
        mBrush = mCursor;
        if (glm::length(mCursor - mPath.back()) >= 2.0)
        {
            mPath.push_back(mCursor);
            mPathPressure.push_back(in.pressure);
        }
        return;
    }
    glm::dvec2 target = mCursor;
    if (mLazy > 0.0)
    {
        // The brush hangs on a string of length lazy behind the cursor and only moves when it pulls taut.
        const glm::dvec2 d = mCursor - mBrush;
        const double len = glm::length(d);
        target = len > mLazy ? mBrush + d * ((len - mLazy) / len) : mBrush;
    }
    if (mSmoothing > 0.0)
        target = mBrush + (target - mBrush) * (1.0 - mSmoothing);
    mBrush = target;
    stepTo(mBrush, in.pressure);
}

void StrokeSampler::end()
{
    if (!mActive)
        return;
    if (mMode == StrokeMode::Line)
    {
        const glm::dvec2 d = mCursor - mStart;
        const double len = glm::length(d);
        push(mStart, mLastPressure, len > 0.0 ? d / len : glm::dvec2(0.0));
        stepTo(mCursor, mPressure);
    }
    if (mMode == StrokeMode::Curve)
    {
        const std::vector<glm::dvec2> path = smoothedPath();
        if (!path.empty())
        {
            const double pressure = mPathPressure.empty() ? mPressure : mPathPressure.front();
            mLast = path.front();
            mLastPressure = pressure;
            const glm::dvec2 d = path.size() > 1 ? path[1] - path[0] : glm::dvec2(0.0);
            push(path.front(), pressure, glm::length(d) > 0.0 ? glm::normalize(d) : glm::dvec2(0.0));
            for (size_t k = 1; k < path.size(); ++k)
            {
                // Pressure follows the drawn path by how far along it each point lies.
                const size_t at = std::min(mPathPressure.size() - 1, k * mPathPressure.size() / path.size());
                stepTo(path[k], mPathPressure.empty() ? mPressure : mPathPressure[at]);
            }
        }
    }
    mActive = false;
}

std::vector<glm::dvec2> StrokeSampler::smoothedPath() const
{
    std::vector<glm::dvec2> pts = mPath;
    if (pts.size() < 3)
        return pts;
    // Even spacing first, so smoothing treats every part of the path alike, then relax with the ends held.
    std::vector<glm::dvec2> even{pts.front()};
    const double step = std::max(2.0, mStep * 0.5);
    double carry = 0.0;
    for (size_t k = 1; k < pts.size(); ++k)
    {
        const glm::dvec2 a = pts[k - 1], b = pts[k];
        const double len = glm::length(b - a);
        double t = step - carry;
        while (t <= len)
        {
            even.push_back(a + (b - a) * (t / len));
            t += step;
        }
        carry = len - (t - step);
    }
    if (glm::length(even.back() - pts.back()) > 1e-6)
        even.push_back(pts.back());
    const int iterations = std::clamp(int(even.size() / 4), 2, 40);
    // The ends smooth too, toward their one neighbour, so their wobble goes; that also draws them in along the path, so
    // afterwards each is carried back out along the smoothed direction to the reach the drawn end had.
    const glm::dvec2 first = even.front(), last = even.back();
    std::vector<glm::dvec2> next = even;
    for (int it = 0; it < iterations; ++it)
    {
        const size_t n = even.size();
        next[0] = (even[0] + even[1]) * 0.5;
        next[n - 1] = (even[n - 1] + even[n - 2]) * 0.5;
        for (size_t k = 1; k + 1 < n; ++k)
            next[k] = even[k] * 0.5 + (even[k - 1] + even[k + 1]) * 0.25;
        std::swap(even, next);
    }
    if (even.size() >= 3)
    {
        auto extend = [](const glm::dvec2& drawn, const glm::dvec2& a, const glm::dvec2& b) {
            const glm::dvec2 dir = a - b;
            const double len = glm::length(dir);
            return len > 1e-9 ? a + dir / len * std::abs(glm::dot(drawn - a, dir / len)) : drawn;
        };
        const size_t n = even.size();
        even[0] = extend(first, even[1], even[2]);
        even[n - 1] = extend(last, even[n - 2], even[n - 3]);
    }
    return even;
}

void StrokeSampler::cancel()
{
    mActive = false;
    mOut.clear();
}

void StrokeSampler::stepTo(glm::dvec2 target, double pressure)
{
    const glm::dvec2 d = target - mLast;
    const double len = glm::length(d);
    if (len <= 0.0)
        return;
    const glm::dvec2 dir = d / len;
    const int count = int(len / mStep);
    for (int k = 1; k <= count; ++k)
    {
        const double f = k * mStep / len;
        mDistance += mStep;
        push(mLast + d * f, mLastPressure + (pressure - mLastPressure) * f, dir);
    }
    if (count > 0)
    {
        const double f = count * mStep / len;
        mLast += d * f;
        mLastPressure += (pressure - mLastPressure) * f;
    }
}

void StrokeSampler::push(glm::dvec2 at, double pressure, glm::dvec2 dir)
{
    StrokeSample s;
    s.index = mIndex++;
    s.position = at;
    s.pressure = std::clamp(pressure, 0.0, 1.0);
    s.direction = dir;
    s.distance = mDistance;
    if (glm::length(dir) > 0.0)
        mDir = dir;
    s.angle = mAngle;
    if (mFollow && glm::length(mDir) > 0.0)
        s.angle += std::atan2(-mDir.y, mDir.x);
    if (mJitter.any())
    {
        if (mJitter.position > 0.0)
        {
            const double a = random01(mSeed, s.index, 0) * 2.0 * std::numbers::pi, r = std::sqrt(random01(mSeed, s.index, 1));
            s.position += glm::dvec2(std::cos(a), std::sin(a)) * (r * mJitter.position * mRadius);
        }
        s.size = 1.0 - mJitter.size * random01(mSeed, s.index, 2);
        s.opacity = 1.0 - mJitter.opacity * random01(mSeed, s.index, 3);
        s.angle += (random01(mSeed, s.index, 4) * 2.0 - 1.0) * mJitter.angle * std::numbers::pi / 180.0;
    }
    mOut.push_back(s);
}

std::vector<StrokeSample> StrokeSampler::take() { return std::exchange(mOut, {}); }

// ------------------------------------------------------------------------------------------------ dabs

double radiusPixels(const BrushSettings& s, const SurfaceHit& hit)
{
    if (s.radiusUnit == RadiusUnit::Screen)
        return s.radius;
    return s.worldRadius * hit.worldToLocal / std::max(hit.pixelSize, 1e-12);
}

Dab makeDab(const BrushSettings& s, const StrokeSample& sample, const SurfaceHit& hit, const Dab* previous)
{
    Dab d;
    d.center = hit.center;
    d.normal = glm::length(hit.normal) > 0.0 ? glm::normalize(hit.normal) : glm::dvec3(0.0, 1.0, 0.0);
    d.viewDir = hit.viewDir;
    d.eye = hit.eye;
    d.pressure = sample.pressure;
    // A light touch still leaves a dab: size never drops below 5%.
    const double size = std::max(0.05, s.pressureSize.map(sample.pressure)) * sample.size;
    const double base = s.radiusUnit == RadiusUnit::Screen ? s.radius * hit.pixelSize : s.worldRadius * hit.worldToLocal;
    d.radius = base * size;
    d.opacity = s.pressureOpacity.map(sample.pressure) * sample.opacity;
    d.flow = s.flow * s.pressureFlow.map(sample.pressure);
    d.direction = previous ? hit.center - previous->center : glm::dvec3(0.0);
    d.angle = sample.angle;
    d.roundness = s.roundness;
    d.alpha = s.alpha;
    d.screen = sample.position;
    d.index = sample.index;
    if (!roundTip(d))
    {
        glm::dvec3 r = hit.right - d.normal * glm::dot(d.normal, hit.right);
        if (glm::length(r) < 1e-9)
            r = glm::cross(d.normal, std::abs(d.normal.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
        d.tangent = rotateAbout(glm::normalize(r), d.normal, d.angle);
    }
    return d;
}

// ------------------------------------------------------------------------------------------------ Stroke

bool Stroke::begin(const BrushSettings& s, const StrokeInput& in, SurfaceQuery query, DabSink& sink, bool requireHit)
{
    mSettings = s;
    mSettings.ensureAlpha();
    mQuery = std::move(query);
    mSink = &sink;
    mDabs.clear();
    double radiusPx = s.radius;
    SurfaceHit hit;
    const bool over = mQuery && mQuery(in.position, hit);
    if (requireHit && s.mode == StrokeMode::Freehand && !over)
        return false;
    if (over)
        radiusPx = radiusPixels(mSettings, hit);
    mSampler.begin(mSettings, in, radiusPx);
    flush();
    return true;
}

void Stroke::move(const StrokeInput& in)
{
    if (!mSampler.active())
        return;
    mSampler.move(in);
    flush();
}

void Stroke::end()
{
    if (!mSampler.active())
        return;
    mSampler.end();
    flush();
    mSink = nullptr;
}

void Stroke::cancel()
{
    mSampler.cancel();
    mSink = nullptr;
}

void Stroke::flush()
{
    LILA_ZONE();

    for (const StrokeSample& s : mSampler.take())
    {
        SurfaceHit hit;
        if (!mQuery || !mQuery(s.position, hit))
            continue;
        if (mSettings.radiusUnit == RadiusUnit::World)
            mSampler.setRadius(radiusPixels(mSettings, hit));
        Dab d = makeDab(mSettings, s, hit, mDabs.empty() ? nullptr : &mDabs.back());
        mDabs.push_back(d);
        if (mSink)
            mSink->dab(d);
    }
}
} // namespace lila::brush
