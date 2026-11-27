#pragma once

// What each kit's layout function builds with: deterministic randomness, fictional names, and emitters for slots,
// envelopes, ground calls and checked parts.

#include "Sites.h"

#include <lila/Names.h>

#include <initializer_list>

namespace openworld::sites
{
inline constexpr double kPi = 3.14159265358979323846;
using NK = lila::names::Kind;

class Kit
{
public:
    Kit(const Spec& s, Layout& out);

    const Spec& spec;
    Layout& L;

    /** 0..1, the same for the same site seed and keys. */
    double rnd(uint64_t a, uint64_t b = 0, uint64_t c = 0) const;
    /** A whole number in [lo, hi]. */
    int pick(int lo, int hi, uint64_t a, uint64_t b = 0, uint64_t c = 0) const;

    /** A fictional name, recorded under role. */
    std::string name(const std::string& role, lila::names::Kind kind, std::string_view program = {});
    /** A fictional street name (cls 0 arterial .. 4 path), recorded under role. */
    std::string street(const std::string& role, int cls);

    SlotOut& slot(std::string path, std::string type, P2 at, double heading = 0.0, double y = 0.0, Level level = Level::Ground);
    /**
     * Slots of a segment type along a-b: pieces close to nominal long, each scaled along its x to fit, numbered from
     * prefix/0. Their x runs along the line. Returns how many.
     */
    int along(const std::string& prefix, const std::string& type, P2 a, P2 b, double nominal, double y = 0.0, Level level = Level::Ground,
              double heightScale = 1.0);
    /** along() over a closed or open polyline, skipping gaps (pairs of distances along it). */
    int alongRing(const std::string& prefix, const std::string& type, const Ring& pts, bool closed, double nominal,
                  const std::vector<std::pair<double, double>>& gaps = {}, double heightScale = 1.0);

    EnvelopeOut& envelope(std::string path, Ring footprint, std::string style, std::string program, std::string use, int floors, double height,
                          std::string roof);
    GroundOp& ground(std::string id, std::string command, json flags);
    Part& part(std::string id, std::string kind, Ring ring, Part::Needs needs);

private:
    lila::names::NameBook mBook;
};

/** Front direction to heading: the heading whose +Z faces f. */
double facing(P2 f);
/** Heading whose +X runs along d. */
double alongDir(P2 d);

void buildPort(Kit& k);
void buildCruise(Kit& k);
void buildLighthouse(Kit& k);
void buildCemetery(Kit& k);
void buildAirstrip(Kit& k);
void buildFarm(Kit& k);
void buildVillage(Kit& k);
void buildCompound(Kit& k);
void buildIsland(Kit& k);
} // namespace openworld::sites
