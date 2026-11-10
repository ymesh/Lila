#pragma once

#include "groom/Locks.h"

#include <groomlab/guide_hierarchy.h>
#include <groomlab/guide_set.h>
#include <groomlab/scalp_mesh.h>

namespace lila::groom
{
/**
 * A groom's guides are a GroomLab GuideHierarchy; Lila's lock tools see them as lock splines (Attrs.h): per curve id,
 * group, width, thickness and profile, per point radius (width scale) and tilt (twist), the painted layers as point
 * attributes. A guide's tube is the lock: width = LockOptions::width x width x radius at each point, aspect =
 * LockOptions::thickness x thickness, twist = tilt, its shape the profile (Crescent and Custom as library shapes).
 */

/** The hierarchy's guides as lock splines, world space, in hierarchy order. */
Splines lockSplines(const groomlab::GuideHierarchy& h, const LockOptions& o);

/**
 * Lock splines back into the hierarchy, matched by id: a guide whose spline is what lockSplines makes of it is left as
 * it is (no drift through the conversion), a changed one takes the new points, tube and layers, a new id is added to
 * its group (or the first group, made when there is none), and a guide with no spline goes. True when anything changed.
 */
bool applyLockSplines(groomlab::GuideHierarchy& h, const Splines& s, const LockOptions& o);

/** A flat guide (a graph's Guides value) as a lock spline, and back; `was` (the guide of that id before, or null) keeps its
 * frame and library shape. */
Spline lockSplineOf(const groomlab::Guide& g, const LockOptions& o);
groomlab::Guide guideOfLock(const Spline& s, const LockOptions& o, const groomlab::Guide* was);

/** The tube a lock spline describes. */
groomlab::TubeProfile tubeOfLock(const Spline& s, const LockOptions& o, groomlab::GuideHierarchy& h);
} // namespace lila::groom
