#pragma once

#include "slots/Slots.h"

#include "gn/Geometry.h"

namespace lila::slots
{
/** How a geometry-nodes generator's instances become slots. */
struct InstanceSlots
{
    /** The owner's stable id; every slot id starts with it. */
    std::string owner;
    /** Slot types; with typeAttribute, its int value picks the entry (clamped), else every instance takes types[0]. */
    std::vector<std::string> types{"prop"};
    std::string typeAttribute;
    /**
     * Int instance attributes whose values make each slot's local path ("rule2/cellX10/cellZ-3"); empty uses the instance
     * index ("i12"), which is stable only while the generator's inputs are.
     */
    std::vector<std::string> pathAttributes;
    std::vector<std::string> regions;
    std::vector<std::string> tags;
    /** The generator's payload for every slot (its own references are what draws until a rule or override says otherwise). */
    Payload defaults;
    /**
     * Per instance, in instance order: slots made by the generator itself (id, type, owner, regions, tags, payload), for
     * generators whose ids are paths rather than int attributes. Their transforms come from the instances. When set, the
     * fields above are ignored.
     */
    std::vector<Slot> slots;
};

/**
 * Makes each instance a slot: publishes the slots under `publisher` (the modifier, ExecEnv::owner; 0 skips publishing),
 * then swaps each instance's reference for its resolved payload's mesh, drops hidden ones, applies pins, and writes the
 * int attribute "slotIndex" (the slot's index in what was published). Instances keep their own reference while no rule
 * or override sets a visual. Safe on evaluation threads: meshes not built yet are built on the main thread after the
 * evaluation, which then runs again.
 */
LILA_SCENE_API gn::GeometryPtr applyToInstances(const gn::GeometryPtr& in, NodeId publisher, const InstanceSlots& spec);

/**
 * The same with slots the generator made itself (ids, types, owners, regions, tags and payloads its own), one per
 * instance in order; each slot's transform is taken from its instance.
 */
LILA_SCENE_API gn::GeometryPtr applySlots(const gn::GeometryPtr& in, NodeId publisher, std::vector<Slot> slots);

/** The local path applyToInstances gives instance k. */
LILA_SCENE_API std::string instancePath(const gn::Instances& in, size_t k, const std::vector<std::string>& pathAttributes);
} // namespace lila::slots
