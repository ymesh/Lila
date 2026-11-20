#pragma once

#include "GenCommon.h"

/**
 * Rooftops: the flat tops of buildings (upward faces nothing stands over), with railings along some of their edges and
 * a tower on some of them. The tops come out as a gridded surface carrying a point float rooftopFree (0 near edges,
 * towers and railings, 1 elsewhere), which Stagehand's Scatter Props reads as its mask to place vents, AC units and pipes.
 *
 * Railings lie along X (scaled to fill each edge), Y up; towers stand Y up at their own size.
 */
namespace weathering::rooftop
{
using gen::Bounds;
using gen::Index;
using gen::Mesh;

inline constexpr const char* kFreeAttr = "rooftopFree";

struct Options
{
    double slopeMax = 5.0;    // degrees from up that counts as flat
    double minArea = 2.0;     // m2, smaller flat tops are left alone
    double edgeMargin = 50.0; // cm kept clear inside each top's edge
    double railings = 0.5;    // 0 to 1: share of top edges that get a railing
    double railInset = 8.0;   // cm in from the edge
    double towers = 0.35;     // 0 to 1: chance a top gets a tower
    double towerMargin = 40.0; // cm kept round a tower
    double cell = 40.0;       // cm, the surface grid
    uint32_t seed = 1;
};

struct Placed
{
    glm::dmat4 xf{1.0};
    int slot = 0; // 0 railing, 1 tower
    int piece = 0;
    float random = 0.0f;
};

struct Top
{
    double y = 0.0;
    double area = 0.0; // cm2
    std::vector<std::vector<glm::dvec2>> faces; // (x, z)
    bool inside(const glm::dvec2& xz) const;
};

struct Result
{
    std::vector<Top> tops;
    Mesh surface;
    std::vector<Placed> placed;
    int railings = 0, towers = 0;
};

Result layout(const Mesh& host, std::span<const Bounds> railings, std::span<const double> railingWeights, std::span<const Bounds> towers,
              std::span<const double> towerWeights, const Options& o);
} // namespace weathering::rooftop
