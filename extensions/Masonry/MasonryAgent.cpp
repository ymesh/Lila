// What agents read about masonry over MCP besides lila://guide/masonry (resources/agents): the building spec schema and the
// make_ruin prompt.

#include "Masonry.h"
#include "MasonryBuilding.h"

#include <lila/Mcp.h>

namespace masonry
{
namespace
{
const char* kPrompt = R"(Build {subject} in Lila with the Masonry tools, then ruin, age, check and ship it.

Style: {style}. Condition: {condition}. Budget: {budget}.

1. Read lila://guide/masonry and lila://masonry/spec-schema.
2. Write one masonryBuilding spec: name every wall and opening the way you would talk about them (north_wall, west_door). Pick stone sizes and scale to suit the subject's size; for very large builders scale the stones too.
3. Build it, read the stats (stones, regions, triangles) and adjust the spec until the shape is right.
4. Ruin it to the condition: left (0.75, 0.5, 0.25), collapse direction, gaps, regions for parts that fell further.
5. Age it, then masonryCheck until ok is true and the stats fit the budget.
6. masonryShip it with wait=True and report what Unreal placed.
)";
} // namespace

void registerAgentDocs(lila::ext::Context& ctx)
{
    using lila::mcp::Server;
    Server& s = Server::get();
    s.addResource(ctx, {.uri = "lila://masonry/spec-schema",
                   .name = "Masonry building spec",
                   .description = "Every key of the masonryBuilding spec with its type, default and meaning.",
                   .mimeType = "application/json",
                   .read = [] { return specSchema().dump(2); }});
    s.addPrompt(ctx, {.name = "make_ruin",
                 .description = "Build a masonry building or ruin from one description: spec, ruin, age, check, ship to Unreal.",
                 .arguments = nlohmann::json::array({
                     {{"name", "subject"}, {"description", "What to build, e.g. \"a ruined Gothic chapel with a bell tower\""}, {"required", true}},
                     {{"name", "style"}, {"description", "Masonry style, e.g. \"ashlar, pointed arches\""}, {"required", false}},
                     {{"name", "condition"}, {"description", "How ruined, e.g. \"half the roof gone, north wall down to a third\""}, {"required", false}},
                     {{"name", "budget"}, {"description", "Stones or triangles, e.g. \"50000 stones\""}, {"required", false}},
                 }),
                 .get = [](const nlohmann::json& args) {
                     std::string text = kPrompt;
                     const std::pair<const char*, const char*> keys[] = {
                         {"subject", "the requested building"}, {"style", "ashlar"}, {"condition", "half ruined"}, {"budget", "100000 stones"}};
                     for (const auto& [key, fallback] : keys)
                     {
                         const std::string value = args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : fallback;
                         const std::string tag = std::string("{") + key + "}";
                         for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
                             text.replace(at, tag.size(), value);
                     }
                     return text;
                 }});
}
} // namespace masonry
