#include "render/CameraRelative.h"

#include <glm/gtc/matrix_transform.hpp>

namespace lila::render
{
void rebaseOnEye(FrameState& s)
{
    const glm::dvec3 eye = s.eye;
    if (eye == glm::dvec3(0.0))
        return;
    s.renderOrigin += eye;
    s.eye = glm::dvec3(0.0);
    s.view = s.view * glm::translate(glm::dmat4(1.0), eye);
    for (LightDraw& l : s.lights)
    {
        l.position -= eye;
        l.shadowViewProj = l.shadowViewProj * glm::translate(glm::dmat4(1.0), eye);
    }
    for (ImagePlaneDraw& p : s.imagePlanes)
        if (!p.screen)
            for (glm::dvec3& c : p.corners)
                c -= eye;
    s.gridOrigin -= eye;
    s.fog.height -= float(s.zUp ? eye.z : eye.y);
}
} // namespace lila::render
