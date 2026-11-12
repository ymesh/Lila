#pragma once

#include "Stagehand.h"

#include <lila/Undo.h>

#include <functional>
#include <memory>
#include <random>

namespace stagehand
{
/** Runs fn with undo recording off, restoring it after. */
template <class Fn> void unrecorded(Fn&& fn)
{
    lila::UndoStack& u = lila::UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    try
    {
        fn();
    }
    catch (...)
    {
        u.setEnabled(was);
        throw;
    }
    u.setEnabled(was);
}

/**
 * Live physics over the scene for interactive tools: the world steps in real time while a hand grabs, pushes,
 * wiggles, drops and shakes things, and the transforms follow without filling the undo stack. commit() keeps the
 * result as one undo step; revert() puts everything back.
 *
 * Nothing here knows about the viewport or the mouse, so other tools can drive it: an animation tool records each
 * step through onStep and keys the poses instead of committing them.
 */
class Session
{
public:
    Session(Graph& g, const WorldOptions& o);
    ~Session();

    World& world() { return *mWorld; }

    /** Advances real time, in fixed steps of `step` seconds; the remainder carries to the next call. */
    void advance(double seconds);
    double time() const { return mTime; }
    double step() const { return mStep; }
    void setPaused(bool on) { mPaused = on; }
    bool paused() const { return mPaused; }
    /** Slows or speeds up the simulation against real time. */
    void setTimeScale(double s) { mTimeScale = s; }

    /** The mover under a ray, with where it was hit, or -1. Colliders don't count. */
    int pick(const glm::dvec3& origin, const glm::dvec3& direction, glm::dvec3* at = nullptr) const;

    /** Holds a mover by a point on it; drag() moves the hand, release() lets go with whatever speed it had. */
    void grab(int body, const glm::dvec3& at);
    void drag(const glm::dvec3& hand);
    void release();
    int held() const { return mHeld; }
    /** How stiffly the hand holds, Hz. */
    double grip = 6.0;

    /** A shove: speed (cm/s) given to the body at the point along the direction. */
    void push(int body, const glm::dvec3& at, const glm::dvec3& velocity);
    /** Small random pushes to every mover within radius of a point, strongest at the centre. */
    void wiggle(const glm::dvec3& center, double radius, double speed);
    /**
     * A broom over every mover within radius of a point: each is steered along the stroke's motion at speed (cm/s), or
     * with gather toward the point, strongest at the centre. Call it every frame the brush is down.
     */
    void sweep(const glm::dvec3& center, const glm::dvec3& motion, double radius, double speed, bool gather);
    /** Wakes every mover so it falls under gravity from where it is. */
    void drop();
    /** Moves a collider or mover by an offset and back over `seconds`, shaking what rests on it. */
    void shake(int body, const glm::dvec3& amplitude, double seconds);
    /** Holds a mover still (static) until unpinned. */
    void pin(int body, bool on);
    bool pinned(int body) const;

    /** Puts the world's poses on the transforms without recording undo. */
    void show(Graph& g);
    /** The transforms as the session started, as one undo step from there to now. */
    void commit(Graph& g, const std::string& label);
    void revert(Graph& g);

    /** After every step: the step's time and each mover's world matrix. */
    std::function<void(double time, const std::vector<glm::dmat4>& movers)> onStep;

private:
    void stepOnce();

    std::unique_ptr<World> mWorld;
    double mTime = 0.0, mCarry = 0.0, mStep = 1.0 / 120.0, mTimeScale = 1.0;
    bool mPaused = false;

    int mHeld = -1;
    glm::dvec3 mHeldLocal{0.0}, mHand{0.0};

    struct Shake
    {
        int body = -1;
        glm::dvec3 amplitude{0.0};
        double start = 0.0, seconds = 0.0;
        physics::Pose rest;
        physics::Role role = physics::Role::Static;
    };
    std::vector<Shake> mShakes;
    std::vector<uint8_t> mPinned;
    std::mt19937 mRandom{0x5741u};
};
} // namespace stagehand
