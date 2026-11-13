#pragma once

// The hand rig's scene side, shared by its commands and its UI: hand nodes, reading and writing a hand's pose.

#include "Common.h"
#include "hand/HandMath.h"

#include <optional>
#include <string>
#include <vector>

namespace puppeteer::hand
{
inline constexpr const char* kHandType = "puppeteerHand";

/** One finger of a set up hand, resolved to nodes. */
struct Finger
{
    Role role = Role::Index;
    std::string roleFrom;
    NodeId meta = lila::scene::kNoNode;
    std::optional<Frame> metaFrame;
    std::vector<NodeId> joints;
    std::vector<Frame> frames;
    NodeId tip = lila::scene::kNoNode;
    /** A rubber hose finger: its hoseLimb, which the pose drives instead of the joints. */
    NodeId hose = lila::scene::kNoNode;
    double hoseCurlSign = 1.0, hoseSpreadSign = 1.0;
};

/** A hand node's setup, resolved. */
struct Hand
{
    NodeId node = lila::scene::kNoNode;
    NodeId character = lila::scene::kNoNode;
    NodeId joint = lila::scene::kNoNode;
    Side side = Side::Center;
    std::string sideFrom;
    std::vector<Finger> fingers;
    glm::dvec3 palm{0.0}, forward{0.0}, thumbSide{0.0}; // in the hand joint's space
    Pose rest;
    bool hasMeta() const;
    const Finger* finger(Role r) const;
};

/** The hand nodes of a character, left first. */
std::vector<NodeId> handsOf(Graph& g, NodeId character);
/** The hand a node belongs to: the hand node itself, its hand joint, or one of its finger joints; kNoNode otherwise. */
NodeId handFor(Graph& g, NodeId node);
/** Reads a hand node; nullopt when its joints are gone. */
std::optional<Hand> load(Graph& g, NodeId handNode);
/** The same character's hand on the other side. */
NodeId oppositeOf(Graph& g, NodeId handNode);

/** The pose the hand holds now. */
Pose currentPose(Graph& g, const Hand& h);
/**
 * Puts the fingers in mask into the pose, within their limits. Records undo. Fills `written` with the plugs changed
 * (node path, attr) when given.
 */
void writePose(Graph& g, const Hand& h, const Pose& p, const Mask& mask, std::vector<std::pair<std::string, std::string>>* written = nullptr);

/** A pose by name: a built-in id, a saved pose, or "rest" (the hand as it was set up). */
std::optional<Pose> namedPose(const Hand& h, const std::string& name);
/** Saved poses, name to pose JSON. */
json savedPoses();

json poseJson(const Pose& p);
Pose poseFromJson(const json& j);
Mask maskFromJson(const json& j);

/** Every value hand poses write on the hands (rotate, or hose curl and spread), to put back exactly after a preview. */
struct Snapshot
{
    std::vector<std::pair<lila::scene::PlugRef, lila::scene::PlugValue>> values;
    void restore(Graph& g) const;
};
Snapshot snapshot(Graph& g, const std::vector<NodeId>& handNodes);

/** Hand-level values of a pose, as the panel and the pad show them. */
json shapeJson(const Hand& h, const Pose& p);
} // namespace puppeteer::hand
