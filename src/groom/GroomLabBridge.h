#pragma once

#include "curve/Spline.h"
#include "geo/Mesh.h"

#include <groomlab/guide_set.h>
#include <groomlab/scalp_maps.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

/** Conversions between Lila's meshes and curves and GroomLab's scalps, guides and strands (GroomLab.cpp). */
namespace lila::groom::groomLabBridge
{
using curve::Splines;
groomlab::Vec3f toVec(const glm::dvec3& p);
/** The mesh's live points in order, moved by `matrix`: the scalp's points as scalpMesh numbers them. */
std::vector<groomlab::Vec3f> worldPoints(const geo::Mesh& mesh, const glm::dmat4& matrix);
/** The mesh's faces fanned into triangles over its live points, renumbered densely and moved by `matrix`. */
std::shared_ptr<const groomlab::ScalpMesh> scalpMesh(const geo::Mesh& mesh, const glm::dmat4& matrix = glm::dmat4(1.0));
/** The mesh's color sets as vertex scalp maps, named after the sets: gray where every point's r, g and b agree, else color. */
groomlab::ScalpMaps colorSetMaps(const geo::Mesh& mesh);
/** The stored maps with the color set maps the stored ones don't already name. */
groomlab::ScalpMaps withColorSetMaps(const std::string& storedJson, const groomlab::ScalpMaps& colorMaps);
/** Curves of at least two points as a GroomLab guide set, IDs 1, 2, ... in order, rooted on the scalp. */
std::shared_ptr<const groomlab::GuideSet> guideSet(const Splines& guides, const groomlab::ScalpMesh& scalp);
/** GroomLab strands as poly curves, a width per point and an id per curve. */
Splines toSplines(const groomlab::StrandSet& strands);
} // namespace lila::groom::groomLabBridge
