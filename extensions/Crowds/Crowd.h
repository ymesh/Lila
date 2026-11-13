#pragma once

#include "CrowdCore.h"

#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Node.h>
#include <lila/Simulation.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

/**
 * Crowds in the scene: the crowd node (a simulation on the base framework: cached per frame, deterministic), its spawn
 * groups stored as JSON on the node, the characters its agents wear, and the bake of agents onto real characters.
 * Lila is centimeters, the runtime meters; both Y up.
 */
namespace crowds
{
using json = nlohmann::json;
using lila::scene::Graph;
using lila::scene::NodeId;

inline constexpr const char* kCrowdType = "crowd";
inline constexpr double kCm = 100.0;

/** A heights grid over the ground (meters), sampled from a mesh or terrain once per setup. */
struct Ground
{
    glm::dvec2 origin{0.0};
    double cell = 0.5;
    int nx = 0, nz = 0;
    std::vector<float> heights; // nx * nz, NaN where there is no ground
    double fallback = 0.0;
    double at(double x, double z) const;
};

/** What a crowd node's solver holds, for drawing, commands and bake. */
struct CrowdView
{
    std::shared_ptr<Runtime> runtime;
    Ground ground;
    /** Character root nodes (paths) the agents wear, as Setup.agents[i].character indexes them. */
    std::vector<std::string> characters;
    /** Groups as stored, with names, for reports. */
    json groups;
    /** Seconds per frame and the start frame the cache counts from. */
    double fps = 30.0;
    int64_t startFrame = 0;
    /** Sampled point nodes, in input order. */
    std::vector<NodeId> points;
    /** One line on what makes the motion: device, or why agents play cycles. */
    std::string status;
};

/** The current view of a crowd node, made by configuring its solver; null when it is not a crowd or can't configure. */
std::shared_ptr<const CrowdView> viewOf(Graph& g, NodeId crowd);

/** World matrix of an agent's root (cm), with the ground height; zero scale when the agent is not alive. */
glm::dmat4 agentMatrix(const CrowdView& v, const std::vector<double>& state, size_t agent);
/** An agent's SOMA pose in the world (meters), ground height added to the root. False when not alive. */
bool agentPose(const CrowdView& v, const std::vector<double>& state, size_t agent, lila::motion::Pose& out);

/** The crowd node a name or the selection points at; throws lila::CommandError. */
NodeId crowdArg(Graph& g, const std::vector<std::string>& objects, const char* cmd);

/** Sets the characters a crowd wears, characterising each for motion when it isn't yet. Undoable; throws CommandError. */
void setCharacters(Graph& g, NodeId crowd, const std::vector<std::string>& characters);

bool registerScene(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
void registerBake(lila::ext::Context& ctx);
} // namespace crowds
