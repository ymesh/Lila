#pragma once

#include "core/Export.h"

#include "gn/NodeLib.h"

#include <memory>

namespace lila::gn
{
/** A group compiled for evaluation: the nodes the output depends on, in dependency order. */
struct Plan
{
    struct Source
    {
        int step = -1;
        int output = -1;
        SocketType type = SocketType::Float;
    };
    struct Step
    {
        size_t node = 0; // index into group.nodes
        const NodeDecl* decl = nullptr;
        Sockets sockets;
        std::vector<std::vector<Source>> inputs; // links per input socket
        /** Depends on time or the view: such a node upstream, or is one. Never cached between evaluations. */
        bool dynamic = false;
        /** Upstream of the simulation node (or is it): what runs while its inputs are sampled. */
        bool feedsStateful = false;
    };

    Group group;
    std::vector<Step> steps;
    int outputStep = -1;
    /** The active Viewer node's step, run after the output's; -1 when no Viewer is active. */
    int viewerStep = -1;
    /** Gizmo nodes' steps (gn/Tool.h), run when the evaluation collects gizmos; a broken one only loses its handle. */
    std::vector<int> gizmoSteps;
    /** Set when the group can't run (a cycle, unknown nodes); evaluation then passes the input through. */
    std::string error;
    bool usesTime = false;
    /** Reads Is Viewport or the active camera, here or in a nested group. */
    bool usesView = false;
    /** The simulation node's step, -1 when the group has none. */
    int statefulStep = -1;
    /** A step feeding the simulation node depends on time, so its inputs differ from frame to frame. */
    bool statefulInputsUseTime = false;
    /** Per step, whether the output or viewer needs it: steps only a baked Bake node reads are skipped. Empty when all run. */
    std::vector<uint8_t> needed;
};

/**
 * Results of a plan's time-independent steps, kept by a modifier between evaluations. Valid while the plan and key
 * (a hash of everything a step can read from outside: group inputs, the input mesh, Object Info) are unchanged.
 */
struct StepCache
{
    const Plan* plan = nullptr;
    uint64_t key = 0;
    std::vector<std::vector<Value>> results;
    std::vector<uint8_t> valid;
};

using PlanPtr = std::shared_ptr<const Plan>;

LILA_SCENE_API PlanPtr compile(const Group& g);
/** Runs the plan on the worker thread. Throws ExecError naming the failing node. */
LILA_SCENE_API GeometryPtr execute(const Plan& plan, const ExecEnv& env);
/** Runs the plan and returns what reached each Group Output socket (geometry first, then the group's outputs). */
LILA_SCENE_API std::vector<Value> executeOutputs(const Plan& plan, const ExecEnv& env);
/** The compiled plan for a definition, shared between evaluations (nested Group nodes use it). */
LILA_SCENE_API PlanPtr cachedPlan(const json& definition);
/** The Viewer node that is switched on in the group, or null. */
LILA_SCENE_API const GNode* activeViewer(const Group& g);
} // namespace lila::gn
