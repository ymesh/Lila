#pragma once

#include "core/Export.h"
#include "scene/Node.h"

#include <string>

namespace lila::scene
{
class Graph;

/**
 * Colour management: how scene-linear light becomes display colour. Lila mode uses the post process's tonemapper;
 * OCIO mode an OpenColorIO config's display, view and look instead. One node per scene, saved with it; without one the
 * scene is in Lila mode. Attribute order matches the Attr enum.
 */
namespace colorManagement
{
enum Attr
{
    kMode,       // lila, ocio
    kConfig,     // builtin, file, environment ($OCIO)
    kConfigPath, // the .ocio file when config is file
    kDisplay,    // empty: the config's default
    kView,       // empty: the display's default
    kLook,       // empty: none
    kExrOutput,  // sceneLinear, display
    kCount
};

/** The node's own attribute indices start after the base node's; this is the first. */
LILA_SCENE_API int firstAttr();

/** The scene's colour management node, or kNoNode. */
LILA_SCENE_API NodeId find(const Graph& g);

/** A pref (studio folders can set and lock it) that forces an OCIO config path for every scene, whatever the scene says. */
inline constexpr const char* kConfigPref = "/colorManagement/ocioConfig";

/** The settings a frame or render uses, resolved from the scene's node, $OCIO and the studio pref. */
struct Settings
{
    bool ocio = false;
    std::string config; // "" for the built-in config, else a path or ocio:// URI
    std::string display, view, look;
    bool exrDisplay = false; // EXR renders written through the view instead of scene-linear
    bool forced = false;     // the studio pref set the config
};
LILA_SCENE_API Settings settings(const Graph& g, const std::string& studioConfig = {});

void registerTypes();
void registerCommands();
} // namespace colorManagement
} // namespace lila::scene
