#pragma once

#include "core/Export.h"

#include "geo/FoldEdges.h"
#include "scene/Modifiers.h"

#include <memory>
#include <vector>

namespace lila::geo
{
struct MeshRemap;
}

namespace lila::scene
{
/**
 * Hand edits to an autoBevelModifier's edge set, stored as edge refs on the modifier's input mesh so they survive
 * upstream topology changes (see geo::EdgeMatcher). The capacities say which mesh the ids belong to.
 */
struct LILA_SCENE_API BevelOverrides final : Data
{
    std::vector<geo::EdgeRef> add;
    std::vector<geo::EdgeRef> remove;
    geo::Index points = 0;
    geo::Index edges = 0;
    geo::Index faces = 0;

    const char* kind() const override { return "bevelOverrides"; }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** What an autoBevelModifier picked on its last evaluation: edge ids on its input mesh. */
struct BevelEdgeSet final : Data
{
    MeshPtr mesh;
    std::vector<geo::Index> edges;
    int lost = 0; // overrides that matched no edge
    double offset = 0.0;  // asked for
    double applied = 0.0; // after the clamp
    /** Per edge of mesh, the largest offset the clamp leaves it; infinity where nothing limits it. Empty unclamped. */
    std::vector<double> limits;

    const char* kind() const override { return "bevelEdgeSet"; }
    json toJson() const override { return {{"kind", kind()}, {"count", edges.size()}, {"lost", lost}}; }
};

namespace autoBevel
{
LILA_SCENE_API bool isAutoBevel(const Node& n);
/** The modifier's last picked edges, null before it evaluated. Evaluates the graph. */
LILA_SCENE_API std::shared_ptr<const BevelEdgeSet> chosen(Graph& g, NodeId modifier);
/** The mesh node at the top of the modifier's chain, kNoNode if it feeds none. */
LILA_SCENE_API NodeId meshOf(Graph& g, NodeId modifier);
LILA_SCENE_API bool editing(const Node& modifier);

/**
 * A direct edit compacted the base mesh: overrides of an autoBevelModifier sitting first in the stack follow
 * their edges to the new ids. Records undo with the edit.
 */
LILA_SCENE_API void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace autoBevel
} // namespace lila::scene
