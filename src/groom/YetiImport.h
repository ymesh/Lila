#pragma once

#include "curve/Spline.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/** Yeti grooms from Maya ASCII text, without Maya or Yeti (YetiImport.cpp). */
namespace lila::groom::yeti
{
/** A pgYetiMaya node of the file: its shape and transform names, its scalp's transform, the grooms feeding it. */
struct Description
{
    std::string node, transform, scalp;
    std::vector<std::string> grooms;
};

/** A Yeti node read for a Lila groom: guides (scene units, as Yeti's grooms hold them), the groom graph, the report. */
struct Imported
{
    Description description;
    curve::Splines guides;
    std::string graph;
    nlohmann::json report;
};

std::vector<Description> describe(const std::string& maText);
/** Throws std::runtime_error for a node that isn't there or can't be read. */
Imported readDescription(const std::string& maText, const std::string& yetiNode);
void registerCommands();
} // namespace lila::groom::yeti
