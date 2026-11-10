#pragma once

#include <lila/PanelLayout.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

/** Plates: each panel of a Panel Layout as a thick plate, split from its neighbours by a gap. */
namespace plating
{
using json = nlohmann::json;

struct PlateOptions
{
    /** Width of the gap between plates along every line, cm. */
    double gap = 0.3;
    /** How far each plate stands up from the surface, cm. */
    double thickness = 1.0;
    /** How far the plate's base moves out along the surface normal first, cm; negative sinks it. */
    double elevation = 0.0;
    /** Width of the bevel round each plate's top edge, cm; 0 for none. */
    double bevel = 0.15;
    int bevelSegments = 2;
    /** A raised rim inside each plate's edge: its width and how far the middle sinks below it, cm. */
    double lipWidth = 0.0;
    double lipDepth = 0.0;
    /** Close each plate underneath, so it is a solid. */
    bool closed = true;
    /** New edges sharper than this angle are hard, degrees. */
    double hardAngle = 40.0;
};

/** One panel's own values, from the layout's panel overrides; unset follows the options. */
struct PanelValues
{
    bool hidden = false;
    std::optional<double> elevation;
    std::optional<double> thickness;
};

PanelValues panelValues(const json& values);

struct PlatesResult
{
    lila::geo::Mesh mesh;
    int plates = 0;
    std::vector<std::string> warnings;
};

/** The plates for every panel of the layout, built on the surface it was resolved on. */
PlatesResult makePlates(const lila::layout::Resolved& layout, const PlateOptions& opts);
} // namespace plating
