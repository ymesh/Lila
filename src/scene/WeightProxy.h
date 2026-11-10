#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <string>
#include <vector>

/**
 * Weight proxies: a light, single-sided stand-in mesh for painting the weights of heavy or thick render meshes (a coat
 * with lining, buttons and cuffs). Its edge loops sit at the joints with an even number between them, so smoothing
 * carries cleanly. Weights move between it and its render meshes only when asked (push and pull).
 */
namespace lila::scene::weightproxy
{
struct Options
{
    /** Edge loops between one joint's loop and the next, at least. */
    int loopsPerBone = 3;
    /** Edges around a limb, at least; more makes the loops closer too. */
    int edgesAround = 8;
    /** Voxels along the longest side when the render meshes are fused; lower fills more detail. */
    int resolution = 128;
    /** Joints to bind to when the render meshes aren't skinned. */
    std::vector<NodeId> joints;
    std::string name;
};

/** Builds and binds a proxy for the meshes, beside the first of them, pulling their weights when they are skinned. */
LILA_SCENE_API NodeId create(Graph& g, const std::vector<NodeId>& meshes, const Options& o);

/** The render meshes a proxy stands in for; empty when the node isn't a proxy. */
LILA_SCENE_API std::vector<NodeId> targetsOf(Graph& g, NodeId proxy);
/** The proxy standing in for a mesh, or kNoNode. */
LILA_SCENE_API NodeId proxyOf(Graph& g, NodeId mesh);

/**
 * Weights for the destination skin's points from the closest point on any of the source skins' surfaces, blended
 * across that face. Influences match by joint (missing ones are added with the source's bind). Locked destination
 * joints keep their weights; an automatic source weight stays automatic. Returns the points written. Undoable.
 */
LILA_SCENE_API size_t transfer(Graph& g, const std::vector<NodeId>& fromSkins, NodeId toSkin);

/** createWeightProxy, pushProxyWeights, pullProxyWeights, weightProxyInfo. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::weightproxy
