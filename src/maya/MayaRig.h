#pragma once

#include "fbx/FbxAxes.h"
#include "geo/Mesh.h"
#include "maya/Maya.h"
#include "maya/MayaIndex.h"
#include "scene/AnimCurve.h"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

/**
 * Rig conversion for the Maya importer: constraints, IK handles, deformers other than skinCluster and blendShape,
 * utility and matrix node networks, set driven keys, surface pins, user attributes, and every connection between
 * them and the converted transforms. Internal to src/maya.
 *
 * Values cross between the two scenes in Maya's internal units: a Maya plug's value is its Lila plug's value times a
 * factor (pi/180 for angles, which Lila keeps in degrees; an axis sign where the up axes differ), and a connection
 * whose factors don't match goes through a unitConversion node.
 */
namespace lila::maya
{
struct RigContext
{
    scene::Graph& g;
    const FileIndex& ix;
    const ImportOptions& opts;
    ImportResult& r;
    /** Centimetres and degrees per file unit, frames per second. */
    double lin = 1.0, ang = 1.0, fps = 24.0;
    /** Maya space to Lila space, and the channel map it makes. */
    glm::dmat3 c3{1.0};
    fbx::AxisMap axes;
    bool identityAxes = true;
    const std::unordered_map<int, scene::NodeId>& transforms;
    const std::unordered_map<int, scene::NodeId>& meshes;
    const std::unordered_map<int, std::vector<geo::Index>>& pointOf;
    std::set<int>& handled;
    /** The Lila plugs a converted blendShape's plug lands on; empty for anything else. */
    std::function<std::vector<scene::PlugRef>(int node, const std::string& attr)> blendPlugs;
    /** A Maya anim curve's keys, values times unit and sign, tangent y in tangentUnit. */
    std::function<std::optional<scene::anim::Curve>(const MNode& n, double unit, double sign, double tangentUnit, bool* approx)> readCurve;
    /** A file texture's stored path as the path Lila reads, looked up beside the scene when it moved. */
    std::function<std::string(const std::string& stored, const std::string& what)> resolveImage;
};

class RigBuilder
{
public:
    explicit RigBuilder(RigContext ctx);
    ~RigBuilder();

    /** Constraints, IK, deformers and surface pins, then every remaining connection into a converted plug. After the curves. */
    void build();

    /**
     * Drives the given Lila plugs (whose value is the Maya value as is) from a Maya source plug, converting what it
     * needs to. False, with why set, when the source can't be converted.
     */
    bool drive(const Plug& src, const std::vector<scene::PlugRef>& dst, std::string* why);

    /** True for a user attribute that became a Lila user attr. */
    bool convertedAttr(int node, const std::string& longName) const;

    /** Shapes and helper nodes the rig conversion owns, which the DAG pass leaves alone. */
    static bool ownsShape(std::string_view mayaType);
    /** Deformers with a Lila counterpart. */
    static bool convertsDeformer(std::string_view mayaType);
    /** Node types whose inputs (curves, set driven keys, dynamic attributes) the rig conversion takes care of. */
    static bool convertsInputsOf(std::string_view mayaType);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila::maya
