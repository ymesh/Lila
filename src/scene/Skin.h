#pragma once

#include "core/Export.h"

#include "scene/Modifiers.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Sparse per-point skin weights, CSR: point p's entries are [offsets[p], offsets[p + 1]). Influence ids are the
 * skinInfluence nodes' index attrs, not chain positions, so rewiring the chain never scrambles weights.
 */
struct LILA_SCENE_API SkinWeights final : Data
{
    std::vector<uint32_t> offsets{0};
    std::vector<uint32_t> influences;
    std::vector<float> weights;
    /** Per entry, 1 where the weight was derived by Lila (bind, spreading, transfer) rather than set by a person. Empty: none. */
    std::vector<uint8_t> automatic;

    uint32_t pointCount() const { return uint32_t(offsets.size() - 1); }
    const char* kind() const override { return "skinWeights"; }
    size_t bytes() const override { return (offsets.size() + influences.size() + weights.size()) * 4 + automatic.size(); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** Editable copy of SkinWeights: one small (influence, weight) list per point. */
struct LILA_SCENE_API WeightTable
{
    using Entry = std::pair<uint32_t, float>;
    std::vector<std::vector<Entry>> points;
    /** Per point, the influences whose weight is automatic (see SkinWeights::automatic). Empty until something is marked. */
    std::vector<std::vector<uint32_t>> automatic;

    static WeightTable from(const SkinWeights* w, size_t pointCount);
    std::shared_ptr<const SkinWeights> pack() const;

    float get(size_t p, uint32_t influence) const;
    void set(size_t p, uint32_t influence, float w);
    /**
     * Scales the unlocked entries so the point sums to 1; locked ones keep their value. When more than one entry
     * absorbs a real change, they are marked automatic: Lila chose the split.
     */
    void normalize(size_t p, const std::vector<char>& locked);
    /** Drops entries below threshold and all but the heaviest maxInfluences, then normalizes. */
    void prune(size_t p, int maxInfluences, float threshold, const std::vector<char>& locked);

    bool isAutomatic(size_t p, uint32_t influence) const;
    void markAutomatic(size_t p, uint32_t influence);
    void markAuthored(size_t p, uint32_t influence);
    /** Every entry of the point. */
    void markPointAutomatic(size_t p);
    void markPointAuthored(size_t p);
    /** Whether any weighted entry of the point is automatic. */
    bool pointAutomatic(size_t p) const;
    /** Points with automatic weight. */
    std::vector<uint32_t> automaticPoints() const;
};

/** One weight layer: weights blended over the layers below by opacity times the mask. */
struct SkinLayer
{
    std::string name;
    bool enabled = true;
    float opacity = 1.0f;
    /** Full weights for every point; only where the mask is above 0 do they show. */
    WeightTable weights;
    /** Per point 0..1. The base layer (0) ignores it. */
    std::vector<float> mask;
};

/** A skinCluster's weight layers: layer 0 is the base, the rest blend over it in order. Its weights are their composite. */
struct LILA_SCENE_API SkinLayers final : Data
{
    std::vector<SkinLayer> layers;
    int active = 0;
    const char* kind() const override { return "skinLayers"; }
    size_t bytes() const override;
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** Payload one skinInfluence passes down the chain: everything upstream plus its own skinning matrix. */
struct InfluenceLink final : Data
{
    DataPtr prev;
    uint32_t index = 0;
    glm::dmat4 matrix{1.0}; // joint world * bind pre-matrix
    const char* kind() const override { return "influenceLink"; }
    json toJson() const override { return {{"kind", kind()}}; }
};

/**
 * One influence of a skinCluster. Graph plugs take a single connection each, so a skinCluster can't take N joint
 * matrices directly; instead each influence is a tiny node that takes one joint's worldMatrix and the previous link,
 * and the last link feeds skinCluster.influences:
 *
 *   joint_a.worldMatrix -> SkinInfluence1.matrix,  SkinInfluence1.link -> SkinInfluence2.previous
 *   joint_b.worldMatrix -> SkinInfluence2.matrix,  SkinInfluence2.link -> SkinCluster1.influences
 *
 * Moving any joint dirties exactly the links after it and the skin, undo and save/load are plain connections and
 * attrs, and the joint's name is always read from the connection, so renames are free.
 */
class LILA_SCENE_API SkinInfluenceNode : public Node
{
public:
    enum Attr
    {
        kMatrix,
        kBindPreMatrix,
        kIndex,
        kLockWeights,
        kPrevious,
        kLink,
    };
    void compute(const EvalContext& ctx) override;
};

struct SkinMethodCache;

/**
 * Linear blend (or dual quaternion, optimized centres of rotation, direct delta mush) skinning as a modifier, so it sits
 * in a mesh's stack like any other:
 *   meshSource -> SkinCluster1 -> Body.inMesh
 * Points are skinned in bind space: out = G^-1 * sum(w * jointWorld * bindPre) * G * p, with G the mesh's world
 * matrix at bind time (geomMatrix), so moving the mesh's own transform double-transforms it, as in Maya.
 */
class LILA_SCENE_API SkinClusterNode : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kEnvelope,
        kSkinningMethod,
        kNormalizeWeights,
        kMaxInfluences,
        kGeomMatrix,
        kWeights,
        kInfluences,
        kOutMesh,
        kDdmIterations,
        kDdmSmoothStep,
        kCorSigma,
        kExportWeights,
        kWeightLayers,
    };
    enum Method : int64_t
    {
        kLinear,
        kDualQuaternion,
        kCentresOfRotation,
        kDirectDeltaMush,
    };
    void compute(const EvalContext& ctx) override;

    /** Bind data of the centres of rotation and direct delta mush methods, rebuilt when the weights or topology change. */
    mutable std::shared_ptr<SkinMethodCache> methodCache;
    /**
     * The last two outputs and the input they copy. Once nothing else holds the older one, the next compute takes its
     * buffers instead of copying and freeing the whole mesh again, which on a dense character costs more than skinning.
     */
    std::shared_ptr<geo::Mesh> lastOut, olderOut;
    std::weak_ptr<const geo::Mesh> outSource;
};

namespace skin
{
struct Influence
{
    NodeId link = kNoNode;
    NodeId joint = kNoNode; // kNoNode if the matrix plug lost its connection
    uint32_t index = 0;
    std::string name;
    bool locked = false;
};

/**
 * A node the weight tools paint like a skinCluster: per-point weights over influences stored as SkinWeights. Attrs are
 * indices on the node's type; layers and maxInfluences are -1 when it has none.
 */
struct WeightHost
{
    int weights = -1, layers = -1, geomMatrix = -1, inMesh = -1, outMesh = -1, maxInfluences = -1;
    int fixedMaxInfluences = 4;
    std::function<std::vector<Influence>(Graph&, NodeId)> influences;
    /** The table before anything is stored in weights. */
    std::function<WeightTable(Graph&, NodeId)> seed;
};
LILA_SCENE_API void registerWeightHost(const std::string& type, WeightHost host);
/** The host of a skinCluster or a registered type, or null. */
LILA_SCENE_API const WeightHost* weightHost(const Graph& g, NodeId node);
/** The node the weight tools paint on a mesh: its skinCluster, else the first weight host in its stack, else kNoNode. */
LILA_SCENE_API NodeId findPainted(Graph& g, NodeId mesh);
LILA_SCENE_API int maxInfluencesOf(Graph& g, NodeId skin);

/** The skinCluster in a mesh's stack, or kNoNode. */
LILA_SCENE_API NodeId find(Graph& g, NodeId mesh);
/** Mesh or skinCluster name to the skinCluster; throws CommandError. */
LILA_SCENE_API NodeId resolve(Graph& g, const std::string& name, const char* cmd);
/** The mesh node a skinCluster feeds, or kNoNode. */
LILA_SCENE_API NodeId meshOf(Graph& g, NodeId skin);

LILA_SCENE_API std::vector<Influence> influences(Graph& g, NodeId skin);
/** Influence id for a joint name, or -1. */
LILA_SCENE_API int64_t indexOf(const std::vector<Influence>& infs, const std::string& joint);
/** Lock flags indexed by influence id. */
LILA_SCENE_API std::vector<char> lockMask(const std::vector<Influence>& infs);

/** The undeformed points the weights refer to. */
LILA_SCENE_API MeshPtr inputMesh(Graph& g, NodeId skin);
LILA_SCENE_API WeightTable table(Graph& g, NodeId skin);
/** Undoable. */
LILA_SCENE_API void setWeights(Graph& g, NodeId skin, const WeightTable& t);

/** Adds a joint as an influence with zero weights; returns its id (existing id if it already is one). Undoable. */
LILA_SCENE_API uint32_t addInfluence(Graph& g, NodeId skin, NodeId joint);

enum class PaintMode
{
    Add,
    Replace,
    Smooth,
    Scale,
};

/** Which influence a brush sample paints and where the weight it moves comes from or goes to. */
struct PaintOptions
{
    uint32_t influence = 0;
    /** The one influence that takes removed weight and gives added weight, or -1 for none. */
    int64_t receiver = -1;
    /** Refuse a point whose weight would have to be split between more than one influence. */
    bool strict = true;
    int maxInfluences = 4;
};

/** What a brush sample did to a point. Every value after Unchanged is a refusal: the point was left as it was. */
enum class PaintOutcome : uint8_t
{
    Painted,
    Unchanged,
    /** More than one unlocked influence could take or give the weight, and strict is on. */
    Ambiguous,
    /** Every other influence is locked or has no weight here. */
    NowhereToGo,
    /** The painted influence or the receiver would need a slot past maxInfluences. */
    InfluenceLimit,
    /** The painted influence is locked. */
    Locked,
};
inline bool refused(PaintOutcome o) { return o > PaintOutcome::Unchanged; }
LILA_SCENE_API const char* outcomeName(PaintOutcome o);

/**
 * One brush sample of `amount` (0..1, falloff times strength) on point p. Add/Replace/Scale use `value`; invert turns
 * Add into subtract. The weight moved goes to (or comes from) the receiver, else the single other unlocked influence
 * with weight at p; with several and strict off it is split in proportion and marked automatic. The changed entries
 * are marked authored. Smooth is a no-op here: use relaxPoints. dryRun only reports the outcome.
 */
LILA_SCENE_API PaintOutcome paintPoint(WeightTable& t, size_t p, PaintMode mode, bool invert, float value, float amount, const std::vector<char>& locked,
                                       const PaintOptions& o, bool dryRun = false);

/** Blends each (point, amount) toward its edge neighbours' average. Locked influences keep their values. */
LILA_SCENE_API void smoothPoints(const geo::Mesh& m, WeightTable& t, const std::vector<std::pair<uint32_t, float>>& points,
                  const std::vector<char>& locked, int maxInfluences);

/**
 * Edge-length weighted relax of each (point, amount) over the rest mesh, `iterations` Jacobi passes. Locked
 * influences keep their values; relaxed points are marked authored. With `fixed`, points flagged there are held as
 * they are and only the others move (they settle between the fixed ones).
 */
LILA_SCENE_API void relaxPoints(const geo::Mesh& rest, WeightTable& t, const std::vector<std::pair<uint32_t, float>>& points, const std::vector<char>& locked,
                                int maxInfluences, int iterations, const std::vector<char>* fixed = nullptr);

/** The skin's influence matrices in the mesh's object space (G^-1 * joint world * bind pre * G), by influence id. */
LILA_SCENE_API std::vector<glm::dmat4> objectMatrices(Graph& g, NodeId skin);

/** The skin's weight layers, or null when it has none. */
LILA_SCENE_API std::shared_ptr<const SkinLayers> layers(Graph& g, NodeId skin);
/** Undoable. Null removes the layers (the weights stay as composited). */
LILA_SCENE_API void setLayers(Graph& g, NodeId skin, std::shared_ptr<const SkinLayers> l);
/** Point p of the layers [0, upTo) blended, into out.points[p] (and its automatic flags), pruned to maxInfluences. */
LILA_SCENE_API void compositePoint(const SkinLayers& l, int upTo, size_t p, WeightTable& out, int maxInfluences);
/** Every point of every layer blended. */
LILA_SCENE_API WeightTable composite(const SkinLayers& l, size_t pointCount, int maxInfluences);

/**
 * An edit of a skin's weights the way painting sees them: the active layer's when it has layers (its unmasked points
 * seeded from the layers below, so an edit starts from what shows), else the weights themselves. Change table(), then
 * commit(): a layer point that changed is unmasked, and the composite becomes the weights. Undoable.
 */
class LILA_SCENE_API WeightEdit
{
public:
    WeightEdit(Graph& g, NodeId skin);
    WeightTable& table() { return mTable; }
    const std::vector<Influence>& influences() const { return mInfluences; }
    const std::vector<char>& locked() const { return mLocked; }
    int maxInfluences() const { return mMaxInf; }
    bool layered() const { return mLayers != nullptr; }
    /** The weights to skin with now, point p updated from table(): for live previews while painting. */
    void compositeInto(size_t p, WeightTable& out) const;
    void commit();

private:
    bool changed(size_t p) const;

    Graph& mGraph;
    NodeId mSkin;
    std::vector<Influence> mInfluences;
    std::vector<char> mLocked;
    int mMaxInf = 4;
    std::shared_ptr<const SkinLayers> mLayers;
    WeightTable mSeed, mTable;
};

/** Reports the skinWeights validator's warning for each skinned mesh with automatic weights (and clears the rest). */
LILA_SCENE_API void reportAutomatic(Graph& g);
} // namespace skin
} // namespace lila::scene
