#pragma once

// What Puppeteer's rig files share: finding characters and the rig elements built on a joint.

#include "Common.h"

#include <lila/RigElements.h>

#include <optional>

namespace puppeteer
{
/** The character a node belongs to: the one whose skeleton root is the node or above it, or whose node holds it. */
NodeId characterFor(Graph& g, NodeId node);
/** Meshes skinned to the skeleton at or below root. */
std::vector<std::string> skeletonMeshes(Graph& g, NodeId root);
/** Every character node, in creation order. */
std::vector<NodeId> characters(Graph& g);
/** The pose library group of a character: its template id, else its root joint's name. */
std::string poseLibraryOf(Graph& g, NodeId character);
/** Where new rig elements for joint go: its character node, else the world. */
NodeId elementParentFor(Graph& g, NodeId joint);

/** Rig element nodes (any type) whose given string attr names joint. */
std::vector<NodeId> elementsNaming(Graph& g, const char* type, const char* attr, const std::string& joint);

using lila::scene::rigel::chainContaining;
using lila::scene::rigel::chainForEffector;
using lila::scene::rigel::chainFrom;
using lila::scene::rigel::footForChain;
using lila::scene::rigel::snapReverseFoot;

/** The node an animator grabs for the chain: its reverse foot, else its ikHandle. */
inline NodeId ikGoalOf(Graph& g, NodeId chain) { return lila::scene::rigel::controllerOf(g, chain); }
/** Whether the chain's key at time is locked. */
inline bool chainLocked(Graph& g, NodeId chain, double time) { return lila::scene::rigel::locked(g, chain, time); }

/** The character's root joint, or none. */
NodeId characterRoot(Graph& g, NodeId character);
/**
 * Stiffness per joint of the character: its rig template's (rig.fullBody.stiffness, by marker), when Rig Templates is
 * loaded and the template has one, else by name.
 */
std::vector<std::pair<NodeId, double>> stiffnessOf(Graph& g, NodeId character, const std::vector<NodeId>& joints);
/** The joints full-body moves hold where they are. */
std::vector<NodeId> pinsOf(Graph& g, NodeId character);
/** Every joint of the character joint belongs to, its root first; empty outside a character. */
std::vector<NodeId> fullBodyJoints(Graph& g, NodeId joint);
/**
 * Moves joints by a world offset by posing their whole character: the joints go there and the pins and locked hands and
 * feet hold, through full-body IK writing every joint's rotate; autoPose lets the auto-poser place the rest. Returns the joints moved; the rest (not deform joints in
 * a character, or a root above the hips) are left to an ordinary move.
 */
std::vector<NodeId> fullBodyMove(Graph& g, const std::vector<NodeId>& nodes, const glm::dvec3& by, bool autoPose = false);

/** Converts older scenes' puppeteerSpace nodes to base spaces with the same keys. Returns how many. Records undo. */
int migrateSpaces(Graph& g);
/**
 * Converts older scenes' Puppeteer IK chains, reverse feet and lookers to base rig elements. Where a chain was in IK
 * its joints get the IK pose keyed as rotations at the keys, and the per-key IK flag goes. Returns how many elements
 * were converted. Records undo.
 */
int migrateRigElements(Graph& g);
} // namespace puppeteer
