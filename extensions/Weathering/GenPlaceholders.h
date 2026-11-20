#pragma once

#include "GenCommon.h"

/** The generators' placeholder pieces: roles, and the procedural meshes behind them. */
namespace weathering::gen
{
/** Roles the generators fill (library tags "weathering" plus the role). */
namespace role
{
inline constexpr const char* kRoofSheet = "roof-sheet";
inline constexpr const char* kRidgeCap = "ridge-cap";
inline constexpr const char* kRoofTile = "roof-tile";
inline constexpr const char* kRoofPlank = "roof-plank";
inline constexpr const char* kFrame = "window-frame";
inline constexpr const char* kSill = "window-sill";
inline constexpr const char* kGlass = "window-glass";
inline constexpr const char* kGlassCracked = "window-glass-cracked";
inline constexpr const char* kGlassBroken = "window-glass-broken";
inline constexpr const char* kBoard = "window-board";
inline constexpr const char* kInsulator = "wire-insulator";
inline constexpr const char* kBracket = "wire-bracket";
inline constexpr const char* kPole = "pole";
inline constexpr const char* kRubble = "rubble";
inline constexpr const char* kPlank = "plank";
inline constexpr const char* kBrick = "brick";
inline constexpr const char* kVent = "roof-vent";
inline constexpr const char* kAcUnit = "ac-unit";
inline constexpr const char* kPipe = "roof-pipe";
inline constexpr const char* kRailing = "railing";
inline constexpr const char* kTower = "tower";
} // namespace role

/** A placeholder's mesh by role and style ("realistic" or "toon"); empty for an unknown role. */
Mesh placeholderMesh(const std::string& role, bool toon);

/** Every role the generators register placeholders for. */
std::vector<std::string> generatorRoles();

/** Registers one realistic and one toon placeholder per role (addPlaceholder). */
void addGeneratorPlaceholders();
} // namespace weathering::gen
