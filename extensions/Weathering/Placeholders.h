#pragma once

#include <lila/Mesh.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * Placeholder pieces: procedural meshes that make every weathering preset work before the user brings their own. Each
 * area adds its builders at registration; weatheringPlaceholders writes them as Stagehand library assets (OBJ plus
 * asset.json) into <user prefs>/weathering-library and adds that folder as the library's "placeholder" tier, searched
 * after every other. Rebuilt when the version or the set of builders changes.
 *
 * Every placeholder is tagged "weathering", "placeholder", its role and its style ("realistic" or "toon"), so
 * piecesOrDefault finds it and the user's own assets with the same role tags take its place.
 */
namespace weathering
{
/** Materials of placeholder pieces start with this; they are the pieces' own, not ones to paint with. */
inline constexpr const char* kPieceMaterialPrefix = "PiecePlaceholder_";

struct Placeholder
{
    std::string collection; // "Weathering/Rubble"
    std::string name;       // "ConcreteChunk1"
    std::string role;       // "rubble"
    std::string style;      // "realistic" or "toon"
    std::vector<std::string> tags; // extra words to find it by
    std::string description;
    std::string physicsRole = "dynamic";
    std::string physicsMaterial = "stone"; // wood, book, paper, cardboard, stone, ceramic, metal, glass, plastic, rubber, fabric
    std::string physicsShape = "hull";
    /** Linear base colour of its material; negative picks one from the role and style. */
    glm::vec3 color{-1.0f};
    /** The mesh in cm, Y up, standing on y = 0 unless it is a card or strip. */
    std::function<lila::geo::Mesh()> build;
};

/** Adds a builder; called by each area at registration. A second of the same collection/name replaces the first. */
void addPlaceholder(Placeholder p);
const std::vector<Placeholder>& placeholders();

/** Writes the placeholders when they are missing or out of date (or force), and adds their library root. */
nlohmann::json buildPlaceholders(bool force);

/** weatheringPlaceholders, and the library root added at load. */
void registerPlaceholderCommands(lila::ext::Context& ctx);
} // namespace weathering
