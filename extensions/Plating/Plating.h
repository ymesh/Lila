#pragma once

#include <lila/Graph.h>
#include <lila/Node.h>

#include <nlohmann/json.hpp>

namespace lila::ext
{
class Context;
}

/** The Plating extension: plates from Panel Layouts, their commands and menus. */
namespace plating
{
inline constexpr const char* kPlatesType = "platesModifier";
inline constexpr const char* kFastenersType = "fastenersModifier";
inline constexpr const char* kPatternType = "patternModifier";

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
bool registerFastenerNodes(lila::ext::Context& ctx);
void registerFastenerCommands(lila::ext::Context& ctx);
bool registerPatternNodes(lila::ext::Context& ctx);
void registerPatternCommands(lila::ext::Context& ctx);
} // namespace plating
