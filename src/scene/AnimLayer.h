#pragma once

#include "core/Export.h"

#include "scene/AnimCurve.h"
#include "scene/Node.h"
#include "scene/Transform.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Animation layers. Every animated channel keeps one input per plug; layering inserts animBlend nodes into that chain:
 * base drivers -> blend (layer 1) -> blend (layer 2) -> ... -> plug. A blend's inputA is everything below it, inputB the
 * layer's own curve (or a static value), and its weight, mode and accumulation come from the layer node through
 * connections, so a blend's layer is the source of its weight. Constraints insert an unlayered blend at the bottom of
 * the chain (inputB = the constraint), so constrained channels can be keyed and layered too.
 *
 * Vec3 members (translate, rotate, scale) get one Vec3 blend whose output drives the plug per component; the inputs
 * accept either component curves or a whole Vec3 driver such as a rotation curve or a constraint.
 */
namespace lila::scene::anim
{
enum class LayerMode : int64_t
{
    Additive,
    Override,
};

enum class Accumulation : int64_t
{
    Quaternion,
    Euler,
};

inline constexpr const char* kBaseLayerName = "BaseAnimation";

/** animBlendLinear (Float), animBlendVector (Vec3) and animBlendRotation (Vec3 rotate). */
class LILA_SCENE_API BlendNode : public Node
{
public:
    enum Attr
    {
        kWeight,
        kMode,
        kAccumulation,
        kInputA,
        kInputB,
        kOutput,
        kRotateOrder, // animBlendRotation only
    };

    void compute(const EvalContext& ctx) override;

    /** inputB that makes the output o, from the evaluated inputs; nullopt for the standard layer maths. May throw. */
    virtual std::optional<glm::dvec3> solveInputB(const glm::dvec3& o) const
    {
        (void)o;
        return std::nullopt;
    }
    /** The same for inputA. */
    virtual std::optional<glm::dvec3> solveInputA(const glm::dvec3& o) const
    {
        (void)o;
        return std::nullopt;
    }
    /** inputB is one control value that is set, never keyed (offset blends). */
    virtual bool staticInputB() const { return false; }

    bool rotation = false;
};

class LILA_SCENE_API LayerNode : public Node
{
public:
    enum Attr
    {
        kWeight,
        kMute,
        kSolo,
        kLock,
        kMode,
        kAccumulation,
        kOrder,
        kActive,
        kSoloMuted,
        kOutWeight,
    };

    void compute(const EvalContext& ctx) override;
};

LILA_SCENE_API bool isBlend(const Node& n);
LILA_SCENE_API bool isLayer(const Node& n);

/**
 * a blended with b at weight w. Additive rotation in quaternion space is a * slerp(identity, b, w), override is
 * slerp(a, b, w); the result is the Euler triple nearest the Euler-space blend, so curves stay continuous.
 */
LILA_SCENE_API glm::dvec3 blendRotation(const glm::dvec3& a, const glm::dvec3& b, double w, LayerMode mode, Accumulation acc,
                                        RotateOrder order);

/** Layer nodes, bottom of the stack first. */
LILA_SCENE_API std::vector<NodeId> layers(Graph& g);
/** The layer keys go to; kNoNode for the base animation. */
LILA_SCENE_API NodeId activeLayer(Graph& g);
/** The layer driving a blend's weight; kNoNode for a constraint blend. */
LILA_SCENE_API NodeId layerOf(Graph& g, NodeId blend);
/** Blends driving member (a whole attribute), top of the chain first. */
LILA_SCENE_API std::vector<NodeId> blendChain(Graph& g, const PlugRef& member);
/** The attribute a blend's chain ends at; invalid when the chain is parked (an inactive take) or dangling. */
LILA_SCENE_API PlugRef memberOf(Graph& g, NodeId blend);
/** Attributes with a blend on the layer, in creation order. */
LILA_SCENE_API std::vector<PlugRef> layerMembers(Graph& g, NodeId layer);
/** The layer's blend on member, or kNoNode. */
LILA_SCENE_API NodeId blendFor(Graph& g, const PlugRef& member, NodeId layer);

/** Creates a layer above the others. Undoable. */
LILA_SCENE_API NodeId createLayer(Graph& g, const std::string& name, LayerMode mode = LayerMode::Additive);
/** Adds member (whole attribute) to the layer, inserting its blend. Returns the blend. */
LILA_SCENE_API NodeId addMember(Graph& g, NodeId layer, const PlugRef& member);
/** The same with a blend of a type derived from the member's usual one (an offset blend). */
LILA_SCENE_API NodeId addMember(Graph& g, NodeId layer, const PlugRef& member, const std::string& blendType);
/** Takes a blend out of its chain and deletes it, with the curves feeding its inputB when deleteInputs. */
LILA_SCENE_API void removeBlend(Graph& g, NodeId blend, bool deleteInputs);
/** Takes the layer's blend out of member's chain and deletes it and the curves feeding its inputB. */
LILA_SCENE_API void removeMember(Graph& g, NodeId layer, const PlugRef& member);

/**
 * Unlayered override blend at the bottom of member's chain with driver on inputB, for constraints. Returns the
 * blend; the chain's existing drivers move to its inputA.
 */
LILA_SCENE_API NodeId insertConstraintBlend(Graph& g, const PlugRef& member, const PlugRef& driver);

/**
 * The plug whose curve stands for plug in the active layer: plug itself when nothing blends it, else the active
 * layer's inputB (or the base input of the chain). Component plugs map to component plugs.
 */
LILA_SCENE_API PlugRef keyInput(Graph& g, const PlugRef& plug);

/** Where keys on one member go when a key router decides instead of the active layer. */
struct KeyRoute
{
    /** The layer; kNoNode for the base animation. */
    NodeId layer = kNoNode;
    /** An unkeyed layer input takes the value as its static value instead of gaining a curve (an offset). */
    bool staticValue = false;
};
/**
 * Picks the layer for keys on member (a whole attribute), over the active layer: nullopt leaves it to the active layer.
 * write is true when a key or value is about to land, so the router may create the layer; reads pass false. A read may
 * answer staticValue with no layer: the write will make a layer whose input takes the value.
 */
using KeyRouter = std::function<std::optional<KeyRoute>(Graph& g, const PlugRef& member, bool write)>;
/** One router (an extension's per-control tracks); an empty function removes it. */
LILA_SCENE_API void setKeyRouter(KeyRouter router);
/** The layer keys on member go to: the router's choice, else the active layer. kNoNode is the base animation. */
LILA_SCENE_API NodeId keyLayer(Graph& g, const PlugRef& member);
/** The layer a key about to be written on plug goes to, made first when the router makes layers. setKey calls it. */
LILA_SCENE_API NodeId routeKey(Graph& g, const PlugRef& plug);
/** True when an edit to plug sets its layer's static value rather than keying (the router said so, nothing keyed). */
LILA_SCENE_API bool editSetsStatic(Graph& g, const PlugRef& plug);

/** Called after auto key keyed plug at time, so a tool can key alongside it (a whole character). */
using AutoKeyListener = std::function<void(Graph& g, const PlugRef& plug, double time)>;
/** One listener; an empty function removes it. */
LILA_SCENE_API void setAutoKeyListener(AutoKeyListener listener);
/** Calls the listener, if any. */
LILA_SCENE_API void notifyAutoKey(Graph& g, const PlugRef& plug, double time);

struct KeyRedirect
{
    PlugRef plug;
    double value = 0.0;
    /** The whole value the input takes (all three components of a Vec3), for rotation curve keys. */
    glm::dvec3 whole{0.0};
    std::string curveName;
    /** Set the input's static value instead of keying it (an unkeyed input the router marks static). */
    bool staticValue = false;
};
/**
 * Where a key on plug really goes, given the active layer: the layer's inputB (adding plug's attribute to the layer
 * first if needed) or the base input under the chain. value is converted so the plug evaluates to it. nullopt when
 * plug is keyed directly. Throws std::invalid_argument when the active layer is locked.
 */
LILA_SCENE_API std::optional<KeyRedirect> redirectKey(Graph& g, const PlugRef& plug, double value);

/**
 * Keys many channels at one time without evaluating between them: while one is alive, redirectKey reads the values below
 * the layer as last evaluated (keys at the same time on the layer can't change them) and each member's value as the keys
 * so far put it. Evaluate after changing the time, before keying there.
 */
class LILA_SCENE_API KeyBatch
{
public:
    KeyBatch();
    ~KeyBatch();
    KeyBatch(const KeyBatch&) = delete;
    KeyBatch& operator=(const KeyBatch&) = delete;
};

/**
 * Keys one component of a layered or constrained transform rotate whose target input takes rotation keys: one with a
 * rotation curve already, or an unkeyed one on a node whose rotationInterpolation isn't Euler. The input gets (or
 * keeps) an animCurveRQ keyed with the whole converted rotation. Returns the curve; nullopt when the key is an
 * ordinary Euler one.
 */
LILA_SCENE_API std::optional<NodeId> setLayeredRotationKey(Graph& g, const PlugRef& plug, double time, double value,
                                                           std::optional<Tangent> in, std::optional<Tangent> out);

LILA_SCENE_API void registerLayerTypes();
LILA_SCENE_API void registerLayerCommands();
} // namespace lila::scene::anim
