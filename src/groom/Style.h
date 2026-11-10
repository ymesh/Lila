#pragma once

#include "core/Export.h"
#include "groom/Locks.h"

/**
 * Style modifiers: curve to curve operations of the groom's stack. Each works on locks and strands alike, scales by a
 * root-to-tip ramp, takes painted per-point amounts where it has one (curl, frizz, clumpWeight, wave) and draws its
 * randomness from each curve's id, so a lock keeps its look while others are added or removed.
 */
namespace lila::groom
{
enum class Style : uint8_t
{
    Split,    // each lock into sub-locks fanned out around it
    Clump,    // curves pulled together toward their clump's middle
    Curl,     // helix around the curve
    Wave,     // side to side in the lock's flat plane
    Frizz,    // fine random kinks
    Noise,    // smooth noise in space, so neighbours bend together
    Length,   // random shortening
    Cut,      // trims to a length
    Flyaways, // stray strands off locks
    Count
};

LILA_SCENE_API const char* styleName(Style s);
LILA_SCENE_API bool parseStyle(std::string_view name, Style* out);

struct StyleOptions
{
    Style kind = Style::Curl;
    /** Curl, clump, length: 0 to 1 strength. Wave, frizz, noise: cm. Flyaways: strays per lock. */
    double amount = 1.0;
    /** Curl, wave, frizz: cycles per cm. Noise: features per cm. */
    double frequency = 0.25;
    /** Curl: radius, cm. */
    double radius = 1.0;
    /** Root to tip scale; null uses the kind's own default. */
    Ramp ramp;
    /** Split: sub-locks per lock. Clump: clumps (0 groups by the clump attr). */
    int count = 3;
    /** Split: how far sub-locks fan out, cm. Flyaways: how far strays stray, cm. */
    double spread = 1.0;
    /** Cut: longest a curve may be, cm. Flyaways: stray length as a fraction of its lock. */
    double length = 0.6;
    /** Split: sub-lock width as a fraction of its lock's. */
    double widthScale = 0.55;
    /** Variation between curves (phase, size), 0 to 1. */
    double randomness = 0.5;
    uint32_t seed = 1;
};

/** Applies one modifier in place. The scalp orients split and wave; optional. */
LILA_SCENE_API void applyStyle(Splines& s, const StyleOptions& o, const Surface* scalp);
} // namespace lila::groom
