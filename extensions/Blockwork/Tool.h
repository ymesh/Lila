#pragma once

#include "Blockwork.h"

#include <string>

namespace blockwork
{
/** The viewport tool's id and its toolkit panel's. */
inline constexpr const char* kToolId = "blockwork.tool";
inline constexpr const char* kToolkitId = "blockworkToolkit";
inline constexpr const char* kTexturePanelId = "blockwork.textures";
inline constexpr const char* kVisgroupPanelId = "blockwork.visgroups";
inline constexpr const char* kPrefabPanelId = "blockwork.prefabs";

enum class Mode
{
    Block,
    Carve,
    Clip,
    Extend,
    Paint,
    Align,
    Entity,
    Prefab,
};

/** What the tool draws and builds, kept in prefs between sessions. */
struct Settings
{
    Mode mode = Mode::Block;
    Shape shape = Shape::Box;
    ShapeOptions options;
    std::string suffix;
    bool flip = false;
    bool removeOverlap = false;
    /** The material Paint puts on faces (picked in Block Textures or from a face). */
    std::string material;
    /** The class the Entity tool places. */
    std::string entityClass = "PlayerStart";
    /** The prefab file the Prefab tool places. */
    std::string prefab;
    /** Placed prefabs turn their up to the surface they land on. */
    bool prefabFollowsNormal = true;
    /** The view flies with the Drone Camera while the tool is on. */
    bool droneCamera = false;
};

Settings& settings();
void saveSettings();
/** Starts the tool in this mode (and shape, for Block and Carve). */
void startTool(Mode mode);
void startTool(Shape shape);
/** Redraws the toolkit after settings change elsewhere. */
void syncToolkit();
/** Flies the active view while the tool is on with Drone camera picked, and lands a flight it started once either stops. */
void syncCamera();
const char* modeLabel(Mode m);
const char* shapeLabel(Shape s);

void registerTool(lila::ext::Context& ctx);
void registerToolkit(lila::ext::Context& ctx);
void registerTexturePanel(lila::ext::Context& ctx);
void registerOrganise(lila::ext::Context& ctx);
void registerOrganisePanels(lila::ext::Context& ctx);
/** Shows and hides visgroup members to match their groups. */
void visgroupsChanged();
} // namespace blockwork
