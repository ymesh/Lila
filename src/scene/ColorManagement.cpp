#include "scene/ColorManagement.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/SceneIndex.h"

#include <cstdlib>

namespace lila::scene::colorManagement
{
namespace
{
constexpr uint32_t kStored = AttrInput | AttrStorable;

std::vector<AttrDef> attrs()
{
    std::vector<AttrDef> a(kCount);
    a[kMode] = {.name = "mode", .shortName = "md", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                .enumNames = {"lila", "ocio"},
                .doc = "Lila uses the post process's tonemapper; OCIO an OpenColorIO config's display and view instead."};
    a[kConfig] = {.name = "config", .shortName = "cfg", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                  .enumNames = {"builtin", "file", "environment"},
                  .doc = "OCIO only: the built-in studio config, a config file, or the one $OCIO names."};
    a[kConfigPath] = {.name = "configPath", .shortName = "cfp", .type = PlugType::String, .defaultValue = std::string(),
                      .flags = kStored | AttrFile, .doc = "OCIO only: the .ocio config file, when config is file."};
    a[kDisplay] = {.name = "display", .shortName = "dsp", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored,
                   .doc = "OCIO only: the display the picture is for; empty for the config's default."};
    a[kView] = {.name = "view", .shortName = "vw", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored,
                .doc = "OCIO only: the view transform; empty for the display's default."};
    a[kLook] = {.name = "look", .shortName = "lk", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored,
                .doc = "OCIO only: a look applied before the view; empty for none."};
    a[kExrOutput] = {.name = "exrOutput", .shortName = "exo", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                     .enumNames = {"sceneLinear", "display"},
                     .doc = "EXR renders: scene-linear light for compositing, or the picture as the view shows it."};
    return a;
}
} // namespace

int firstAttr()
{
    const NodeType* t = types::find("colorManagement");
    return t ? int(t->attrs.size()) - kCount : 0;
}

NodeId find(const Graph& g)
{
    for (const Node* n : sceneIndex::ofType(g, "colorManagement"))
        return n->id();
    return kNoNode;
}

Settings settings(const Graph& g, const std::string& studioConfig)
{
    Settings s;
    const NodeId id = find(g);
    const Node* n = id != kNoNode ? g.node(id) : nullptr;
    if (n)
    {
        const int a = firstAttr();
        s.ocio = n->as<int64_t>(a + kMode) == 1;
        const int64_t config = n->as<int64_t>(a + kConfig);
        if (config == 1)
            s.config = n->as<std::string>(a + kConfigPath);
        else if (config == 2)
            if (const char* env = std::getenv("OCIO"))
                s.config = env;
        s.display = n->as<std::string>(a + kDisplay);
        s.view = n->as<std::string>(a + kView);
        s.look = n->as<std::string>(a + kLook);
        s.exrDisplay = n->as<int64_t>(a + kExrOutput) == 1;
    }
    if (!studioConfig.empty())
    {
        s.config = studioConfig;
        s.forced = true;
    }
    return s;
}

void registerTypes()
{
    if (types::find("colorManagement"))
        return;
    NodeType t;
    t.name = "colorManagement";
    t.icon = "palette";
    t.doc = "Colour management: Lila's tonemapper, or an OpenColorIO config's display, view and look. One per scene.";
    t.ownAttrs = attrs();
    t.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    if (ops::find("colorManagement"))
        return;
    ops::registerCommand({
        .name = "colorManagement",
        .doc = "The scene's colour management node, created if it has none. With flags, sets them on it. Returns its name.",
        .flags = {
            {"mode", "md", T::String, nullptr, "lila or ocio."},
            {"configPath", "cfp", T::String, nullptr, "An .ocio file; sets config to file. Empty goes back to the built-in config."},
            {"display", "dsp", T::String, nullptr, "OCIO display; empty for the config's default."},
            {"view", "vw", T::String, nullptr, "OCIO view; empty for the display's default."},
            {"look", "lk", T::String, nullptr, "OCIO look; empty for none."},
        },
        .run = [](const Args& args) -> json {
            Graph& g = Graph::current();
            NodeId id = find(g);
            std::string name = id != kNoNode ? g.node(id)->name()
                                             : ops::execute("createNode", {{"args", {"colorManagement"}}, {"name", "colorManagement"}}).get<std::string>();
            auto set = [&](const char* attr, json value) { ops::execute("setAttr", {{"args", {name + "." + attr, value}}}); };
            if (args.has("mode"))
                set("mode", args.get<std::string>("mode") == "ocio" ? 1 : 0);
            if (args.has("configPath"))
            {
                const std::string path = args.get<std::string>("configPath");
                set("configPath", path);
                set("config", path.empty() ? 0 : 1);
            }
            for (const char* flag : {"display", "view", "look"})
                if (args.has(flag))
                    set(flag, args.get<std::string>(flag));
            return name;
        },
    });
}
} // namespace lila::scene::colorManagement
