#pragma once

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <variant>

/** Argument and scene lookups the retopology and topology commands share. */
namespace lila::retopo
{
/** The mesh node called name in the current graph; a CommandError naming cmd otherwise. */
inline scene::Node& meshArg(const std::string& name, const char* cmd)
{
    scene::Node* n = scene::Graph::current().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** The mesh stored on p after evaluating the current graph, empty when none is. */
inline geo::Mesh baseMesh(const scene::PlugRef& p)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::MeshPtr& m = std::get<scene::MeshPtr>(g.storedValue(p));
    return m ? *m : geo::Mesh();
}

inline glm::dmat4 worldOf(const scene::Node& n) { return n.as<glm::dmat4>(scene::Transform::kWorldMatrix); }

/** A retopo mesh's reference surface in the retopo mesh's object space, with its Bvh. */
struct ReferenceSurface
{
    scene::MeshPtr source;
    glm::dmat4 toLocal{1.0};
    geo::Mesh mesh;
    geo::MeshBvh bvh;
};

/** The reference surface of retopo, null when it has none; the last one built is kept for the next call. */
const ReferenceSurface* referenceSurface(const scene::Node& retopo);
} // namespace lila::retopo
