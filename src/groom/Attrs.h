#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <cstdint>
#include <functional>
#include <string_view>

/**
 * The groom's attribute schema on lock splines. Per-curve values live in Spline::attrs, per-point ones in
 * Spline::pointAttrs (radius and tilt are the lock's width scale and twist). Names are GroomLab's channel names where
 * GroomLab has one (export_view.h), so a guide's attributes become the strands' channels of the same name, and the
 * export maps them to Unreal's groom_* in one place.
 */
namespace lila::groom::attr
{
// Per curve.
inline constexpr std::string_view kId = "id";               // stable per lock: seeds its randomness, groom_id
inline constexpr std::string_view kWidth = "width";         // scale on the groom's lock width, groom_width
inline constexpr std::string_view kThickness = "thickness"; // scale on the groom's thickness
inline constexpr std::string_view kProfile = "profile";     // profile index, absent for the groom's own
inline constexpr std::string_view kGroup = "groupId";       // the guide's group in the hierarchy
inline constexpr std::string_view kClump = "clumpId";       // locks drawn together (a braid's strands)
inline constexpr std::string_view kGuide = "guide";         // 1 on guides, 0 on curves a modifier made, groom_guide
inline constexpr std::string_view kStray = "stray";         // 1 on flyaways
inline constexpr std::string_view kColorR = "colorR";       // groom_color
inline constexpr std::string_view kColorG = "colorG";
inline constexpr std::string_view kColorB = "colorB";
inline constexpr std::string_view kRoughness = "roughness"; // groom_roughness

// Per point, painted: added to the matching modifier's amount.
inline constexpr std::string_view kCurl = "curl";
inline constexpr std::string_view kFrizz = "frizz";
inline constexpr std::string_view kClumpWeight = "clumpWeight";
inline constexpr std::string_view kWave = "wave";

// Per point, on resolved locks (the groom's outCurve): the swept cross-section.
inline constexpr std::string_view kHalfWidth = "halfWidth";
inline constexpr std::string_view kHalfThick = "halfThick";
inline constexpr std::string_view kSideX = "sideX";
inline constexpr std::string_view kSideY = "sideY";
inline constexpr std::string_view kSideZ = "sideZ";
inline constexpr std::string_view kAlong = "v"; // 0 at the root to 1 at the tip

LILA_SCENE_API double get(const curve::Spline& s, std::string_view name, double def);
LILA_SCENE_API void set(curve::Spline& s, std::string_view name, double v);
LILA_SCENE_API double point(const curve::Spline& s, std::string_view name, size_t i, double def);
/** The named per-point array, sized to the spline and filled with def when it was missing. */
LILA_SCENE_API std::vector<double>& points(curve::Spline& s, std::string_view name, double def);

/** Gives every curve without an id the next free one. Returns how many it named. */
LILA_SCENE_API int ensureIds(curve::Splines& s);
LILA_SCENE_API int64_t nextId(const curve::Splines& s);
/** A seed for curve s mixed with a modifier's own, so a lock keeps its look while others come and go. */
LILA_SCENE_API uint32_t seed(const curve::Spline& s, size_t index, uint32_t salt);

/** The curve as a poly of n points evenly spaced by length, every per-point array (radius, tilt, attrs) carried along. */
LILA_SCENE_API curve::Spline resampled(const curve::Spline& s, int n);
/** Length along the points. */
LILA_SCENE_API double length(const curve::Spline& s);
} // namespace lila::groom::attr
