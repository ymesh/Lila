#include "ui/ViewInfo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace lila::ui
{
std::optional<QPointF> ViewInfo::project(const glm::dvec3& world) const
{
    glm::dvec4 clip = viewProj() * glm::dvec4(world, 1.0);
    if (clip.w <= 1e-9)
        return std::nullopt;
    return QPointF((clip.x / clip.w * 0.5 + 0.5) * width, (clip.y / clip.w * 0.5 + 0.5) * height);
}

void ViewInfo::ray(QPointF px, glm::dvec3& origin, glm::dvec3& dir) const
{
    // Two depths along the pixel; works for perspective and orthographic alike. Reversed-Z: 1 is the near plane.
    // Unprojected relative to the eye: the two points are a fraction of a cm apart, which far from the origin would
    // leave the direction to rounding.
    const glm::dmat4 inv = glm::inverse(proj * view * glm::translate(glm::dmat4(1.0), eye));
    double x = px.x() / width * 2.0 - 1.0, y = px.y() / height * 2.0 - 1.0;
    glm::dvec4 a = inv * glm::dvec4(x, y, 1.0, 1.0), b = inv * glm::dvec4(x, y, 0.5, 1.0);
    glm::dvec3 nearPoint = glm::dvec3(a) / a.w, midPoint = glm::dvec3(b) / b.w;
    origin = eye + nearPoint;
    dir = glm::normalize(midPoint - nearPoint);
}

double ViewInfo::unitsPerPixel(const glm::dvec3& at) const
{
    glm::dvec3 right(view[0][0], view[1][0], view[2][0]);
    auto a = project(at), b = project(at + right);
    if (!a || !b)
        return 1.0;
    double d = std::hypot(a->x() - b->x(), a->y() - b->y());
    return d > 1e-9 ? 1.0 / d : 1.0;
}
} // namespace lila::ui
