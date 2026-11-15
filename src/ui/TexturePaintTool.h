#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

class QWidget;

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
 *   [ ]                  radius smaller / larger
 *   Esc                  cancel the stroke, clear the buffer, or leave the tool
 */
LILA_UI_API void startTexturePaint();

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
/** The brush tip's alpha (brush::alphas path), empty for round. Setting one starts Texture Paint. */
LILA_UI_API std::string texturePaintAlpha();
LILA_UI_API void setTexturePaintAlpha(const std::string& alphaPath);
/** The image the brush paints through, empty unless the Texture source is on. Setting one turns it on and starts Texture Paint. */
LILA_UI_API std::string texturePaintTexture();
LILA_UI_API void setTexturePaintTexture(const std::string& file);

/** tex.paint and the paint layer actions (tex.rasterizeLayer, tex.flattenLayer, tex.clearLayer). */
LILA_UI_API void registerTexturePaintUi();
} // namespace lila::ui
