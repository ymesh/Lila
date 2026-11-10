#pragma once

#include "core/Export.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::groom
{
/** A tool in the groom brush palette: GroomLab's brush catalog. */
struct GroomBrushEntry
{
    std::string id;
    std::string label;
    std::string category;
    std::string icon;
    std::string hint;
    double defaultStrength = 1.0;
    /** Strength runs -1 to 1; negative works the other way. */
    bool signedStrength = false;
    /** The settings the tool shows (GroomLab's setting ids: radius, strength, falloff, symmetry, ...). */
    std::vector<std::string> settings;
};

/** The palette's tools in order, grouped by category. */
LILA_SCENE_API const std::vector<GroomBrushEntry>& groomBrushCatalog();
LILA_SCENE_API std::vector<std::string> groomBrushCategories();
LILA_SCENE_API const GroomBrushEntry* groomBrushEntry(const std::string& id);
/** A setting's caption and tooltip. */
LILA_SCENE_API std::pair<std::string, std::string> groomBrushSettingText(const std::string& id);

/** What the brush settings are, as the palette edits them and prefs keep them. */
struct GroomBrushSettings
{
    std::string mode = "comb";
    double radius = 60.0; // pixels
    double strength = 0.5;
    double falloff = 1.0; // 0 hard, 1 soft
    bool pressure = true;
    bool collide = true;
    bool frontOnly = true;
    bool stretch = false;
    bool selectedOnly = false;
    std::string tubeChannel = "width"; // width, flatness, twist
    std::string targets = "guides";    // guides, parts, flow, any comma separated
    // New guides (Place, Draw).
    double placeLength = 10.0;
    int placeSegments = 12;
    double placeSpacing = 1.0;
    bool matchNeighbors = true;
    std::string group = "primary";
    // Paint.
    std::string attribute = "clump";
    double value = 1.0;
    std::string paintKind = "guides"; // guides, vertices, texture
    double smoothStrength = 1.0;
    // Part and Flow.
    std::string partName = "part";
    std::string flowSink; // "" lays arrows; whorl, whorlcw, converge, part
    /** Brush every level (-1) or one. */
    int level = 0;
};

/** The view a stroke is seen through: pixels to world and back. */
struct GroomBrushView
{
    std::function<std::optional<glm::dvec2>(const glm::dvec3&)> project;
    std::function<void(glm::dvec2, glm::dvec3& origin, glm::dvec3& direction)> ray;
    glm::dvec3 eye{0.0};
    glm::dvec3 viewDir{0.0, 0.0, -1.0};
    bool ortho = false;
    /** The view's height in pixels: x and y are from its top left. */
    double height = 1.0;
};

enum GroomBrushModifier : unsigned
{
    kGroomBrushCtrl = 1u,
    kGroomBrushShift = 2u,
};

/**
 * GroomLab's brushes on a groom node: strokes read the node's scalp, guides, parts, flow and maps, show live on it
 * (undo off while the stroke runs) and land as one undo step. Symmetry follows the node's.
 */
class LILA_SCENE_API GroomBrush
{
public:
    GroomBrush();
    ~GroomBrush();
    GroomBrush(const GroomBrush&) = delete;
    GroomBrush& operator=(const GroomBrush&) = delete;

    /** The groom brushed; kNoNode stops. */
    void setGroom(scene::NodeId groom);
    scene::NodeId groom() const;

    void setSettings(const GroomBrushSettings& settings);
    const GroomBrushSettings& settings() const;

    /** Starts a stroke; false when none started (warnings say why). */
    bool press(const GroomBrushView& view, double x, double y, double pressure, unsigned modifiers);
    void drag(const GroomBrushView& view, double x, double y, double pressure);
    /** Ends the stroke; true when it changed the groom. */
    bool release(const GroomBrushView& view);
    /** Drops a stroke without committing it. */
    void cancel();
    bool stroking() const;

    /** The guides the brush would touch at (x, y), world space, as drawn. */
    std::vector<std::vector<glm::dvec3>> affectedLines(const GroomBrushView& view, double x, double y);
    /** Part and Flow: the path drawn so far on the scalp. */
    std::vector<glm::dvec3> drawnPath() const;
    /** The guides of the groom as shown, world space (for drawing them while brushing). */
    std::vector<std::vector<glm::dvec3>> guideLines();
    /** Picks the level and group of the guide under (x, y) for the brush; -2 when none is there. */
    int pickLevel(const GroomBrushView& view, double x, double y);

    /** Length Keys: its overlay as coloured lines (keys, rings, the hovered and active ones marked). */
    struct ColoredLine
    {
        std::vector<glm::dvec3> points;
        glm::vec3 color{1.0f};
    };
    std::vector<ColoredLine> lengthKeyLines(const GroomBrushView& view, double x, double y);
    /** Length Keys has edits waiting for Apply. */
    bool lengthKeysEdited() const;
    /** Commits the guides at their keyed lengths as one undo step; false when nothing was to apply. */
    bool applyLengthKeys();
    void cancelLengthKeys();
    void deleteLengthKey();
    void clearLengthKeys();

    std::vector<std::string> takeWarnings();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila::groom
