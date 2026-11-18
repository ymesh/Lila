#pragma once

#include "core/Export.h"

#include "render/RenderMesh.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

class QImage;

namespace lila::render
{
struct FrameState;
}

/**
 * The Quick Mask overlay: while the quick mask holds coverage, a translucent tint over what strokes can't reach, on the
 * meshes in the viewport and in the 2D View. Drawn from the quick mask paint buffer, so it follows the mask as it is
 * painted. Colour, opacity and whether it shows are general prefs.
 */
namespace lila::ui::quickMaskOverlay
{
/** The paint buffer Quick Mask paints, outside the stack. */
inline constexpr const char* kBuffer = "quickmask";
inline constexpr const char* kShownPref = "/texture/quickMaskOverlay";
inline constexpr const char* kColorPref = "/texture/quickMaskColor";
inline constexpr const char* kOpacityPref = "/texture/quickMaskOpacity";
inline constexpr const char* kDefaultColor = "#d9263a";
inline constexpr double kDefaultOpacity = 0.45;

struct Style
{
    glm::vec3 color{0.85f, 0.15f, 0.23f}; // sRGB
    float opacity = 0.45f;
    bool shown = true;
};
LILA_UI_API Style style();
LILA_UI_API void setShown(bool on);

/**
 * The tint over a set's uv square (tile 1001): RGBA8 sRGB, rows top first, the style's colour at its opacity times what
 * the quick mask leaves out, clear where it covers. Null when the set has no quick mask buffer. Rebuilt after the
 * buffer's paint or the style changes; version counts the rebuilds.
 */
LILA_UI_API render::TexturePtr tint(const std::string& set, uint64_t* version = nullptr);
/** The same tint as a QImage for 2D views; null image when there is none. */
LILA_UI_API QImage tintImage(const std::string& set);

/**
 * Adds a tint draw over every mesh draw in fs whose material slots use set: the same mesh again, unlit and translucent,
 * the tint on the slots of the set and nothing on the others. Nothing while the style hides it or the set has no tint.
 */
LILA_UI_API void addDraws(render::FrameState& fs, const std::string& set);

/** texQuickMaskOverlay. */
LILA_UI_API void registerCommands();
} // namespace lila::ui::quickMaskOverlay
