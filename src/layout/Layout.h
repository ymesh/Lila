#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * Panel Layout: one graph of lines drawn on a mesh's surface. Junctions are surface points; runs are smooth lines
 * between two junctions (or one, for a closed loop) through control points on the surface. Closed loops of runs bound
 * panels. Line sets class the runs (Major, Minor ...) and say how each class renders: as grooves cut into the mesh,
 * grooves displaced into a dense mesh, texture detail or decals.
 */
namespace lila::layout
{
using json = nlohmann::json;
using geo::Index;
using geo::kInvalid;

/**
 * A spot on the mesh: a blend of three of a face's points, so it follows the surface as points move (sculpting), plus
 * the position it had when last resolved, which finds it again by closest point once the topology changes.
 */
struct Anchor
{
    Index face = kInvalid;
    std::array<Index, 3> points{kInvalid, kInvalid, kInvalid};
    glm::dvec3 bary{0.0};
    glm::dvec3 pos{0.0};

    bool operator==(const Anchor&) const = default;
};

/** Cross-section of a groove. */
enum class Profile : uint8_t
{
    V,
    U,
    Square,
    Step,
    Chamfer,
};

/** How a line set renders. */
enum class Output : uint8_t
{
    /** Cut into the mesh: new edges along the profile, then shaped. */
    Geometry,
    /** Displaced into the existing points, for dense (sculpt) meshes. */
    Sculpt,
    /** Height and normal in a texture set; the layout only provides the maps. */
    Texture,
    /** Decal strips along the lines. */
    Decal,
    /** Lines that only divide panels. */
    None,
};

/** End of a run that stops at a junction with nothing else attached. */
enum class EndStyle : uint8_t
{
    Round,
    Square,
    Fade,
};

struct Junction
{
    uint32_t id = 0;
    Anchor at;
    /** Where exactly two runs meet: a sharp corner rather than a line passing through. */
    bool corner = false;
    EndStyle end = EndStyle::Round;
};

struct Run
{
    uint32_t id = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    /** Control points between the two junctions, in order from a. None is a straight line on the surface. */
    std::vector<Anchor> controls;
    uint32_t set = 0;
    /** Per-run overrides of the set's values; unset follows the set. */
    std::optional<double> width;
    std::optional<double> depth;
    std::optional<Profile> profile;
    /** Released from its fit: dense controls that a Cleanup change or an overdraw refits. */
    bool loose = false;
};

struct LineSet
{
    uint32_t id = 0;
    std::string name;
    double width = 0.4;
    double depth = 0.2;
    Profile profile = Profile::V;
    Output output = Output::Geometry;
};

/** Values one panel holds for the systems that read panels (plates, fills); found again by a point inside it. */
struct PanelOverride
{
    Anchor seed;
    json values = json::object();
};

struct LILA_SCENE_API PanelLayout
{
    std::vector<Junction> junctions;
    std::vector<Run> runs;
    std::vector<LineSet> sets;
    std::vector<PanelOverride> panels;
    /** The set new strokes go to. */
    uint32_t activeSet = 1;
    /** Mirror plane through the mesh's origin across this axis (0 x, 1 y, 2 z), -1 for none. */
    int mirrorAxis = -1;
    /** The side the lines are drawn on, +1 or -1 along the axis; the other side is generated. */
    int mirrorSide = 1;
    uint32_t nextId = 1;

    uint32_t newId() { return nextId++; }

    const Junction* junction(uint32_t id) const;
    Junction* junction(uint32_t id);
    const Run* run(uint32_t id) const;
    Run* run(uint32_t id);
    const LineSet* set(uint32_t id) const;
    LineSet* set(uint32_t id);
    /** The run's set, or the first set when it names none that exists. */
    const LineSet& setOf(const Run& r) const;
    double widthOf(const Run& r) const { return r.width.value_or(setOf(r).width); }
    double depthOf(const Run& r) const { return r.depth.value_or(setOf(r).depth); }
    Profile profileOf(const Run& r) const { return r.profile.value_or(setOf(r).profile); }

    /** Runs touching the junction, each once (a loop on it counts once). */
    std::vector<uint32_t> runsAt(uint32_t junction) const;
    /** Number of run ends at the junction (a loop counts twice). */
    int degree(uint32_t junction) const;

    /** Drops junctions no run uses. */
    void removeUnusedJunctions();

    /** Major and Minor, Major active: what a new layout starts with. */
    static PanelLayout withDefaultSets();

    json toJson() const;
    static PanelLayout fromJson(const json& j);
};

LILA_SCENE_API const char* profileName(Profile p);
LILA_SCENE_API std::optional<Profile> profileFromName(std::string_view s);
LILA_SCENE_API const char* outputName(Output o);
LILA_SCENE_API std::optional<Output> outputFromName(std::string_view s);
LILA_SCENE_API const char* endStyleName(EndStyle e);
LILA_SCENE_API std::optional<EndStyle> endStyleFromName(std::string_view s);

/** Depth across a groove as a fraction of its full depth, at x = distance from the centre / half the width (0 to 1). */
LILA_SCENE_API double profileDepth(Profile p, double x);
/** The lateral positions (0 to 1 of the half width) where the profile bends, where Geometry output cuts edges. */
LILA_SCENE_API std::vector<double> profileBreaks(Profile p);

/** A layout as a plug value. */
struct LILA_SCENE_API LayoutData final : scene::Data
{
    PanelLayout layout;

    explicit LayoutData(PanelLayout l = {}) : layout(std::move(l)) {}
    const char* kind() const override { return "panelLayout"; }
    json toJson() const override;
    size_t bytes() const override;
    static scene::DataPtr fromJson(const json& j);
};

LILA_SCENE_API scene::DataPtr makeLayoutData(PanelLayout l);
/** The layout a plug value holds, or null. */
LILA_SCENE_API const PanelLayout* layoutOf(const scene::DataPtr& d);
} // namespace lila::layout
