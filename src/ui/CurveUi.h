#pragma once

#include "groom/GroomBrush.h"

#include "core/Export.h"
#include "ui/ViewportTool.h"

#include <QPointF>

#include <vector>

namespace lila::ui
{
/**
 * Curves in the viewport and menus: the "lila.curve" draw override (the curve, and while editing its hull, handles, points
 * and curvature comb), the draw tools (Bezier pen, CV, EP, pencil, Catmull-Rom, kappa) and every curve action, placed in the
 * Curves menu and the modeling toolkit.
 */
LILA_UI_API void registerCurveUi();
/** The draw tools (CurveTools.cpp); called by registerCurveUi. */
void registerCurveTools();
/** The Curve Point tool and Insert Point (CurvePointTool.cpp); called by registerCurveUi. */
void registerCurvePointTool();
/** The EZ Curve tool (EzCurveTool.cpp); called by registerCurveUi. */
void registerEzCurveTool();
/** The pipe tool and pipe actions (PipeTool.cpp); called by registerCurveUi. */
void registerPipeTool();
/** The cable tool and cable action (CableTool.cpp); called by registerCurveUi. */
void registerCableTool();
/** Pipe Route tool, Pipe Supports and Carve Channel actions, and their Level Toolkit section. */
void registerPipeLevelTools();
/** The section tool and contour action (SectionTool.cpp); called by registerCurveUi. */
void registerSectionTool();
/** The strap tool and strap preset actions (StrapTool.cpp); called by registerCurveUi. */
void registerStrapTool();
/** Blend, match, trim, extend, split, fair, curve network and border blend actions and tools (VehicleCurveUi.cpp). */
void registerVehicleCurveUi();
/** The blend curve tool (BlendCurveTool.cpp). */
void registerBlendCurveTool();
/** The trim / extend tool (TrimCurveTool.cpp). */
void registerTrimCurveTool();
/** The sketch constraints tool and sketch actions (SketchTool.cpp); called by registerCurveUi. */
void registerSketchTool();
/** The Sculpt Curves tool and grooming actions (CurveSculptTool.cpp); called by registerCurveUi. */
void registerCurveSculptTool();
/** The Draw Hair tool, groom actions and the groom Attribute Editor section (GroomTool.cpp); called by registerCurveUi. */
void registerGroomUi();
/** Sculpt Curves' brush (groom::Brush) and its Settle While Grabbing, for the Groom palette. */
int curveSculptBrush();
void setCurveSculptBrush(int brush);
bool curveSculptSettleGrab();
void setCurveSculptSettleGrab(bool on);
/** Sculpt Curves' stroke and brush settings back to their defaults, keeping the brush. */
void resetCurveSculptSettings();
/** The Sculpt Curves keys that matter now, for the key list. */
std::vector<KeyHint> curveSculptKeys();
/** The Groom palette's pages: Draw Hair's modes, Sculpt Curves' brushes on curves, the groom brushes on a groom. */
enum class GroomPalettePage
{
    Draw,
    Sculpt,
    Groom,
};
/** The Groom palette: shown while Draw Hair, Sculpt Curves or Groom Brushes is active, on that tool's page; synced when settings change. */
void showGroomPalette(GroomPalettePage page);
/** Groom Brushes (GroomBrushTool.cpp): GroomLab's tools on a groom; called by registerGroomUi. */
void registerGroomBrushTool();
const groom::GroomBrushSettings& groomBrushSettings();
void setGroomBrushSettings(const groom::GroomBrushSettings& settings);
/** Picks a groom brush, bringing back its strength, and starts Groom Brushes. */
void setGroomBrushMode(const std::string& id);
void resetGroomBrushSettings();
bool groomBrushToolActive();
std::vector<KeyHint> groomBrushKeys();
/** The Groom palette's Groom Brushes page. */
QWidget* makeGroomBrushPage(QWidget* parent);
void syncGroomBrushPage(QWidget* page);
void hideGroomPalette();
/** A mesh or clay under `at` that hair can grow from, or kNoNode; hovered meshes are highlighted for the groom tools. */
scene::NodeId groomScalpUnder(const ViewInfo& view, QPointF at);
/** Starts a groom on a mesh, or on a clay object baked to a mesh first (the log says so); null when it can't. */
scene::Node* startGroomOn(scene::NodeId scalp);
/** The groom the groom tools work on: the selection's, else one started on the selected mesh or clay, else the scene's only groom. */
scene::Node* groomForSelection();
void syncGroomPalette();
/** Groom strands on a curve node drawn as hair into a frame (captures; the viewport draws them itself). False: not strands. */
bool drawStrandsFor(render::FrameState& frame, const scene::Node& n, const glm::dmat4& world, const ViewInfo& view);
/** Whether the tools list their keys in the corner of the view. */
bool groomKeysShown();
/** Text, Bevel Plus, curve booleans, region offsets and the straight skeleton tools (PlanarCurveUi.cpp); called by registerCurveUi. */
void registerPlanarCurveUi();
} // namespace lila::ui
