#pragma once

#include "core/Export.h"
#include "pointcloud/Octree.h"
#include "scene/Transform.h"

#include <memory>
#include <string>

namespace lila::pointcloud
{
/**
 * pointCloud: a scan streamed from its .lilapc cache, drawn by level of detail under a point budget. Its points are in the
 * node's space (placed by the import's georeference), so it moves like any transform.
 */
class LILA_SCENE_API PointCloud : public scene::Transform
{
public:
    enum Attr
    {
        kCacheFile = scene::Transform::kCount,
        kSourceFile,
        kPointSize,
        kColorMode,
        kColor,
        kBudget,
        kDetail,
        kClipBoxes,
        kClipMode,
        kEyeDomeLighting,
        kCount
    };
    enum ColorMode
    {
        Rgb,
        Intensity,
        Elevation,
        Classification,
        Single,
    };
    enum ClipMode
    {
        ClipOff,
        ClipInside, // keep what is inside any box
        ClipOutside,
    };
};

/** pointCloudClip: a box (the unit cube, -0.5 to 0.5, under its transform) a pointCloud keeps or cuts away. */
class LILA_SCENE_API PointCloudClip : public scene::Transform
{
public:
    enum Attr
    {
        kEnabled = scene::Transform::kCount,
        kCount
    };
};

/** The open cache of a pointCloud node (shared, opened once per file); null when it can't be read. */
LILA_SCENE_API std::shared_ptr<const PointCloudFile> cacheOf(const scene::Node& n);

/** The clip boxes a pointCloud names, as world-to-box matrices (inside is |x|, |y|, |z| <= 0.5); disabled ones left out. */
LILA_SCENE_API std::vector<glm::dmat4> clipBoxes(scene::Graph& g, const scene::Node& cloud);

LILA_SCENE_API void registerTypes();
/** importPointCloud, pointCloudClipBox, pointCloudExtract, exportPointCloud, pointCloudInfo. */
LILA_SCENE_API void registerCommands();
} // namespace lila::pointcloud
