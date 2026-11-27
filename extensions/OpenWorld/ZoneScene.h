#pragma once

#include "Zones.h"

#include <lila/Heightfield.h>
#include <lila/Node.h>

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/** Zones in the scene: the zone set node, its spec (cached by text), its ground, and names for new zones. */
namespace openworld::zones
{
/** A transform holding a zone spec on its zones attribute; saved with the scene, every edit a setAttr. */
inline constexpr const char* kSetType = "zoneSet";
inline constexpr const char* kSpecAttr = "zones";
inline constexpr const char* kGroundAttr = "ground";
inline constexpr const char* kOverlayAttr = "overlay";
inline constexpr const char* kOpacityAttr = "fillOpacity";
/** Faint: the fill tints the ground under the outlines and names without hiding it. */
inline constexpr double kDefaultFillOpacity = 0.15;

/** The scene's ground plane, which zones live on: world XZ in a Y-up scene, world XY in a Z-up one. */
glm::dvec3 worldOf(Vec2 p, double up);
Vec2 planeOf(const glm::dvec3& w);
double upOf(const glm::dvec3& w);

/** A terrain the zones drape over: its last heightfield and where it sits. */
struct Ground
{
    std::string mesh;
    std::shared_ptr<const lila::geo::Heightfield> h;
    glm::dmat4 world{1.0};
    glm::dmat4 inv{1.0};

    bool valid() const { return h && !h->empty(); }
    /** World height at a ground plane point; 0 without a ground. */
    double heightAt(Vec2 p) const;
    /** The heightfield's extent on the ground plane. */
    bool extent(Vec2& lo, Vec2& hi) const;
};
/** The terrain a mesh path names (its last heightfield-making modifier); invalid when it is not one. */
Ground groundOf(const std::string& mesh);

/** Every zone set in the scene. */
std::vector<lila::scene::NodeId> zoneSets();
/** The set's spec, parsed once per text; null when the text is broken (error set). */
std::shared_ptr<const ZoneSpec> specOf(const lila::scene::Node& set, std::string* error = nullptr);
/** The set's ground attribute. */
std::string groundPathOf(const lila::scene::Node& set);

/** A ground plane point from [x, z] or [x, y, z] ([x, y] in a Z-up scene). */
Vec2 planePoint(const nlohmann::json& v, const std::string& what, const std::string& cmd);

void registerZoneCommands(lila::ext::Context& ctx);
void registerZoneUi(lila::ext::Context& ctx);
} // namespace openworld::zones
