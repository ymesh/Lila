#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <string>

namespace lila::tex
{
/** What a text layer draws: its words and how they look. */
struct TextImageSpec
{
    std::string text;
    /** A font file, or a family and style (curve::findFont). Empty takes the bundled default. */
    std::string font;
    /** Extra space after each letter, in ems. */
    double tracking = 0.0;
    /** Outline width around the letters, in ems; 0 for none. */
    double outline = 0.0;
    /** Linear colours of the letters and their outline. */
    glm::vec3 color{1.0f};
    glm::vec3 outlineColor{0.0f};
    /** Pixels per line of text. */
    int resolution = 256;
};

struct TextImage
{
    /** The rendered picture (EXR, linear rgb, alpha coverage), cached by the spec. Empty on failure. */
    std::string file;
    /** Its width over its height, so a layer can size its box to the words. */
    double aspect = 1.0;
    std::string error;
};

/**
 * Renders text through a signed distance field of its glyph outlines (exact Beziers, curve::textCurves): letters and
 * outline anti-aliased at any resolution. The file lives in the user cache under a hash of the spec, so the same text is
 * drawn once and a scene opened elsewhere draws it again.
 */
LILA_SCENE_API TextImage renderText(const TextImageSpec& spec);
} // namespace lila::tex
