#pragma once

// Hand rig maths, free of the scene: finding hands and fingers in a skeleton, each finger joint's anatomical frame, the
// shapes (curl, spread, thumb, cup) and their spread over the joints, limits, blending and the built-in poses.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace puppeteer::hand
{
enum class Role : int
{
    Thumb,
    Index,
    Middle,
    Ring,
    Pinky,
};
inline constexpr int kRoles = 5;
/** Joints a pose holds per finger: proximal, middle and distal (the thumb's base, middle and tip joints). */
inline constexpr int kCanon = 3;

inline const char* roleName(Role r)
{
    static const char* names[kRoles] = {"thumb", "index", "middle", "ring", "pinky"};
    return names[int(r)];
}
inline const char* roleLabel(Role r)
{
    static const char* names[kRoles] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
    return names[int(r)];
}
inline std::optional<Role> roleFromName(const std::string& s)
{
    for (int r = 0; r < kRoles; ++r)
        if (s == roleName(Role(r)))
            return Role(r);
    return std::nullopt;
}

inline constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }

/** A joint's turn from straight, degrees: bend toward the palm, splay toward the thumb side, twist rolling the palm side toward the thumb. */
struct Angles
{
    double bend = 0.0, splay = 0.0, twist = 0.0;
    Angles operator+(const Angles& o) const { return {bend + o.bend, splay + o.splay, twist + o.twist}; }
    Angles operator-(const Angles& o) const { return {bend - o.bend, splay - o.splay, twist - o.twist}; }
    Angles operator*(double k) const { return {bend * k, splay * k, twist * k}; }
    bool operator==(const Angles&) const = default;
};
inline Angles mix(const Angles& a, const Angles& b, double t) { return a + (b - a) * t; }

/** One finger of a pose: its metacarpal (when the hand has them) and its three canonical joints. */
struct FingerPose
{
    Angles meta;
    std::array<Angles, kCanon> joints{};
    bool operator==(const FingerPose&) const = default;
};

/** A hand pose, independent of the skeleton: what each finger it holds does. */
struct Pose
{
    std::array<std::optional<FingerPose>, kRoles> fingers{};
};

/** Fingers an edit reaches, by role. */
using Mask = std::array<bool, kRoles>;
inline Mask allFingers() { return {true, true, true, true, true}; }

inline Pose mix(const Pose& a, const Pose& b, double t, const Mask& mask = allFingers())
{
    Pose out = a;
    for (int r = 0; r < kRoles; ++r)
    {
        if (!mask[r] || !b.fingers[r])
            continue;
        if (!a.fingers[r])
        {
            out.fingers[r] = b.fingers[r];
            continue;
        }
        FingerPose f;
        f.meta = mix(a.fingers[r]->meta, b.fingers[r]->meta, t);
        for (int j = 0; j < kCanon; ++j)
            f.joints[j] = mix(a.fingers[r]->joints[j], b.fingers[r]->joints[j], t);
        out.fingers[r] = f;
    }
    return out;
}

/** The fingers in mask from `edited`, the rest from `base`. */
inline Pose masked(const Pose& base, const Pose& edited, const Mask& mask)
{
    Pose out = base;
    for (int r = 0; r < kRoles; ++r)
        if (mask[r] && edited.fingers[r])
            out.fingers[r] = edited.fingers[r];
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Resampling a pose's three joints onto a finger's n (and back), keeping each channel's total.

/** values spread over n equal bins, re-binned into m: every bin's share of the total kept, so the sum is kept. */
inline std::vector<double> resample(const std::vector<double>& v, size_t m)
{
    const size_t n = v.size();
    std::vector<double> out(m, 0.0);
    if (n == 0 || m == 0)
        return out;
    if (n == m)
        return v;
    for (size_t i = 0; i < n; ++i)
    {
        const double a = double(i) / double(n), b = double(i + 1) / double(n);
        for (size_t k = 0; k < m; ++k)
        {
            const double c = double(k) / double(m), d = double(k + 1) / double(m);
            const double overlap = std::min(b, d) - std::max(a, c);
            if (overlap > 0.0)
                out[k] += v[i] * overlap / (b - a);
        }
    }
    return out;
}

/** Canonical joint angles onto n joints: bends re-binned by length of finger, splay and twist on the first joint. */
inline std::vector<Angles> toJoints(const std::array<Angles, kCanon>& c, size_t n)
{
    std::vector<double> bends(kCanon);
    double splay = 0.0, twist = 0.0;
    for (int j = 0; j < kCanon; ++j)
    {
        bends[j] = c[j].bend;
        splay += c[j].splay;
        twist += c[j].twist;
    }
    if (n == kCanon)
        return {c.begin(), c.end()};
    const std::vector<double> b = resample(bends, n);
    std::vector<Angles> out(n);
    for (size_t k = 0; k < n; ++k)
        out[k].bend = b[k];
    if (n > 0)
    {
        out[0].splay = splay;
        out[0].twist = twist;
    }
    return out;
}

inline std::array<Angles, kCanon> fromJoints(const std::vector<Angles>& js)
{
    std::array<Angles, kCanon> out{};
    if (js.size() == kCanon)
    {
        std::copy(js.begin(), js.end(), out.begin());
        return out;
    }
    std::vector<double> bends;
    double splay = 0.0, twist = 0.0;
    for (const Angles& a : js)
    {
        bends.push_back(a.bend);
        splay += a.splay;
        twist += a.twist;
    }
    const std::vector<double> b = resample(bends, kCanon);
    for (int j = 0; j < kCanon; ++j)
        out[j].bend = b[j];
    out[0].splay = splay;
    out[0].twist = twist;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Limits.

struct Range
{
    double lo = -180.0, hi = 180.0;
    double clamp(double v) const { return std::clamp(v, lo, hi); }
};
struct Limit
{
    Range bend, splay, twist;
    Angles clamp(const Angles& a) const { return {bend.clamp(a.bend), splay.clamp(a.splay), twist.clamp(a.twist)}; }
};

/** Natural ranges, degrees: the metacarpal, then the canonical joints, of a finger with this role. */
inline Limit metaLimit() { return {{-8, 22}, {-10, 10}, {-12, 22}}; }
inline std::array<Limit, kCanon> canonLimits(Role r)
{
    if (r == Role::Thumb)
        return {Limit{{-35, 60}, {-25, 75}, {-30, 50}}, Limit{{-20, 70}, {-8, 8}, {-10, 10}}, Limit{{-25, 90}, {-4, 4}, {-6, 6}}};
    return {Limit{{-35, 95}, {-28, 28}, {-15, 15}}, Limit{{-5, 115}, {-4, 4}, {-6, 6}}, Limit{{-20, 90}, {-4, 4}, {-6, 6}}};
}

/** The canonical ranges carried onto n joints the way angles are, so a curl that fits the canonical ranges fits these. */
inline std::vector<Limit> jointLimits(Role r, size_t n)
{
    const std::array<Limit, kCanon> c = canonLimits(r);
    if (n == kCanon)
        return {c.begin(), c.end()};
    std::vector<double> lo, hi;
    for (const Limit& l : c)
    {
        lo.push_back(l.bend.lo);
        hi.push_back(l.bend.hi);
    }
    const std::vector<double> l2 = resample(lo, n), h2 = resample(hi, n);
    std::vector<Limit> out(n);
    for (size_t k = 0; k < n; ++k)
    {
        out[k].bend = {l2[k], h2[k]};
        out[k].splay = k == 0 ? c[0].splay : c[1].splay;
        out[k].twist = k == 0 ? c[0].twist : c[1].twist;
    }
    return out;
}

inline FingerPose clamp(const FingerPose& f, Role r)
{
    FingerPose out = f;
    out.meta = metaLimit().clamp(f.meta);
    const std::array<Limit, kCanon> l = canonLimits(r);
    for (int j = 0; j < kCanon; ++j)
        out.joints[j] = l[j].clamp(f.joints[j]);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes: a few numbers per finger that spread naturally over its joints.

/** Degrees each canonical joint bends at curl 1, the curl of a fist. */
inline std::array<double, kCanon> fullCurl(Role r)
{
    if (r == Role::Thumb)
        return {0.0, 55.0, 80.0};
    return {90.0, 105.0, 75.0};
}

/**
 * A finger as curl (0 straight, 1 a fist's curl), tip (how the curl splits: -1 all in the knuckle, 0 even, 1 all in the
 * two outer joints, a hook), splay and twist (degrees, at the knuckle).
 */
struct FingerShape
{
    double curl = 0.0, tip = 0.0, splay = 0.0, twist = 0.0;
};

/** The thumb's base joint swings rather than curls, so its curl starts at its middle joint. */
inline int curlBase(Role r) { return r == Role::Thumb ? 1 : 0; }

inline std::array<double, kCanon> tipWeights(double tip, Role r)
{
    tip = std::clamp(tip, -1.0, 1.0);
    const double base = 1.0 - std::max(0.0, tip), outer = 1.0 + std::min(0.0, tip);
    std::array<double, kCanon> w{};
    for (int j = curlBase(r); j < kCanon; ++j)
        w[j] = j == curlBase(r) ? base : outer;
    return w;
}

/** A finger's joints for a shape (non-thumb fingers; the thumb's two outer joints use it too, its base aside). */
inline std::array<Angles, kCanon> anglesFor(const FingerShape& s, Role r)
{
    const std::array<double, kCanon> full = fullCurl(r), w = tipWeights(s.tip, r);
    std::array<Angles, kCanon> out{};
    for (int j = 0; j < kCanon; ++j)
        out[j].bend = s.curl * full[j] * w[j];
    if (r != Role::Thumb)
    {
        out[0].splay = s.splay;
        out[0].twist = s.twist;
    }
    return out;
}

/** Curl and tip that give these bends: the exact inverse of anglesFor for any curl it makes. */
inline void fitCurl(const std::array<Angles, kCanon>& a, Role r, double& curl, double& tip)
{
    const std::array<double, kCanon> full = fullCurl(r);
    const int b = curlBase(r);
    const double r0 = a[b].bend / full[b];
    double r12 = 0.0;
    for (int j = b + 1; j < kCanon; ++j)
        r12 += a[j].bend / full[j];
    r12 /= double(kCanon - b - 1);
    const bool knuckle = std::abs(r0) >= std::abs(r12);
    curl = knuckle ? r0 : r12;
    if (std::abs(curl) < 1e-9)
    {
        tip = 0.0;
        return;
    }
    tip = std::clamp(knuckle ? r12 / curl - 1.0 : 1.0 - r0 / curl, -1.0, 1.0);
}

inline FingerShape fitShape(const std::array<Angles, kCanon>& a, Role r)
{
    FingerShape s;
    fitCurl(a, r, s.curl, s.tip);
    s.splay = a[0].splay;
    s.twist = a[0].twist;
    return s;
}

/** The thumb: curl of its outer joints, across (its base swung over the palm, 1 to the pinky side), out (away from the palm), twist. */
struct ThumbShape
{
    double curl = 0.0, tip = 0.0, across = 0.0, out = 0.0, twist = 0.0;
};
inline constexpr double kThumbAcross = 45.0, kThumbOut = 50.0, kThumbRoll = 25.0;

inline std::array<Angles, kCanon> anglesFor(const ThumbShape& t)
{
    std::array<Angles, kCanon> a = anglesFor(FingerShape{t.curl, t.tip}, Role::Thumb);
    a[0] = {t.across * kThumbAcross, t.out * kThumbOut, t.across * kThumbRoll + t.twist};
    return a;
}
inline ThumbShape fitThumb(const std::array<Angles, kCanon>& a)
{
    ThumbShape t;
    fitCurl(a, Role::Thumb, t.curl, t.tip);
    t.across = a[0].bend / kThumbAcross;
    t.out = a[0].splay / kThumbOut;
    t.twist = a[0].twist - t.across * kThumbRoll;
    return t;
}

/** How far each finger fans at spread 1, degrees toward the thumb side. */
inline double fanOf(Role r)
{
    static const double fan[kRoles] = {0.0, 14.0, 3.0, -9.0, -18.0};
    return fan[int(r)];
}
/** What cup 1 does to each metacarpal: bend toward the palm and roll its palm side toward the thumb. */
inline Angles cupOf(Role r)
{
    static const Angles cup[kRoles] = {{0, 0, 0}, {2, 0, -3}, {4, 0, 0}, {10, 0, 6}, {16, 0, 12}};
    return cup[int(r)];
}
/** Cup on a hand without metacarpals: the knuckles roll and close instead. */
inline Angles cupKnuckleOf(Role r)
{
    static const Angles cup[kRoles] = {{0, 0, 0}, {1, -2, -4}, {2, 0, 0}, {4, 2, 6}, {7, 4, 12}};
    return cup[int(r)];
}

/** A whole hand as shapes, how the built-in poses are written. */
struct Shape
{
    std::array<FingerShape, 4> fingers{}; // index, middle, ring, pinky
    ThumbShape thumb;
    double cup = 0.0;
};

inline Pose poseFrom(const Shape& s)
{
    Pose p;
    FingerPose t;
    t.joints = anglesFor(s.thumb);
    t.joints[0].bend += s.cup * 0.3 * kThumbAcross;
    t.joints[0].twist += s.cup * 0.3 * kThumbRoll;
    p.fingers[0] = clamp(t, Role::Thumb);
    for (int f = 0; f < 4; ++f)
    {
        const Role r = Role(f + 1);
        FingerPose fp;
        fp.joints = anglesFor(s.fingers[f], r);
        fp.meta = cupOf(r) * s.cup;
        p.fingers[f + 1] = clamp(fp, r);
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------
// Hand level edits. Each measures the pose, then adds the change the new value makes to the shape, so whatever the pose
// holds beyond the shape (a hand-keyed twist, an odd joint) stays.

/** Mean curl of the non-thumb fingers in mask. */
inline double measureCurl(const Pose& p, const Mask& m = allFingers())
{
    double sum = 0.0;
    int n = 0;
    for (int r = 1; r < kRoles; ++r)
        if (m[r] && p.fingers[r])
        {
            sum += fitShape(p.fingers[r]->joints, Role(r)).curl;
            ++n;
        }
    return n ? sum / n : 0.0;
}

inline double measureFingerCurl(const Pose& p, Role r)
{
    if (!p.fingers[int(r)])
        return 0.0;
    if (r == Role::Thumb)
        return fitThumb(p.fingers[0]->joints).curl;
    return fitShape(p.fingers[int(r)]->joints, r).curl;
}

inline void addCurl(FingerPose& f, Role r, double by)
{
    if (r == Role::Thumb)
    {
        const ThumbShape s = fitThumb(f.joints);
        ThumbShape t = s;
        t.curl += by;
        const std::array<Angles, kCanon> a = anglesFor(s), b = anglesFor(t);
        for (int j = 0; j < kCanon; ++j)
            f.joints[j] = f.joints[j] + (b[j] - a[j]);
        return;
    }
    const FingerShape s = fitShape(f.joints, r);
    FingerShape t = s;
    t.curl += by;
    const std::array<Angles, kCanon> a = anglesFor(s, r), b = anglesFor(t, r);
    for (int j = 0; j < kCanon; ++j)
        f.joints[j] = f.joints[j] + (b[j] - a[j]);
}

/** Every non-thumb finger in mask curled so their mean curl is v. */
inline Pose setCurl(Pose p, double v, const Mask& m = allFingers())
{
    const double by = v - measureCurl(p, m);
    for (int r = 1; r < kRoles; ++r)
        if (m[r] && p.fingers[r])
        {
            addCurl(*p.fingers[r], Role(r), by);
            p.fingers[r] = clamp(*p.fingers[r], Role(r));
        }
    return p;
}

inline Pose setFingerCurl(Pose p, Role r, double v)
{
    if (!p.fingers[int(r)])
        return p;
    addCurl(*p.fingers[int(r)], r, v - measureFingerCurl(p, r));
    p.fingers[int(r)] = clamp(*p.fingers[int(r)], r);
    return p;
}

/** Spread: the least squares fit of the fingers' splay to their fan. */
inline double measureSpread(const Pose& p, const Mask& m = allFingers())
{
    double num = 0.0, den = 0.0;
    for (int r = 1; r < kRoles; ++r)
        if (m[r] && p.fingers[r])
        {
            num += p.fingers[r]->joints[0].splay * fanOf(Role(r));
            den += fanOf(Role(r)) * fanOf(Role(r));
        }
    return den > 0.0 ? num / den : 0.0;
}

inline Pose setSpread(Pose p, double v, const Mask& m = allFingers())
{
    const double by = v - measureSpread(p, m);
    for (int r = 1; r < kRoles; ++r)
        if (m[r] && p.fingers[r])
        {
            p.fingers[r]->joints[0].splay += by * fanOf(Role(r));
            p.fingers[r] = clamp(*p.fingers[r], Role(r));
        }
    return p;
}

/** A finger's own spread, its knuckle's splay as a share of its fan (the middle finger, which barely fans, in 10 degree units). */
inline double fanUnit(Role r) { return std::abs(fanOf(r)) < 5.0 ? 10.0 : fanOf(r); }
inline double measureFingerSpread(const Pose& p, Role r)
{
    if (!p.fingers[int(r)])
        return 0.0;
    if (r == Role::Thumb)
        return fitThumb(p.fingers[0]->joints).out;
    return p.fingers[int(r)]->joints[0].splay / fanUnit(r);
}
inline Pose setFingerSpread(Pose p, Role r, double v)
{
    if (!p.fingers[int(r)])
        return p;
    FingerPose& f = *p.fingers[int(r)];
    if (r == Role::Thumb)
        f.joints[0].splay += (v - measureFingerSpread(p, r)) * kThumbOut;
    else
        f.joints[0].splay += (v - measureFingerSpread(p, r)) * fanUnit(r);
    p.fingers[int(r)] = clamp(f, r);
    return p;
}

/** The thumb slider: -1 out and straight (a thumbs up), 0 alongside, 1 across the palm and curled. */
inline ThumbShape thumbFor(double t) { return {.curl = t > 0.0 ? 0.6 * t : 0.15 * t, .across = t}; }
inline double measureThumb(const Pose& p) { return p.fingers[0] ? fitThumb(p.fingers[0]->joints).across : 0.0; }
inline Pose setThumb(Pose p, double v)
{
    if (!p.fingers[0])
        return p;
    const std::array<Angles, kCanon> a = anglesFor(thumbFor(measureThumb(p))), b = anglesFor(thumbFor(v));
    for (int j = 0; j < kCanon; ++j)
        p.fingers[0]->joints[j] = p.fingers[0]->joints[j] + (b[j] - a[j]);
    p.fingers[0] = clamp(*p.fingers[0], Role::Thumb);
    return p;
}

/** Cup, from the metacarpals when the hand has them (hasMeta), else from the knuckles' roll. */
inline double measureCup(const Pose& p, bool hasMeta, const Mask& m = allFingers())
{
    double num = 0.0, den = 0.0;
    for (int r = 1; r < kRoles; ++r)
        if (m[r] && p.fingers[r])
        {
            const Angles c = hasMeta ? cupOf(Role(r)) : cupKnuckleOf(Role(r));
            const Angles a = hasMeta ? p.fingers[r]->meta : p.fingers[r]->joints[0];
            num += a.twist * c.twist + (hasMeta ? a.bend * c.bend : 0.0);
            den += c.twist * c.twist + (hasMeta ? c.bend * c.bend : 0.0);
        }
    return den > 0.0 ? num / den : 0.0;
}
inline Pose setCup(Pose p, double v, bool hasMeta, const Mask& m = allFingers())
{
    const double by = v - measureCup(p, hasMeta, m);
    for (int r = 1; r < kRoles; ++r)
        if (m[r] && p.fingers[r])
        {
            if (hasMeta)
                p.fingers[r]->meta = p.fingers[r]->meta + cupOf(Role(r)) * by;
            else
                p.fingers[r]->joints[0] = p.fingers[r]->joints[0] + cupKnuckleOf(Role(r)) * by;
            p.fingers[r] = clamp(*p.fingers[r], Role(r));
        }
    if (m[0] && p.fingers[0])
    {
        p.fingers[0]->joints[0].bend += by * 0.3 * kThumbAcross;
        p.fingers[0]->joints[0].twist += by * 0.3 * kThumbRoll;
        p.fingers[0] = clamp(*p.fingers[0], Role::Thumb);
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------
// Built-in poses.

struct Library
{
    std::string id;
    std::string label;
    std::string icon;
    Shape shape;
};

inline FingerShape fs(double curl, double tip = 0.0, double splay = 0.0, double twist = 0.0) { return {curl, tip, splay, twist}; }
inline ThumbShape ts(double curl, double across, double out, double tip = 0.0) { return {.curl = curl, .tip = tip, .across = across, .out = out}; }

/** The built-in poses, most used first. */
inline const std::vector<Library>& library()
{
    static const std::vector<Library> poses = [] {
        std::vector<Library> l;
        auto add = [&](const char* id, const char* label, const char* icon, std::array<FingerShape, 4> f, ThumbShape t, double cup) {
            l.push_back({id, label, icon, Shape{f, t, cup}});
        };
        add("relaxed", "Relaxed", "hand-stop", {fs(0.18, 0.1, 3), fs(0.25, 0.1, 0.5), fs(0.32, 0.1, -3), fs(0.4, 0.1, -6)}, ts(0.2, 0.15, 0.15), 0.2);
        add("fist", "Fist", "hand-rock", {fs(1, 0, -2), fs(1), fs(1, 0, 2), fs(1, 0, 4)}, ts(0.6, 0.9, 0.2), 0.5);
        add("point", "Point", "hand-finger", {fs(0, 0, 2), fs(1), fs(1, 0, 2), fs(1, 0, 4)}, ts(0.6, 0.8, 0.2), 0.3);
        add("grip", "Grip", "hand-grab", {fs(0.7, 0.1), fs(0.72, 0.1), fs(0.74, 0.1), fs(0.76, 0.1)}, ts(0.4, 0.7, 0.5), 0.35);
        add("pinch", "Pinch", "hand-two-fingers", {fs(0.45, 0.3, 2), fs(0.3, 0.1), fs(0.36, 0.1, -3), fs(0.42, 0.1, -6)}, ts(0.35, 0.55, 0.45), 0.3);
        add("ok", "OK", "hand-ring-finger", {fs(0.6, 0.35), fs(0.08, 0, 2), fs(0.12, 0, -4), fs(0.16, 0, -10)}, ts(0.35, 0.55, 0.45), 0.2);
        add("flat", "Flat", "hand-stop", {fs(0), fs(0), fs(0), fs(0)}, ts(0, 0, 0), 0.0);
        add("spread", "Spread", "hand-stop", {fs(-0.05, 0, 14), fs(-0.05, 0, 3), fs(-0.05, 0, -10), fs(-0.05, 0, -20)}, ts(-0.1, -0.6, 0.2), -0.2);
        add("claw", "Claw", "hand-grab", {fs(0.75, 0.8, 10), fs(0.75, 0.8, 2), fs(0.75, 0.8, -8), fs(0.75, 0.8, -16)}, ts(0.5, 0.2, 0.5), 0.3);
        add("thumbsUp", "Thumbs Up", "thumb-up", {fs(1, 0, -2), fs(1), fs(1, 0, 2), fs(1, 0, 4)}, ts(-0.2, -0.5, 0.3), 0.4);
        add("peace", "Peace", "hand-two-fingers", {fs(0, 0, 10), fs(0, 0, -8), fs(1, 0, 2), fs(1, 0, 4)}, ts(0.6, 0.9, 0.2), 0.3);
        add("cup", "Cup", "hand-move", {fs(0.25, 0.2, -3), fs(0.25, 0.2, -1), fs(0.28, 0.2, 1), fs(0.3, 0.2, 3)}, ts(0.2, 0.6, 0.5), 1.0);
        add("hook", "Hook", "hand-grab", {fs(0.75, 0.9), fs(0.75, 0.9), fs(0.75, 0.9), fs(0.75, 0.9)}, ts(0.1, 0.2, 0.1), 0.2);
        add("trigger", "Trigger", "hand-click", {fs(0.35, 0, 0), fs(0.75, 0.1), fs(0.78, 0.1), fs(0.8, 0.1)}, ts(0.4, 0.7, 0.4), 0.3);
        add("ball", "Ball", "hand-grab", {fs(0.45, 0.1, 10), fs(0.45, 0.1, 3), fs(0.47, 0.1, -7), fs(0.5, 0.1, -14)}, ts(0.3, 0.5, 0.6), 0.6);
        add("pen", "Pen", "writing", {fs(0.4, 0.2), fs(0.5, 0.3, -2), fs(0.6, 0.2, -4), fs(0.7, 0.2, -6)}, ts(0.3, 0.5, 0.5), 0.2);
        add("gun", "Finger Gun", "hand-finger", {fs(0), fs(0), fs(1, 0, 2), fs(1, 0, 4)}, ts(-0.1, -0.2, 0.4), 0.3);
        add("blade", "Blade", "hand-stop", {fs(0, -0.2, -2), fs(0, -0.2), fs(0, -0.2, 2), fs(0, -0.2, 4)}, ts(0.3, 0.5, 0), 0.0);
        add("horns", "Horns", "hand-love-you", {fs(0, 0, 6), fs(1), fs(1, 0, 2), fs(0, 0, -12)}, ts(0.6, 0.9, 0.2), 0.3);
        add("tense", "Tense", "hand-stop", {fs(-0.2, -1, 12), fs(-0.2, -1, 3), fs(-0.2, -1, -9), fs(-0.2, -1, -18)}, ts(-0.2, -0.6, 0.3), -0.3);
        return l;
    }();
    return poses;
}

inline const Library* findLibrary(const std::string& id)
{
    for (const Library& l : library())
        if (l.id == id)
            return &l;
    return nullptr;
}

/** The Relax slider: drags toward the relaxed pose (positive) or the tense one (negative) from base. */
inline Pose relax(const Pose& base, double v, const Mask& m = allFingers())
{
    const Library* target = findLibrary(v >= 0.0 ? "relaxed" : "tense");
    return mix(base, masked(base, poseFrom(target->shape), m), std::clamp(std::abs(v), 0.0, 1.0), m);
}

// ---------------------------------------------------------------------------------------------------------------------
// Joint frames: the rotations of the maths above, on a skeleton's joints.

/** Rotation about x, then y, then z composed as Rx * Ry * Rz. */
inline glm::dmat3 rotX(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return glm::dmat3(1, 0, 0, 0, c, s, 0, -s, c);
}
inline glm::dmat3 rotY(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return glm::dmat3(c, 0, -s, 0, 1, 0, s, 0, c);
}
inline glm::dmat3 rotZ(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return glm::dmat3(c, s, 0, -s, c, 0, 0, 0, 1);
}
inline glm::dmat3 eulerXYZ(const glm::dvec3& a) { return rotX(a.x) * rotY(a.y) * rotZ(a.z); }
/** Angles of R = Rx(a.x) * Ry(a.y) * Rz(a.z). */
inline glm::dvec3 decomposeXYZ(const glm::dmat3& m)
{
    // Row r, column c of the maths is m[c][r].
    const double sy = std::clamp(m[2][0], -1.0, 1.0);
    const double y = std::asin(sy);
    if (std::abs(sy) > 1.0 - 1e-12)
        return {std::atan2(m[1][2], m[1][1]), y, 0.0};
    return {std::atan2(-m[2][1], m[2][2]), y, std::atan2(-m[1][0], m[0][0])};
}

inline glm::dvec3 unit(const glm::dvec3& v, const glm::dvec3& fallback = {1, 0, 0})
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}
inline glm::dvec3 perp(const glm::dvec3& v, const glm::dvec3& to) { return v - glm::dot(v, to) * to; }

/**
 * A joint's anatomical frame, in its parent's space: axes columns are (bend axis, toward the palm, straight along the
 * finger), zero the joint's local rotation with every angle at 0. A local rotation L is parentWorld^T * jointWorld.
 */
struct Frame
{
    glm::dmat3 axes{1.0};
    glm::dmat3 zero{1.0};
    double splaySign = 1.0, twistSign = -1.0;
};

/**
 * The frame of a joint whose parent and own world rotations at rest are parentRest and rest, laid with straight (the
 * direction the bone points at zero) and bendAxis (world). bone is where the bone points at rest; restIsZero makes the
 * rest pose the zero (metacarpals and the thumb's base, which have no straight). sideRef: positive splay is toward it.
 */
inline Frame makeFrame(const glm::dmat3& parentRest, const glm::dmat3& rest, const glm::dvec3& straight, const glm::dvec3& bone,
                       const glm::dvec3& bendAxis, const glm::dvec3& sideRef, bool restIsZero)
{
    const glm::dmat3 pt = glm::transpose(parentRest);
    const glm::dvec3 s = unit(pt * (restIsZero ? bone : straight));
    const glm::dvec3 b = unit(perp(pt * bendAxis, s), unit(glm::cross(s, glm::dvec3(0, 1, 0)), {1, 0, 0}));
    const glm::dvec3 n = glm::cross(s, b);
    Frame f;
    f.axes = glm::dmat3(b, n, s);
    f.splaySign = glm::dot(parentRest * b, sideRef) >= 0.0 ? 1.0 : -1.0;
    f.twistSign = -f.splaySign;
    const glm::dmat3 local = pt * rest;
    if (restIsZero)
    {
        f.zero = local;
        return f;
    }
    const glm::dvec3 d = glm::transpose(f.axes) * unit(pt * bone);
    const glm::dmat3 restDelta = f.axes * eulerXYZ({std::atan2(-d.y, d.z), std::asin(std::clamp(d.x, -1.0, 1.0)), 0.0}) * glm::transpose(f.axes);
    f.zero = glm::transpose(restDelta) * local;
    return f;
}

inline glm::dmat3 localFor(const Frame& f, const Angles& a)
{
    const glm::dvec3 e(-rad(a.bend), f.splaySign * rad(a.splay), f.twistSign * rad(a.twist));
    return f.axes * eulerXYZ(e) * glm::transpose(f.axes) * f.zero;
}

inline Angles anglesOf(const Frame& f, const glm::dmat3& local)
{
    const glm::dvec3 e = decomposeXYZ(glm::transpose(f.axes) * local * glm::transpose(f.zero) * f.axes);
    return {-deg(e.x), deg(e.y) * f.splaySign, deg(e.z) * f.twistSign};
}

// ---------------------------------------------------------------------------------------------------------------------
// Finding hands.

enum class Side
{
    Center,
    Left,
    Right,
};
inline const char* sideName(Side s) { return s == Side::Left ? "left" : s == Side::Right ? "right" : "center"; }

struct SkelJoint
{
    std::string name; // short name, no path
    int parent = -1;
    glm::dvec3 position{0.0};
    glm::dmat3 rotation{1.0}; // world, at rest
};

struct FingerRig
{
    Role role = Role::Index;
    int meta = -1;           // metacarpal joint, or -1
    std::vector<int> joints; // the finger's turning joints, base first
    int tip = -1;            // an end joint past the last, not turned
    std::string roleFrom;    // "name" or "layout"
};

struct HandRig
{
    int hand = -1;
    Side side = Side::Center;
    std::string sideFrom; // "name" or "position"
    std::vector<FingerRig> fingers;
    glm::dvec3 forward{0, 0, 1}, thumbSide{1, 0, 0}, palm{0, -1, 0}; // world, at rest
};

inline std::string lowerOf(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

/** Left or right from a joint name: _l, l_, .l, left, Left... */
inline Side sideFromName(const std::string& raw)
{
    std::string s = lowerOf(raw);
    if (const size_t colon = s.rfind(':'); colon != std::string::npos)
        s = s.substr(colon + 1);
    if (has(s, "left"))
        return Side::Left;
    if (has(s, "right"))
        return Side::Right;
    auto token = [&](char c) {
        const std::string seps = "_.- ";
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] != c)
                continue;
            const bool before = i == 0 || seps.find(s[i - 1]) != std::string::npos;
            const bool after = i + 1 == s.size() || seps.find(s[i + 1]) != std::string::npos || std::isdigit(static_cast<unsigned char>(s[i + 1]));
            if (before && after)
                return true;
        }
        return false;
    };
    if (token('l'))
        return Side::Left;
    if (token('r'))
        return Side::Right;
    // Camel case suffixes: HandL, handR.
    if (raw.size() > 1 && std::islower(static_cast<unsigned char>(raw[raw.size() - 2])))
    {
        if (raw.back() == 'L')
            return Side::Left;
        if (raw.back() == 'R')
            return Side::Right;
    }
    return Side::Center;
}

/** A finger's role from its joints' names, or nullopt. */
inline std::optional<Role> roleFromJointName(const std::string& raw)
{
    const std::string s = lowerOf(raw);
    if (has(s, "thumb") || has(s, "pollex"))
        return Role::Thumb;
    if (has(s, "index") || has(s, "pointer") || has(s, "forefinger"))
        return Role::Index;
    if (has(s, "middle"))
        return Role::Middle;
    if (has(s, "ring"))
        return Role::Ring;
    if (has(s, "pinky") || has(s, "pinkie") || has(s, "little"))
        return Role::Pinky;
    // Bip-style Finger0 (thumb) to Finger4 (little).
    if (const size_t f = s.find("finger"); f != std::string::npos && f + 6 < s.size() && std::isdigit(static_cast<unsigned char>(s[f + 6])))
        if (const int d = s[f + 6] - '0'; d >= 0 && d < kRoles)
            return Role(d);
    return std::nullopt;
}

inline bool helperName(const std::string& raw)
{
    const std::string s = lowerOf(raw);
    for (const char* w : {"twist", "weapon", "prop", "attach", "socket", "ik_", "roll", "helper", "corrective", "handle", "_offset"})
        if (has(s, w))
            return true;
    return false;
}

inline bool tipName(const std::string& raw)
{
    const std::string s = lowerOf(raw);
    return has(s, "end") || has(s, "tip") || has(s, "nub") || has(s, "null");
}

inline bool handName(const std::string& raw)
{
    const std::string s = lowerOf(raw);
    if (helperName(raw) || has(s, "ik"))
        return false;
    return has(s, "hand") || has(s, "wrist") || has(s, "palm");
}

inline bool notHandName(const std::string& raw)
{
    const std::string s = lowerOf(raw);
    for (const char* w : {"foot", "toe", "ankle", "ball", "pelvis", "hip", "spine", "neck", "head", "root", "jaw", "face", "eye", "tongue", "tail", "leg", "thigh", "calf"})
        if (has(s, w))
            return true;
    return false;
}

struct Skeleton
{
    const std::vector<SkelJoint>& joints;
    std::vector<std::vector<int>> children;
    explicit Skeleton(const std::vector<SkelJoint>& js) : joints(js), children(js.size())
    {
        for (size_t i = 0; i < js.size(); ++i)
            if (js[i].parent >= 0)
                children[size_t(js[i].parent)].push_back(int(i));
    }
    glm::dvec3 pos(int i) const { return joints[size_t(i)].position; }
    /** The leaf below i farthest from it. */
    double reach(int i) const
    {
        double best = 0.0;
        for (int c : children[size_t(i)])
            best = std::max(best, glm::length(pos(c) - pos(i)) + reach(c));
        return best;
    }
    /** From i down to its tip, following the child that reaches farthest. */
    std::vector<int> chain(int i) const
    {
        std::vector<int> out{i};
        for (int cur = i; !children[size_t(cur)].empty();)
        {
            int best = -1;
            double farthest = -1.0;
            for (int c : children[size_t(cur)])
                if (const double r = glm::length(pos(c) - pos(cur)) + reach(c); r > farthest && !helperName(joints[size_t(c)].name))
                {
                    farthest = r;
                    best = c;
                }
            if (best < 0)
                break;
            out.push_back(best);
            cur = best;
        }
        return out;
    }
};

/** The finger chains under a joint: each non-helper child's chain of at least two joints. */
inline std::vector<std::vector<int>> fingerChains(const Skeleton& sk, int hand)
{
    std::vector<std::vector<int>> out;
    for (int c : sk.children[size_t(hand)])
    {
        if (helperName(sk.joints[size_t(c)].name))
            continue;
        std::vector<int> ch = sk.chain(c);
        if (ch.size() >= 2)
            out.push_back(std::move(ch));
    }
    return out;
}

/** Whether the joint looks like a hand by its children alone: several short chains from the end of a limb. */
inline bool handByLayout(const Skeleton& sk, int i)
{
    const SkelJoint& j = sk.joints[size_t(i)];
    if (j.parent < 0 || sk.joints[size_t(j.parent)].parent < 0 || notHandName(j.name))
        return false;
    const std::vector<std::vector<int>> chains = fingerChains(sk, i);
    if (chains.size() < 3)
        return false;
    const double limb = glm::length(j.position - sk.pos(j.parent));
    for (const std::vector<int>& c : chains)
        if (sk.reach(c.front()) + glm::length(sk.pos(c.front()) - j.position) > 1.2 * limb)
            return false;
    return true;
}

/** Fills a finger's metacarpal, turning joints and tip from its chain. */
inline FingerRig fingerFrom(const Skeleton& sk, int hand, const std::vector<int>& chain, bool thumb)
{
    FingerRig f;
    std::vector<int> js = chain;
    const std::string first = lowerOf(sk.joints[size_t(js.front())].name);
    const bool namedMeta = has(first, "meta") || has(first, "carpal") || has(first, "palm");
    if (!thumb && js.size() >= 3)
    {
        const double toFirst = glm::length(sk.pos(js[0]) - sk.pos(hand)), firstBone = glm::length(sk.pos(js[1]) - sk.pos(js[0]));
        const bool tipped = tipName(sk.joints[size_t(js.back())].name);
        const size_t turning = js.size() - (tipped ? 1 : 0);
        if (namedMeta || (turning >= 4 && toFirst < firstBone))
        {
            f.meta = js.front();
            js.erase(js.begin());
        }
    }
    // Past three turning joints, a leaf is an end marker (Index4, thumb_end).
    if (js.size() > 1 && (tipName(sk.joints[size_t(js.back())].name) || js.size() > 3) && sk.children[size_t(js.back())].empty())
    {
        f.tip = js.back();
        js.pop_back();
    }
    f.joints = js;
    return f;
}

/** Hands in a skeleton, by name first; with no hand named, by layout. */
inline std::vector<HandRig> detectHands(const std::vector<SkelJoint>& joints, const glm::dvec3& lateral = {1, 0, 0}, const glm::dvec3& front = {0, 0, 1})
{
    const Skeleton sk(joints);
    std::vector<int> hands;
    for (size_t i = 0; i < joints.size(); ++i)
        if (handName(joints[i].name) && fingerChains(sk, int(i)).size() >= 2)
            hands.push_back(int(i));
    if (hands.empty())
        for (size_t i = 0; i < joints.size(); ++i)
            if (handByLayout(sk, int(i)))
                hands.push_back(int(i));

    glm::dvec3 centre(0.0);
    for (const SkelJoint& j : joints)
        if (j.parent < 0)
            centre = j.position;

    std::vector<HandRig> out;
    for (int h : hands)
    {
        HandRig hr;
        hr.hand = h;
        const glm::dvec3 hp = sk.pos(h);
        hr.side = sideFromName(joints[size_t(h)].name);
        hr.sideFrom = "name";
        if (hr.side == Side::Center)
        {
            hr.side = glm::dot(hp - centre, lateral) >= 0.0 ? Side::Left : Side::Right;
            hr.sideFrom = "position";
        }
        std::vector<std::vector<int>> chains = fingerChains(sk, h);
        // Roles by name.
        std::vector<std::optional<Role>> roles;
        for (const std::vector<int>& c : chains)
        {
            std::optional<Role> r;
            for (int j : c)
                if ((r = roleFromJointName(joints[size_t(j)].name)))
                    break;
            roles.push_back(r);
        }
        // The thumb by layout: the finger starting nearest the wrist along the hand, well short of the others.
        auto baseOf = [&](size_t k) {
            const FingerRig f = fingerFrom(sk, h, chains[k], false);
            return f.joints.empty() ? sk.pos(chains[k].front()) : sk.pos(f.joints.front());
        };
        glm::dvec3 forward(0.0);
        for (size_t k = 0; k < chains.size(); ++k)
            forward += baseOf(k) - hp;
        forward = unit(forward);
        int thumb = -1;
        for (size_t k = 0; k < chains.size(); ++k)
            if (roles[k] == Role::Thumb)
                thumb = int(k);
        if (thumb < 0 && std::none_of(roles.begin(), roles.end(), [](auto r) { return r.has_value(); }) && chains.size() >= 2)
        {
            std::vector<double> along;
            for (size_t k = 0; k < chains.size(); ++k)
                along.push_back(glm::dot(sk.pos(chains[k].front()) - hp, forward));
            std::vector<double> sorted = along;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            const size_t lo = size_t(std::min_element(along.begin(), along.end()) - along.begin());
            if (along[lo] < 0.75 * median)
                thumb = int(lo);
        }
        // Hand frame: forward along the fingers, thumbSide toward the thumb, palm the side the fingers curl to.
        glm::dvec3 knuckles(0.0);
        int nk = 0;
        for (size_t k = 0; k < chains.size(); ++k)
            if (int(k) != thumb)
            {
                knuckles += baseOf(k);
                ++nk;
            }
        knuckles = nk ? knuckles / double(nk) : hp + forward;
        hr.forward = unit(knuckles - hp, forward);
        glm::dvec3 thumbSide;
        if (thumb >= 0)
            thumbSide = perp(sk.pos(chains[size_t(thumb)].front()) - knuckles, hr.forward);
        else
        {
            // No thumb: the line of the knuckles, toward the front of the character.
            glm::dvec3 a = baseOf(0), b = baseOf(chains.size() - 1);
            thumbSide = perp(a - b, hr.forward);
            if (glm::dot(thumbSide, front) < 0.0)
                thumbSide = -thumbSide;
        }
        hr.thumbSide = unit(thumbSide, unit(glm::cross(hr.forward, glm::dvec3(0, 1, 0))));
        const glm::dvec3 c = unit(glm::cross(hr.forward, hr.thumbSide));
        hr.palm = hr.side == Side::Right ? -c : c;

        // Non-thumb fingers by role: by name, else ordered from the thumb side.
        std::vector<size_t> others;
        for (size_t k = 0; k < chains.size(); ++k)
            if (int(k) != thumb)
                others.push_back(k);
        std::sort(others.begin(), others.end(), [&](size_t a, size_t b) { return glm::dot(baseOf(a), hr.thumbSide) > glm::dot(baseOf(b), hr.thumbSide); });
        static const std::map<size_t, std::vector<Role>> byCount = {
            {1, {Role::Index}},
            {2, {Role::Index, Role::Middle}},
            {3, {Role::Index, Role::Middle, Role::Pinky}},
            {4, {Role::Index, Role::Middle, Role::Ring, Role::Pinky}},
        };
        std::array<bool, kRoles> taken{};
        for (const auto& r : roles)
            if (r)
                taken[int(*r)] = true;
        if (thumb >= 0)
        {
            FingerRig f = fingerFrom(sk, h, chains[size_t(thumb)], true);
            f.role = Role::Thumb;
            f.roleFrom = roles[size_t(thumb)] ? "name" : "layout";
            hr.fingers.push_back(f);
        }
        const std::vector<Role>& order = byCount.count(std::min<size_t>(others.size(), 4)) ? byCount.at(std::min<size_t>(others.size(), 4)) : byCount.at(4);
        for (size_t i = 0; i < others.size(); ++i)
        {
            const size_t k = others[i];
            FingerRig f = fingerFrom(sk, h, chains[k], false);
            if (roles[k] && *roles[k] != Role::Thumb)
            {
                f.role = *roles[k];
                f.roleFrom = "name";
            }
            else
            {
                Role pick = i < order.size() ? order[i] : Role::Pinky;
                for (int r = int(pick); r < kRoles && taken[r]; ++r)
                    pick = Role(std::min(r + 1, kRoles - 1));
                f.role = pick;
                f.roleFrom = "layout";
            }
            taken[int(f.role)] = true;
            if (std::any_of(hr.fingers.begin(), hr.fingers.end(), [&](const FingerRig& o) { return o.role == f.role; }))
                continue;
            hr.fingers.push_back(f);
        }
        std::sort(hr.fingers.begin(), hr.fingers.end(), [](const FingerRig& a, const FingerRig& b) { return int(a.role) < int(b.role); });
        if (!hr.fingers.empty())
            out.push_back(std::move(hr));
    }
    return out;
}

/** Where a joint's bone points at rest: toward its next joint, else along the axis its parent's bone lies on. */
inline glm::dvec3 boneDir(const std::vector<SkelJoint>& js, int joint, int next)
{
    if (next >= 0)
        return unit(js[size_t(next)].position - js[size_t(joint)].position);
    const int p = js[size_t(joint)].parent;
    const glm::dvec3 pb = p >= 0 ? unit(js[size_t(joint)].position - js[size_t(p)].position) : glm::dvec3(1, 0, 0);
    if (p < 0)
        return pb;
    // The parent's local axis nearest its bone, read on this joint.
    const glm::dmat3& pr = js[size_t(p)].rotation;
    int best = 0;
    double score = 0.0, sign = 1.0;
    for (int a = 0; a < 3; ++a)
        if (const double d = glm::dot(unit(pr[a]), pb); std::abs(d) > score)
        {
            score = std::abs(d);
            best = a;
            sign = d >= 0.0 ? 1.0 : -1.0;
        }
    return unit(js[size_t(joint)].rotation[best] * sign, pb);
}

/** Frames of a finger's metacarpal (when it has one) and turning joints, from the skeleton at rest. */
struct FingerFrames
{
    std::optional<Frame> meta;
    std::vector<Frame> joints;
};

inline FingerFrames fingerFrames(const std::vector<SkelJoint>& js, const HandRig& h, const FingerRig& f)
{
    FingerFrames out;
    const bool thumb = f.role == Role::Thumb;
    // The bend axis: across the finger, so the bend turns it toward the palm. The thumb bends across the palm, toward the little finger.
    const glm::dvec3 first = boneDir(js, f.joints.front(), f.joints.size() > 1 ? f.joints[1] : f.tip);
    const glm::dvec3 curlTo = thumb ? unit(perp(-h.thumbSide + 0.5 * h.palm, first)) : h.palm;
    const glm::dvec3 axis = thumb ? unit(glm::cross(curlTo, first)) : unit(glm::cross(h.palm, h.forward));
    const glm::dvec3 sideRef = thumb ? h.palm : h.thumbSide;
    auto rotOf = [&](int j) { return js[size_t(j)].rotation; };
    auto parentRot = [&](int j) { return js[size_t(j)].parent >= 0 ? rotOf(js[size_t(j)].parent) : glm::dmat3(1.0); };
    if (f.meta >= 0)
    {
        const glm::dvec3 bone = boneDir(js, f.meta, f.joints.front());
        out.meta = makeFrame(parentRot(f.meta), rotOf(f.meta), bone, bone, axis, sideRef, true);
    }
    for (size_t k = 0; k < f.joints.size(); ++k)
    {
        const int j = f.joints[k];
        const int next = k + 1 < f.joints.size() ? f.joints[k + 1] : f.tip;
        const glm::dvec3 bone = boneDir(js, j, next);
        const int p = js[size_t(j)].parent;
        glm::dvec3 straight = p >= 0 ? unit(js[size_t(j)].position - js[size_t(p)].position) : bone;
        if (k == 0 && !thumb)
        {
            // The knuckle is straight with the fingers together: along the hand, tilted only as its metacarpal tilts out of the palm.
            const glm::dvec3 ref = f.meta >= 0 ? boneDir(js, f.meta, j) : unit(js[size_t(j)].position - js[size_t(h.hand)].position, bone);
            straight = unit(glm::dot(ref, h.forward) * h.forward + glm::dot(ref, h.palm) * h.palm, h.forward);
        }
        out.joints.push_back(makeFrame(parentRot(j), rotOf(j), straight, bone, axis, sideRef, k == 0 && thumb));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Grip: closing a finger until it touches.

/**
 * Closes a finger's joints from `from` toward `to` until it touches: contact(angles) returns the first segment touching
 * (0 the base's bone), or -1. The joints up to the touching segment stop, the rest keep closing, so the finger wraps.
 * steps per pass, then a few halvings for where the touch is.
 */
inline std::vector<Angles> wrap(std::vector<Angles> from, const std::vector<Angles>& to, const std::function<int(const std::vector<Angles>&)>& contact,
                                int steps = 24, int refine = 6)
{
    const size_t n = from.size();
    if (contact(from) >= 0)
        return from;
    size_t active = 0; // joints below this have stopped
    while (active < n)
    {
        auto at = [&](double u) {
            std::vector<Angles> a = from;
            for (size_t j = active; j < n; ++j)
                a[j] = mix(from[j], to[j], u);
            return a;
        };
        double lo = 0.0, hi = -1.0;
        int hit = -1;
        for (int s = 1; s <= steps; ++s)
        {
            const double u = double(s) / steps;
            if ((hit = contact(at(u))) >= 0)
            {
                hi = u;
                break;
            }
            lo = u;
        }
        if (hit < 0)
            return at(1.0);
        for (int r = 0; r < refine; ++r)
        {
            const double mid = 0.5 * (lo + hi);
            if (const int c = contact(at(mid)); c >= 0)
            {
                hi = mid;
                hit = c;
            }
            else
                lo = mid;
        }
        from = at(lo);
        active = std::max(active + 1, size_t(hit) + 1);
    }
    return from;
}

/**
 * A way out for a finger that starts in what it grips: each of `outs` (poses opened away from it) is tried on the way
 * from `from`, and the first point clear of contact on the one needing the least turn wins. None when no way clears
 * (the finger's base is in it).
 */
inline std::optional<std::vector<Angles>> openOut(const std::vector<Angles>& from, const std::vector<std::vector<Angles>>& outs,
                                                  const std::function<int(const std::vector<Angles>&)>& contact, int steps = 24, int refine = 8)
{
    std::optional<std::vector<Angles>> best;
    double bestTurn = std::numeric_limits<double>::max();
    for (const std::vector<Angles>& out : outs)
    {
        if (out.size() != from.size())
            continue;
        auto at = [&](double u) {
            std::vector<Angles> a(from.size());
            for (size_t j = 0; j < from.size(); ++j)
                a[j] = mix(from[j], out[j], u);
            return a;
        };
        double lo = 0.0, hi = -1.0;
        for (int s = 1; s <= steps && hi < 0.0; ++s)
        {
            const double u = double(s) / steps;
            if (contact(at(u)) < 0)
                hi = u;
            else
                lo = u;
        }
        if (hi < 0.0)
            continue;
        for (int r = 0; r < refine; ++r)
        {
            const double mid = 0.5 * (lo + hi);
            (contact(at(mid)) < 0 ? hi : lo) = mid;
        }
        double turn = 0.0;
        for (size_t j = 0; j < from.size(); ++j)
        {
            const Angles d = (out[j] - from[j]) * hi;
            turn = std::max({turn, std::abs(d.bend), std::abs(d.splay), std::abs(d.twist)});
        }
        if (turn < bestTurn)
        {
            bestTurn = turn;
            best = at(hi);
        }
    }
    return best;
}
} // namespace puppeteer::hand
