// The rig model: what the node types hold, read into BodyDefs and friends; shape geometry for drawing and picking;
// fitting shapes to the skinned mesh and limits to the animation; semantic labels and their defaults.

#include "PhysicsAnimation.h"
#include "World.h"

#include <lila/Graph.h>
#include <lila/Grapple.h>
#include <lila/Rig.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

using namespace lila;
using namespace lila::scene;

namespace physanim
{
namespace
{
constexpr double kDeg = kPi / 180.0;

glm::dquat quatOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
        r[i] /= std::max(glm::length(r[i]), 1e-300);
    return glm::normalize(glm::quat_cast(r));
}

glm::dquat eulerQuat(const glm::dvec3& degrees) { return quatOf(eulerToMatrix(degrees, RotateOrder::XYZ)); }

glm::dvec3 quatEuler(const glm::dquat& q) { return matrixToEuler(glm::mat3_cast(q), RotateOrder::XYZ); }

bool isJointNode(const Node& n) { return n.type().isA("joint"); }

const Transform* transformOf(const Node* n) { return n && n->type().isA("transform") ? static_cast<const Transform*>(n) : nullptr; }

/** The node's own stored, undriven, non-keyable values, for the parameters hash; keyable ones are sampled per step. */
uint64_t hashOwn(Graph& g, const Node& n, uint64_t h)
{
    h = sim::hashBytes(n.name().data(), n.name().size(), h);
    for (const AttrDef& a : n.type().ownAttrs)
    {
        const int i = n.type().attrIndex(a.name);
        if (i < 0 || !(a.flags & AttrStorable) || (a.flags & AttrOutput) || (a.flags & AttrKeyable) || a.name == sim::kRecordingsAttr)
            continue;
        if (g.source({n.id(), i, -1}))
            continue;
        const std::string v = toJson(n.value(i)).dump();
        h = sim::hashBytes(v.data(), v.size(), h);
    }
    return h;
}

template <class T> uint64_t hashPod(const T& v, uint64_t h) { return sim::hashBytes(&v, sizeof v, h); }

int depthOf(Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId c = g.parent(id); c; c = g.parent(c))
        ++d;
    return d;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string bareName(const std::string& n)
{
    const size_t colon = n.rfind(':');
    return colon == std::string::npos ? n : n.substr(colon + 1);
}

// Geometry ------------------------------------------------------------------------------------------------------------

void addQuadStrip(std::vector<glm::dvec3>& tris, const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    const size_t n = a.size();
    for (size_t k = 0; k < n; ++k)
    {
        const size_t k1 = (k + 1) % n;
        tris.insert(tris.end(), {a[k], b[k], b[k1], a[k], b[k1], a[k1]});
    }
}

void addRing(std::vector<glm::dvec3>& lines, const std::vector<glm::dvec3>& ring)
{
    for (size_t k = 0; k < ring.size(); ++k)
        lines.insert(lines.end(), {ring[k], ring[(k + 1) % ring.size()]});
}

/** A capsule along X centred on the origin; length 0 is a sphere. */
void capsuleGeometry(ShapeGeometry& out, double r, double length)
{
    constexpr int kSeg = 16, kCap = 4;
    const double half = length * 0.5;
    std::vector<std::vector<glm::dvec3>> rings;
    auto ring = [&](double x, double radius) {
        std::vector<glm::dvec3> p;
        for (int k = 0; k < kSeg; ++k)
        {
            const double a = 2.0 * kPi * k / kSeg;
            p.push_back({x, std::cos(a) * radius, std::sin(a) * radius});
        }
        return p;
    };
    for (int i = kCap; i >= 1; --i)
    {
        const double a = 0.5 * kPi * i / kCap;
        rings.push_back(ring(-half - std::sin(a) * r, std::cos(a) * r));
    }
    rings.push_back(ring(-half, r));
    rings.push_back(ring(half, r));
    for (int i = 1; i <= kCap; ++i)
    {
        const double a = 0.5 * kPi * i / kCap;
        rings.push_back(ring(half + std::sin(a) * r, std::cos(a) * r));
    }
    for (size_t i = 0; i + 1 < rings.size(); ++i)
        addQuadStrip(out.triangles, rings[i], rings[i + 1]);
    const glm::dvec3 tipA(-half - r, 0, 0), tipB(half + r, 0, 0);
    for (int k = 0; k < kSeg; ++k)
    {
        const int k1 = (k + 1) % kSeg;
        out.triangles.insert(out.triangles.end(), {tipA, rings.front()[size_t(k1)], rings.front()[size_t(k)]});
        out.triangles.insert(out.triangles.end(), {tipB, rings.back()[size_t(k)], rings.back()[size_t(k1)]});
    }
    // Outline: the two seam rings and four profile lines with their caps.
    addRing(out.lines, rings[kCap]);
    addRing(out.lines, rings[kCap + 1]);
    for (int q = 0; q < 4; ++q)
    {
        const double a = 0.5 * kPi * q;
        const glm::dvec3 d(0, std::cos(a), std::sin(a));
        out.lines.insert(out.lines.end(), {glm::dvec3(-half, 0, 0) + d * r, glm::dvec3(half, 0, 0) + d * r});
        glm::dvec3 prevA = glm::dvec3(-half, 0, 0) + d * r, prevB = glm::dvec3(half, 0, 0) + d * r;
        for (int i = 1; i <= kCap * 2; ++i)
        {
            const double t = 0.5 * kPi * i / (kCap * 2);
            const glm::dvec3 pa = glm::dvec3(-half - std::sin(t) * r, 0, 0) + d * (std::cos(t) * r);
            const glm::dvec3 pb = glm::dvec3(half + std::sin(t) * r, 0, 0) + d * (std::cos(t) * r);
            out.lines.insert(out.lines.end(), {prevA, pa, prevB, pb});
            prevA = pa, prevB = pb;
        }
    }
}

void boxGeometry(ShapeGeometry& out, const glm::dvec3& size)
{
    const glm::dvec3 h = size * 0.5;
    glm::dvec3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = {(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
    const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces)
        out.triangles.insert(out.triangles.end(), {c[f[0]], c[f[1]], c[f[2]], c[f[0]], c[f[2]], c[f[3]]});
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges)
        out.lines.insert(out.lines.end(), {c[e[0]], c[e[1]]});
}

void hullGeometry(ShapeGeometry& out, const std::vector<glm::dvec3>& points)
{
    out.triangles = hullTriangles(points);
    // Outline only where neighbouring faces fold, so the fan diagonals of flat faces stay hidden.
    std::map<std::pair<std::tuple<double, double, double>, std::tuple<double, double, double>>, std::vector<glm::dvec3>> edges;
    auto key = [](const glm::dvec3& p) { return std::make_tuple(p.x, p.y, p.z); };
    for (size_t t = 0; t + 2 < out.triangles.size(); t += 3)
    {
        const glm::dvec3* v = &out.triangles[t];
        const glm::dvec3 n = glm::normalize(glm::cross(v[1] - v[0], v[2] - v[0]));
        for (int e = 0; e < 3; ++e)
        {
            auto a = key(v[e]), b = key(v[(e + 1) % 3]);
            edges[a < b ? std::make_pair(a, b) : std::make_pair(b, a)].push_back(n);
        }
    }
    for (const auto& [e, normals] : edges)
        if (normals.size() != 2 || glm::dot(normals[0], normals[1]) < 0.999)
            out.lines.insert(out.lines.end(), {glm::dvec3(std::get<0>(e.first), std::get<1>(e.first), std::get<2>(e.first)),
                                               glm::dvec3(std::get<0>(e.second), std::get<1>(e.second), std::get<2>(e.second))});
}

uint64_t shapeHash(const ShapeDesc& s)
{
    uint64_t h = hashPod(s.kind, 1469598103934665603ull);
    for (double v : {s.radius, s.length, s.extents.x, s.extents.y, s.extents.z, s.offset.x, s.offset.y, s.offset.z, s.rotation.w, s.rotation.x,
                     s.rotation.y, s.rotation.z})
        h = hashPod(v, h);
    return sim::hashBytes(s.hull.data(), s.hull.size() * sizeof(glm::dvec3), h);
}

std::optional<double> rayTriangle(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
    const double det = glm::dot(e1, p);
    if (std::abs(det) < 1e-14)
        return std::nullopt;
    const double inv = 1.0 / det;
    const glm::dvec3 s = o - a;
    const double u = glm::dot(s, p) * inv;
    if (u < 0.0 || u > 1.0)
        return std::nullopt;
    const glm::dvec3 q = glm::cross(s, e1);
    const double v = glm::dot(d, q) * inv;
    if (v < 0.0 || u + v > 1.0)
        return std::nullopt;
    const double t = glm::dot(e2, q) * inv;
    return t > 0.0 ? std::optional(t) : std::nullopt;
}

// Labels --------------------------------------------------------------------------------------------------------------

struct AliasRow
{
    std::string label;
    std::vector<std::string> names;
};

const std::vector<AliasRow>& aliasTable()
{
    static const std::vector<AliasRow> table = [] {
        std::vector<AliasRow> t = {
            {"pelvis", {"hips", "pelvis", "bip01_pelvis", "hip", "cog"}},
            {"spine_01", {"spine", "spine01", "bip01_spine", "spine1", "spine_1"}},
            {"spine_02", {"spine1", "spine02", "bip01_spine1", "spine2", "spine_2"}},
            {"spine_03", {"spine2", "spine03", "bip01_spine2", "spine3", "spine_3", "chest"}},
            {"spine_04", {"spine3", "spine04", "spine4", "spine_4", "upperchest"}},
            {"spine_05", {"spine4", "spine05", "spine5", "spine_5"}},
            {"neck_01", {"neck", "neck1", "bip01_neck"}},
            {"neck_02", {"neck2"}},
            {"head", {"head", "bip01_head"}},
        };
        for (auto [u, word, letter] : {std::tuple{"l", "left", "l"}, std::tuple{"r", "right", "r"}})
        {
            const std::string W = word, L = letter, s = std::string("_") + u;
            t.push_back({"clavicle" + s, {W + "shoulder", W + "clavicle", "bip01_" + L + "_clavicle", "clavicle_" + L, L + "_clavicle"}});
            t.push_back({"upperarm" + s, {W + "arm", W + "upperarm", "bip01_" + L + "_upperarm", "shoulder_" + L, L + "_shoulder", "upperarm_" + L}});
            t.push_back({"lowerarm" + s, {W + "forearm", W + "lowerarm", "bip01_" + L + "_forearm", "elbow_" + L, L + "_elbow", "forearm_" + L}});
            t.push_back({"hand" + s, {W + "hand", "bip01_" + L + "_hand", "wrist_" + L, L + "_wrist", "hand_" + L}});
            t.push_back({"thigh" + s, {W + "upleg", W + "upperleg", W + "thigh", "bip01_" + L + "_thigh", "hip_" + L, L + "_hip"}});
            t.push_back({"calf" + s, {W + "leg", W + "lowerleg", W + "shin", W + "calf", "bip01_" + L + "_calf", "knee_" + L, L + "_knee"}});
            t.push_back({"foot" + s, {W + "foot", "bip01_" + L + "_foot", "ankle_" + L, L + "_ankle"}});
            t.push_back({"ball" + s, {W + "toebase", W + "toes", W + "toe", "bip01_" + L + "_toe0", "toe_" + L, L + "_toe"}});
        }
        return t;
    }();
    return table;
}

std::string labelForName(const std::string& name)
{
    const std::string n = lower(bareName(name));
    for (const AliasRow& r : aliasTable())
        if (n == r.label)
            return r.label;
    for (const AliasRow& r : aliasTable())
        for (const std::string& a : r.names)
            if (n == a)
                return r.label;
    return {};
}

/** Maya's joint type labels that name one bone. */
std::string labelForType(const std::string& type)
{
    static const std::map<std::string, std::string> map = {{"hip", "thigh"},       {"knee", "calf"},       {"foot", "foot"},     {"toe", "ball"},
                                                           {"head", "head"},       {"collar", "clavicle"}, {"shoulder", "upperarm"},
                                                           {"elbow", "lowerarm"}, {"hand", "hand"},       {"root", "pelvis"}};
    auto it = map.find(type);
    return it == map.end() ? std::string() : it->second;
}
} // namespace

int attr(const Node& n, const char* name) { return n.type().attrIndex(name); }

double num(const Node& n, const char* name)
{
    const int a = attr(n, name);
    return a >= 0 ? n.as<double>(a) : 0.0;
}

int64_t integer(const Node& n, const char* name)
{
    const int a = attr(n, name);
    return a >= 0 ? n.as<int64_t>(a) : 0;
}

bool flag(const Node& n, const char* name)
{
    const int a = attr(n, name);
    return a >= 0 && n.as<bool>(a);
}

Node* named(Graph& g, const std::string& path)
{
    try
    {
        return path.empty() ? nullptr : g.find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

std::string text(const Node& n, const char* name)
{
    const int a = attr(n, name);
    return a >= 0 ? n.as<std::string>(a) : std::string();
}

glm::dvec3 vec(const Node& n, const char* name)
{
    const int a = attr(n, name);
    return a >= 0 ? n.as<glm::dvec3>(a) : glm::dvec3(0.0);
}

glm::dmat4 rigid(const glm::dmat4& m)
{
    glm::dmat4 r = glm::mat4_cast(quatOf(m));
    r[3] = m[3];
    return r;
}

ShapeDesc shapeOf(const Node& m)
{
    ShapeDesc s;
    s.kind = ShapeKind(std::clamp<int64_t>(integer(m, "shape"), 0, 3));
    s.radius = num(m, "radius");
    s.length = num(m, "length");
    s.extents = vec(m, "extents");
    s.offset = vec(m, "shapeOffset");
    s.rotation = eulerQuat(vec(m, "shapeRotate"));
    if (s.kind == ShapeKind::ConvexHull)
    {
        const json j = json::parse(text(m, "hullPoints"), nullptr, false);
        if (j.is_array())
            for (const json& p : j)
                if (p.is_array() && p.size() == 3)
                    s.hull.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
    }
    return s;
}

void writeShape(Graph& g, NodeId marker, const ShapeDesc& s)
{
    const Node& m = *g.node(marker);
    auto set = [&](const char* name, PlugValue v) { g.setValue({marker, attr(m, name), -1}, std::move(v)); };
    set("shape", int64_t(s.kind));
    set("radius", s.radius);
    set("length", s.length);
    set("extents", s.extents);
    set("shapeOffset", s.offset);
    set("shapeRotate", quatEuler(s.rotation));
    json hull = json::array();
    for (const glm::dvec3& p : s.hull)
        hull.push_back({p.x, p.y, p.z});
    set("hullPoints", s.hull.empty() ? std::string() : hull.dump());
}

std::shared_ptr<const ShapeGeometry> geometryOf(const ShapeDesc& s)
{
    static std::map<uint64_t, std::shared_ptr<const ShapeGeometry>> cache;
    const uint64_t h = shapeHash(s);
    if (auto it = cache.find(h); it != cache.end())
        return it->second;
    auto g = std::make_shared<ShapeGeometry>();
    switch (s.kind)
    {
    case ShapeKind::Capsule: capsuleGeometry(*g, std::max(s.radius, 0.05), std::max(s.length, 0.0)); break;
    case ShapeKind::Box: boxGeometry(*g, glm::max(s.extents, glm::dvec3(0.1))); break;
    case ShapeKind::Sphere: capsuleGeometry(*g, std::max(s.radius, 0.05), 0.0); break;
    case ShapeKind::ConvexHull:
        hullGeometry(*g, s.hull);
        if (g->triangles.empty())
            capsuleGeometry(*g, std::max(s.radius, 0.05), 0.0);
        break;
    }
    const glm::dmat4 place = glm::translate(glm::dmat4(1.0), s.offset) * glm::mat4_cast(s.rotation);
    for (glm::dvec3& p : g->triangles)
        p = glm::dvec3(place * glm::dvec4(p, 1.0));
    for (glm::dvec3& p : g->lines)
        p = glm::dvec3(place * glm::dvec4(p, 1.0));
    if (cache.size() > 1024)
        cache.clear();
    cache[h] = g;
    return g;
}

std::optional<double> rayHit(const ShapeDesc& s, const glm::dmat4& world, const glm::dvec3& origin, const glm::dvec3& dir)
{
    const glm::dmat4 inv = glm::inverse(rigid(world));
    const glm::dvec3 o(inv * glm::dvec4(origin, 1.0)), d(inv * glm::dvec4(dir, 0.0));
    const std::shared_ptr<const ShapeGeometry> geo = geometryOf(s);
    std::optional<double> best;
    for (size_t t = 0; t + 2 < geo->triangles.size(); t += 3)
        if (auto hit = rayTriangle(o, d, geo->triangles[t], geo->triangles[t + 1], geo->triangles[t + 2]); hit && (!best || *hit < *best))
            best = hit;
    return best;
}

bool isMarker(const Node& n) { return n.type().isA(kMarkerType); }

NodeId jointOf(Graph& g, const Node& marker)
{
    Node* j = named(g, text(marker, "joint"));
    return j && transformOf(j) ? j->id() : kNoNode;
}

NodeId markerFor(Graph& g, NodeId joint)
{
    Node* j = g.node(joint);
    if (!j)
        return kNoNode;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && isMarker(*n) && named(g, text(*n, "joint")) == j)
            return id;
    return kNoNode;
}

NodeId solverOf(Graph& g, NodeId node)
{
    for (NodeId c = node; c; c = g.parent(c))
        if (Node* n = g.node(c); n && n->type().isA(kSolverType))
            return c;
    return kNoNode;
}

std::vector<NodeId> membersOf(Graph& g, NodeId solver, const char* type)
{
    std::vector<NodeId> out;
    for (NodeId d : g.descendants(solver))
        if (Node* n = g.node(d); n && n->type().isA(type))
            out.push_back(d);
    std::sort(out.begin(), out.end());
    return out;
}

glm::dquat restRotation(Graph& g, NodeId ancestor, NodeId child)
{
    glm::dmat4 r(1.0);
    for (NodeId c = child; c && c != ancestor; c = g.parent(c))
        if (const Transform* t = transformOf(g.node(c)))
            r = t->preRotation() * t->rotateAxisMatrix() * r;
    return quatOf(r);
}

glm::dvec3 restOffset(Graph& g, NodeId ancestor, NodeId child)
{
    glm::dmat4 m(1.0);
    for (NodeId c = child; c && c != ancestor; c = g.parent(c))
        if (const Transform* t = transformOf(g.node(c)))
        {
            const glm::dmat4 rot = t->preRotation() * t->rotateAxisMatrix();
            m = glm::translate(glm::dmat4(1.0), t->as<glm::dvec3>(Transform::kTranslate)) * rot * m;
        }
    return glm::dvec3(m[3]);
}

glm::dvec3 boneAxis(Graph& g, NodeId joint)
{
    for (NodeId c : g.children(joint))
        if (Node* n = g.node(c); n && isJointNode(*n))
            if (const glm::dvec3 t = n->as<glm::dvec3>(Transform::kTranslate); glm::length(t) > 1e-6)
                return glm::normalize(t);
    // A leaf carries on along the bone that reaches it.
    if (Node* j = g.node(joint); j && isJointNode(*j))
        if (const glm::dvec3 t = j->as<glm::dvec3>(Transform::kTranslate); glm::length(t) > 1e-6)
            return glm::normalize(glm::conjugate(restRotation(g, g.parent(joint), joint)) * glm::normalize(t));
    return {1.0, 0.0, 0.0};
}

glm::dquat boneFrame(Graph& g, NodeId joint)
{
    const glm::dvec3 a = boneAxis(g, joint);
    glm::dvec3 b = std::abs(a.y) < std::abs(a.z) ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
    b = glm::normalize(b - a * glm::dot(a, b));
    return glm::normalize(glm::quat_cast(glm::dmat3(a, b, glm::cross(a, b))));
}

glm::dvec3 swingTwist(const glm::dquat& q0)
{
    glm::dquat q = glm::normalize(q0.w < 0.0 ? -q0 : q0);
    const double tl = std::sqrt(q.w * q.w + q.x * q.x);
    const glm::dquat twist = tl > 1e-12 ? glm::dquat(q.w / tl, q.x / tl, 0.0, 0.0) : glm::dquat(1, 0, 0, 0);
    glm::dquat swing = q * glm::conjugate(twist);
    if (swing.w < 0.0)
        swing = -swing;
    return {2.0 * std::atan2(twist.x, twist.w), 2.0 * std::atan2(swing.y, swing.w), 2.0 * std::atan2(swing.z, swing.w)};
}

glm::dquat fromSwingTwist(const glm::dvec3& a)
{
    const glm::dquat twist = glm::angleAxis(a.x, glm::dvec3(1, 0, 0));
    const glm::dquat swing = glm::normalize(glm::dquat(1.0, 0.0, std::tan(std::clamp(a.y, -3.1, 3.1) * 0.5), std::tan(std::clamp(a.z, -3.1, 3.1) * 0.5)));
    return glm::normalize(swing * twist);
}

RigDef buildRig(Graph& g, NodeId solver)
{
    Node* s = g.node(solver);
    if (!s || !s->type().isA(kSolverType))
        throw std::invalid_argument("the physics solver is gone");
    RigDef rig;
    rig.solver = solver;
    rig.gravity = vec(*s, "gravity");
    rig.ground = flag(*s, "ground");
    rig.groundHeight = num(*s, "groundHeight");
    rig.groundFriction = num(*s, "groundFriction");
    rig.selfCollision = flag(*s, "selfCollision");
    rig.velocitySteps = int(std::clamp<int64_t>(integer(*s, "velocitySteps"), 2, 64));
    rig.positionSteps = int(std::clamp<int64_t>(integer(*s, "positionSteps"), 1, 32));
    uint64_t h = hashOwn(g, *s, 0x51ed270b2a8c4e3full);

    // Bodies: enabled, non-proxy markers on joints, one per joint (the first marker wins), parents first.
    std::vector<std::pair<int, size_t>> order;
    std::vector<BodyDef> found;
    std::set<NodeId> taken;
    for (NodeId mid : membersOf(g, solver, kMarkerType))
    {
        const Node& m = *g.node(mid);
        h = hashOwn(g, m, h);
        if (!flag(m, "enabled"))
            continue;
        const Behaviour behaviour = Behaviour(std::clamp<int64_t>(integer(m, "behaviour"), 0, 3));
        const NodeId joint = jointOf(g, m);
        if (behaviour == Behaviour::Proxy || !joint || !taken.insert(joint).second)
            continue;
        BodyDef b;
        b.marker = mid;
        b.joint = joint;
        b.behaviour = behaviour;
        b.space = PoseSpace(std::clamp<int64_t>(integer(m, "poseSpace"), 0, 2));
        b.shape = shapeOf(m);
        b.mass = std::max(num(m, "mass"), 0.0);
        b.density = std::max(num(m, "density"), 1e-3);
        b.friction = std::max(num(m, "friction"), 0.0);
        b.restitution = std::clamp(num(m, "restitution"), 0.0, 1.0);
        b.linearDamping = std::max(num(m, "linearDamping"), 0.0);
        b.angularDamping = std::max(num(m, "angularDamping"), 0.0);
        b.group = integer(m, "collisionGroup");
        b.limited = flag(m, "limitEnabled");
        b.twist = std::clamp(num(m, "limitTwist"), 0.5, 180.0) * kDeg;
        b.swing1 = std::clamp(num(m, "limitSwing1"), 0.5, 180.0) * kDeg;
        b.swing2 = std::clamp(num(m, "limitSwing2"), 0.5, 180.0) * kDeg;
        b.stiffnessAttr = attr(m, "poseStiffness");
        b.dampingAttr = attr(m, "poseDamping");
        order.push_back({depthOf(g, joint), found.size()});
        found.push_back(std::move(b));
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::map<NodeId, int> bodyOf;
    for (const auto& [d, i] : order)
    {
        bodyOf[found[i].joint] = int(rig.bodies.size());
        rig.bodies.push_back(std::move(found[i]));
    }
    for (BodyDef& b : rig.bodies)
    {
        for (NodeId a = g.parent(b.joint); a && b.parent < 0; a = g.parent(a))
            if (auto it = bodyOf.find(a); it != bodyOf.end())
                b.parent = it->second;
        b.frameInChild = boneFrame(g, b.joint);
        b.skeleton = lila::scene::grapple::skeletonRoot(g, b.joint);
        if (b.parent >= 0)
        {
            const NodeId pj = rig.bodies[size_t(b.parent)].joint;
            const glm::dvec3 orient = vec(*g.node(b.marker), "limitOrient") * kDeg;
            b.frameInParent = glm::normalize(restRotation(g, pj, b.joint) * b.frameInChild * fromSwingTwist(orient));
            b.anchorInParent = restOffset(g, pj, b.joint);
        }
        h = hashPod(b.joint, h);
        h = hashPod(b.parent, h);
        h = hashPod(b.frameInChild, h);
        h = hashPod(b.frameInParent, h);
        h = hashPod(b.anchorInParent, h);
    }

    for (NodeId pid : membersOf(g, solver, kPinType))
    {
        const Node& p = *g.node(pid);
        h = hashOwn(g, p, h);
        if (!flag(p, "enabled"))
            continue;
        PinDef pin;
        pin.node = pid;
        if (Node* target = named(g, text(p, "marker")))
        {
            const NodeId joint = isMarker(*target) ? jointOf(g, *target) : target->id();
            if (auto it = bodyOf.find(joint); it != bodyOf.end())
                pin.body = it->second;
        }
        pin.orient = flag(p, "orient");
        pin.damping = std::max(num(p, "damping"), 0.0);
        pin.stiffnessAttr = attr(p, "stiffness");
        h = hashPod(pin.body, h);
        rig.pins.push_back(pin);
    }

    for (NodeId cid : membersOf(g, solver, kColliderType))
    {
        const Node& c = *g.node(cid);
        h = hashOwn(g, c, h);
        Node* mesh = named(g, text(c, "mesh"));
        if (!flag(c, "enabled") || !mesh || !mesh->type().isA("mesh"))
            continue;
        const int out = attr(*mesh, "outMesh");
        const PlugValue v = g.value({mesh->id(), out, -1});
        const MeshPtr* mp = std::get_if<MeshPtr>(&v);
        if (!mp || !*mp)
            continue;
        const glm::dmat4 world = mesh->as<glm::dmat4>(Transform::kWorldMatrix);
        const glm::dvec3 scale(glm::length(glm::dvec3(world[0])), glm::length(glm::dvec3(world[1])), glm::length(glm::dvec3(world[2])));
        auto tris = std::make_shared<std::vector<glm::dvec3>>();
        const geo::Mesh& m = **mp;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            std::vector<glm::dvec3> poly;
            m.forEachFaceCorner(f, [&](geo::Index k) { poly.push_back(m.position(m.cornerPoint(k)) * scale); });
            for (size_t k = 1; k + 1 < poly.size(); ++k)
                tris->insert(tris->end(), {poly[0], poly[k], poly[k + 1]});
        }
        ColliderDef cd;
        cd.node = cid;
        cd.mesh = mesh->id();
        cd.triangles = tris;
        cd.friction = std::max(num(c, "friction"), 0.0);
        cd.restitution = std::clamp(num(c, "restitution"), 0.0, 1.0);
        h = sim::hashBytes(tris->data(), tris->size() * sizeof(glm::dvec3), h);
        rig.colliders.push_back(std::move(cd));
    }

    for (NodeId fid : membersOf(g, solver, kFieldType))
    {
        const Node& f = *g.node(fid);
        h = hashOwn(g, f, h);
        if (!flag(f, "enabled"))
            continue;
        FieldDef fd;
        fd.node = fid;
        fd.kind = FieldKind(std::clamp<int64_t>(integer(f, "kind"), 0, 3));
        fd.direction = vec(f, "direction");
        fd.frequency = num(f, "frequency");
        fd.seed = uint64_t(integer(f, "seed"));
        fd.magnitudeAttr = attr(f, "magnitude");
        rig.fields.push_back(fd);
    }
    if (Node* gr = named(g, text(*s, "grapple")); gr && gr->type().isA(lila::scene::grapple::kType))
    {
        namespace gp = lila::scene::grapple;
        rig.figures = true;
        // The body a joint rides: its own, else the nearest one above it.
        auto bodyFor = [&](NodeId joint) {
            for (NodeId j = joint; j; j = g.parent(j))
                if (auto it = bodyOf.find(j); it != bodyOf.end())
                    return it->second;
            return -1;
        };
        auto rotationOf = [](const glm::dmat4& m) { return glm::normalize(glm::quat_cast(glm::dmat3(rigid(m)))); };
        for (NodeId cid : gp::contactsOf(g, gr->id()))
        {
            const Node& c = *g.node(cid);
            h = hashOwn(g, c, h);
            const auto [sj, tj] = gp::contactJoints(g, cid);
            if (!flag(c, "enabled") || !sj || !tj)
                continue;
            ContactDef cd;
            cd.node = cid;
            cd.a = bodyFor(sj);
            cd.b = bodyFor(tj);
            if (cd.a < 0 || cd.b < 0 || cd.a == cd.b)
                continue;
            const auto [ps, pt] = gp::contactPoints(g, cid);
            const glm::dmat4 wa = g.node(rig.bodies[size_t(cd.a)].joint)->as<glm::dmat4>(Transform::kWorldMatrix);
            const glm::dmat4 wb = g.node(rig.bodies[size_t(cd.b)].joint)->as<glm::dmat4>(Transform::kWorldMatrix);
            cd.pointA = glm::dvec3(glm::inverse(rigid(wa)) * glm::dvec4(ps, 1.0));
            cd.pointB = glm::dvec3(glm::inverse(rigid(wb)) * glm::dvec4(pt, 1.0));
            if (flag(c, "orient"))
            {
                const glm::dquat held = glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(vec(c, "rotation"), RotateOrder::XYZ))));
                const glm::dmat4 ws = g.node(sj)->as<glm::dmat4>(Transform::kWorldMatrix), wt = g.node(tj)->as<glm::dmat4>(Transform::kWorldMatrix);
                const glm::dquat towardTarget = glm::conjugate(rotationOf(wb)) * rotationOf(wt);
                const glm::dquat fromSource = glm::conjugate(rotationOf(ws)) * rotationOf(wa);
                cd.rotation = glm::normalize(towardTarget * held * fromSource);
            }
            cd.weightAttr = attr(c, "weight");
            cd.stiffnessAttr = attr(c, "stiffness");
            h = hashPod(cd.a, h);
            h = hashPod(cd.b, h);
            for (double v : {cd.pointA.x, cd.pointA.y, cd.pointA.z, cd.pointB.x, cd.pointB.y, cd.pointB.z})
                h = hashPod(v, h);
            rig.contacts.push_back(cd);
        }
    }
    rig.hash = h;
    return rig;
}

namespace
{
bool helperJoint(const std::string& name)
{
    const std::string n = lower(bareName(name));
    return n.find("twist") != std::string::npos || n.find("corrective") != std::string::npos || n.starts_with("ik_");
}

/** World position of the first joint below top named name (namespaces ignored); none when there is none. */
std::optional<glm::dvec3> findJoint(Graph& g, NodeId top, const std::string& name)
{
    for (NodeId d : g.descendants(top))
        if (Node* n = g.node(d); n && isJointNode(*n) && lower(bareName(n->name())) == name)
            return glm::dvec3(n->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    return std::nullopt;
}
} // namespace

ShapeDesc bodySizedShape(Graph& g, NodeId joint, ShapeKind kind, const glm::dquat& frame)
{
    ShapeDesc s;
    s.kind = kind;
    s.rotation = frame;
    s.fromBones = true;
    NodeId top = joint;
    while (g.parent(top) && isJointNode(*g.node(g.parent(top))))
        top = g.parent(top);
    // The skeleton's height and shoulder span set the proportions, so a body comes out about a body's size and mass.
    double lo = 1e300, hi = -1e300;
    for (NodeId d : g.descendants(top))
        if (Node* n = g.node(d); n && isJointNode(*n))
        {
            const double y = n->as<glm::dmat4>(Transform::kWorldMatrix)[3].y;
            lo = std::min(lo, y), hi = std::max(hi, y);
        }
    const double h = std::max(hi > lo ? hi - lo : 0.0, 20.0);
    const auto shoulderL = findJoint(g, top, "upperarm_l"), shoulderR = findJoint(g, top, "upperarm_r");
    const double span = shoulderL && shoulderR ? std::max(glm::length(*shoulderL - *shoulderR), 0.12 * h) : 0.22 * h;

    const glm::dmat4 world = rigid(g.node(joint)->as<glm::dmat4>(Transform::kWorldMatrix));
    const glm::dmat4 inv = glm::inverse(world);
    const glm::dvec3 axis = frame * glm::dvec3(1, 0, 0);
    double len = 0.0;
    for (NodeId c : g.children(joint))
        if (Node* n = g.node(c); n && isJointNode(*n) && !helperJoint(n->name()))
            len = std::max(len, glm::dot(glm::dvec3(inv * n->as<glm::dmat4>(Transform::kWorldMatrix)[3]), axis));
    if (len <= 1e-6)
        if (Node* j = g.node(joint))
            len = glm::length(j->as<glm::dvec3>(Transform::kTranslate)) * 0.5;

    std::string label = semanticLabel(g, joint);
    if (label.ends_with("_l") || label.ends_with("_r"))
        label.resize(label.size() - 2);
    // Left to right in the shape's frame: the wide side of a torso box faces that way.
    const glm::dvec3 side = shoulderL && shoulderR ? glm::dvec3(inv * glm::dvec4(*shoulderL - *shoulderR, 0.0)) : glm::dvec3(0.0);
    const bool wideOnY = std::abs(glm::dot(side, frame * glm::dvec3(0, 1, 0))) >= std::abs(glm::dot(side, frame * glm::dvec3(0, 0, 1)));
    auto box = [&](double along, double wide, double deep) {
        s.extents = wideOnY ? glm::dvec3(along, wide, deep) : glm::dvec3(along, deep, wide);
        s.radius = std::min(wide, deep) * 0.5;
        s.length = std::max(along - 2.0 * s.radius, 0.0);
    };
    auto capsule = [&](double along, double r) {
        s.radius = r;
        s.length = std::max(along - 2.0 * r, 0.0);
        s.extents = glm::dvec3(along, 2.0 * r, 2.0 * r);
    };
    double along = std::max(len, 0.02 * h);
    if (label == "pelvis")
        along = std::max(len, 0.08 * h), box(along, span * 0.9, span * 0.6);
    else if (label.starts_with("spine"))
        along = std::max(len, 0.03 * h), box(along, span * 0.85, span * 0.55);
    else if (label.starts_with("neck"))
        capsule(along, 0.03 * h);
    else if (label == "head")
    {
        s.radius = 0.065 * h;
        along = 2.0 * s.radius;
        s.extents = glm::dvec3(along, 1.6 * s.radius, 1.8 * s.radius);
        s.length = 0.0;
    }
    else if (label == "clavicle")
        capsule(along, 0.03 * h);
    else if (label == "upperarm")
        capsule(along, std::max(0.03 * h, len * 0.15));
    else if (label == "lowerarm")
        capsule(along, std::max(0.025 * h, len * 0.13));
    else if (label == "hand")
        along = 0.1 * h, box(along, 0.05 * h, 0.025 * h);
    else if (label == "thigh")
        capsule(along, std::max(0.045 * h, len * 0.15));
    else if (label == "calf")
        capsule(along, std::max(0.035 * h, len * 0.12));
    else if (label == "foot")
        along = std::max(len, 0.08 * h), box(along, 0.055 * h, 0.05 * h);
    else if (label == "ball")
        along = std::max(len, 0.05 * h), box(along, 0.055 * h, 0.03 * h);
    else
        capsule(along, std::clamp(len * 0.18, 0.015 * h, 0.06 * h));
    s.offset = frame * glm::dvec3(along * 0.5, 0.0, 0.0);
    if (kind == ShapeKind::Sphere)
        s.radius = std::max(s.radius, along * 0.5);
    if (kind == ShapeKind::ConvexHull)
    {
        const glm::dvec3 e = s.extents * 0.5;
        for (int i = 0; i < 8; ++i)
            s.hull.push_back({(i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z});
    }
    return s;
}

ShapeDesc fitShape(Graph& g, NodeId joint, ShapeKind kind)
{
    g.evaluate();
    ShapeDesc s;
    s.kind = kind;
    const glm::dquat frame = boneFrame(g, joint);
    const glm::dmat4 toJoint = glm::inverse(rigid(g.node(joint)->as<glm::dmat4>(Transform::kWorldMatrix)));

    // The dominant-influence region's points, in the bone frame.
    std::vector<glm::dvec3> pts;
    for (NodeId id : g.nodes())
    {
        Node* mesh = g.node(id);
        if (!mesh || !mesh->type().isA("mesh"))
            continue;
        const std::vector<uint32_t> faces = rig::dominatedFaces(g, id, joint);
        if (faces.empty())
            continue;
        const PlugValue v = g.value({id, attr(*mesh, "outMesh"), -1});
        const MeshPtr* mp = std::get_if<MeshPtr>(&v);
        if (!mp || !*mp)
            continue;
        const geo::Mesh& m = **mp;
        const glm::dmat4 toBone = glm::mat4_cast(glm::conjugate(frame)) * toJoint * mesh->as<glm::dmat4>(Transform::kWorldMatrix);
        std::set<geo::Index> seen;
        for (uint32_t f : faces)
            if (m.faceValid(f))
                m.forEachFaceCorner(f, [&](geo::Index c) {
                    if (const geo::Index p = m.cornerPoint(c); seen.insert(p).second)
                        pts.push_back(glm::dvec3(toBone * glm::dvec4(m.position(p), 1.0)));
                });
    }

    s.rotation = frame;
    if (pts.size() >= 4)
    {
        glm::dvec3 lo(1e300), hi(-1e300), mean(0.0);
        for (const glm::dvec3& p : pts)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
            mean += p;
        }
        mean /= double(pts.size());
        const glm::dvec3 mid = (lo + hi) * 0.5;
        switch (kind)
        {
        case ShapeKind::Capsule:
        {
            double r = 0.0;
            for (const glm::dvec3& p : pts)
                r += glm::length(glm::dvec2(p.y - mean.y, p.z - mean.z));
            r = std::max(r / double(pts.size()) * 1.1, 0.5);
            s.radius = r;
            s.length = std::max(hi.x - lo.x - 2.0 * r, 0.0);
            s.offset = frame * glm::dvec3(mid.x, mean.y, mean.z);
            break;
        }
        case ShapeKind::Box:
            s.extents = glm::max((hi - lo) * 0.95, glm::dvec3(0.5));
            s.offset = frame * mid;
            break;
        case ShapeKind::Sphere:
        {
            double r = 0.0;
            for (const glm::dvec3& p : pts)
                r += glm::length(p - mid);
            s.radius = std::max(r / double(pts.size()) * 1.1, 0.5);
            s.offset = frame * mid;
            break;
        }
        case ShapeKind::ConvexHull:
        {
            // Farthest-point sampling from the bone's far end: an even spread, the same every time.
            std::vector<double> dist(pts.size(), 1e300);
            size_t pick = size_t(std::max_element(pts.begin(), pts.end(), [](const glm::dvec3& a, const glm::dvec3& b) { return a.x < b.x; }) - pts.begin());
            for (size_t k = 0; k < std::min<size_t>(pts.size(), 48); ++k)
            {
                s.hull.push_back(pts[pick]);
                size_t next = 0;
                for (size_t i = 0; i < pts.size(); ++i)
                {
                    dist[i] = std::min(dist[i], glm::length(pts[i] - pts[pick]));
                    if (dist[i] > dist[next])
                        next = i;
                }
                pick = next;
            }
            s.offset = glm::dvec3(0.0);
            s.radius = std::max(glm::length(hi - lo) * 0.25, 0.5);
            break;
        }
        }
        return s;
    }

    s = bodySizedShape(g, joint, kind, frame);
    return s;
}

LimitFit fitLimits(Graph& g, NodeId markerId, int64_t start, int64_t end, double margin)
{
    LimitFit fit;
    Node* marker = g.node(markerId);
    const NodeId solver = solverOf(g, markerId);
    if (!marker || !solver)
        return fit;
    const NodeId joint = jointOf(g, *marker);
    if (!joint)
        return fit;
    // The parent body: the nearest ancestor joint carrying a simulated marker of the same solver.
    std::set<NodeId> bodyJoints;
    for (NodeId m : membersOf(g, solver, kMarkerType))
        if (Node* n = g.node(m); n && flag(*n, "enabled") && Behaviour(integer(*n, "behaviour")) != Behaviour::Proxy)
            if (NodeId j = jointOf(g, *n))
                bodyJoints.insert(j);
    NodeId parent = kNoNode;
    for (NodeId a = g.parent(joint); a && !parent; a = g.parent(a))
        if (bodyJoints.contains(a))
            parent = a;
    if (!parent)
        return fit;

    const glm::dquat fc = boneFrame(g, joint);
    const glm::dquat fp = restRotation(g, parent, joint) * fc;
    std::vector<glm::dquat> samples;
    const double saved = g.time();
    if (end < start)
        std::swap(start, end);
    for (int64_t f = start; f <= end; ++f)
    {
        g.setTime(double(f) / sim::framesPerSecond());
        g.evaluate();
        const glm::dquat p = quatOf(g.node(parent)->as<glm::dmat4>(Transform::kWorldMatrix));
        const glm::dquat c = quatOf(g.node(joint)->as<glm::dmat4>(Transform::kWorldMatrix));
        samples.push_back(glm::conjugate(fp) * glm::conjugate(p) * c * fc);
    }
    g.setTime(saved);
    g.evaluate();
    if (samples.empty())
        return fit;

    glm::dvec3 lo(1e300), hi(-1e300);
    for (const glm::dquat& q : samples)
    {
        const glm::dvec3 a = swingTwist(q);
        lo = glm::min(lo, a);
        hi = glm::max(hi, a);
    }
    const glm::dvec3 centre = (lo + hi) * 0.5;
    const glm::dquat c = fromSwingTwist(centre);
    glm::dvec3 half(0.0);
    for (const glm::dquat& q : samples)
        half = glm::max(half, glm::abs(swingTwist(glm::conjugate(c) * q)));
    const double minHalf = 5.0;
    fit.orient = centre / kDeg;
    fit.twist = std::min(std::max(half.x / kDeg + margin, minHalf), 180.0);
    fit.swing1 = std::min(std::max(half.y / kDeg + margin, minHalf), 180.0);
    fit.swing2 = std::min(std::max(half.z / kDeg + margin, minHalf), 180.0);
    fit.frames = int(samples.size());
    return fit;
}

std::string semanticLabel(Graph& g, NodeId joint)
{
    Node* j = g.node(joint);
    if (!j)
        return {};
    const int64_t side = integer(*j, "side");
    auto sided = [side](std::string label) {
        if (label.empty() || label.ends_with("_l") || label.ends_with("_r") || label == "pelvis" || label == "head")
            return label;
        if (side == 1)
            return label + "_l";
        if (side == 2)
            return label + "_r";
        return label;
    };
    if (std::string other = lower(text(*j, "otherType")); !other.empty())
    {
        if (std::string l = labelForName(other); !l.empty())
            return sided(l);
        return sided(other);
    }
    if (const int t = attr(*j, "type"); t >= 0)
    {
        const AttrDef& def = j->type().attrs[size_t(t)];
        const int64_t v = j->as<int64_t>(t);
        if (v > 0 && size_t(v) < def.enumNames.size())
            if (std::string l = labelForType(def.enumNames[size_t(v)]); !l.empty())
                return sided(l);
    }
    return labelForName(j->name());
}

std::optional<LabelDefaults> labelDefaults(const std::string& label)
{
    std::string base = label;
    if (base.ends_with("_l") || base.ends_with("_r"))
        base.resize(base.size() - 2);
    LabelDefaults d;
    auto limits = [&](double t, double s1, double s2) {
        d.twist = t;
        d.swing1 = s1;
        d.swing2 = s2;
    };
    if (base == "pelvis")
    {
        d.shape = ShapeKind::Box;
        d.space = PoseSpace::Both;
        d.stiffness = 8.0;
    }
    else if (base.starts_with("spine"))
    {
        d.shape = ShapeKind::Box;
        limits(15, 20, 20);
    }
    else if (base.starts_with("neck"))
        limits(25, 30, 30);
    else if (base == "head")
    {
        d.shape = ShapeKind::Sphere;
        limits(35, 35, 35);
    }
    else if (base == "clavicle")
        limits(10, 15, 15);
    else if (base == "upperarm")
        limits(60, 80, 80);
    else if (base == "lowerarm")
        limits(60, 75, 75);
    else if (base == "hand")
    {
        d.shape = ShapeKind::Box;
        limits(30, 45, 45);
    }
    else if (base == "thigh")
        limits(35, 70, 70);
    else if (base == "calf")
        limits(20, 70, 70);
    else if (base == "foot")
    {
        d.shape = ShapeKind::Box;
        limits(15, 35, 35);
    }
    else if (base == "ball")
    {
        d.shape = ShapeKind::Box;
        limits(5, 30, 30);
    }
    else
        return std::nullopt;
    return d;
}
} // namespace physanim
