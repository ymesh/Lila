#pragma once

#include "scene/Graph.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace lila::ui::outliner
{
/** What an outliner context menu acts on: the whole selection when the clicked row is in it, else just that row. */
inline std::vector<scene::NodeId> menuTargets(scene::NodeId clicked, const std::vector<scene::NodeId>& selection)
{
    if (clicked == scene::kNoNode || std::find(selection.begin(), selection.end(), clicked) != selection.end())
        return selection;
    return {clicked};
}

/** False when the node's own visibility is off. Nodes without a visibility channel count as visible. */
inline bool visibleSelf(scene::Graph& g, scene::NodeId id)
{
    scene::Node* n = g.node(id);
    if (!n)
        return true;
    int attr = n->type().attrIndex("visibility");
    if (attr < 0)
        return true;
    const scene::PlugValue v = g.value(scene::PlugRef{id, attr, -1});
    const bool* b = std::get_if<bool>(&v);
    return !b || *b;
}

/** Hidden itself or under a hidden ancestor, so the viewport doesn't draw it. */
inline bool hiddenInHierarchy(scene::Graph& g, scene::NodeId id)
{
    for (scene::NodeId p = id; p != scene::kNoNode; p = g.parent(p))
        if (!visibleSelf(g, p))
            return true;
    return false;
}

/** Tabler icon for a node's row: its type's icon. */
inline std::string iconName(const scene::Node& n)
{
    static const std::string kFallback = "point";
    return scene::types::iconFor(n.type(), kFallback);
}

/** Every node that has to be expanded for id's row to show. */
inline std::vector<scene::NodeId> ancestors(const scene::Graph& g, scene::NodeId id)
{
    std::vector<scene::NodeId> out;
    for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
        out.push_back(p);
    return out;
}
/** Ancestors of the selected nodes that aren't selected themselves, which the outliner shades to show the chain. */
inline std::set<scene::NodeId> selectionAncestors(const scene::Graph& g, const std::vector<scene::NodeId>& selection)
{
    std::set<scene::NodeId> out;
    for (scene::NodeId id : selection)
        for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
            if (!out.insert(p).second)
                break;
    for (scene::NodeId id : selection)
        out.erase(id);
    return out;
}

/** The parent(index=) for a drop in front of anchor among siblings (kNoNode anchor = the end), the moved ones skipped. */
inline int dropIndex(const std::vector<scene::NodeId>& siblings, const std::vector<scene::NodeId>& moved,
                     scene::NodeId anchor)
{
    int index = 0;
    for (scene::NodeId s : siblings)
    {
        if (s == anchor)
            break;
        if (std::find(moved.begin(), moved.end(), s) == moved.end())
            ++index;
    }
    return index;
}
} // namespace lila::ui::outliner
