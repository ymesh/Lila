#include "Session.h"

#include <lila/Profile.h>
#include <lila/Undo.h>

#include <algorithm>
#include <cmath>

using namespace lila;

namespace stagehand
{
Session::Session(Graph& g, const WorldOptions& o) : mWorld(std::make_unique<World>(g, o))
{
    mPinned.assign(mWorld->scene().size(), 0);
}

Session::~Session() = default;

void Session::advance(double seconds)
{
    if (mPaused)
        return;
    mCarry += std::max(0.0, seconds) * mTimeScale;
    // A stalled frame catches up a little, never a long freeze's worth.
    mCarry = std::min(mCarry, 0.1);
    while (mCarry >= mStep)
    {
        stepOnce();
        mCarry -= mStep;
    }
}

void Session::stepOnce()
{
    physics::RigidScene& s = mWorld->scene();
    if (mHeld >= 0)
        s.pull(mHeld, mHeldLocal, mHand, grip, mStep);
    for (auto it = mShakes.begin(); it != mShakes.end();)
    {
        const double t = (mTime - it->start) / std::max(it->seconds, 1e-6);
        if (t >= 1.0)
        {
            s.moveTo(it->body, it->rest, mStep);
            s.setPose(it->body, it->rest);
            s.setRole(it->body, it->role);
            it = mShakes.erase(it);
            continue;
        }
        // Four shakes over the time, fading out.
        const double wave = std::sin(t * 8.0 * kPi) * (1.0 - t);
        s.moveTo(it->body, {it->rest.position + it->amplitude * wave, it->rest.rotation}, mStep);
        ++it;
    }
    s.step(mStep);
    mTime += mStep;
    if (onStep)
    {
        std::vector<glm::dmat4> poses(mWorld->movers());
        for (size_t i = 0; i < poses.size(); ++i)
            poses[i] = mWorld->worldMatrix(int(i));
        onStep(mTime, poses);
    }
}

int Session::pick(const glm::dvec3& origin, const glm::dvec3& direction, glm::dvec3* at) const
{
    const auto hit = mWorld->scene().raycast(origin, direction, 1e7);
    if (!hit || hit->body < 0 || size_t(hit->body) >= mWorld->movers())
        return -1;
    if (at)
        *at = hit->point;
    return hit->body;
}

void Session::grab(int body, const glm::dvec3& at)
{
    if (body < 0 || size_t(body) >= mWorld->movers())
        return;
    const physics::Pose p = mWorld->scene().pose(body);
    mHeld = body;
    mHeldLocal = glm::conjugate(p.rotation) * (at - p.position);
    mHand = at;
    mWorld->scene().wake(body);
}

void Session::drag(const glm::dvec3& hand) { mHand = hand; }

void Session::release() { mHeld = -1; }

void Session::push(int body, const glm::dvec3& at, const glm::dvec3& velocity)
{
    physics::RigidScene& s = mWorld->scene();
    if (body < 0 || size_t(body) >= mWorld->movers())
        return;
    s.impulse(body, velocity * s.mass(body), at);
}

void Session::wiggle(const glm::dvec3& center, double radius, double speed)
{
    physics::RigidScene& s = mWorld->scene();
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (size_t i = 0; i < mWorld->movers(); ++i)
    {
        if (mPinned[i])
            continue;
        const glm::dvec3 c = s.centerOfMass(int(i));
        const double d = glm::length(c - center);
        if (d > radius)
            continue;
        const double fall = 1.0 - d / std::max(radius, 1e-6);
        // Mostly sideways and a little up, so things shuffle and resettle rather than fly.
        const glm::dvec3 dir = glm::normalize(glm::dvec3(u(mRandom), 0.35 + 0.3 * std::abs(u(mRandom)), u(mRandom)));
        glm::dvec3 lo, hi;
        s.bounds(int(i), lo, hi);
        const glm::dvec3 at = c + glm::dvec3(u(mRandom), u(mRandom), u(mRandom)) * (hi - lo) * 0.25;
        s.impulse(int(i), dir * speed * fall * s.mass(int(i)), at);
    }
}

void Session::sweep(const glm::dvec3& center, const glm::dvec3& motion, double radius, double speed, bool gather)
{
    physics::RigidScene& s = mWorld->scene();
    const glm::dvec3 gravity = s.settings().gravity;
    const glm::dvec3 up = glm::length(gravity) > 1e-9 ? -glm::normalize(gravity) : glm::dvec3(0, 1, 0);
    auto flat = [&](const glm::dvec3& v) { return v - up * glm::dot(v, up); };
    const glm::dvec3 along = flat(motion);
    if (!gather && glm::length(along) < 1e-9)
        return;
    for (size_t i = 0; i < mWorld->movers(); ++i)
    {
        if (mPinned[i] || int(i) == mHeld)
            continue;
        const glm::dvec3 c = s.centerOfMass(int(i));
        const glm::dvec3 off = flat(center - c);
        const double d = glm::length(off);
        if (d > radius)
            continue;
        glm::dvec3 want;
        if (gather)
        {
            // Slows to a stop at the centre instead of overshooting it.
            if (d < 1e-6)
                continue;
            want = off / d * speed * std::min(1.0, 2.0 * d / std::max(radius, 1e-6));
        }
        else
            want = glm::normalize(along) * speed;
        const double fall = 1.0 - 0.5 * d / std::max(radius, 1e-6);
        // Takes on the broom's sideways velocity (half of it at the rim), so what it catches travels with it; falling
        // and toppling stay with the simulation.
        s.kick(int(i), (want - flat(s.velocity(int(i)))) * fall);
    }
}

void Session::drop()
{
    for (size_t i = 0; i < mWorld->movers(); ++i)
        if (!mPinned[i])
            mWorld->scene().wake(int(i));
}

void Session::shake(int body, const glm::dvec3& amplitude, double seconds)
{
    physics::RigidScene& s = mWorld->scene();
    if (body < 0 || size_t(body) >= s.size())
        return;
    for (const Shake& k : mShakes)
        if (k.body == body)
            return;
    Shake k;
    k.body = body;
    k.amplitude = amplitude;
    k.start = mTime;
    k.seconds = seconds;
    k.rest = s.pose(body);
    k.role = s.desc(body).role;
    s.setRole(body, physics::Role::Kinematic);
    mShakes.push_back(k);
    for (size_t i = 0; i < mWorld->movers(); ++i)
        s.wake(int(i));
}

void Session::pin(int body, bool on)
{
    if (body < 0 || size_t(body) >= mWorld->movers())
        return;
    mPinned[size_t(body)] = on ? 1 : 0;
    mWorld->scene().setRole(body, on ? physics::Role::Static : physics::Role::Dynamic);
}

bool Session::pinned(int body) const { return body >= 0 && size_t(body) < mPinned.size() && mPinned[size_t(body)]; }

void Session::show(Graph& g)
{
    LILA_ZONE();

    unrecorded([&] {
        mWorld->write(g);
        for (const Shake& k : mShakes)
            setWorld(g, mWorld->node(k.body), mWorld->worldMatrix(k.body));
    });
}

void Session::commit(Graph& g, const std::string& label)
{
    LILA_ZONE();

    std::vector<glm::dmat4> finals(mWorld->movers());
    for (size_t i = 0; i < finals.size(); ++i)
        finals[i] = mWorld->worldMatrix(int(i));
    // Shaken colliders end where they started; only the movers change.
    unrecorded([&] {
        mWorld->revert(g);
        for (const Shake& k : mShakes)
            setWorld(g, mWorld->node(k.body), mWorld->startMatrix(k.body));
    });
    UndoChunk chunk(label);
    for (size_t i = 0; i < finals.size(); ++i)
        if (finals[i] != mWorld->startMatrix(int(i)))
            setWorld(g, mWorld->node(int(i)), finals[i]);
}

void Session::revert(Graph& g)
{
    unrecorded([&] {
        mWorld->revert(g);
        for (const Shake& k : mShakes)
            setWorld(g, mWorld->node(k.body), mWorld->startMatrix(k.body));
    });
}
} // namespace stagehand
