#include <doctest/doctest.h>

#include "LargeWorld.h"
#include "ui/Camera.h"
#include "ui/GeometrySnap.h"
#include "ui/GizmoMath.h"
#include "ui/ViewInfo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <optional>

using namespace lila;
using namespace largeworld;

namespace
{
ui::ViewInfo viewAt(const glm::dvec3& offset)
{
    ui::Camera cam;
    cam.target = offset + glm::dvec3(0.0, 20.0, 0.0);
    cam.distance = 140.0;
    cam.yaw = 35.0;
    cam.pitch = -22.0;
    ui::ViewInfo v;
    v.view = cam.view();
    v.proj = cam.proj(16.0 / 9.0);
    v.eye = cam.eye();
    v.width = 1600;
    v.height = 900;
    return v;
}
} // namespace

TEST_CASE("vertex and edge snapping pick the same targets far from the origin")
{
    std::optional<glm::dvec3> homePoint, homeEdge;
    for (const glm::dvec3& offset : kOffsets)
    {
        CAPTURE(offset.x);
        const ui::ViewInfo view = viewAt(offset);
        const std::optional<QPointF> corner = view.project(offset + glm::dvec3(10.0, 20.0, 10.0));
        REQUIRE(corner);
        const QPointF cursor = *corner + QPointF(6.0, 5.0);
        ui::geometrySnap::Nearest points(cursor, view, 40.0);
        for (int c = 0; c < 8; ++c)
            points.point(offset + glm::dvec3((c & 1) ? 10.0 : -10.0, (c & 2) ? 20.0 : 0.0, (c & 4) ? 10.0 : -10.0));
        ui::geometrySnap::Nearest edges(cursor, view, 40.0);
        edges.segment(offset + glm::dvec3(-10.0, 20.0, 10.0), offset + glm::dvec3(10.0, 20.0, 10.0));
        edges.segment(offset + glm::dvec3(-10.0, 0.0, 10.0), offset + glm::dvec3(-10.0, 20.0, 10.0));
        REQUIRE(points.hit());
        REQUIRE(edges.hit());
        const glm::dvec3 p = *points.hit() - offset, e = *edges.hit() - offset;
        if (!homePoint)
        {
            homePoint = p;
            homeEdge = e;
            continue;
        }
        CHECK(glm::length(p - *homePoint) < 1e-6);
        CHECK(glm::length(e - *homeEdge) < 1e-4);
    }
}

TEST_CASE("gizmo drags move by the same amount far from the origin")
{
    // A move along X and in the ground plane, as the move gizmo works them out from two cursor positions.
    std::optional<double> homeAxis;
    std::optional<glm::dvec3> homePlane;
    for (const glm::dvec3& offset : kOffsets)
    {
        CAPTURE(offset.x);
        const ui::ViewInfo view = viewAt(offset);
        const glm::dvec3 pivot = offset + glm::dvec3(0.0, 10.0, 0.0);
        glm::dvec3 o0, d0, o1, d1;
        view.ray(QPointF(800.0, 450.0), o0, d0);
        view.ray(QPointF(912.5, 431.25), o1, d1);
        const glm::dvec3 x(1.0, 0.0, 0.0);
        const double along = ui::gizmo::lineParam(pivot, x, o1, d1) - ui::gizmo::lineParam(pivot, x, o0, d0);
        glm::dvec3 h0, h1;
        REQUIRE(ui::gizmo::rayPlane(o0, d0, pivot, glm::dvec3(0, 1, 0), h0));
        REQUIRE(ui::gizmo::rayPlane(o1, d1, pivot, glm::dvec3(0, 1, 0), h1));
        if (!homeAxis)
        {
            homeAxis = along;
            homePlane = h1 - h0;
            continue;
        }
        CHECK(along == doctest::Approx(*homeAxis).epsilon(1e-7));
        CHECK(glm::length((h1 - h0) - *homePlane) < 1e-5);
    }
}

TEST_CASE("the camera reaches continental distances")
{
    ui::Camera cam;
    cam.target = kOffsets[1];
    for (int i = 0; i < 200; ++i)
        cam.dolly(2.0);
    CHECK(cam.distance == doctest::Approx(ui::kMaxCameraDistance));
    CHECK(cam.distance >= 1e9);
    cam.frame(kOffsets[2], 5e7, 1.0);
    CHECK(cam.distance > 5e7);
    // The eye stays where the target and distance say, to well under a millimetre.
    CHECK(glm::length(cam.eye() + cam.forward() * cam.distance - cam.target) < 1e-3);
    cam.ortho = true;
    cam.dolly(1e12);
    CHECK(cam.orthoWidth == doctest::Approx(ui::kMaxCameraDistance));
}
