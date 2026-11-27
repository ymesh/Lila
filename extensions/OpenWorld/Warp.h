#pragma once

#include "RemixSpec.h"

#include <vector>

namespace openworld
{
/**
 * The smooth warp of a remix: rigid motions inside move and keep regions, pins pulling points, and the land between
 * following by inverse-distance blending of their velocities. It is integrated in small steps, each checked to keep the
 * Jacobian positive (more steps where it would not), so the composed map never folds where the check samples: a flow of
 * fold-free steps is fold-free. Inside a region the velocity is exactly the region's, so regions stay rigid.
 */
class Warp
{
public:
    /** Cells per side of the grid the composed map is checked on, here and by remixCheck. */
    static constexpr int kFoldGrid = 128;

    struct Handle
    {
        Polygon polygon;
        Rigid motion;
    };

    Warp() = default;
    /** lo and hi bound the map the step checks sample. */
    Warp(std::vector<Handle> regions, std::vector<Pin> pins, double falloff, int maxSteps, Vec2 lo, Vec2 hi);

    bool identity() const { return mIdentity; }
    Vec2 forward(Vec2 p) const;
    Vec2 inverse(Vec2 q) const;
    int steps() const { return mSteps; }
    /** Smallest Jacobian determinant of one step over the check samples; 1 for the identity. */
    double minStepJacobian() const { return mMinStepDet; }

    struct Folds
    {
        /** Points checked. */
        int cells = 0;
        int folded = 0;
        /** Smallest Jacobian determinant: how much area the map keeps (negative where it folds). */
        double minJacobian = 1.0;
        std::vector<Vec2> where;
    };
    /** The composed map's Jacobian at (res + 1)^2 points over [lo, hi]: counts where it is not positive. */
    Folds folds(Vec2 lo, Vec2 hi, int res) const;

private:
    Vec2 velocity(int step, Vec2 p) const;
    double stepJacobian(int step, Vec2 p, double h) const;
    bool tryRun(int steps, const std::vector<Vec2>& probes, double h, double* minDet) const;

    std::vector<Handle> mRegions;
    std::vector<Pin> mPins;
    double mFalloff = 1.0;
    int mSteps = 0;
    double mMinStepDet = 1.0;
    bool mIdentity = true;
};

/** The warp's inverse on a grid over [lo, hi] (bilinear between samples), exact inside the given rigid targets. */
class InverseGrid
{
public:
    InverseGrid() = default;
    InverseGrid(const Warp& w, Vec2 lo, Vec2 hi, int res, std::vector<Warp::Handle> rigid);
    Vec2 operator()(Vec2 q) const;

private:
    const Warp* mWarp = nullptr;
    Vec2 mLo{0.0}, mStep{1.0};
    int mRes = 0;
    std::vector<Vec2> mAt;
    /** Targets (polygon after motion) and motions, for the exact inverse inside them. */
    std::vector<Warp::Handle> mRigid;
};
} // namespace openworld
