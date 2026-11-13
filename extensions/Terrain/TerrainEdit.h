#pragma once

// What the water, road and volume commands share: finding the terrain a command names, and editing the node group
// that makes it.

#include "Terrain.h"

#include <lila/Command.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>

#include <initializer_list>
#include <memory>
#include <set>
#include <string>

namespace terrain::edit
{
using json = nlohmann::json;

bool zUp();
glm::dvec3 upVector();
double upOf(const glm::dvec3& w);
/** A world point on the ground plane at g (x, z; x, y on a Z-up scene), up along the scene's up axis. */
glm::dvec3 fromGround(glm::dvec2 g, double up = 0.0);
/** A world point from [x, z] on the ground (x, y on a Z-up scene) or [x, y, z]. */
glm::dvec3 worldPoint(const json& p, const std::string& cmd);
json vec3(const glm::dvec3& p);

/** A terrain as the commands see it: mesh, modifier, heightfield in its own space (X, Z across, Y up) and world placement. */
struct TerrainTarget
{
    lila::scene::Node* mesh = nullptr;
    lila::scene::Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> local;
    glm::dmat4 m{1.0};
    glm::dmat4 inv{1.0};

    glm::dvec2 local2(const glm::dvec3& w) const
    {
        const glm::dvec4 q = inv * glm::dvec4(w, 1.0);
        return {q.x, q.z};
    }
    glm::dvec3 toLocal(const glm::dvec3& w) const { return glm::dvec3(inv * glm::dvec4(w, 1.0)); }
    glm::dvec3 world(double x, double y, double z) const { return glm::dvec3(m * glm::dvec4(x, y, z, 1.0)); }
    glm::dvec3 world(const glm::dvec3& p) const { return world(p.x, p.y, p.z); }
    glm::dvec3 direction(glm::dvec2 d) const { return glm::normalize(glm::dvec3(m * glm::dvec4(d.x, 0.0, d.y, 0.0))); }
    /** The local height at xz whose world point sits at up along the scene's up axis. */
    double localHeight(double up, glm::dvec2 xz) const;
    double across() const { return 0.5 * (glm::length(glm::dvec3(m[0])) + glm::length(glm::dvec3(m[2]))); }
    double vertical() const { return glm::length(glm::dvec3(m[1])); }
    glm::dvec2 centre() const { return local->origin + local->extent() * 0.5; }
    std::string path(const std::string& fallback) const { return mesh ? mesh->path() : fallback; }
};

/** The terrain made by name: a terrain mesh, or a geometry nodes modifier making a heightfield. */
TerrainTarget findTerrain(lila::scene::Graph& g, const std::string& name, const std::string& cmd);
/** The one terrain a command's arguments name. */
TerrainTarget oneTerrain(const lila::Args& a, const std::string& cmd);

/** The node group making a terrain, read for editing. */
struct GroupEdit
{
    lila::scene::Node* node = nullptr;
    lila::gn::Group group;
};

GroupEdit readGroup(lila::scene::Graph& g, const TerrainTarget& t, const std::string& cmd);
void commitGroup(GroupEdit& e, const std::string& cmd);

/**
 * Terrain edits on this thread made as one (terrainBatch): while one is open, terrain reads skip evaluating the scene,
 * group edits stay in memory, and the stamp and road commands return without their results. commit() writes each
 * edited group once.
 */
class EditBatch
{
public:
    EditBatch();
    ~EditBatch();
    EditBatch(const EditBatch&) = delete;
    EditBatch& operator=(const EditBatch&) = delete;
    static bool active();
    void commit(const std::string& cmd);
};
/** The batch's edit of the group on groupNode into out; false when no batch holds one. */
bool batchedGroup(const lila::scene::Node& groupNode, lila::gn::Group& out);
/** Keeps group as the batch's edit of groupNode; false when no batch is open. */
bool batchGroup(const lila::scene::Node& groupNode, lila::gn::Group& group);

/**
 * A group node's JSON list property (stamps, roads) to edit. In a batch the list is parsed once and edited in place
 * across commands, and the batch writes it back when it commits; otherwise store() writes it to n.
 */
class ListEdit
{
public:
    ListEdit(const lila::scene::Node& groupNode, const lila::gn::GNode& n, const char* prop);
    json& list() { return *mList; }
    void store(lila::gn::GNode& n);

private:
    const char* mProp;
    json mOwn;
    json* mList;
};

lila::gn::GNode* nodeNamed(lila::gn::Group& grp, const std::string& name);
lila::gn::GLink* linkInto(lila::gn::Group& grp, const std::string& node, const std::string& socket);

/**
 * The node named name of type, or a new one spliced into the heightfield chain running into the group output, before
 * the trailing nodes whose types are in after. yOffset places it in the editor relative to its neighbours.
 */
lila::gn::GNode& chainNode(lila::gn::Group& grp, const std::string& name, const std::string& type, const std::set<std::string>& after, double yOffset,
                           const std::string& cmd);

/** A node property holding JSON as a string; fallback when it is missing, unreadable or not fallback's type. */
json jsonProp(const lila::gn::GNode& n, const char* prop, json fallback);

/** The Object Info node named with prefix following object, made when the group has none. */
std::string sourceNode(lila::gn::Group& grp, const std::string& prefix, const std::string& object);
/** Removes prefix Object Info nodes no entry of the listed props on nodes of type refers to. */
void dropUnusedSources(lila::gn::Group& grp, const std::string& prefix, const std::string& type, std::initializer_list<const char*> props);
} // namespace terrain::edit
