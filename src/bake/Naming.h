#pragma once

#include "core/Export.h"

#include <string>
#include <string_view>
#include <vector>

namespace lila::bake
{
/** Values of the output name tokens. */
struct NameTokens
{
    std::string project;
    std::string group;
    std::string mesh;
    std::string material;
    std::string map;
    /** Without the dot: png, exr, tga. */
    std::string ext;
    int udim = 1001;
    /** How {udim} is written: 1001, uv (u1_v1) or uv0 (u0_v0); see image::udimLabel. */
    std::string udimStyle = "1001";
    int width = 0;
    int height = 0;
    /** A texture set variant's name; empty for the base. */
    std::string variant;
};

/** Token names a template may use, without braces. */
LILA_SCENE_API const std::vector<std::string>& nameTokens();

/** Namespace colons, DAG bars and other characters files can't hold become underscores. */
LILA_SCENE_API std::string fileSafe(std::string_view s);

/** Whether the template uses {token}. */
LILA_SCENE_API bool hasToken(std::string_view tmpl, std::string_view token);

/**
 * The file name a template gives: {project} {group} {mesh} {material} {map} {udim} {res} {ext} {variant} replaced (values made
 * file safe; {res} is "2048" for square sizes and "2048x1024" otherwise), ".{ext}" appended when the template has no
 * {ext}. A token may start with exclusions, each removed from the start or end of its value: {[!M_]material} turns
 * M_Rock into Rock, {[!M_][!_Mat]material} also strips a _Mat suffix. Unknown tokens are kept as written. forceUdim inserts ".<udim>" before the extension when the template has no
 * {udim}, for meshes spanning several tiles.
 */
LILA_SCENE_API std::string expandName(std::string_view tmpl, const NameTokens& t, bool forceUdim = false);

/** Tokens in tmpl that expandName doesn't know. */
LILA_SCENE_API std::vector<std::string> unknownTokens(std::string_view tmpl);
} // namespace lila::bake
