#pragma once

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

/** Helpers the OpenWorld test files share. */
namespace owtest
{
inline nlohmann::json run(const char* cmd, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(cmd, call); }

inline std::string data(const char* f) { return std::string(LILA_TEST_OPENWORLD_DATA) + "/" + f; }

/** Each failed build step as a "step: error" line, empty when every step succeeded. */
inline std::string failedSteps(const nlohmann::json& r)
{
    std::string out;
    for (const nlohmann::json& s : r["steps"])
        if (!s.value("ok", true))
            out += s["step"].get<std::string>() + ": " + s.value("error", std::string()) + "\n";
    return out;
}

/** Loads Terrain, the asked-for Settlement and Buildings, then OpenWorld, skipping any already loaded. */
inline void loadPackages(bool settlement, bool buildings)
{
    if (!lila::ops::find("terrainCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (settlement && !lila::ops::find("settlementCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (buildings && !lila::ops::find("buildingCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_BUILDINGS_PACKAGE) == "Buildings");
    if (!lila::ops::find("remixCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_OPENWORLD_PACKAGE) == "OpenWorld");
}
} // namespace owtest
