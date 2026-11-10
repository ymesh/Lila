#pragma once

#include "core/Export.h"

#include "layout/Resolve.h"
#include "scene/Modifiers.h"

#include <memory>

namespace lila::layout
{
/**
 * panelLayoutModifier: holds a Panel Layout drawn on its input mesh and renders its Geometry and Sculpt lines into its
 * output. outLayout gives the resolved layout to every other reader (plates, texture masks, material and geometry nodes).
 */
class LILA_SCENE_API PanelLayoutNode final : public scene::ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kLayout,
        kWriteAttributes,
        kOutMesh,
        kOutLayout,
        kWarning,
    };

    PanelLayoutNode();
    void compute(const scene::EvalContext& ctx) override;
};

inline constexpr const char* kLayoutType = "panelLayoutModifier";

/** A copy of the mesh with panelId, panelRandom and panelLineDistance point attributes from the layout. */
LILA_SCENE_API geo::Mesh withPanelAttributes(const geo::Mesh& mesh, const Resolved& layout);

/** The layout node a node names: itself, or the topmost layout on a mesh's stack. Null for neither. */
LILA_SCENE_API scene::Node* findLayoutNode(scene::Graph& g, scene::NodeId node);
/** The resolved layout a layout node gives out, evaluating first. */
LILA_SCENE_API std::shared_ptr<const Resolved> resolvedLayout(scene::Graph& g, scene::NodeId layoutNode);

/** Registers the Panel Lines, Panel ID and Panel Line Height bake maps; registerTypes calls it. */
LILA_SCENE_API void registerBakeMaps();

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::layout
