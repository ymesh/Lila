#pragma once

#include "core/Export.h"
#include "render/FrameState.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace lila::render::grade
{
/** Rows of a GradeCurves table, and the postProcess curve attrs in the same order. */
enum Curve
{
    kMaster,
    kRed,
    kGreen,
    kBlue,
    kHueVsSat, // saturation multiplier by hue, 0.5 neutral (0 grey, 1 doubles)
    kHueVsHue, // hue shift by hue, 0.5 neutral (0 and 1 a half turn either way)
    kLumVsSat, // saturation multiplier by luminance, 0.5 neutral
    kCurveCount
};

/** A curve's points from its attribute text, "x y, x y, ...", sorted by x; empty text gives none (the neutral curve). */
LILA_UI_API std::vector<glm::vec2> parseCurve(const std::string& text);
/** Points back to attribute text, three decimals. */
LILA_UI_API std::string formatCurve(const std::vector<glm::vec2>& points);
/** The neutral curve's value at x: x for the tone curves, 0.5 for the hue and saturation ones. */
LILA_UI_API float neutral(Curve c, float x);
/**
 * A curve through its points with monotone cubic segments (no overshoot), flat past the ends; the hue curves wrap around.
 * No points is the neutral curve.
 */
LILA_UI_API float evaluate(Curve c, const std::vector<glm::vec2>& points, float x);

/** Every curve sampled into a table (GradeCurves::kSize per row), cached by text; null when all are neutral. */
LILA_UI_API std::shared_ptr<const GradeCurves> curves(const std::array<std::string, kCurveCount>& texts);

/**
 * The display-space half of the grade the compose pass applies, on one display-encoded (sRGB) colour: offset, lift,
 * gamma and gain with their masters, tonal balance, curves, then the LUT. Matches compose.slang.
 */
LILA_UI_API glm::vec3 applyDisplay(glm::vec3 s, const PostSettings& p);

/**
 * The scene-linear half before the view: white balance, contrast and saturation (exposure aside). Matches compose.slang.
 */
LILA_UI_API glm::vec3 applyScene(glm::vec3 c, const PostSettings& p);
/** Lila's tonemappers on the CPU (PostSettings::Tonemapper, AgX look), display-linear out. Match compose.slang. */
LILA_UI_API glm::vec3 tonemap(glm::vec3 c, int mode, int agxLook);

/**
 * The whole picture for one scene-linear colour, as compose.slang draws it with no lens effects: exposure (stops),
 * scene grade, tonemapper or OCIO view, display gamma, display grade. Display code values (sRGB-encoded) out.
 */
LILA_UI_API glm::vec3 applyAll(glm::vec3 c, const PostSettings& p, float exposure, float displayGamma);
} // namespace lila::render::grade
