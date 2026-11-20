#pragma once

#include "GenCommon.h"

namespace lila::ext
{
class Context;
}

/** The generators' geometry nodes: Roofing, Windows, Rooftop (type WeatheringRooftop) and Curve to Truss. */
namespace weathering::nodes
{
using gen::Bounds;
using gen::Index;
using gen::Mesh;

/** Node type names, as gnBuildGroup specs use them. */
inline constexpr const char* kRoofingType = "WeatheringRoofing";
inline constexpr const char* kWindowsType = "WeatheringWindows";
inline constexpr const char* kRooftopType = "WeatheringRooftop";
inline constexpr const char* kTrussType = "CurveToTruss";

bool registerGeneratorNodes(lila::ext::Context& ctx);
} // namespace weathering::nodes
