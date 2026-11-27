#pragma once

#include "Sites.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace openworld::sites
{
/** What a Site Kit node evaluated last, by its modifier's scene node id: the commands and the check read it. */
struct Evaluated
{
    Spec spec;
    std::shared_ptr<const Layout> layout;
    /** Per envelope: lowest and highest ground under its footprint, world up, cm. */
    std::vector<std::pair<double, double>> envelopeGround;
    /** Ground at the site's origin. */
    double originHeight = 0.0;
    /** False when no ground was connected: everything stands at height 0. */
    bool hasGround = false;
};

bool evaluatedSite(uint64_t modifier, Evaluated* out);
void forgetSite(uint64_t modifier);

/** The layout for a spec, cached by its text (and the avoided names); throws std::runtime_error for a bad spec. */
std::shared_ptr<const Layout> layoutFor(const Spec& s);


/** An envelope in world space, as Settlement exports them, with the site's own fields. */
json envelopeJson(const Evaluated& e, size_t index);

bool registerSiteNode(lila::ext::Context& ctx);
/** Forgets what site modifiers no longer in the scene evaluated. */
void forgetGoneSites();
/** Forgets the ground grids kept for the last terrains sites were laid on (each holds its terrain mesh). */
void forgetGrounds();
} // namespace openworld::sites
