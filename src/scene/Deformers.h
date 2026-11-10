#pragma once

#include "core/Export.h"

#include "scene/Modifiers.h"

#include <glm/glm.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
namespace deformEval
{
struct Program;
}

/** A deformer whose binding is not captured yet: reported on the node, not logged, as it happens while binding. */
struct Unbound : std::runtime_error
{
    Unbound() : std::runtime_error("not bound: run rebindDeformer") {}
};

/** A per-point weight map on a deformer: missing entries (past the end) weigh 1. */
struct LILA_SCENE_API PointWeights final : Data
{
    std::vector<float> values;
    const char* kind() const override { return "pointWeights"; }
    size_t bytes() const override { return values.size() * sizeof(float); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/**
 * A deformer: a modifier that only moves points. Every one has envelope, a per-point weight map, an optional field
 * and an optional membership (the points it may move), and blends each point between its input and deformed position
 * by envelope times its weight (scene/DeformWeights.h). Normals and tangents the input carried are rebuilt. evalTime
 * reports how long the last evaluation took.
 */
class LILA_SCENE_API DeformerNode : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kEnvelope,
        kWeights,
        kField,
        kFieldMode,
        kMembership,
        kEvalTime,
        kFirstParam,
    };

    void compute(const EvalContext& ctx) override;
    uint32_t traits() const override { return kModifier | kDeformer; }

    /** Moves points (input positions in, deformed out). Throws to report an error and pass the input through. */
    virtual void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const = 0;

    /**
     * The deformer as a GPU program (scene/DeformEval.h) for this input, or false when it has no GPU form for the
     * current settings. Throws like deform. The default has none.
     */
    virtual bool gpuProgram(const geo::Mesh& in, deformEval::Program& out) const
    {
        (void)in;
        (void)out;
        return false;
    }

    /** The deformed mesh: deform, then the envelope and weight map blend, then normals. Throws like deform. */
    geo::Mesh run(const geo::Mesh& in) const;
    /** run, reporting whether the GPU deformed the points. */
    geo::Mesh run(const geo::Mesh& in, bool* ranOnGpu) const;

    /**
     * False for a deformer whose output depends on more than its inputs (a simulation's published state), so it
     * recomputes whenever it is dirty. The others reuse their last output while every input is unchanged.
     */
    virtual bool cacheable() const { return true; }

    /**
     * False for a deformer moved by its own controls rather than by joints (Soft Net): fitting skin weights over joint
     * poses learns nothing from it, so exports leave the skin's own weights instead of refitting.
     */
    virtual bool followsJoints() const { return true; }

    /** Whether the last evaluation ran on the GPU, so its points carry float precision: exports re-run it on the CPU. */
    bool ranOnGpu() const { return mRanOnGpu; }

    int attr(const char* name) const { return type().attrIndex(name); }
    double num(const char* name) const;
    int64_t integer(const char* name) const;
    glm::dmat4 matrix(const char* name) const;

private:
    /** The inputs the last evaluation saw and what it produced, so evaluating with identical inputs costs nothing. */
    std::vector<PlugValue> mLastInputs;
    std::vector<std::vector<std::pair<int, PlugValue>>> mLastElements; // arrays' elements, beside mLastInputs
    MeshPtr mLastOut;
    bool mRanOnGpu = false;
};

namespace deformers
{
LILA_SCENE_API bool isDeformer(const Node& n);

/**
 * A deformer node type: the common attributes (inMesh, enabled, exportEnabled, envelope, weights) come first, then
 * params (Vec3 params get X/Y/Z children), then outMesh. Read params in deform() by name (num("angle")) or by index
 * from DeformerNode::kFirstParam.
 */
struct DeformerSpec
{
    std::string name; // the node type, "bulgeDeformer"
    std::string icon; // Tabler icon name
    std::string doc;
    std::vector<AttrDef> params;
    std::function<std::unique_ptr<DeformerNode>()> make;
    /** Attr names older scene files may hold that mean nothing now (NodeType::retiredAttrs). */
    std::vector<std::string> retired;
    /** Recomputed whenever the scene time changes (NodeType::timeDependent); compute's EvalContext has the time. */
    bool timeDependent = false;
};

/** Throws std::invalid_argument when the name is taken. */
LILA_SCENE_API const NodeType& registerDeformer(DeformerSpec spec);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the name is taken. */
LILA_SCENE_API const NodeType* registerDeformer(ext::Context& ctx, DeformerSpec spec);

/**
 * Adds a deformer of a registered type to the top of each mesh's stack, or with belowSkin just under any skinCluster
 * (where morph sits), and returns them. Undoable when run inside a command.
 */
LILA_SCENE_API std::vector<NodeId> addDeformer(Graph& g, const std::string& type, const std::vector<NodeId>& meshes, const std::string& name = {},
                                               bool belowSkin = false);

/** The deformers in a mesh's stack, applied first first. */
LILA_SCENE_API std::vector<NodeId> list(Graph& g, NodeId mesh);

/** A tension deformer output named for Unreal: exports sample it as a curve, and the link flags it as a material curve. */
struct TensionCurve
{
    NodeId deformer = kNoNode;
    NodeId mesh = kNoNode;
    int attr = -1;
    std::string name;
};

/** Every export-enabled tension deformer's stretchCurve and compressCurve that is set, deduplicated by name. */
LILA_SCENE_API std::vector<TensionCurve> tensionCurves(Graph& g);

/** The deformer's per-point weight map, expanded to n points. */
LILA_SCENE_API std::vector<float> pointWeights(Graph& g, NodeId deformer, size_t n);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace deformers
} // namespace lila::scene
