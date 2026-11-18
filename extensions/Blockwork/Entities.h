#pragma once

#include "Blockwork.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace blockwork
{
/** The node type of point entities. */
inline constexpr const char* kEntityType = "blockworkEntity";

struct EntityProperty
{
    std::string name;
    /** number, int, bool, string, color, enum or node (a path to another node, drawn as a line to it). */
    std::string type = "string";
    json defaultValue;
    /** The Unreal property it sets; empty sends nothing. */
    std::string unreal;
    std::vector<std::string> choices;
    std::string doc;
};

/** A class from an entity definition file. */
struct EntityClass
{
    std::string name;
    /** point (a node with a box) or volume (a block). */
    std::string kind = "point";
    /** The Unreal class or Blueprint path it becomes, such as /Script/Engine.PlayerStart or /Game/BP_Door.BP_Door_C. */
    std::string unreal;
    glm::dvec3 size{32.0};
    glm::vec3 color{0.8f};
    std::vector<EntityProperty> properties;
    std::string doc;
    std::string file;
};

const std::vector<EntityClass>& entityClasses();
const EntityClass* findEntityClass(const std::string& name);
bool isEntity(const lila::scene::Node& n);
void registerEntities(lila::ext::Context& ctx);
} // namespace blockwork
