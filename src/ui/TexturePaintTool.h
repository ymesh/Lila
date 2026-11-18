#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

class QWidget;

namespace lila::geo
{
class Mesh;
}
namespace lila::scene
{
class Node;
}
namespace lila::tex::paint
{
struct PaintSymmetry;
}

namespace lila::ui
{
/**
 * Texture painting (action "tex.paint"): paints the material channels of the texture set bound to the material under the
 * brush, into the set's active paint layer (a new one when it has none), in texture space by 3D distance so strokes
 * cross UV seams cleanly. Strokes show live in the viewport and commit as texPaintStroke records, one undo step each.
 * The panel picks a preset and the tool: paint, direction, normal, smudge, blur, sharpen, clone, heal, fill, projection.
 *
 *   LMB drag             paint (fill: click)
 *   Ctrl                 erase (the stroke's blend becomes Erase)
 *   Ctrl+Alt+click       clone / heal source (on the surface; on the buffer for buffer cloning)
 *   Enter                projection: project the paint buffer
 *   B or F held + drag   brush radius (F tapped still frames)
 *   M or Shift+F + drag  strength
 *   Shift+F tapped       wireframe while painting
 *   [ ]                  radius smaller / larger
 *   Esc                  cancel the stroke, clear the buffer, or leave the tool
 */
LILA_UI_API void startTexturePaint();

/**
 * The meshes Texture Paint reaches, in the viewport and the 2D View: the selected meshes and the meshes under selected
 * groups, else every mesh. Ids are scene::NodeId.
 */
LILA_UI_API std::vector<uint64_t> texturePaintTargets();

/** Painting's own symmetry, apart from the mesh's: off, x, y, z, or mesh (the mesh's setting, radial included). */
LILA_UI_API std::string texturePaintSymmetry();
/** The symmetry a stroke on the mesh (surface: what it paints, its evaluated or sculpted mesh) paints with, into out. */
LILA_UI_API void texturePaintSymmetryFor(const scene::Node& mesh, const geo::Mesh* surface, tex::paint::PaintSymmetry& out);

/** Texture Paint is the active viewport tool (the 2D View paints with it then). */
LILA_UI_API bool texturePaintActive();

/** Texture Paint shows the mesh wire: only Fill picking triangles, faces or UV islands, which needs to see them. */
LILA_UI_API bool texturePaintShowsWire();

/** Wireframe While Painting (tex.paintWire): the mesh wire stays visible, dimmed, while Texture Paint hides it. */
LILA_UI_API bool texturePaintDimWire();

/**
 * The texture paint tool's current stroke settings, for other surfaces that paint with them (the UV editor):
 * {"brush": brush::BrushSettings JSON, "payload": tex::paint::Payload JSON, "particles": spec or null}.
 */
LILA_UI_API nlohmann::json texturePaintSettings();

/** The Brush page (the texture paint settings) reparented under parent: one page, which the Texture Set panel hosts. */
LILA_UI_API QWidget* texturePaintPage(QWidget* parent);

/** The brush preset last applied (tex::brushes id), empty for none. Applying one starts Texture Paint. */
LILA_UI_API std::string texturePaintPreset();
LILA_UI_API void applyTexturePaintPreset(const std::string& id);
/** The default round brush's settings (brush::BrushSettings JSON). */
LILA_UI_API nlohmann::json defaultTexturePaintBrush();
/** The default round brush again, as before any preset (tex.resetBrush). Starts Texture Paint. */
LILA_UI_API void resetTexturePaintBrush();
/** The brush tip's alpha (brush::alphas path), empty for round. Setting one starts Texture Paint. */
LILA_UI_API std::string texturePaintAlpha();
LILA_UI_API void setTexturePaintAlpha(const std::string& alphaPath);
/** The image the brush paints through, empty unless the Texture source is on. Setting one turns it on and starts Texture Paint. */
LILA_UI_API std::string texturePaintTexture();
LILA_UI_API void setTexturePaintTexture(const std::string& file);
/** The texture set the quick mask limits strokes on (Quick Mask on, or its mask held), empty when it holds nothing. */
LILA_UI_API std::string quickMaskSet();
/** The paint picker at a uv of a set (the 2D View and UV editor): its channel values into the brush, or with add a new swatch. */
LILA_UI_API void pickTextureChannelsAt(const std::string& set, double u, double v, bool add);
/** The paint tool's gradient as a fill payload from (u0, v0) to (u1, v1) in uv, for the 2D View and UV editor. */
LILA_UI_API nlohmann::json texturePaintGradient(double u0, double v0, double u1, double v1, bool reverse);

/** tex.paint and the paint layer actions (tex.rasterizeLayer, tex.flattenLayer, tex.clearLayer). */
LILA_UI_API void registerTexturePaintUi();
} // namespace lila::ui
