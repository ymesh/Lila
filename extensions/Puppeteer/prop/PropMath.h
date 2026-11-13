#pragma once

// Props and weapons, the maths: per-prop settings and the presets that fill them, reading a prop's shape, fitting
// grip points to it, and where a hand's grip sits.
// Pure: no scene, so tests and any thread can use it.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace puppeteer::prop
{
/** What a point on a prop is for. Grip points (main, off, alt) are where hands go. */
enum class Role : int
{
    Main,
    Off,
    Alt,
    Muzzle, // the aimed axis of a firearm or bow: +X out of it
    Tip,    // the aimed axis of a blade or pole
    Stock,  // what goes to the shoulder
    Holster, // on a character: where the prop's main grip rests when put away
    Count,
};

/** How a prop's shape is read when fitting points; Auto reads it from the points themselves. */
enum class Shape : int
{
    Auto,
    Blade,   // long, handle at one end below a guard
    Haft,    // long, a wider head or body at one end (axes, hammers, bottles, guitars)
    Pole,    // long and even (spears, staffs, chair legs)
    Firearm, // a barrel with a grip below it
    Tube,    // a launcher: a tube with grips below
    Plate,   // flat and broad (shields, trays, boards)
    Bow,     // a bent limb, held in the middle
    Other,   // anything else: the narrower end is the handle
    Count,
};

enum class AimStyle : int
{
    Turn,
    Shoulder,
    Extend,
    Guard,
};

/** Where a prop is put away on a character when it has no holster point yet. */
enum class HolsterSpot : int
{
    Back,
    HipLeft,
    HipRight,
    ThighRight,
    ThighLeft,
    Belt,
    Count,
};

/** A holster spot: a joint by name (first found) and the grip's frame there. */
struct HolsterDefault
{
    std::vector<std::string> joints;
    /** In the character's frame: right, up, forward (cm) from the joint. */
    glm::dvec3 offset{0.0};
    /** The grip's +X (handle toward the thumb) and +Z (palm side) in the character's frame. */
    glm::dvec3 x{0.0, 1.0, 0.0}, z{0.0, 0.0, 1.0};
    const char* id = "belt";
    const char* label = "Belt";
};

inline const HolsterDefault& holsterSpot(HolsterSpot s)
{
    static const std::array<HolsterDefault, size_t(HolsterSpot::Count)> all = {{
        {{"spine_05", "spine_04", "spine_03", "spine_02", "chest", "spine"}, {0.0, 4.0, -16.0}, glm::normalize(glm::dvec3(-0.55, -0.83, 0.0)), {0.0, 0.0, -1.0}, "back", "Back"},
        {{"pelvis", "hips", "hip"}, {14.0, -2.0, 2.0}, glm::normalize(glm::dvec3(0.25, -0.95, 0.3)), {1.0, 0.0, 0.0}, "hipLeft", "Left hip"},
        {{"pelvis", "hips", "hip"}, {-14.0, -2.0, 2.0}, glm::normalize(glm::dvec3(-0.25, -0.95, 0.3)), {-1.0, 0.0, 0.0}, "hipRight", "Right hip"},
        {{"thigh_r", "rightupleg", "upleg_r", "pelvis", "hips"}, {-9.0, -10.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, "thighRight", "Right thigh"},
        {{"thigh_l", "leftupleg", "upleg_l", "pelvis", "hips"}, {9.0, -10.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, "thighLeft", "Left thigh"},
        {{"pelvis", "hips", "hip"}, {0.0, 0.0, -14.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, "belt", "Belt"},
    }};
    return all[size_t(std::clamp(int(s), 0, int(HolsterSpot::Count) - 1))];
}

/** How a prop is held, aimed and put away: per prop, filled by a preset or by what its fit found. */
struct Settings
{
    bool twoHanded = false;
    /** Hand poses (Hands panel names) for the main and off hand. */
    std::string mainPose = "grip", offPose = "grip";
    /** The main hand's index finger stays posed rather than wrapping (on the trigger). */
    bool trigger = false;
    AimStyle aim = AimStyle::Turn;
    /** Guard: cm below the line to the target. Extend and Guard: share of the arm the grip is held out. */
    double drop = 0.0, reach = 0.9;
    HolsterSpot holster = HolsterSpot::Belt;
    /** How Fit Points reads the shape. */
    Shape shape = Shape::Auto;
    bool operator==(const Settings&) const = default;
};

/** A one-click set of settings for a kind of prop. */
struct Preset
{
    const char* id;
    const char* label;
    const char* icon;
    Settings settings;
};

inline const std::vector<Preset>& presets()
{
    using A = AimStyle;
    using H = HolsterSpot;
    using S = Shape;
    static const std::vector<Preset> all = {
        {"sword1h", "Sword", "sword", {false, "grip", "grip", false, A::Guard, 12.0, 0.6, H::HipLeft, S::Blade}},
        {"sword2h", "Greatsword", "swords", {true, "grip", "grip", false, A::Guard, 18.0, 0.55, H::Back, S::Blade}},
        {"dagger", "Dagger", "cut", {false, "grip", "grip", false, A::Guard, 8.0, 0.6, H::ThighRight, S::Blade}},
        {"axe", "Axe / Hammer", "axe", {false, "grip", "grip", false, A::Guard, 10.0, 0.6, H::HipLeft, S::Haft}},
        {"spear", "Spear / Staff", "wand", {true, "grip", "grip", false, A::Guard, 6.0, 0.6, H::Back, S::Pole}},
        {"shield", "Shield", "shield", {false, "grip", "grip", false, A::Guard, 0.0, 0.45, H::Back, S::Plate}},
        {"bow", "Bow", "bow", {true, "grip", "hook", false, A::Extend, 0.0, 0.95, H::Back, S::Bow}},
        {"pistol", "Pistol", "target", {false, "trigger", "grip", true, A::Extend, 0.0, 0.9, H::ThighRight, S::Firearm}},
        {"rifle", "Rifle", "crosshair", {true, "trigger", "grip", true, A::Shoulder, 0.0, 0.9, H::Back, S::Firearm}},
        {"shotgun", "Shotgun", "crosshair", {true, "trigger", "grip", true, A::Shoulder, 0.0, 0.9, H::Back, S::Firearm}},
        {"launcher", "Launcher", "rocket", {true, "trigger", "grip", true, A::Shoulder, 0.0, 0.9, H::Back, S::Tube}},
        {"generic", "Generic", "box", {false, "grip", "grip", false, A::Turn, 0.0, 0.9, H::Belt, S::Auto}},
    };
    return all;
}

/** A preset by id or by a common name people and other tools use; null when none. */
inline const Preset* presetFor(const std::string& id)
{
    static const std::pair<const char*, const char*> kAlias[] = {
        {"sword", "sword1h"},  {"longsword", "sword2h"}, {"greatsword", "sword2h"}, {"knife", "dagger"},  {"hammer", "axe"},
        {"mace", "axe"},       {"staff", "spear"},       {"polearm", "spear"},      {"gun", "pistol"},    {"handgun", "pistol"},
        {"smg", "rifle"},      {"carbine", "rifle"},     {"bazooka", "launcher"},   {"rpg", "launcher"}, {"rocket", "launcher"},
        {"crossbow", "rifle"},
    };
    std::string want = id;
    for (const auto& [name, to] : kAlias)
        if (id == name)
            want = to;
    for (const Preset& p : presets())
        if (want == p.id)
            return &p;
    return nullptr;
}

/** The preset whose settings the prop's match exactly, else null. */
inline const Preset* matchingPreset(const Settings& s)
{
    for (const Preset& p : presets())
        if (p.settings == s)
            return &p;
    return nullptr;
}

inline const char* roleId(Role r)
{
    static const char* kIds[] = {"main", "off", "alt", "muzzle", "tip", "stock", "holster"};
    return kIds[std::clamp(int(r), 0, int(Role::Count) - 1)];
}
inline std::optional<Role> roleFromId(const std::string& s)
{
    for (int i = 0; i < int(Role::Count); ++i)
        if (s == roleId(Role(i)))
            return Role(i);
    return std::nullopt;
}
inline bool isGrip(Role r) { return r == Role::Main || r == Role::Off || r == Role::Alt; }

inline const char* aimStyleId(AimStyle s)
{
    static const char* kIds[] = {"turn", "shoulder", "extend", "guard"};
    return kIds[std::clamp(int(s), 0, 3)];
}
inline std::optional<AimStyle> aimStyleFromId(const std::string& s)
{
    for (int i = 0; i < 4; ++i)
        if (s == aimStyleId(AimStyle(i)))
            return AimStyle(i);
    return std::nullopt;
}

inline const char* shapeId(Shape s)
{
    static const char* kIds[] = {"auto", "blade", "haft", "pole", "firearm", "tube", "plate", "bow", "other"};
    return kIds[std::clamp(int(s), 0, int(Shape::Count) - 1)];
}
inline std::optional<Shape> shapeFromId(const std::string& s)
{
    for (int i = 0; i < int(Shape::Count); ++i)
        if (s == shapeId(Shape(i)))
            return Shape(i);
    return std::nullopt;
}

inline std::optional<HolsterSpot> holsterSpotFromId(const std::string& s)
{
    for (int i = 0; i < int(HolsterSpot::Count); ++i)
        if (s == holsterSpot(HolsterSpot(i)).id)
            return HolsterSpot(i);
    return std::nullopt;
}

// Frames -----------------------------------------------------------------------------------------------------------

/**
 * A frame from an origin, its +X and a direction +Z should lean toward (made orthogonal to X). Grip points use X along
 * the handle toward the thumb (the blade, the slide) and Z the way the palm faces (into the handle).
 */
inline glm::dmat4 frameXZ(const glm::dvec3& origin, glm::dvec3 x, glm::dvec3 z)
{
    x = glm::normalize(x);
    z = z - x * glm::dot(z, x);
    if (glm::dot(z, z) < 1e-12)
    {
        z = std::abs(x.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        z = z - x * glm::dot(z, x);
    }
    z = glm::normalize(z);
    const glm::dvec3 y = glm::cross(z, x);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x, 0.0);
    m[1] = glm::dvec4(y, 0.0);
    m[2] = glm::dvec4(z, 0.0);
    m[3] = glm::dvec4(origin, 1.0);
    return m;
}

/** A hand's measurements in its joint's space, from the hand rig. */
struct HandShape
{
    glm::dvec3 forward{1.0, 0.0, 0.0};   // along the fingers
    glm::dvec3 thumbSide{0.0, 0.0, 1.0}; // toward the thumb
    glm::dvec3 palm{0.0, -1.0, 0.0};     // the way the fingers curl
    /** The non-thumb fingers' first joints, averaged. */
    glm::dvec3 knuckles{9.0, 0.0, 0.0};
    /** Each non-thumb finger's first joint and its thickness (cm), where the hand rig has them. */
    struct Finger
    {
        glm::dvec3 knuckle{0.0};
        double thickness = 1.0;
    };
    std::vector<Finger> fingers;
};

/**
 * Where a hand holds a handle of the radius given, in the hand joint's space: the handle's centre across the palm under
 * the first finger bones, X toward the thumb, Z the way the palm faces, then moved off the palm until every finger's
 * knuckle clears it by the finger's thickness (an arched hand's pinky knuckle sits lower than the index's), so each
 * finger can close onto it. The same grip frame sits the same way in either hand, mirrored: one prop point fits a right
 * and a left hand.
 */
inline glm::dmat4 gripFrame(const HandShape& h, double radius)
{
    const double k = std::max(glm::length(h.knuckles), 1e-3);
    const glm::dvec3 f = glm::normalize(h.forward), p = glm::normalize(h.palm);
    glm::dmat4 m = frameXZ(f * (0.85 * k) + p * (0.25 * k + std::max(radius, 0.0)), h.thumbSide, p);
    const glm::dvec3 x(m[0]), z(m[2]);
    double shift = 0.0;
    for (const HandShape::Finger& fg : h.fingers)
    {
        // Clear by a thickness and a quarter: the grip's first contact test samples a quarter of the way along the bone.
        const double need = std::max(radius, 0.0) + 1.25 * fg.thickness;
        const glm::dvec3 d = fg.knuckle - glm::dvec3(m[3]);
        const glm::dvec3 across = d - x * glm::dot(d, x);
        const double along = glm::dot(across, z);
        const double side = glm::length(across - z * along);
        if (side < need)
            shift = std::max(shift, along + std::sqrt(need * need - side * side));
    }
    m[3] += glm::dvec4(z * shift, 0.0);
    return m;
}

/** The world matrix that puts a prop's point (local: the point's frame in the prop root's space) on a world frame. */
inline glm::dmat4 placeOn(const glm::dmat4& frameWorld, const glm::dmat4& pointLocal) { return frameWorld * glm::inverse(pointLocal); }

/** Rigid part of a matrix (scale removed from its axes). */
inline glm::dmat4 rigidOf(const glm::dmat4& m)
{
    glm::dmat4 r = m;
    for (int c = 0; c < 3; ++c)
    {
        const double l = glm::length(glm::dvec3(m[c]));
        if (l > 1e-12)
            r[c] = glm::dvec4(glm::dvec3(m[c]) / l, 0.0);
    }
    return r;
}

// Fitting ------------------------------------------------------------------------------------------------------------

struct PointDef
{
    Role role = Role::Main;
    std::string name;
    /** In the prop root's space. */
    glm::dmat4 frame{1.0};
    /** Handle radius at a grip, cm. */
    double radius = 1.6;
};

struct Fit
{
    std::vector<PointDef> points;
    /** How the points were found ("mesh", or why not). */
    std::string from;
    /** The long axis and its length, for reports. */
    glm::dvec3 axis{1.0, 0.0, 0.0};
    double length = 0.0;
    /** The shape the points were placed for: the one asked for, or the one read from the points. */
    Shape shape = Shape::Other;
    /** How a prop of this shape and size is held, aimed and put away; its shape setting left at Auto. */
    Settings suggested;
};

/** What a fit may be told rather than read from the points. */
struct FitHints
{
    Shape shape = Shape::Auto;
    /** Whether a second grip is an off hand's (else an alternate) where the shape could go either way. */
    std::optional<bool> twoHanded;
};

namespace detail
{
/** Eigenvectors of a symmetric 3x3 matrix, largest eigenvalue first (Jacobi rotations). */
inline std::array<glm::dvec3, 3> principalAxes(const glm::dmat3& cov)
{
    double a[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            a[i][j] = cov[j][i];
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 50; ++sweep)
    {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-18)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::abs(a[p][q]) < 1e-30)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 3; ++k)
                {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int x, int y) { return a[x][x] > a[y][y]; });
    std::array<glm::dvec3, 3> out;
    for (int i = 0; i < 3; ++i)
        out[size_t(i)] = glm::normalize(glm::dvec3(v[0][order[size_t(i)]], v[1][order[size_t(i)]], v[2][order[size_t(i)]]));
    return out;
}

/** The point set measured along an axis: slices from its low end to its high end. */
struct Slices
{
    glm::dvec3 centre{0.0}, axis{1.0, 0.0, 0.0};
    double lo = 0.0, hi = 0.0;
    std::vector<double> radius; // largest distance from the axis line in each slice
    std::vector<glm::dvec3> mean;
    std::vector<int> count;
    double at(int i) const { return lo + (hi - lo) * (double(i) + 0.5) / double(radius.size()); }
    int sliceOf(double t) const
    {
        const int n = int(radius.size());
        return std::clamp(int((t - lo) / std::max(hi - lo, 1e-9) * n), 0, n - 1);
    }
    /** A point on the axis at a fraction from lo (0) to hi (1). */
    glm::dvec3 along(double f) const { return centre + axis * (lo + (hi - lo) * f); }
};

inline Slices slice(const std::vector<glm::dvec3>& pts, const glm::dvec3& centre, const glm::dvec3& axis, int n = 32)
{
    Slices s;
    s.centre = centre;
    s.axis = axis;
    s.lo = 1e300;
    s.hi = -1e300;
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p - centre, axis);
        s.lo = std::min(s.lo, t);
        s.hi = std::max(s.hi, t);
    }
    s.radius.assign(size_t(n), 0.0);
    s.mean.assign(size_t(n), glm::dvec3(0.0));
    s.count.assign(size_t(n), 0);
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p - centre, axis);
        const int i = s.sliceOf(t);
        const glm::dvec3 off = (p - centre) - axis * t;
        s.radius[size_t(i)] = std::max(s.radius[size_t(i)], glm::length(off));
        s.mean[size_t(i)] += p;
        ++s.count[size_t(i)];
    }
    for (int i = 0; i < n; ++i)
        if (s.count[size_t(i)])
            s.mean[size_t(i)] /= double(s.count[size_t(i)]);
        else
            s.mean[size_t(i)] = s.centre + axis * s.at(i);
    // Empty slices (the long sides of a low-poly part have no vertices) take the narrower of the nearest slices either
    // side: a handle runs at its own radius up to the guard, which has vertices of its own.
    const std::vector<double> measured = s.radius;
    for (int i = 0; i < n; ++i)
        if (!s.count[size_t(i)])
        {
            double below = -1.0, above = -1.0;
            for (int j = i - 1; j >= 0 && below < 0.0; --j)
                if (s.count[size_t(j)])
                    below = measured[size_t(j)];
            for (int j = i + 1; j < n && above < 0.0; ++j)
                if (s.count[size_t(j)])
                    above = measured[size_t(j)];
            s.radius[size_t(i)] = below < 0.0 ? std::max(above, 0.0) : above < 0.0 ? below : std::min(below, above);
        }
    return s;
}

/** Median of a range of slice radii, [from, to] inclusive. */
inline double medianRadius(const Slices& s, int from, int to)
{
    std::vector<double> r;
    for (int i = std::max(0, std::min(from, to)); i <= std::min(int(s.radius.size()) - 1, std::max(from, to)); ++i)
        r.push_back(s.radius[size_t(i)]);
    if (r.empty())
        return 1.6;
    std::sort(r.begin(), r.end());
    return r[r.size() / 2];
}

/** The centre of the slice at a fraction along the axis (its points' mean, kept on the axis line where it has none). */
inline glm::dvec3 centreAt(const Slices& s, double f)
{
    const int i = s.sliceOf(s.lo + (s.hi - s.lo) * f);
    if (!s.count[size_t(i)])
        return s.along(f);
    const glm::dvec3 m = s.mean[size_t(i)];
    const double t = s.lo + (s.hi - s.lo) * f;
    return m + s.axis * (t - glm::dot(m - s.centre, s.axis));
}
} // namespace detail

namespace detail
{
/** The points' mean and principal axes, longest first, and their extents along those axes. */
struct Frame
{
    glm::dvec3 centre{0.0};
    std::array<glm::dvec3, 3> axes{};
    glm::dvec3 extent{0.0};
};

inline Frame frameOf(const std::vector<glm::dvec3>& pts)
{
    Frame f;
    for (const glm::dvec3& p : pts)
        f.centre += p;
    f.centre /= double(std::max<size_t>(pts.size(), 1));
    glm::dmat3 cov(0.0);
    for (const glm::dvec3& p : pts)
    {
        const glm::dvec3 d = p - f.centre;
        cov += glm::outerProduct(d, d);
    }
    cov /= double(std::max<size_t>(pts.size(), 1));
    f.axes = principalAxes(cov);
    for (int k = 0; k < 3; ++k)
    {
        double lo = 1e300, hi = -1e300;
        for (const glm::dvec3& p : pts)
        {
            const double v = glm::dot(p - f.centre, f.axes[size_t(k)]);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        f.extent[k] = hi - lo;
    }
    return f;
}

/**
 * Whether the points read as a firearm along `along` with its grip hanging toward `down`: a body along the top that runs
 * the whole length, and below it something narrower hanging clear of both ends (a grip, a magazine). An axe or a hammer
 * hangs its head at one end, which this leaves out.
 */
inline bool hangsLikeFirearm(const std::vector<glm::dvec3>& pts, const glm::dvec3& c, const glm::dvec3& along, const glm::dvec3& down)
{
    double lo = 1e300, hi = -1e300, top = -1e300, bot = 1e300;
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p - c, along), v = -glm::dot(p - c, down);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
        top = std::max(top, v);
        bot = std::min(bot, v);
    }
    const double length = hi - lo, height = top - bot;
    if (length < 1e-6 || height < 1e-6 || length < height)
        return false;
    const int n = 20;
    std::vector<char> lower(size_t(n), 0);
    double upperLo = 1e300, upperHi = -1e300;
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p - c, along), v = -glm::dot(p - c, down);
        if (v > top - 0.3 * height)
        {
            upperLo = std::min(upperLo, t);
            upperHi = std::max(upperHi, t);
        }
        if (v < bot + 0.45 * height)
            lower[size_t(std::clamp(int((t - lo) / length * n), 0, n - 1))] = 1;
    }
    // A sparse mesh leaves single empty slices inside a part.
    for (int i = 1; i + 1 < n; ++i)
        if (!lower[size_t(i)] && lower[size_t(i - 1)] && lower[size_t(i + 1)])
            lower[size_t(i)] = 2;
    int lowerCount = 0;
    // Some run of hanging slices stands clear of both ends (a grip, a magazine), not only a head at one end.
    bool clearOfEnds = false;
    for (int i = 0; i < n; ++i)
    {
        lowerCount += lower[size_t(i)] != 0;
        if (lower[size_t(i)] && (i == 0 || !lower[size_t(i - 1)]))
        {
            int j = i;
            while (j + 1 < n && lower[size_t(j + 1)])
                ++j;
            clearOfEnds |= i > 0 && j < n - 1;
        }
    }
    return upperHi - upperLo >= 0.85 * length && lowerCount <= n * 55 / 100 && clearOfEnds;
}
} // namespace detail

/**
 * Reads a prop's shape from its points (in its own space; up is the world's up there): flat and broad is a plate, a body
 * with a grip hanging below it a firearm, a long bent limb a bow, a narrow handle under a sudden wide guard a blade, one
 * end much wider than the other a haft, long and even a pole, anything else other.
 */
inline Shape classify(const std::vector<glm::dvec3>& pts, const glm::dvec3& up = {0.0, 1.0, 0.0})
{
    using namespace detail;
    (void)up;
    if (pts.size() < 4)
        return Shape::Other;
    const Frame f = frameOf(pts);
    if (f.extent[0] < 1e-6)
        return Shape::Other;
    // Firearms first: a pistol is compact and as thin as a plate. Props are usually modelled square to their own axes,
    // which a grip and stock pull the principal axes off, so both sets are tried.
    std::vector<glm::dvec3> axes(f.axes.begin(), f.axes.end());
    for (int k = 0; k < 3; ++k)
    {
        glm::dvec3 a(0.0);
        a[k] = 1.0;
        axes.push_back(a);
    }
    auto extentAlong = [&](const glm::dvec3& a) {
        double lo = 1e300, hi = -1e300;
        for (const glm::dvec3& p : pts)
        {
            lo = std::min(lo, glm::dot(p, a));
            hi = std::max(hi, glm::dot(p, a));
        }
        return hi - lo;
    };
    // The barrel runs along the prop's length, the grip hangs across it: the axes of one set, the first about the longest.
    for (size_t set = 0; set < 2; ++set)
        for (size_t i = set * 3; i < set * 3 + 3; ++i)
        {
            if (extentAlong(axes[i]) < 0.75 * f.extent[0])
                continue;
            for (size_t j = set * 3; j < set * 3 + 3; ++j)
                if (j != i)
                    for (double sign : {1.0, -1.0})
                        if (hangsLikeFirearm(pts, f.centre, axes[i], axes[j] * sign))
                            return Shape::Firearm;
        }
    if (f.extent[2] < 0.25 * f.extent[1] && f.extent[1] > 0.45 * f.extent[0])
        return Shape::Plate;
    if (f.extent[0] < 1.6 * f.extent[1])
        return Shape::Other;
    const Slices s = slice(pts, f.centre, f.axes[0]);
    const int n = int(s.radius.size());
    // A bow's middle stands off the line between its tips.
    {
        glm::dvec3 tipA(0.0), tipB(0.0);
        int ca = 0, cb = 0;
        for (int i = 0; i < n / 8; ++i)
        {
            if (s.count[size_t(i)])
            {
                tipA += s.mean[size_t(i)] * double(s.count[size_t(i)]);
                ca += s.count[size_t(i)];
            }
            if (s.count[size_t(n - 1 - i)])
            {
                tipB += s.mean[size_t(n - 1 - i)] * double(s.count[size_t(n - 1 - i)]);
                cb += s.count[size_t(n - 1 - i)];
            }
        }
        if (ca && cb)
        {
            tipA /= double(ca);
            tipB /= double(cb);
            glm::dvec3 off = centreAt(s, 0.5) - 0.5 * (tipA + tipB);
            off -= f.axes[0] * glm::dot(off, f.axes[0]);
            // Bent well past the limb's own thickness, the thinnest extent.
            if (glm::length(off) > 0.08 * f.extent[0] && glm::length(off) > 2.5 * f.extent[2])
                return Shape::Bow;
        }
    }
    // A guard: one narrow band of slices far wider than the handle below it and the blade above, short of the middle.
    {
        const int g = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        const int fromEnd = std::min(g, n - 1 - g);
        const bool handleLow = g < n / 2;
        const double handle = handleLow ? medianRadius(s, 0, std::max(0, g - 2)) : medianRadius(s, std::min(n - 1, g + 2), n - 1);
        const double blade = handleLow ? medianRadius(s, std::min(n - 1, g + 3), n - 1) : medianRadius(s, 0, std::max(0, g - 3));
        const double r = s.radius[size_t(g)];
        const bool narrow = (g - 3 < 0 || s.radius[size_t(g - 3)] < 0.75 * r) && (g + 3 >= n || s.radius[size_t(g + 3)] < 0.75 * r);
        if (fromEnd >= 1 && fromEnd < n * 45 / 100 && narrow && r > 1.8 * handle && r > 1.4 * blade)
            return Shape::Blade;
    }
    const double a = medianRadius(s, 0, n / 3), b = medianRadius(s, n - 1 - n / 3, n - 1);
    if (std::max(a, b) > 1.6 * std::max(std::min(a, b), 1e-6))
        return Shape::Haft;
    if (f.extent[0] >= 60.0)
        return Shape::Pole;
    return Shape::Other;
}

/** How a prop of a shape and size is held, aimed and put away, when nothing says otherwise. */
inline Settings suggestedSettings(Shape shape, double length, bool twoHanded, bool handgun)
{
    Settings s;
    s.twoHanded = twoHanded;
    switch (shape)
    {
    case Shape::Blade:
        s.aim = AimStyle::Guard;
        s.drop = twoHanded ? 18.0 : 12.0;
        s.reach = twoHanded ? 0.55 : 0.6;
        s.holster = twoHanded ? HolsterSpot::Back : length < 40.0 ? HolsterSpot::ThighRight : HolsterSpot::HipLeft;
        break;
    case Shape::Haft:
        s.aim = AimStyle::Guard;
        s.drop = 10.0;
        s.reach = 0.6;
        s.holster = length > 70.0 ? HolsterSpot::Back : HolsterSpot::HipLeft;
        break;
    case Shape::Pole:
        s.aim = AimStyle::Guard;
        s.drop = 6.0;
        s.reach = 0.6;
        s.holster = HolsterSpot::Back;
        break;
    case Shape::Firearm:
    case Shape::Tube:
        s.trigger = true;
        s.mainPose = "trigger";
        s.aim = handgun ? AimStyle::Extend : AimStyle::Shoulder;
        s.holster = handgun ? HolsterSpot::ThighRight : HolsterSpot::Back;
        break;
    case Shape::Plate:
        s.aim = AimStyle::Guard;
        s.reach = 0.45;
        s.holster = HolsterSpot::Back;
        break;
    case Shape::Bow:
        s.aim = AimStyle::Extend;
        s.reach = 0.95;
        s.offPose = "hook";
        s.holster = HolsterSpot::Back;
        break;
    default:
        s.holster = length > 70.0 ? HolsterSpot::Back : HolsterSpot::Belt;
        break;
    }
    return s;
}

/**
 * Grip points for a prop fitted to its mesh points (in the prop root's space), from its shape alone unless hints say
 * what it is: the long axis by principal components, which end is the handle by the cross-section along it (a guard, a
 * head, a grip below a barrel), and the grips, muzzle or tip, and stock where that shape has them. up is the world's up
 * in the prop root's space, which breaks ties for shapes that don't tell (a plain tube). Always gives points; `from`
 * says how they were placed.
 */
inline Fit fitPoints(const std::vector<glm::dvec3>& pts, const glm::dvec3& upIn = {0.0, 1.0, 0.0}, const FitHints& hints = {})
{
    using namespace detail;
    const glm::dvec3 up = glm::length(upIn) > 1e-9 ? glm::normalize(upIn) : glm::dvec3(0, 1, 0);
    const Shape shape = hints.shape != Shape::Auto ? hints.shape : classify(pts, up);
    Fit fit;
    fit.shape = shape;
    auto add = [&](Role r, const std::string& name, const glm::dmat4& frame, double radius = 1.6) {
        fit.points.push_back({r, name, frame, radius});
    };
    if (pts.size() < 4)
    {
        // Nothing to measure: points at the origin, main along +X, for the user to place.
        fit.from = "no mesh under the prop: points start at its origin";
        const bool two = hints.twoHanded.value_or(false);
        add(Role::Main, "main", frameXZ(glm::dvec3(0.0), {0, 1, 0}, {0, 0, 1}));
        if (two)
            add(Role::Off, "off", frameXZ(glm::dvec3(0.0, -10.0, 0.0), {0, 1, 0}, {0, 0, 1}));
        add(shape == Shape::Firearm || shape == Shape::Tube || shape == Shape::Bow ? Role::Muzzle : Role::Tip, "aim",
            frameXZ(glm::dvec3(0.0, 0.0, 20.0), {0, 0, 1}, up));
        fit.suggested = suggestedSettings(shape, 0.0, two, true);
        return fit;
    }
    fit.from = "mesh";
    const Frame pf = frameOf(pts);
    const glm::dvec3 c = pf.centre;
    std::array<glm::dvec3, 3> e = pf.axes;
    bool two = false, handgun = false;
    // A firearm's grip and stock pull principal axes off the barrel; props are usually modelled square to their own
    // axes, so those win when the longest of them is about as long as the principal one.
    if (shape == Shape::Firearm || shape == Shape::Tube)
    {
        glm::dvec3 lo(1e300), hi(-1e300);
        for (const glm::dvec3& p : pts)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        const glm::dvec3 ext = hi - lo;
        std::array<int, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int a, int b) { return ext[a] > ext[b]; });
        double pcaLen = 0.0;
        {
            double a = 1e300, b = -1e300;
            for (const glm::dvec3& p : pts)
            {
                a = std::min(a, glm::dot(p - c, e[0]));
                b = std::max(b, glm::dot(p - c, e[0]));
            }
            pcaLen = b - a;
        }
        if (ext[order[0]] >= 0.75 * pcaLen)
            for (int k = 0; k < 3; ++k)
            {
                glm::dvec3 axis(0.0);
                axis[order[size_t(k)]] = 1.0;
                e[size_t(k)] = axis;
            }
    }
    Slices s = slice(pts, c, e[0]);
    const int n = int(s.radius.size());
    fit.length = s.hi - s.lo;
    const double len = fit.length;

    // Flip the long axis so +axis runs from the handle end toward the business end; returns the slices re-measured.
    auto orient = [&](bool flip) {
        if (flip)
        {
            e[0] = -e[0];
            s = slice(pts, c, e[0]);
        }
        fit.axis = e[0];
    };

    switch (shape)
    {
    case Shape::Blade:
    {
        // The guard is the widest slice; the handle is the shorter side of it (pommel to guard).
        int guard = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        orient(guard > n / 2);
        guard = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        double guardT = s.at(guard);
        // A blade with no guard (widest slice in the middle): the narrower end is the handle.
        if (guard > n * 2 / 5)
        {
            const double a = medianRadius(s, 0, n / 6), b = medianRadius(s, n - 1 - n / 6, n - 1);
            if (a > b)
                orient(true);
            guardT = s.lo + 0.22 * len;
            guard = s.sliceOf(guardT);
        }
        const double handleLen = guardT - s.lo;
        const double r = medianRadius(s, 0, std::max(0, guard - 1));
        // The handle's cross-section: palms face its flat side, the blade's edge leads the fingers.
        const glm::dvec3 edge = e[1], flat = e[2];
        const double fromGuard = std::min(4.5, 0.3 * handleLen);
        const glm::dvec3 main = centreAt(s, (guardT - fromGuard - s.lo) / len);
        add(Role::Main, "main", frameXZ(main, e[0], flat), r);
        // Room for two hands, or a pommel to hold by.
        two = hints.twoHanded.value_or(handleLen >= 17.0);
        if (two || handleLen > 15.0)
        {
            const double offT = std::max(guardT - fromGuard - 9.0, s.lo + 0.25 * handleLen);
            add(two ? Role::Off : Role::Alt, two ? "off" : "pommel", frameXZ(centreAt(s, (offT - s.lo) / len), e[0], flat), r);
        }
        add(Role::Tip, "tip", frameXZ(centreAt(s, 1.0) + e[0] * (s.hi - glm::dot(centreAt(s, 1.0) - c, e[0])), e[0], edge));

        break;
    }
    case Shape::Haft:
    {
        // The head is the widest slice and sits at the business end; the grip is near the other.
        const int head = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        orient(head < n / 2);
        // Each grip's radius where it is: a bottle's neck and its body differ.
        auto radiusAt = [&](double f) {
            const int i = s.sliceOf(s.lo + (s.hi - s.lo) * f);
            return medianRadius(s, i - 1, i + 1);
        };
        two = hints.twoHanded.value_or(len > 70.0);
        add(Role::Main, "main", frameXZ(centreAt(s, 0.1), e[0], e[2]), radiusAt(0.1));
        add(two ? Role::Off : Role::Alt, two ? "off" : "choke", frameXZ(centreAt(s, 0.4), e[0], e[2]), radiusAt(0.4));
        add(Role::Tip, "head", frameXZ(centreAt(s, 1.0), e[0], e[1]));
        break;
    }
    case Shape::Pole:
    {
        // A spearhead or a heavier end leads; an even staff leads with whichever end faces the world's forward.
        const double a = medianRadius(s, 0, n / 8), b = medianRadius(s, n - 1 - n / 8, n - 1);
        orient(std::abs(a - b) > 0.15 * std::max(a, b) ? a > b : false);
        const double r = medianRadius(s, n / 4, n * 3 / 4);
        add(Role::Main, "main", frameXZ(centreAt(s, 0.32), e[0], e[2]), r);
        add(Role::Off, "off", frameXZ(centreAt(s, 0.58), e[0], e[2]), r);
        add(Role::Alt, "middle", frameXZ(centreAt(s, 0.5), e[0], e[2]), r);
        add(Role::Tip, "tip", frameXZ(s.along(1.0), e[0], e[1]));
        two = hints.twoHanded.value_or(true);
        break;
    }
    case Shape::Firearm:
    case Shape::Tube:
    {
        // Down is the side the grip (and magazine) hangs: the cross axis whose far side reaches furthest.
        glm::dvec3 down(0.0);
        double best = -1.0;
        for (int k = 1; k < 3; ++k)
            for (double sign : {1.0, -1.0})
            {
                const glm::dvec3 d = e[size_t(k)] * sign;
                double reach = 0.0, mean = 0.0;
                for (const glm::dvec3& p : pts)
                {
                    const double v = glm::dot(p - c, d);
                    reach = std::max(reach, v);
                    mean += v;
                }
                // A tube with nothing hanging off it: down is the world's down.
                const double score = reach + (shape == Shape::Tube ? 0.5 * std::max(0.0, glm::dot(d, -up)) * reach : 0.0);
                if (score > best)
                {
                    best = score;
                    down = d;
                }
            }
        if (glm::dot(down, -up) < -0.5 && shape == Shape::Tube)
            down = -down;
        const glm::dvec3 upAxis = -down;
        double topV = -1e300, botV = 1e300;
        for (const glm::dvec3& p : pts)
        {
            topV = std::max(topV, glm::dot(p - c, upAxis));
            botV = std::min(botV, glm::dot(p - c, upAxis));
        }
        const double height = topV - botV;
        // The grip region: the lowest part, below the barrel.
        std::vector<glm::dvec3> low;
        for (const glm::dvec3& p : pts)
            if (glm::dot(p - c, upAxis) < botV + 0.45 * height)
                low.push_back(p);
        glm::dvec3 lowMean(0.0);
        for (const glm::dvec3& p : low)
            lowMean += p;
        lowMean = low.empty() ? c : lowMean / double(low.size());
        // The muzzle is the end further from where the grip hangs.
        orient(glm::dot(lowMean - c, e[0]) > 0.0);
        const glm::dvec3 fwd = e[0];
        // The barrel line: the top band's centre height.
        const double barrelV = topV - 0.2 * height;
        auto onBarrel = [&](double f) {
            glm::dvec3 p = s.along(f);
            return p + upAxis * (barrelV - glm::dot(p - c, upAxis));
        };
        if (shape == Shape::Tube)
        {
            const double rad = 0.5 * height;
            const glm::dvec3 axisLine = s.along(0.0) + upAxis * (topV - rad - glm::dot(s.along(0.0) - c, upAxis));
            auto onTube = [&](double f) { return axisLine + fwd * (len * f); };
            add(Role::Main, "main", frameXZ(onTube(0.45) - upAxis * (rad + 4.0), upAxis, fwd), 1.6);
            add(Role::Off, "off", frameXZ(onTube(0.62) - upAxis * (rad + 4.0), upAxis, fwd), 1.6);
            add(Role::Stock, "shoulder", frameXZ(onTube(0.4) - upAxis * rad, fwd, upAxis));
            add(Role::Muzzle, "muzzle", frameXZ(onTube(1.0), fwd, upAxis));
            two = hints.twoHanded.value_or(true);
            break;
        }
        // The main grip: the rear-most part of the grip region, its handle running up toward the barrel.
        std::vector<glm::dvec3> grip;
        double rear = 1e300;
        for (const glm::dvec3& p : low)
            rear = std::min(rear, glm::dot(p - c, fwd));
        const bool pistol = len < 35.0;
        handgun = pistol;
        two = hints.twoHanded.value_or(!pistol);
        for (const glm::dvec3& p : low)
        {
            const double t = glm::dot(p - c, fwd);
            // A long gun's stock reaches lowest at the back: its grip is ahead of it, a fifth of the way along.
            const double from = pistol ? rear : s.lo + 0.18 * len, to = pistol ? rear + 0.6 * len : s.lo + 0.42 * len;
            if (t >= from && t <= to)
                grip.push_back(p);
        }
        glm::dvec3 g(0.0);
        for (const glm::dvec3& p : grip)
            g += p;
        g = grip.empty() ? (pistol ? lowMean : onBarrel(0.3) - upAxis * 5.0) : g / double(grip.size());
        double gripR = 0.0;
        for (const glm::dvec3& p : grip)
        {
            const glm::dvec3 d = p - g;
            gripR = std::max(gripR, glm::length(d - upAxis * glm::dot(d, upAxis)));
        }
        gripR = grip.empty() ? 1.6 : std::clamp(0.6 * gripR, 1.0, 3.5);
        // The handle leans back as it goes down: its axis from the grip's bottom toward where it meets the barrel.
        const glm::dvec3 handleUp = glm::normalize(upAxis - 0.25 * fwd);
        add(Role::Main, "main", frameXZ(g, handleUp, fwd), gripR);
        if (pistol)
        {
            // The support hand cups the shooting hand: a little ahead and to the side.
            const glm::dvec3 side = glm::normalize(glm::cross(upAxis, fwd));
            add(Role::Off, "support", frameXZ(g - side * 2.5 + fwd * 1.5 - upAxis * 1.5, handleUp, fwd), gripR);
            add(Role::Muzzle, "muzzle", frameXZ(onBarrel(1.0), fwd, upAxis));
        }
        else
        {
            // The off hand under the handguard, palm up, thumb along the barrel.
            const double offF = 0.6;
            const glm::dvec3 under = onBarrel(offF) - upAxis * (0.08 * height + 2.0);
            add(Role::Off, "off", frameXZ(under, fwd, upAxis), std::clamp(0.12 * height, 1.5, 3.0));
            add(Role::Alt, "magwell", frameXZ(onBarrel(0.45) - upAxis * (0.25 * height), upAxis, fwd), 2.0);
            add(Role::Stock, "stock", frameXZ(onBarrel(0.0) - upAxis * (0.15 * height), fwd, upAxis));
            add(Role::Muzzle, "muzzle", frameXZ(onBarrel(1.0), fwd, upAxis));
        }
        break;
    }
    case Shape::Plate:
    {
        // The normal is the thin axis; the handle side is the one things stand out on (straps, a grip).
        glm::dvec3 nrm = e[2];
        double front = 0.0, back = 0.0;
        for (const glm::dvec3& p : pts)
        {
            const double v = glm::dot(p - c, nrm);
            front = std::max(front, v);
            back = std::max(back, -v);
        }
        if (front > back)
        {
            nrm = -nrm;
            std::swap(front, back);
        }
        // nrm now points out of the face; the hand is behind, gripping a handle that runs up the shield.
        glm::dvec3 vertical = std::abs(glm::dot(e[0], up)) > std::abs(glm::dot(e[1], up)) ? e[0] : e[1];
        if (glm::dot(vertical, up) < 0.0)
            vertical = -vertical;
        fit.axis = nrm;
        const glm::dvec3 grip = c - nrm * (0.6 * back + 2.0);
        add(Role::Main, "main", frameXZ(grip, vertical, nrm), 1.6);
        add(Role::Tip, "face", frameXZ(c + nrm * front, nrm, vertical));
        break;
    }
    case Shape::Bow:
    {
        // Upright; the arrow flies across the bow, away from the string side, which the limbs' tips lean toward.
        glm::dvec3 vertical = e[0];
        if (glm::dot(vertical, up) < 0.0)
            vertical = -vertical;
        fit.axis = vertical;
        Slices sv = slice(pts, c, vertical);
        const glm::dvec3 tips = 0.5 * (sv.mean.front() + sv.mean.back());
        const glm::dvec3 mid = centreAt(sv, 0.5);
        glm::dvec3 aim = mid - tips;
        aim -= vertical * glm::dot(aim, vertical);
        if (glm::length(aim) < 1e-6)
            aim = e[1];
        aim = glm::normalize(aim);
        const double r = medianRadius(sv, n * 2 / 5, n * 3 / 5);
        add(Role::Main, "main", frameXZ(mid, vertical, aim), r);
        // The draw hand on the string at rest, level with the grip.
        const double stringOff = std::max(0.0, glm::dot(mid - tips, aim)) + 1.0;
        add(Role::Off, "string", frameXZ(mid - aim * stringOff + vertical * 2.0, vertical, -aim), 0.5);
        add(Role::Muzzle, "rest", frameXZ(mid + vertical * (r + 2.0), aim, vertical));
        two = hints.twoHanded.value_or(true);
        break;
    }
    case Shape::Auto:
    case Shape::Count:
    case Shape::Other:
    {
        // The narrower end is the handle.
        const double a = medianRadius(s, 0, n / 5), b = medianRadius(s, n - 1 - n / 5, n - 1);
        orient(a > b);
        const double r = medianRadius(s, 0, n / 4);
        add(Role::Main, "main", frameXZ(centreAt(s, 0.12), e[0], e[2]), r);
        two = hints.twoHanded.value_or(len > 70.0);
        // Long enough for a second hand: one further along, for two hands or to change grip.
        if (len > 40.0)
            add(two ? Role::Off : Role::Alt, two ? "off" : "grip", frameXZ(centreAt(s, 0.45), e[0], e[2]), r);
        add(Role::Tip, "tip", frameXZ(s.along(1.0), e[0], e[1]));
        break;
    }
    }
    fit.suggested = suggestedSettings(shape, len, two, handgun);
    return fit;
}
} // namespace puppeteer::prop
