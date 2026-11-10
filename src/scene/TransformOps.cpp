#include "scene/TransformOps.h"

#include "core/Profile.h"
#include "scene/Graph.h"

#include <glm/gtc/matrix_transform.hpp>

#include <unordered_set>

namespace lila::scene::xformops
{
namespace
{
glm::dvec3 v3(Node* n, int attr) { return n->as<glm::dvec3>(attr); }

glm::dmat3 orthonormal(const glm::dmat3& m)
{
    glm::dvec3 x = glm::normalize(m[0]);
    glm::dvec3 y = m[1] - x * glm::dot(x, m[1]);
    y = glm::length(y) > 1e-12 ? glm::normalize(y) : glm::dvec3(0, 1, 0);
    glm::dvec3 z = glm::cross(x, y);
    // Mirrored frames (negative scale) keep their handedness out of the rotation.
    if (glm::dot(z, m[2]) < 0.0)
        z = -z;
    return {x, y, z};
}

double wrapToward(double a, double hint)
{
    return a + 360.0 * std::round((hint - a) / 360.0);
}

glm::dmat3 rotationOf(Node* n)
{
    return glm::dmat3(eulerToMatrix(v3(n, Transform::kRotate), RotateOrder(n->as<int64_t>(Transform::kRotateOrder))));
}
} // namespace

std::vector<NodeId> topLevel(Graph& g, const std::vector<NodeId>& ids)
{
    std::unordered_set<NodeId> set(ids.begin(), ids.end());
    std::vector<NodeId> out;
    for (NodeId id : ids)
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("transform"))
            continue;
        bool covered = false;
        for (NodeId p = g.parent(id); p != kNoNode && !covered; p = g.parent(p))
            covered = set.contains(p);
        if (!covered)
            out.push_back(id);
    }
    return out;
}

glm::dvec3 worldPivot(Graph& g, NodeId id)
{
    g.evaluate();
    Node* n = g.node(id);
    glm::dvec3 local = v3(n, Transform::kTranslate) + v3(n, Transform::kRotatePivotTranslate) + v3(n, Transform::kRotatePivot);
    return glm::dvec3(n->as<glm::dmat4>(Transform::kParentMatrix) * glm::dvec4(local, 1.0));
}

glm::dmat3 frame(Graph& g, NodeId id, Space space)
{
    if (space == Space::World)
        return glm::dmat3(1.0);
    g.evaluate();
    Node* n = g.node(id);
    if (space == Space::Parent)
        return orthonormal(glm::dmat3(n->as<glm::dmat4>(Transform::kParentMatrix)));
    return orthonormal(glm::dmat3(n->as<glm::dmat4>(Transform::kWorldMatrix)));
}

void moveBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& delta, Space space)
{
    LILA_ZONE();

    g.evaluate();
    for (NodeId id : topLevel(g, ids))
    {
        Node* n = g.node(id);
        glm::dvec3 world = frame(g, id, space) * delta;
        glm::dmat4 parentInv = glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix));
        glm::dvec3 local = glm::dvec3(parentInv * glm::dvec4(world, 0.0));
        g.setValue({id, Transform::kTranslate}, g.keepLocked({id, Transform::kTranslate}, v3(n, Transform::kTranslate) + local));
    }
}

glm::dvec3 rotatedEuler(Graph& g, NodeId id, const glm::dquat& rotation, Space space)
{
    g.evaluate();
    Node* n = g.node(id);
    glm::dmat3 r = glm::mat3_cast(rotation);
    glm::dmat3 parent = orthonormal(glm::dmat3(n->as<glm::dmat4>(Transform::kParentMatrix)));
    glm::dmat3 current = rotationOf(n);
    glm::dmat3 next;
    if (space == Space::Object)
    {
        // The object's own axes include rotateAxis, which sits after R.
        glm::dmat3 ra(static_cast<Transform*>(n)->rotateAxisMatrix());
        next = current * ra * r * glm::transpose(ra);
    }
    else
    {
        // Changing R alone turns the object about its rotate pivot, which is exactly what we want.
        glm::dmat3 worldR = space == Space::Parent ? parent * r * glm::transpose(parent) : r;
        // Joints rotate inside their jointOrient, so that frame sits between the parent and R.
        glm::dmat3 f = parent * glm::dmat3(static_cast<Transform*>(n)->preRotation());
        next = glm::transpose(f) * worldR * f * current;
    }
    auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
    return closestEuler(next, order, v3(n, Transform::kRotate));
}

glm::dmat3 gimbalFrame(Graph& g, NodeId id)
{
    static constexpr int kAxes[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
    g.evaluate();
    Node* n = g.node(id);
    const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
    const int* axes = kAxes[int(order)];
    const glm::dvec3 r = v3(n, Transform::kRotate);
    glm::dvec3 outer(0.0);
    outer[axes[2]] = r[axes[2]];
    glm::dvec3 outerTwo = r;
    outerTwo[axes[0]] = 0.0;
    const glm::dmat3 f =
        orthonormal(glm::dmat3(n->as<glm::dmat4>(Transform::kParentMatrix))) * glm::dmat3(static_cast<Transform*>(n)->preRotation());
    glm::dmat3 out;
    out[axes[2]] = f[axes[2]];
    out[axes[1]] = f * glm::dmat3(eulerToMatrix(outer, order))[axes[1]];
    out[axes[0]] = f * glm::dmat3(eulerToMatrix(outerTwo, order))[axes[0]];
    return out;
}

void rotateChannelsBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& degrees)
{
    LILA_ZONE();

    for (NodeId id : topLevel(g, ids))
        g.setValue({id, Transform::kRotate}, g.keepLocked({id, Transform::kRotate}, v3(g.node(id), Transform::kRotate) + degrees));
}

void rotateBy(Graph& g, const std::vector<NodeId>& ids, const glm::dquat& rotation, Space space)
{
    LILA_ZONE();

    g.evaluate();
    for (NodeId id : topLevel(g, ids))
        g.setValue({id, Transform::kRotate}, g.keepLocked({id, Transform::kRotate}, rotatedEuler(g, id, rotation, space)));
}

void scaleBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& factor)
{
    LILA_ZONE();

    for (NodeId id : topLevel(g, ids))
    {
        Node* n = g.node(id);
        g.setValue({id, Transform::kScale}, g.keepLocked({id, Transform::kScale}, v3(n, Transform::kScale) * factor));
    }
}

void setWorldPivot(Graph& g, NodeId id, const glm::dvec3& world)
{
    LILA_ZONE();

    g.evaluate();
    Node* n = g.node(id);
    glm::dvec3 t = v3(n, Transform::kTranslate);
    glm::dvec3 rpt = v3(n, Transform::kRotatePivotTranslate);
    glm::dvec3 rp = v3(n, Transform::kRotatePivot);
    glm::dvec3 sp = v3(n, Transform::kScalePivot);
    glm::dvec3 spt = v3(n, Transform::kScalePivotTranslate);
    glm::dvec3 s = v3(n, Transform::kScale);
    const auto* xf = static_cast<const Transform*>(n);
    glm::dmat3 r = glm::dmat3(xf->preRotation()) * rotationOf(n) * glm::dmat3(xf->rotateAxisMatrix());

    glm::dvec3 target = glm::dvec3(glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix)) * glm::dvec4(world, 1.0));
    // Pivot sits at T + Rpt + Rp in parent space; solve for Rp with Rpt compensating so the matrix is unchanged.
    glm::dvec3 newRp = glm::transpose(r) * (target - t - rpt - rp + r * rp);
    glm::dvec3 newRpt = rpt + (rp - newRp) - r * (rp - newRp);

    // The scale pivot is the pre-scale point that scales onto the new rotate pivot, so scaling holds the pivot still.
    glm::dvec3 newSp = sp;
    for (int i = 0; i < 3; ++i)
        if (std::abs(s[i]) > 1e-12)
            newSp[i] = sp[i] + (newRp[i] - sp[i] - spt[i]) / s[i];
    glm::dvec3 newSpt = spt + (sp - newSp) - s * (sp - newSp);

    g.setValue({id, Transform::kRotatePivot}, newRp);
    g.setValue({id, Transform::kRotatePivotTranslate}, newRpt);
    g.setValue({id, Transform::kScalePivot}, newSp);
    g.setValue({id, Transform::kScalePivotTranslate}, newSpt);
}

glm::dmat3 pivotFrame(Graph& g, NodeId id)
{
    glm::dmat3 object = frame(g, id, Space::Object);
    return object * glm::dmat3(eulerToMatrix(v3(g.node(id), Transform::kPivotOrient), RotateOrder::XYZ));
}

void setWorldPivotFrame(Graph& g, NodeId id, const glm::dmat3& world)
{
    LILA_ZONE();

    glm::dmat3 object = frame(g, id, Space::Object);
    Node* n = g.node(id);
    glm::dmat3 local = glm::transpose(object) * world;
    g.setValue({id, Transform::kPivotOrient}, closestEuler(local, RotateOrder::XYZ, v3(n, Transform::kPivotOrient)));
}

void resetPivot(Graph& g, NodeId id)
{
    LILA_ZONE();

    g.evaluate();
    Node* n = g.node(id);
    glm::dmat4 local = n->as<glm::dmat4>(Transform::kMatrix);
    g.setValue({id, Transform::kPivotOrient}, glm::dvec3(0.0));
    for (int a : {Transform::kRotatePivot, Transform::kRotatePivotTranslate, Transform::kScalePivot, Transform::kScalePivotTranslate})
        g.setValue({id, a}, glm::dvec3(0.0));
    g.evaluate();
    // With pivots gone the old matrix is plain TRS again, so translation is all that needs restoring.
    glm::dmat4 now = Transform::localMatrix(*n);
    g.setValue({id, Transform::kTranslate}, v3(n, Transform::kTranslate) + glm::dvec3(local[3]) - glm::dvec3(now[3]));
}

glm::dvec3 closestEuler(const glm::dmat3& rotation, RotateOrder order, const glm::dvec3& hint)
{
    glm::dvec3 a = matrixToEuler(rotation, order);
    // Every rotation has a second Euler solution: first and last axes +180, middle mirrored about 180.
    static constexpr int kAxes[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
    const int* ax = kAxes[int(order)];
    glm::dvec3 b = a;
    b[ax[0]] += 180.0;
    b[ax[1]] = 180.0 - b[ax[1]];
    b[ax[2]] += 180.0;

    auto fit = [&](glm::dvec3 e) {
        for (int i = 0; i < 3; ++i)
            e[i] = wrapToward(e[i], hint[i]);
        return e;
    };
    a = fit(a);
    b = fit(b);
    glm::dmat3 check(eulerToMatrix(b, order));
    double err = 0.0;
    for (int c = 0; c < 3; ++c)
        err += glm::length(check[c] - rotation[c]);
    if (err > 1e-6)
        return a;
    auto dist = [&](const glm::dvec3& e) { return glm::length(e - hint); };
    return dist(b) < dist(a) ? b : a;
}
} // namespace lila::scene::xformops
