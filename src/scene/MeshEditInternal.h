#pragma once

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Graph.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

/** Helpers shared by the MeshEdit*.cpp command files. */
namespace lila::scene::detail
{
using components::Ids;
using geo::Index;

inline Graph& graph() { return Graph::current(); }

PlugRef basePlug(Node& n);
geo::Mesh baseMesh(const PlugRef& p);
int symmetryAxis(const Node& n);
double symmetryTolerance(const geo::Mesh& mesh);
std::string symmetryLayer(const geo::SymmetrySpec& spec);
std::string symmetryName(const geo::SymmetrySpec& spec);
std::optional<geo::SymmetryMap> symmetryFor(const Node& n, geo::Mesh& mesh);
void maintainSymmetry(const Node& n, geo::Mesh& mesh, const std::vector<glm::dvec3>& before, int driveWedge);
Ids remapIds(const Ids& ids, const std::vector<Index>& map);

struct EditResult
{
    ComponentMode kind = ComponentMode::Object;
    Ids ids;
};

using EditFn = std::function<EditResult(geo::Mesh&, const Ids& selection, const geo::SymmetryMap* symmetry)>;

json editSelected(const char* label, ComponentMode kind, const EditFn& fn, bool allowWhole = false,
                  const std::function<bool(const Node&)>& handledWhole = nullptr);
/** Inside an editSelected fn: the linear part of the edited mesh's world matrix, for ops that take world distances. */
glm::dmat3 editSpace();

inline EditResult faces(Ids ids) { return {ComponentMode::Face, std::move(ids)}; }
inline EditResult edges(Ids ids) { return {ComponentMode::Edge, std::move(ids)}; }
inline EditResult points(Ids ids) { return {ComponentMode::Vertex, std::move(ids)}; }

Ids withMirror(const geo::Mesh& m, const geo::SymmetryMap* s, const Ids& ids, ComponentMode kind);
Node& meshArg(const std::string& name, const char* cmd);
Node& meshFlag(const Args& a, const char* cmd);

inline glm::dvec3 vec(const json& j) { return glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()); }

void registerMeshEditComponentCommands();
void registerMeshEditMeshOpsCommands();
void registerMeshEditTransformCommands();
void registerMeshEditSymmetryCommands();
void registerMeshEditKnifeCommands();
void registerMeshEditNormalsCommands();
} // namespace lila::scene::detail
