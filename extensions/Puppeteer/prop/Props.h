#pragma once

// Props and weapons, the scene side shared by the commands and the UI: prop and point nodes, who holds a prop when,
// and the grapple that solves the arms holding it.

#include "Common.h"
#include "prop/PropMath.h"

#include <optional>
#include <string>
#include <vector>

namespace puppeteer::prop
{
inline constexpr const char* kPropType = "puppeteerProp";
inline constexpr const char* kPointType = "puppeteerPropPoint";
/** A throw's arc under its prop: launch, spin and landing, keyed onto the prop and edited in the viewport. */
inline constexpr const char* kArcType = "puppeteerPropArc";
/** The dynamic attr marking the grapple that solves every character's props. */
inline constexpr const char* kPropGrappleAttr = "propsGrapple";

/** A prop node, resolved. */
struct Prop
{
    NodeId node = lila::scene::kNoNode;
    /** The transform that moves when the prop is held: the prop node's parent. */
    NodeId root = lila::scene::kNoNode;
    /** The hand that holds it by default: true right. */
    bool rightHanded = true;
    Settings settings;
    std::vector<NodeId> points;
    NodeId point(Graph& g, Role r) const;
};

/** The prop a node belongs to: the prop node, its root, one of its points, or a node under the root. */
NodeId propFor(Graph& g, NodeId node);
std::optional<Prop> load(Graph& g, NodeId propNode);
std::vector<NodeId> props(Graph& g);
/** Writes a prop node's settings, undoably. */
void writeSettings(Graph& g, NodeId propNode, const Settings& s);

Role roleOf(const Node& point);
/** A point's frame in its prop root's space. */
glm::dmat4 pointLocal(Graph& g, const Prop& p, NodeId point);

/** One stretch of time a prop spends in one place, from its space channel's keys. */
struct Hold
{
    double time = 0.0;
    /** "world", "parent", or the joint or holster point's path. */
    std::string space;
    NodeId target = lila::scene::kNoNode;
    /** hand, holster, world or parent. */
    std::string kind;
    /** Right or left when held by a hand. */
    std::string side;
};
/** The prop's holds in time order (one at the playback start when it was never keyed). */
std::vector<Hold> holdsOf(Graph& g, const Prop& p);
/**
 * The prop's handoffs as the track lists them, in time order: each {time, kind, ...} with kind hold, take, catch, drop,
 * holster, plant, throw, place or pass, and what it involved (hand, point, character, arc, on, from, to).
 */
json eventsOf(Graph& g, const Prop& p);
/** Records a handoff at its time, replacing one within half a frame of it. */
void recordEvent(Graph& g, const Prop& p, const json& ev);
/** The joint holding the prop now (its active space's target when a joint), else kNoNode. */
NodeId holderNow(Graph& g, const Prop& p);

/**
 * The scene's prop grapple, which solves every character holding props (and two sharing one in a pass) together, with
 * the character among its figures: made or added when make is set.
 */
NodeId propGrapple(Graph& g, NodeId character, bool make);
/** The meshes under the prop's root, for grip wraps and fitting. */
std::vector<NodeId> propMeshes(Graph& g, const Prop& p);

json propJson(Graph& g, const Prop& p);
} // namespace puppeteer::prop
