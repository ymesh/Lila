#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * Reciprocal velocity obstacles (ORCA, after van den Berg et al. and the RVO2 library): each agent picks the velocity
 * nearest the one it prefers that keeps it clear of every neighbour for timeHorizon seconds, each side of a pair taking
 * half the avoidance, and clear of static obstacles (convex polygons) for obstacleHorizon seconds. Ground plane (x, z),
 * meters, seconds. Deterministic: agents are solved in index order from the same inputs.
 */
namespace crowds::orca
{
struct Agent
{
    glm::dvec2 position{0.0};
    glm::dvec2 velocity{0.0};
    glm::dvec2 preferred{0.0};
    double radius = 0.3;
    double maxSpeed = 1.5;
    /** Agents that don't avoid (standing in a queue, seated) still count as obstacles for the others. */
    bool active = true;
};

/** A convex polygon, counter-clockwise seen from above (+Y), in ground coordinates (x, z). */
struct Obstacle
{
    std::vector<glm::dvec2> points;
};

struct Options
{
    double neighbourDistance = 4.0;
    int maxNeighbours = 10;
    double timeHorizon = 2.0;
    double obstacleHorizon = 1.0;
};

/** New velocities for every agent (inactive ones keep their preferred velocity). */
std::vector<glm::dvec2> solve(const std::vector<Agent>& agents, const std::vector<Obstacle>& obstacles, const Options& options);

/** Signed distance from p to a convex polygon (negative inside) and the nearest point on its boundary. */
double polygonDistance(const Obstacle& o, glm::dvec2 p, glm::dvec2* nearest = nullptr);
} // namespace crowds::orca
