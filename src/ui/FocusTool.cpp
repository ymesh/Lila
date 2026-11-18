#include "ui/FocusTool.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/SurfaceSnap.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"

#include <QMouseEvent>

namespace lila::ui
{
namespace
{
/**
 * Pick Focus: clicking a surface sets the looked-through camera's focus distance to it (along the view, as the lens
 * measures it) and turns its depth of field on. One undo step per click; Esc or another tool ends it.
 */
class FocusTool : public ViewportTool
{
public:
    const char* name() const override { return "Pick Focus"; }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || e->modifiers() & (Qt::AltModifier | Qt::ControlModifier))
            return false;
        scene::Node* cam = scene::Graph::current().find(view.camera);
        if (!cam || !cam->type().isA("camera"))
        {
            log::warn("Pick Focus: look through a camera to set its focus");
            return true;
        }
        const std::optional<surfaceSnap::Hit> hit = surfaceSnap::underCursor(e->position(), view);
        if (!hit)
        {
            log::warn("Pick Focus: no surface under the cursor");
            return true;
        }
        const glm::dvec3 forward = -glm::dvec3(glm::inverse(view.view)[2]);
        const double distance = std::max(glm::dot(hit->position - view.eye, glm::normalize(forward)), 0.1);
        try
        {
            ops::execute("setAttr", {{"args", {cam->name() + ".focusDistance", distance}}}, CallSource::UI);
            if (!cam->as<bool>(scene::Camera::kDepthOfField))
                ops::execute("setAttr", {{"args", {cam->name() + ".depthOfField", true}}}, CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::error("{}", ex.what());
        }
        return true;
    }
};
} // namespace

void registerFocusTool()
{
    if (!viewportTools::registry().find("camera.pickFocus"))
        viewportTools::add({.id = "camera.pickFocus", .label = "Pick Focus", .icon = "focus-2", .create = [] { return std::make_unique<FocusTool>(); }});
    actions::add({.id = "camera.pickFocus",
                  .label = "Pick Focus",
                  .category = "View",
                  .run = [] { viewportTools::activate("camera.pickFocus"); },
                  .doc = "Click a surface to focus the camera you look through on it (and turn its depth of field on).",
                  .python = "lila.ui.run_action(\"camera.pickFocus\")",
                  .icon = "focus-2",
                  .keywords = {"focus", "dof", "depth of field", "focus pull", "autofocus", "bokeh"}});
}
} // namespace lila::ui
