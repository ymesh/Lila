#include "sculpt/Sculpt.h"

#include "brush/Symmetry.h"
#include "core/Profile.h"
#include "geo/RenderMesh.h"

#include <glm/gtc/quaternion.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace lila::sculpt
{
namespace
{
constexpr std::array<std::string_view, size_t(Brush::Count)> kBrushNames = {
    "draw",      "clayStrips", "inflate", "grab", "smooth", "flatten", "pinch",  "crease", "scrape",
    "damStandard", "trimDynamic", "polish", "fold", "mask", "relax", "freeze", "paint", "fill", "clay", "layer", "blob", "magnify",
    "planar", "snakeHook", "elasticGrab", "nudge", "thumb", "rotate", "pose", "boundary"};
constexpr std::array<std::string_view, size_t(Falloff::Count)> kFalloffNames = {"smooth", "sphere", "root", "sharp", "linear", "constant"};

// Per-dab step sizes as fractions of the radius at full strength; with dabs every tenth of a radius a stroke builds up
// a few tenths of a radius.
constexpr double kDrawStep = 0.03;
constexpr double kPinchStep = 0.15;
/** Pinch also sinks the surface this far, so the crease it draws together deepens. */
constexpr double kPinchDepth = 0.01;
/** Smooth's bi-Laplacian gain: below 0.5 so even a checkerboard of noise settles rather than flipping. */
constexpr double kSmoothGain = 0.45;
constexpr double kClayDepth = 0.12;
constexpr double kDamPinch = 0.25;
constexpr double kTrimDepth = 0.04;
constexpr double kFoldPinch = 0.3;
constexpr double kLayerHeight = 0.08;
constexpr double kBlobSpread = 0.08;
constexpr double kSnakePinch = 0.2;
constexpr double kNudgeStep = 0.25;
/** Elastic Grab reaches this many radii, its weight falling as (1 + (d/r)^2)^-1.5. */
constexpr double kElasticReach = 3.0;
/** Boundary moves the open border and this many rings of points in from it. */
constexpr int kBoundaryRings = 3;

glm::dvec3 reflectVector(const geo::SymmetryPlane& plane, const glm::dvec3& v) { return v - 2.0 * glm::dot(v, plane.normal) * plane.normal; }

glm::dvec3 tangential(const glm::dvec3& v, const glm::dvec3& n) { return v - n * glm::dot(n, v); }

/** Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5). */
glm::dvec3 closestOnTriangle(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return a;
    const glm::dvec3 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return a + ab * (d1 / (d1 - d3));
    const glm::dvec3 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

bool movesPoints(Brush b) { return b != Brush::Mask && b != Brush::Freeze && b != Brush::Paint; }

/** Brushes that gather their region once, at the stroke's first dab, and move it as the cursor drags. */
bool heldRegion(Brush b)
{
    return b == Brush::Grab || b == Brush::ElasticGrab || b == Brush::Rotate || b == Brush::Pose || b == Brush::Boundary;
}

glm::dvec3 rotateAbout(const glm::dvec3& v, const glm::dvec3& axis, double angle) { return glm::angleAxis(angle, axis) * v; }

constexpr glm::vec4 kWhite(1.0f);
} // namespace

Registry<BrushDef>& brushRegistry()
{
    static Registry<BrushDef> r;
    return r;
}

ext::Context::Handle registerBrush(ext::Context& ctx, std::string name, BrushDef def)
{
    if (name.empty() || brushFromName(name))
    {
        ctx.log(LogLevel::Error, "sculpt brush '" + name + "': that name belongs to a built-in brush");
        return 0;
    }
    if (!def.displace)
    {
        ctx.log(LogLevel::Error, "sculpt brush '" + name + "' has no displace function");
        return 0;
    }
    try
    {
        return ctx.add(brushRegistry(), "sculptBrush", std::move(name), std::move(def));
    }
    catch (const std::exception& e)
    {
        ctx.log(LogLevel::Error, std::string("sculpt brush: ") + e.what());
        return 0;
    }
}

std::string brushId(const BrushSettings& s) { return s.brush == Brush::Custom ? s.custom : std::string(brushName(s.brush)); }

bool setBrush(BrushSettings& s, std::string_view name)
{
    if (auto b = brushFromName(name))
    {
        s.brush = *b;
        s.custom.clear();
        return true;
    }
    if (!brushRegistry().contains(name))
        return false;
    s.brush = Brush::Custom;
    s.custom = std::string(name);
    return true;
}

bool dragsCursor(Brush b) { return heldRegion(b) || b == Brush::SnakeHook; }

std::string_view brushName(Brush b) { return b < Brush::Count ? kBrushNames[size_t(b)] : std::string_view(); }

std::optional<Brush> brushFromName(std::string_view name)
{
    for (size_t i = 0; i < kBrushNames.size(); ++i)
        if (kBrushNames[i] == name)
            return Brush(i);
    return std::nullopt;
}

std::string_view falloffName(Falloff f) { return f < Falloff::Count ? kFalloffNames[size_t(f)] : std::string_view(); }

std::optional<Falloff> falloffFromName(std::string_view name)
{
    for (size_t i = 0; i < kFalloffNames.size(); ++i)
        if (kFalloffNames[i] == name)
            return Falloff(i);
    return std::nullopt;
}

double falloffWeight(Falloff f, double t) { return f < Falloff::Count ? brush::shapeWeight(toShape(f), t) : 0.0; }

SculptMesh::SculptMesh(geo::Mesh mesh) : mMesh(std::move(mesh))
{
    LILA_ZONE();

    const Index n = mMesh.pointCapacity();
    mBvh.build(mMesh);
    // Builds the partial refit's tables now rather than on the first dab.
    mBvh.refitPoints(mMesh, {});
    mMask.assign(n, 0.0f);
    mFrozen.assign(n, 0);
    mNormals.assign(n, glm::vec3(0.0f, 1.0f, 0.0f));
    mStrokeStamp.assign(n, 0);
    mRecordIndex.assign(n, 0);
    mDabStamp.assign(n, 0);
    mGatherStamp.assign(n, 0);
    mDisp.assign(n, glm::dvec3(0.0));
    mMaskDelta.assign(n, 0.0f);
    mFreezeSet.assign(n, 0);
    mPaint.assign(n, 0.0f);
    mBlend.assign(n, 0.0);

    mAdjOffsets.assign(size_t(n) + 1, 0);
    for (Index e = 0; e < mMesh.edgeCapacity(); ++e)
        if (mMesh.edgeValid(e))
        {
            ++mAdjOffsets[mMesh.edgePoint(e, 0) + 1];
            ++mAdjOffsets[mMesh.edgePoint(e, 1) + 1];
        }
    for (size_t i = 1; i < mAdjOffsets.size(); ++i)
        mAdjOffsets[i] += mAdjOffsets[i - 1];
    mAdj.resize(mAdjOffsets.back());
    std::vector<uint32_t> fill(mAdjOffsets.begin(), mAdjOffsets.end() - 1);
    for (Index e = 0; e < mMesh.edgeCapacity(); ++e)
        if (mMesh.edgeValid(e))
        {
            Index a = mMesh.edgePoint(e, 0), b = mMesh.edgePoint(e, 1);
            mAdj[fill[a]++] = b;
            mAdj[fill[b]++] = a;
        }

    const auto& corners = mBvh.triangles().corners;
    mTriOffsets.assign(size_t(n) + 1, 0);
    for (Index c : corners)
        ++mTriOffsets[mMesh.cornerPoint(c) + 1];
    for (size_t i = 1; i < mTriOffsets.size(); ++i)
        mTriOffsets[i] += mTriOffsets[i - 1];
    mTris.resize(mTriOffsets.back());
    fill.assign(mTriOffsets.begin(), mTriOffsets.end() - 1);
    for (size_t i = 0; i < corners.size(); ++i)
        mTris[fill[mMesh.cornerPoint(corners[i])]++] = Index(i / 3);

    std::vector<Index> all;
    all.reserve(n);
    for (Index p = 0; p < n; ++p)
        if (mMesh.pointValid(p))
            all.push_back(p);
    updateNormals(all);
}

void SculptMesh::setSymmetry(const std::optional<geo::SymmetryPlane>& plane, double tolerance)
{
    LILA_ZONE();

    if (!plane)
    {
        mSymmetry = {};
        return;
    }
    mSymmetry = geo::SymmetryMap::build(mMesh, *plane, tolerance);
    // Exactly mirrored positions keep mirrored strokes exact: reflecting an axis plane through the origin only flips a sign.
    std::vector<Index> moved;
    const geo::SymmetryPlane& pl = mSymmetry.plane();
    for (Index p = 0; p < mMesh.pointCapacity(); ++p)
    {
        if (!mMesh.pointValid(p))
            continue;
        Index q = mSymmetry.mirrorPoint(p);
        if (q == geo::kInvalid || q < p)
            continue;
        glm::dvec3 at = mMesh.position(p);
        if (q == p)
        {
            glm::dvec3 on = pl.project(at);
            if (on != at)
            {
                mMesh.setPosition(p, on);
                moved.push_back(p);
            }
            continue;
        }
        glm::dvec3 avg = (at + pl.reflect(mMesh.position(q))) * 0.5;
        glm::dvec3 mirrored = pl.reflect(avg);
        if (avg != at)
        {
            mMesh.setPosition(p, avg);
            moved.push_back(p);
        }
        if (mirrored != mMesh.position(q))
        {
            mMesh.setPosition(q, mirrored);
            moved.push_back(q);
        }
    }
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
}

void SculptMesh::setSymmetry(const std::optional<geo::SymmetrySpec>& spec, double tolerance)
{
    LILA_ZONE();

    if (!spec || !spec->radial)
    {
        setSymmetry(spec ? std::optional<geo::SymmetryPlane>(spec->plane()) : std::nullopt, tolerance);
        return;
    }
    mSymmetry = geo::SymmetryMap::buildRadial(mMesh, *spec->radial, tolerance);
    // Exact orbits keep repeated strokes exact.
    std::vector<glm::dvec3> before(mMesh.positions().begin(), mMesh.positions().end());
    std::vector<glm::dvec3> positions = before;
    mSymmetry.averageOrbits(positions, true);
    std::vector<Index> moved;
    for (Index p = 0; p < mMesh.pointCapacity(); ++p)
        if (mMesh.pointValid(p) && positions[p] != before[p])
        {
            mMesh.setPosition(p, positions[p]);
            moved.push_back(p);
        }
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
}

bool SculptMesh::raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const
{
    return !mBvh.empty() && mBvh.raycast(origin, dir, hit);
}

glm::dvec3 SculptMesh::normalAt(const geo::BvhHit& hit) const
{
    const Index* c = &mBvh.triangles().corners[size_t(hit.triangle) * 3];
    glm::dvec3 n(0.0);
    for (int k = 0; k < 3; ++k)
        n += glm::dvec3(mNormals[mMesh.cornerPoint(c[k])]) * hit.bary[k];
    double len = glm::length(n);
    return len > 1e-12 ? n / len : glm::dvec3(0.0, 1.0, 0.0);
}

void SculptMesh::beginStroke(const BrushSettings& settings)
{
    mSettings = settings;
    mSettings.strength = std::clamp(settings.strength, 0.0, 1.0);
    mFalloffTable = brush::FalloffTable(mSettings.falloff);
    mCustom = nullptr;
    if (mSettings.brush == Brush::Custom)
        if (const BrushDef* def = brushRegistry().find(mSettings.custom))
            mCustom = def->displace;
    if (painting())
        ensureColor();
    mStroking = true;
    mGrab.clear();
    mPlanes.clear();
    mRecord = {};
    if (++mStroke == 0)
    {
        std::fill(mStrokeStamp.begin(), mStrokeStamp.end(), 0u);
        mStroke = 1;
    }
}

double SculptMesh::pointWeight(Index p, double falloff) const
{
    const double w = falloff * mSettings.strength;
    if (!movesPoints(mSettings.brush))
        return w;
    if (mFrozen[p])
        return 0.0;
    return w * (1.0 - double(mMask[p]));
}

void SculptMesh::gather(Side& s, bool forGrab)
{
    LILA_ZONE();

    s.points.clear();
    s.weights.clear();
    const double r = s.dab.radius;
    if (r <= 0.0)
        return;
    geo::Bounds box;
    box.extend(s.dab.center - glm::dvec3(r));
    box.extend(s.dab.center + glm::dvec3(r));
    if (++mGatherId == 0)
    {
        std::fill(mGatherStamp.begin(), mGatherStamp.end(), 0u);
        mGatherId = 1;
    }
    const auto& corners = mBvh.triangles().corners;
    const bool front = mSettings.frontFacesOnly && glm::length(s.dab.viewDir) > 0.0;
    const bool round = brush::roundTip(s.dab);
    for (Index t : mBvh.overlapping(box))
        for (int k = 0; k < 3; ++k)
        {
            Index p = mMesh.cornerPoint(corners[size_t(t) * 3 + k]);
            if (mGatherStamp[p] == mGatherId)
                continue;
            mGatherStamp[p] = mGatherId;
            const glm::dvec3 rel = mMesh.position(p) - s.dab.center;
            double d = glm::length(rel);
            if (d >= r)
                continue;
            if (front && !forGrab && glm::dot(glm::dvec3(mNormals[p]), s.dab.viewDir) > 0.0)
                continue;
            double f = round ? mSettings.falloff.evaluate(d / r) : double(brush::tipWeight(s.dab, mFalloffTable, rel));
            if (f <= 0.0)
                continue;
            s.points.push_back(p);
            s.weights.push_back(f);
        }
}

void SculptMesh::computeArea(Side& s) const
{
    glm::dvec3 n(0.0), c(0.0);
    double total = 0.0;
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        n += glm::dvec3(mNormals[s.points[i]]) * s.weights[i];
        c += mMesh.position(s.points[i]) * s.weights[i];
        total += s.weights[i];
    }
    double len = glm::length(n);
    s.areaNormal = len > 1e-12 ? n / len : s.dab.normal;
    s.areaCenter = total > 0.0 ? c / total : s.dab.center;
}

void SculptMesh::touch(Index p)
{
    if (mDabStamp[p] == mDabId)
        return;
    mDabStamp[p] = mDabId;
    mChanged.push_back(p);
    mDisp[p] = glm::dvec3(0.0);
    mMaskDelta[p] = 0.0f;
    mFreezeSet[p] = 0;
    mPaint[p] = 0.0f;
    mBlend[p] = 0.0;
}

bool SculptMesh::painting() const
{
    return mSettings.brush == Brush::Paint || (mSettings.paint && movesPoints(mSettings.brush) && !dragsCursor(mSettings.brush));
}

void SculptMesh::ensureColor()
{
    if (mColor.empty())
        mColor.assign(mMesh.pointCapacity(), kWhite);
}

bool SculptMesh::painted() const
{
    return std::ranges::any_of(mColor, [](const glm::vec4& c) { return c != kWhite; });
}

glm::dvec3 SculptMesh::originalPosition(Index p) const
{
    return mStroking && mStrokeStamp[p] == mStroke ? mRecord.before[mRecordIndex[p]] : mMesh.position(p);
}

glm::dvec3 SculptMesh::projectToOriginal(Index p, const glm::dvec3& at) const
{
    const auto& corners = mBvh.triangles().corners;
    glm::dvec3 best = at;
    double bestD = std::numeric_limits<double>::max();
    for (uint32_t k = mTriOffsets[p]; k < mTriOffsets[p + 1]; ++k)
    {
        const Index* c = &corners[size_t(mTris[k]) * 3];
        glm::dvec3 q = closestOnTriangle(at, originalPosition(mMesh.cornerPoint(c[0])), originalPosition(mMesh.cornerPoint(c[1])),
                                         originalPosition(mMesh.cornerPoint(c[2])));
        double d = glm::dot(q - at, q - at);
        if (d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

glm::dvec3 SculptMesh::laplacian(Index p) const
{
    auto nb = neighbours(p);
    if (nb.empty())
        return glm::dvec3(0.0);
    glm::dvec3 avg(0.0);
    for (Index q : nb)
        avg += mMesh.position(q);
    return avg / double(nb.size()) - mMesh.position(p);
}

glm::dvec3 SculptMesh::smoothStep(Index p) const
{
    auto nb = neighbours(p);
    if (nb.empty())
        return glm::dvec3(0.0);
    glm::dvec3 around(0.0);
    for (Index q : nb)
        around += laplacian(q);
    return (laplacian(p) - around / double(nb.size())) * kSmoothGain;
}

void SculptMesh::addDisplacement(const Side& s)
{
    LILA_ZONE();

    const size_t n = s.points.size();
    std::vector<glm::dvec3> out(n, glm::dvec3(0.0));
    std::vector<uint8_t> reached(n, 0);
    const bool paint = painting();
    std::vector<float> paintOut(paint ? n : 0, 0.0f);
    const double r = s.dab.radius, sign = mSettings.invert ? -1.0 : 1.0, pressure = s.dab.opacity * s.dab.flow;
    const glm::dvec3 N = s.areaNormal, C = s.areaCenter;

    // Clay strips: a square footprint in the plane, lined up with the stroke.
    glm::dvec3 ax = tangential(s.dab.direction, N);
    if (glm::length(ax) < 1e-9)
        ax = tangential(std::abs(N.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0), N);
    ax = glm::normalize(ax);
    const glm::dvec3 ay = glm::cross(N, ax);
    const glm::dvec3 clayPlane = C + N * (r * kClayDepth * sign);
    const Brush brush = mSettings.brush;
    if (brush == Brush::Custom && !mCustom)
        return;
    const BrushArea area{.mesh = *this, .dab = s.dab, .normal = N, .center = C, .radius = r, .sign = sign};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 1024), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
        {
            const Index p = s.points[i];
            const glm::dvec3 P = mMesh.position(p);
            double w = pointWeight(p, s.weights[i]) * pressure;
            if (w <= 0.0)
                continue;
            reached[i] = 1;
            if (paint)
                paintOut[i] = float(std::min(w, 1.0));
            glm::dvec3 d(0.0);
            switch (brush)
            {
            case Brush::Draw: d = N * (w * r * kDrawStep * sign); break;
            case Brush::Inflate: d = glm::dvec3(mNormals[p]) * (w * r * kDrawStep * sign); break;
            case Brush::Smooth: d = (mSettings.keepVolume ? smoothStep(p) : laplacian(p)) * std::min(w, 1.0); break;
            case Brush::Relax:
            {
                auto nb = neighbours(p);
                if (nb.empty())
                    break;
                glm::dvec3 avg(0.0);
                for (Index q : nb)
                    avg += mMesh.position(q);
                avg /= double(nb.size());
                d = projectToOriginal(p, P + tangential(avg - P, glm::dvec3(mNormals[p])) * std::min(w, 1.0)) - P;
                break;
            }
            case Brush::Flatten: d = -N * (glm::dot(P - C, N) * std::min(w, 1.0) * sign); break;
            case Brush::Scrape:
            {
                const double dist = glm::dot(P - C, N);
                if (dist * sign > 0.0)
                    d = -N * (dist * std::min(w, 1.0));
                break;
            }
            case Brush::ClayStrips:
            {
                const glm::dvec3 rel = P - C;
                const double t = std::max(std::abs(glm::dot(rel, ax)), std::abs(glm::dot(rel, ay))) / r;
                const double f = mSettings.falloff.evaluate(t) / std::max(s.weights[i], 1e-12);
                const double dist = glm::dot(P - clayPlane, N);
                if (dist * sign < 0.0)
                    d = -N * (dist * std::min(w * f, 1.0));
                break;
            }
            case Brush::Pinch: d = tangential(C - P, N) * (w * kPinchStep * sign) - N * (w * r * kPinchDepth * mSettings.pinchDepth * sign); break;
            case Brush::DamStandard:
                // The cut falls off faster than the pinch, so the groove stays a narrow V.
                d = tangential(C - P, N) * (std::min(w, 1.0) * kDamPinch) - N * (w * w * r * kDrawStep * 1.5 * sign);
                break;
            case Brush::TrimDynamic:
            {
                // A plane just under the brush, moving with it: what stands above is cut flat (Ctrl: what sinks below is filled).
                const double dist = glm::dot(P - (C - N * (r * kTrimDepth * sign)), N);
                if (dist * sign > 0.0)
                    d = -N * (dist * std::min(w, 1.0));
                break;
            }
            case Brush::Polish:
            {
                auto nb = neighbours(p);
                if (nb.empty())
                    break;
                glm::dvec3 avg(0.0);
                for (Index q : nb)
                    avg += mMesh.position(q);
                avg /= double(nb.size());
                // Bumps are smoothed only along the brush normal and the surface eased onto its plane, so forms go flat and crisp.
                const double k = std::min(w, 1.0);
                d = N * (glm::dot(avg - P, N) * k) - N * (glm::dot(P - C, N) * k * 0.25);
                break;
            }
            case Brush::Fold:
            {
                // A ridge along the stroke: raised, and pinched toward its centre line from either side only.
                const double across = glm::dot(P - C, ay);
                d = N * (w * r * kDrawStep * sign) - ay * (across * std::min(w, 1.0) * kFoldPinch);
                break;
            }
            case Brush::Crease: d = tangential(C - P, N) * (w * kPinchStep) - N * (w * r * kDrawStep * sign); break;
            case Brush::Custom: d = mCustom(area, BrushSample{.point = p, .position = P, .normal = glm::dvec3(mNormals[p]), .weight = w}); break;
            case Brush::Fill:
            case Brush::Planar:
            {
                // Fill raises what sits under the plane (Ctrl: shaves what stands over it); Planar presses both onto it.
                const double dist = glm::dot(P - C, N);
                if (brush == Brush::Planar || dist * sign < 0.0)
                    d = -N * (dist * std::min(w, 1.0));
                break;
            }
            case Brush::Clay:
            {
                const double dist = glm::dot(P - clayPlane, N);
                if (dist * sign < 0.0)
                    d = -N * (dist * std::min(w, 1.0) * 0.5);
                break;
            }
            case Brush::Layer:
            {
                // Toward one height over the stroke's start, never past it, however often the stroke passes.
                const double height = r * kLayerHeight * sign;
                const double offset = glm::dot(P - originalPosition(p), N);
                if ((height - offset) * sign > 0.0)
                    d = N * ((height - offset) * std::min(w, 1.0));
                break;
            }
            case Brush::Blob: d = N * (w * r * kDrawStep * sign) + tangential(P - C, N) * (w * kBlobSpread * sign); break;
            case Brush::Magnify: d = tangential(P - C, N) * (w * kPinchStep * sign); break;
            case Brush::SnakeHook:
            {
                // The cursor's pull, pinched in across it so the tendril stays thin.
                const double len = glm::length(s.dab.delta);
                if (len <= 0.0)
                    break;
                const glm::dvec3 along = s.dab.delta / len;
                d = s.dab.delta * std::min(w, 1.0) + tangential(C - P, along) * (std::min(w, 1.0) * kSnakePinch);
                break;
            }
            case Brush::Nudge:
            case Brush::Thumb:
            {
                const glm::dvec3 dir = tangential(s.dab.direction, N);
                const double len = glm::length(dir);
                if (len <= 1e-12)
                    break;
                d = dir / len * (w * r * kNudgeStep);
                if (brush == Brush::Thumb)
                    d -= N * (glm::dot(P - C, N) * std::min(w, 1.0) * 0.25);
                break;
            }
            case Brush::Grab:
            case Brush::ElasticGrab:
            case Brush::Rotate:
            case Brush::Pose:
            case Brush::Boundary:
            case Brush::Mask:
            case Brush::Freeze:
            case Brush::Paint:
            case Brush::Count: break;
            }
            out[i] = d;
            if (paint && mSettings.cavityPaint)
            {
                const double inward = -glm::dot(glm::dvec3(mNormals[p]), d);
                paintOut[i] = inward > 0.0 ? float(std::min(1.0, inward / std::max(1e-9, r * kDrawStep * 0.5))) : 0.0f;
            }
        }
    });
    for (size_t i = 0; i < n; ++i)
    {
        // A point reached but left still counts toward the blend, so a copy's move fades out with it instead of jumping.
        if (reached[i])
        {
            const Index p = s.points[i];
            touch(p);
            mDisp[p] += out[i] * s.weights[i];
            mBlend[p] += s.weights[i];
        }
        if (paint && paintOut[i] > 0.0f)
        {
            touch(s.points[i]);
            mPaint[s.points[i]] = std::max(mPaint[s.points[i]], paintOut[i]);
        }
    }
}

void SculptMesh::addPaint(const Side& s)
{
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        const Index p = s.points[i];
        const double w = s.weights[i] * mSettings.strength * s.dab.opacity * s.dab.flow * (1.0 - double(mMask[p]));
        if (w <= 0.0)
            continue;
        touch(p);
        mPaint[p] = std::max(mPaint[p], float(std::min(w, 1.0)));
    }
}

void SculptMesh::addMaskChange(const Side& s)
{
    const double sign = mSettings.invert ? -1.0 : 1.0;
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        const Index p = s.points[i];
        touch(p);
        if (mSettings.brush != Brush::Mask)
        {
            mFreezeSet[p] = mSettings.invert ? -1 : 1;
            continue;
        }
        const double amount = s.weights[i] * mSettings.strength * s.dab.opacity * s.dab.flow;
        double change = amount * sign;
        if (mSettings.blur)
        {
            float sum = mMask[p];
            for (Index q : neighbours(p))
                sum += mMask[q];
            change = (double(sum) / double(neighbours(p).size() + 1) - double(mMask[p])) * std::min(amount, 1.0);
        }
        mMaskDelta[p] += float(change * s.weights[i]);
        mBlend[p] += s.weights[i];
    }
}

void SculptMesh::blendSides()
{
    for (Index p : mChanged)
        if (mBlend[p] > 0.0)
        {
            mDisp[p] /= mBlend[p];
            mMaskDelta[p] = float(double(mMaskDelta[p]) / mBlend[p]);
        }
}

void SculptMesh::symmetrizeChanges()
{
    if (mSymmetry.empty())
        return;
    if (mSymmetry.isRadial())
    {
        const size_t count = mChanged.size();
        for (int g = 1; g < mSymmetry.order(); ++g)
            for (size_t i = 0; i < count; ++i)
                if (Index q = mSymmetry.image(g, mChanged[i]); q != geo::kInvalid)
                    touch(q);
        // Every orbit's displacement and mask change become the symmetric average of its members'.
        const int n = mSymmetry.order();
        for (Index p : mChanged)
        {
            if (mSymmetry.orbitPoint(p, 0) != p)
                continue;
            glm::dvec3 disp(0.0);
            float mask = 0.0f, paint = 0.0f;
            for (int g = 0; g < n; ++g)
            {
                const Index q = mSymmetry.orbitPoint(p, g);
                disp += mSymmetry.transformVector(mSymmetry.inverse(g), mDisp[q]);
                mask += mMaskDelta[q];
                paint += mPaint[q];
            }
            disp /= double(n);
            mask /= float(n);
            paint /= float(n);
            for (int g = 0; g < n; ++g)
            {
                const Index q = mSymmetry.orbitPoint(p, g);
                mDisp[q] = mSymmetry.transformVector(g, disp);
                mMaskDelta[q] = mask;
                mPaint[q] = paint;
            }
        }
        return;
    }
    const size_t count = mChanged.size();
    for (size_t i = 0; i < count; ++i)
        if (Index q = mSymmetry.mirrorPoint(mChanged[i]); q != geo::kInvalid)
            touch(q);
    const geo::SymmetryPlane& pl = mSymmetry.plane();
    for (Index p : mChanged)
    {
        const Index q = mSymmetry.mirrorPoint(p);
        if (q == geo::kInvalid || q < p)
            continue;
        if (q == p)
        {
            mDisp[p] = tangential(mDisp[p], pl.normal);
            continue;
        }
        const glm::dvec3 avg = (mDisp[p] + reflectVector(pl, mDisp[q])) * 0.5;
        mDisp[p] = avg;
        mDisp[q] = reflectVector(pl, avg);
        const float m = (mMaskDelta[p] + mMaskDelta[q]) * 0.5f;
        mMaskDelta[p] = mMaskDelta[q] = m;
        const float paint = (mPaint[p] + mPaint[q]) * 0.5f;
        mPaint[p] = mPaint[q] = paint;
        const int8_t f = mFreezeSet[p] ? mFreezeSet[p] : mFreezeSet[q];
        mFreezeSet[p] = mFreezeSet[q] = f;
    }
}

void SculptMesh::commitChanges()
{
    LILA_ZONE();

    std::vector<Index> moved, recolored, retinted;
    bool maskChanged = false;
    const glm::vec4 brushColor(mSettings.color, 1.0f);
    // Blurring paint mixes each point toward its neighbours' colours as they stood before this dab.
    const bool blurColor = mSettings.brush == Brush::Paint && mSettings.blur && !mColor.empty();
    std::vector<glm::vec4> blurred(blurColor ? mChanged.size() : 0);
    if (blurColor)
        tbb::parallel_for(size_t(0), mChanged.size(), [&](size_t i) {
            const Index p = mChanged[i];
            glm::vec4 sum = mColor[p];
            for (Index q : neighbours(p))
                sum += mColor[q];
            blurred[i] = sum / float(neighbours(p).size() + 1);
        });
    for (size_t i = 0; i < mChanged.size(); ++i)
    {
        const Index p = mChanged[i];
        const glm::vec4 target = blurColor ? blurred[i] : brushColor;
        const bool move = mDisp[p] != glm::dvec3(0.0);
        const float newMask = std::clamp(mMask[p] + mMaskDelta[p], 0.0f, 1.0f);
        const uint8_t newFrozen = mFreezeSet[p] > 0 ? 1 : mFreezeSet[p] < 0 ? 0 : mFrozen[p];
        const bool recolor = mPaint[p] > 0.0f && !mColor.empty() && mColor[p] != target;
        if (!move && !recolor && newMask == mMask[p] && newFrozen == mFrozen[p])
            continue;
        if (mStroking && mStrokeStamp[p] != mStroke)
        {
            mStrokeStamp[p] = mStroke;
            mRecordIndex[p] = uint32_t(mRecord.points.size());
            mRecord.points.push_back(p);
            mRecord.before.push_back(mMesh.position(p));
            mRecord.maskBefore.push_back(mMask[p]);
            mRecord.frozenBefore.push_back(mFrozen[p]);
            if (!mColor.empty())
                mRecord.colorBefore.push_back(mColor[p]);
        }
        if (move)
        {
            mMesh.setPosition(p, mMesh.position(p) + mDisp[p]);
            moved.push_back(p);
        }
        const bool retint = newMask != mMask[p] || newFrozen != mFrozen[p];
        maskChanged |= retint;
        mMask[p] = newMask;
        mFrozen[p] = newFrozen;
        if (recolor)
        {
            mColor[p] = glm::mix(mColor[p], target, std::min(mPaint[p], 1.0f));
            recolored.push_back(p);
        }
        else if (retint)
            retinted.push_back(p);
    }
    mChanged.clear();
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
    if (!recolored.empty())
    {
        writeVertices(recolored);
        ++mColorVersion;
    }
    writeVertices(retinted);
    if (maskChanged)
        ++mMaskVersion;
}

void SculptMesh::dab(const Dab& d)
{
    LILA_ZONE();

    if (!mStroking)
        beginStroke(mSettings);
    if (++mDabId == 0)
    {
        std::fill(mDabStamp.begin(), mDabStamp.end(), 0u);
        mDabId = 1;
    }
    mChanged.clear();

    Dab first = d;
    first.normal = glm::length(d.normal) > 0.0 ? glm::normalize(d.normal) : glm::dvec3(0.0, 1.0, 0.0);
    // The mirror plane's copies come after the radial ones, so grab keeps each side's gathered points by position in the list.
    std::vector<Side> sides;
    if (mSymmetry.isRadial())
        for (Dab& copy : brush::orbitDabs(first, mSymmetry))
            sides.push_back(Side{.dab = std::move(copy)});
    else
        for (Dab& copy : brush::symmetricDabs(first, mSymmetry.empty() ? nullptr : &mSymmetry.plane(), mSettings.radial))
            sides.push_back(Side{.dab = std::move(copy)});

    if (heldRegion(mSettings.brush))
    {
        if (mGrab.empty())
        {
            mGrab = sides;
            for (Side& s : mGrab)
                gatherHeld(s);
        }
        for (size_t k = 0; k < mGrab.size() && k < sides.size(); ++k)
            addHeldDisplacement(mGrab[k], sides[k].dab.delta, d.opacity * d.flow);
    }
    else
        for (size_t k = 0; k < sides.size(); ++k)
        {
            Side& s = sides[k];
            gather(s, false);
            if (movesPoints(mSettings.brush))
            {
                computeArea(s);
                if (mSettings.brush == Brush::Planar)
                {
                    if (mPlanes.size() <= k)
                        mPlanes.emplace_back(s.areaCenter, s.areaNormal);
                    std::tie(s.areaCenter, s.areaNormal) = mPlanes[k];
                }
                addDisplacement(s);
            }
            else if (mSettings.brush == Brush::Paint)
                addPaint(s);
            else
                addMaskChange(s);
        }
    blendSides();
    symmetrizeChanges();
    commitChanges();
}

void SculptMesh::gatherHeld(Side& s)
{
    const Brush brush = mSettings.brush;
    const double r = s.dab.radius;
    if (brush == Brush::ElasticGrab)
    {
        s.dab.radius = r * kElasticReach;
        gather(s, true);
        s.dab.radius = r;
        for (size_t i = 0; i < s.points.size(); ++i)
        {
            const double t = glm::length(mMesh.position(s.points[i]) - s.dab.center) / r;
            s.weights[i] = std::pow(1.0 + t * t, -1.5) * (1.0 - t / kElasticReach);
        }
        return;
    }
    gather(s, true);
    computeArea(s);
    if (brush != Brush::Pose && brush != Brush::Boundary)
        return;
    std::vector<uint8_t> inside(mMesh.pointCapacity(), 0);
    for (Index p : s.points)
        inside[p] = 1;
    if (brush == Brush::Pose)
    {
        // The joint is the ring where the region meets the rest of the mesh; the region bends about it rigidly,
        // easing in over the half of the way from the ring to where the stroke began.
        glm::dvec3 ring(0.0);
        size_t count = 0;
        for (Index p : s.points)
            for (Index q : neighbours(p))
                if (!inside[q])
                {
                    ring += mMesh.position(p);
                    ++count;
                    break;
                }
        s.pivot = count ? ring / double(count) : s.dab.center;
        const double reach = std::max(glm::length(s.dab.center - s.pivot), r * 1e-3);
        for (size_t i = 0; i < s.points.size(); ++i)
        {
            const double t = std::clamp(glm::length(mMesh.position(s.points[i]) - s.pivot) / (reach * 0.5), 0.0, 1.0);
            s.weights[i] = t * t * (3.0 - 2.0 * t);
        }
        return;
    }
    // Boundary: rings in from the open border, each a step weaker; points further in stay put.
    std::vector<int> ring(mMesh.pointCapacity(), -1);
    std::vector<Index> front;
    for (Index p : s.points)
        if (mMesh.isBoundaryPoint(p))
        {
            ring[p] = 0;
            front.push_back(p);
        }
    for (int k = 1; k <= kBoundaryRings && !front.empty(); ++k)
    {
        std::vector<Index> next;
        for (Index p : front)
            for (Index q : neighbours(p))
                if (inside[q] && ring[q] < 0)
                {
                    ring[q] = k;
                    next.push_back(q);
                }
        front = std::move(next);
    }
    size_t kept = 0;
    for (size_t i = 0; i < s.points.size(); ++i)
        if (const int k = ring[s.points[i]]; k >= 0)
        {
            s.points[kept] = s.points[i];
            s.weights[kept++] = s.weights[i] * (1.0 - double(k) / double(kBoundaryRings + 1));
        }
    s.points.resize(kept);
    s.weights.resize(kept);
}

void SculptMesh::addHeldDisplacement(Side& s, const glm::dvec3& delta, double amount)
{
    const Brush brush = mSettings.brush;
    s.travel += delta;
    const glm::dvec3 N = s.areaNormal, C = s.dab.center;
    // Rotate turns by the angle the cursor has swept around where the stroke began, once it has left the centre.
    double angle = 0.0;
    glm::dvec3 axis = N, pivot = C;
    if (brush == Brush::Rotate)
    {
        const glm::dvec3 now = tangential(s.travel, N);
        if (glm::length(s.rotateFrom) == 0.0)
        {
            if (glm::length(now) < s.dab.radius * 0.05)
                return;
            s.rotateFrom = glm::normalize(now);
            return;
        }
        if (glm::length(now) < 1e-12)
            return;
        angle = std::atan2(glm::dot(glm::cross(s.rotateFrom, now), N), glm::dot(s.rotateFrom, now));
    }
    else if (brush == Brush::Pose)
    {
        // The part swings about its joint so the point under the cursor follows it.
        pivot = s.pivot;
        const glm::dvec3 from = C - pivot, to = C + s.travel - pivot;
        const glm::dvec3 c = glm::cross(from, to);
        if (glm::length(c) < 1e-12 * glm::length(from) * glm::length(to))
            return;
        axis = glm::normalize(c);
        angle = std::atan2(glm::length(c), glm::dot(from, to));
    }
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        const Index p = s.points[i];
        const double w = mFrozen[p] ? 0.0 : s.weights[i] * (1.0 - double(mMask[p])) * amount;
        if (w <= 0.0)
            continue;
        touch(p);
        glm::dvec3 d = brush == Brush::Grab && mSettings.invert ? N * (glm::dot(delta, N) * w) : delta * w;
        // Absolute from where the stroke began, so the turn follows the cursor back as well as forward.
        if (brush == Brush::Rotate || brush == Brush::Pose)
            d = pivot + rotateAbout(originalPosition(p) - pivot, axis, angle * std::min(w, 1.0)) - mMesh.position(p);
        mDisp[p] += d * s.weights[i];
        mBlend[p] += s.weights[i];
    }
}

StrokeRecord SculptMesh::endStroke()
{
    mStroking = false;
    mGrab.clear();
    StrokeRecord r = std::move(mRecord);
    mRecord = {};
    r.after.resize(r.points.size());
    r.maskAfter.resize(r.points.size());
    r.frozenAfter.resize(r.points.size());
    const bool colored = !mColor.empty() && r.colorBefore.size() == r.points.size();
    if (colored)
        r.colorAfter.resize(r.points.size());
    for (size_t i = 0; i < r.points.size(); ++i)
    {
        r.after[i] = mMesh.position(r.points[i]);
        r.maskAfter[i] = mMask[r.points[i]];
        r.frozenAfter[i] = mFrozen[r.points[i]];
        if (colored)
            r.colorAfter[i] = mColor[r.points[i]];
    }
    if (++mStroke == 0)
    {
        std::fill(mStrokeStamp.begin(), mStrokeStamp.end(), 0u);
        mStroke = 1;
    }
    return r;
}

void SculptMesh::apply(const StrokeRecord& r, bool undo)
{
    LILA_ZONE();

    const auto& pos = undo ? r.before : r.after;
    const auto& mask = undo ? r.maskBefore : r.maskAfter;
    const auto& frozen = undo ? r.frozenBefore : r.frozenAfter;
    const auto& colors = undo ? r.colorBefore : r.colorAfter;
    if (!colors.empty())
        ensureColor();
    std::vector<Index> moved, recolored, retinted;
    bool maskChanged = false;
    for (size_t i = 0; i < r.points.size(); ++i)
    {
        const Index p = r.points[i];
        if (p >= mMesh.pointCapacity())
            continue;
        if (i < pos.size() && pos[i] != mMesh.position(p))
        {
            mMesh.setPosition(p, pos[i]);
            moved.push_back(p);
        }
        bool retint = false;
        if (i < mask.size() && mask[i] != mMask[p])
        {
            mMask[p] = mask[i];
            retint = true;
        }
        if (i < frozen.size() && frozen[i] != mFrozen[p])
        {
            mFrozen[p] = frozen[i];
            retint = true;
        }
        maskChanged |= retint;
        if (i < colors.size() && colors[i] != mColor[p])
        {
            mColor[p] = colors[i];
            recolored.push_back(p);
        }
        else if (retint)
            retinted.push_back(p);
    }
    writeVertices(retinted);
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
    if (!recolored.empty())
    {
        writeVertices(recolored);
        ++mColorVersion;
    }
    if (maskChanged)
        ++mMaskVersion;
}

StrokeRecord SculptMesh::maskOp(MaskOp op, Layer layer, int iterations)
{
    LILA_ZONE();

    const Index n = mMesh.pointCapacity();
    std::vector<float> next(mMask);
    std::vector<uint8_t> nextFrozen(mFrozen);
    if (layer == Layer::Freeze)
    {
        for (Index p = 0; p < n; ++p)
            switch (op)
            {
            case MaskOp::Clear: nextFrozen[p] = 0; break;
            case MaskOp::Fill: nextFrozen[p] = 1; break;
            case MaskOp::Invert: nextFrozen[p] = mFrozen[p] ? 0 : 1; break;
            case MaskOp::Blur: break;
            }
    }
    else if (op == MaskOp::Blur)
    {
        std::vector<float> cur(mMask);
        for (int it = 0; it < std::max(1, iterations); ++it)
        {
            tbb::parallel_for(Index(0), n, [&](Index p) {
                auto nb = neighbours(p);
                if (nb.empty())
                    return;
                float sum = cur[p];
                for (Index q : nb)
                    sum += cur[q];
                next[p] = sum / float(nb.size() + 1);
            });
            cur = next;
        }
    }
    else
        for (Index p = 0; p < n; ++p)
            next[p] = op == MaskOp::Clear ? 0.0f : op == MaskOp::Fill ? 1.0f : 1.0f - mMask[p];

    StrokeRecord r;
    for (Index p = 0; p < n; ++p)
        if (mMesh.pointValid(p) && (next[p] != mMask[p] || nextFrozen[p] != mFrozen[p]))
        {
            r.points.push_back(p);
            r.maskBefore.push_back(mMask[p]);
            r.maskAfter.push_back(next[p]);
            r.frozenBefore.push_back(mFrozen[p]);
            r.frozenAfter.push_back(nextFrozen[p]);
            mMask[p] = next[p];
            mFrozen[p] = nextFrozen[p];
        }
    writeVertices(r.points);
    if (!r.empty())
        ++mMaskVersion;
    return r;
}

void SculptMesh::setPositions(std::span<const glm::dvec3> positions)
{
    std::vector<Index> moved;
    for (Index p = 0; p < mMesh.pointCapacity() && p < positions.size(); ++p)
        if (mMesh.pointValid(p) && positions[p] != mMesh.position(p))
        {
            mMesh.setPosition(p, positions[p]);
            moved.push_back(p);
        }
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
}

void SculptMesh::setMask(std::span<const float> mask)
{
    std::vector<Index> all;
    for (Index p = 0; p < mMask.size() && p < mask.size(); ++p)
    {
        mMask[p] = std::clamp(mask[p], 0.0f, 1.0f);
        all.push_back(p);
    }
    writeVertices(all);
    ++mMaskVersion;
}

void SculptMesh::setColors(std::span<const glm::vec4> colors)
{
    ensureColor();
    std::vector<Index> all;
    for (Index p = 0; p < mColor.size() && p < colors.size(); ++p)
    {
        mColor[p] = colors[p];
        all.push_back(p);
    }
    writeVertices(all);
    ++mColorVersion;
}

StrokeRecord SculptMesh::colorChange(const std::vector<glm::vec4>& next)
{
    StrokeRecord r;
    for (Index p = 0; p < mColor.size(); ++p)
        if (mMesh.pointValid(p) && next[p] != mColor[p])
        {
            r.points.push_back(p);
            r.colorBefore.push_back(mColor[p]);
            r.colorAfter.push_back(next[p]);
        }
    apply(r, false);
    return r;
}

StrokeRecord SculptMesh::fillColor(const glm::vec3& color)
{
    ensureColor();
    std::vector<glm::vec4> next(mColor);
    for (Index p = 0; p < next.size(); ++p)
        next[p] = glm::mix(mColor[p], glm::vec4(color, 1.0f), 1.0f - mMask[p]);
    return colorChange(next);
}

StrokeRecord SculptMesh::clearColor()
{
    if (mColor.empty())
        return {};
    return colorChange(std::vector<glm::vec4>(mColor.size(), kWhite));
}

void SculptMesh::refresh(std::span<const Index> moved)
{
    LILA_ZONE();

    mBvh.refitPoints(mMesh, moved);
    // Normals change one ring out from every moved point.
    if (++mGatherId == 0)
    {
        std::fill(mGatherStamp.begin(), mGatherStamp.end(), 0u);
        mGatherId = 1;
    }
    std::vector<Index> ring;
    ring.reserve(moved.size() * 2);
    for (Index p : moved)
    {
        if (mGatherStamp[p] != mGatherId)
        {
            mGatherStamp[p] = mGatherId;
            ring.push_back(p);
        }
        for (Index q : neighbours(p))
            if (mGatherStamp[q] != mGatherId)
            {
                mGatherStamp[q] = mGatherId;
                ring.push_back(q);
            }
    }
    updateNormals(ring);
    writeVertices(ring);
}

void SculptMesh::updateNormals(std::span<const Index> points)
{
    LILA_ZONE();

    const auto& corners = mBvh.triangles().corners;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 2048), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            const Index p = points[i];
            glm::dvec3 n(0.0);
            for (uint32_t k = mTriOffsets[p]; k < mTriOffsets[p + 1]; ++k)
            {
                const Index* c = &corners[size_t(mTris[k]) * 3];
                const glm::dvec3 a = mMesh.position(mMesh.cornerPoint(c[0]));
                n += glm::cross(mMesh.position(mMesh.cornerPoint(c[1])) - a, mMesh.position(mMesh.cornerPoint(c[2])) - a);
            }
            const double len = glm::length(n);
            if (len > 1e-300)
                mNormals[p] = glm::vec3(n / len);
        }
    });
}

void SculptMesh::writeVertices(std::span<const Index> points)
{
    if (!mRender)
        return;
    render::DynamicVertices& dv = *mRender->dynamic;
    std::scoped_lock lock(dv.mutex);
    for (Index p : points)
    {
        render::MeshVertex v{};
        v.pos = glm::vec3(mMesh.position(p));
        v.normal = mNormals[p];
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        v.color = shownColor(p);
        for (uint32_t k = mPointVertOffsets[p]; k < mPointVertOffsets[p + 1]; ++k)
        {
            v.uv = v.uv1 = mVertUv[mPointVerts[k]];
            dv.write(mPointVerts[k], v);
        }
    }
}

glm::vec4 SculptMesh::shownColor(Index p) const
{
    glm::vec4 c = mColor.empty() ? kWhite : mColor[p];
    if (mFrozen[p])
        return glm::vec4(glm::mix(glm::vec3(c), glm::vec3(0.92f), 0.45f), c.a);
    return glm::vec4(glm::mix(glm::vec3(c), glm::vec3(0.02f), 0.7f * mMask[p]), c.a);
}

std::shared_ptr<const render::RenderMesh> SculptMesh::renderMesh()
{
    if (mRender)
        return mRender;
    LILA_ZONE();

    // Split at uv seams and grouped by face material, so the mesh's own materials and textures draw on the sculpt.
    geo::RenderMeshOptions opts;
    opts.maxUvSets = 1;
    opts.colors = false;
    opts.wireframe = false;
    const geo::RenderMesh layout = geo::buildRenderMesh(mMesh, opts);
    const Index n = layout.vertexCount();
    const bool uvs = !layout.uvs.empty() && layout.uvs[0].size() == size_t(n);
    mVertUv.assign(size_t(n), glm::vec2(0.0f));
    mPointVertOffsets.assign(size_t(mMesh.pointCapacity()) + 1, 0);
    for (Index v = 0; v < n; ++v)
        ++mPointVertOffsets[size_t(layout.vertexPoint[v]) + 1];
    for (size_t i = 1; i < mPointVertOffsets.size(); ++i)
        mPointVertOffsets[i] += mPointVertOffsets[i - 1];
    mPointVerts.resize(size_t(n));
    {
        std::vector<uint32_t> fill(mPointVertOffsets.begin(), mPointVertOffsets.end() - 1);
        for (Index v = 0; v < n; ++v)
            mPointVerts[fill[layout.vertexPoint[v]]++] = uint32_t(v);
    }

    auto rm = std::make_shared<render::RenderMesh>();
    rm->dynamic = std::make_shared<render::DynamicVertices>();
    auto& verts = rm->dynamic->vertices;
    verts.resize(size_t(n));
    tbb::parallel_for(Index(0), n, [&](Index i) {
        const Index p = layout.vertexPoint[i];
        render::MeshVertex& v = verts[i];
        v.pos = glm::vec3(mMesh.position(p));
        v.normal = mNormals[p];
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        mVertUv[i] = uvs ? layout.uvs[0][i] : glm::vec2(0.0f);
        v.uv = v.uv1 = mVertUv[i];
        v.color = shownColor(p);
    });
    rm->indices.assign(layout.indices.begin(), layout.indices.end());
    for (const geo::RenderSection& sec : layout.sections)
        rm->sections.push_back({uint32_t(std::max(0, sec.material)), sec.firstIndex, sec.indexCount});
    // Which triangle sides are polygon edges, so the shader draws the wireframe without the triangulation's diagonals.
    const size_t tris = layout.indices.size() / 3;
    rm->edgeMask.assign((tris + 7) / 8, 0u);
    tbb::parallel_for(size_t(0), rm->edgeMask.size(), [&](size_t w) {
        uint32_t word = 0;
        for (size_t t = w * 8; t < std::min(w * 8 + 8, tris); ++t)
            for (int k = 0; k < 3; ++k)
                if (layout.triangleSides[t] & (1u << k))
                    word |= 1u << ((t & 7) * 4 + size_t(k));
        rm->edgeMask[w] = word;
    });
    // Strokes may push the surface past its starting bounds, so leave room.
    geo::Bounds b = mMesh.bounds();
    glm::dvec3 pad = b.size() * 0.25 + glm::dvec3(1e-3);
    rm->boundsMin = glm::vec3(b.min - pad);
    rm->boundsMax = glm::vec3(b.max + pad);
    mRender = rm;
    return mRender;
}
} // namespace lila::sculpt
