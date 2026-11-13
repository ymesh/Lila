#include "ui/RigViewport.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Ik.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "ui/Actions.h"
#include "ui/AnimProxyUi.h"
#include "ui/CursorBadges.h"
#include "ui/DominantInfluenceUi.h"
#include "ui/RigHandles.h"
#include "ui/SoftNetDraw.h"
#include "ui/SpacesUi.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/WeightPaintTool.h"
#include "scene/SceneIndex.h"

#include <QColor>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace lila::ui
{
namespace
{
// Same grayscale ranks as locators: unselected, under a selected node, selected.
const glm::vec4 kJointColor(0.02f, 0.02f, 0.02f, 1.0f);
const glm::vec4 kChildOfSelectedJointColor(0.55f, 0.55f, 0.55f, 1.0f);
const glm::vec4 kSelectedJointColor(0.95f, 0.95f, 0.95f, 1.0f);

using scene::Joint;
using scene::NodeId;

double displayScale() { return PrefStore::get().value<double>("general", "/rig/jointDisplayScale", 1.0); }

bool jointDrawn(const scene::Node& n)
{
    return scene::rig::isJoint(n) && scene::selectionMask::shown(n) && scene::effectivelyVisible(scene::Graph::current(), n.id()) &&
           n.as<int64_t>(Joint::kDrawStyle) != Joint::kHidden;
}

glm::dmat3 axesOf(const glm::dmat4& world)
{
    glm::dmat3 m(world);
    for (int i = 0; i < 3; ++i)
        m[i] = glm::length(m[i]) > 1e-12 ? glm::normalize(m[i]) : glm::dvec3(i == 0, i == 1, i == 2);
    return m;
}

void addCircle(std::vector<render::LineVertex>& out, const glm::dvec3& c, const glm::dvec3& u, const glm::dvec3& v, double r,
               const glm::vec4& color, int segments = 20)
{
    for (int k = 0; k < segments; ++k)
    {
        double a0 = 2.0 * std::numbers::pi * k / segments, a1 = 2.0 * std::numbers::pi * (k + 1) / segments;
        out.push_back({glm::dvec3(c + (u * std::cos(a0) + v * std::sin(a0)) * r), color});
        out.push_back({glm::dvec3(c + (u * std::cos(a1) + v * std::sin(a1)) * r), color});
    }
}

void addLine(std::vector<render::LineVertex>& out, const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& color)
{
    out.push_back({glm::dvec3(a), color});
    out.push_back({glm::dvec3(b), color});
}

/** Maya-style bone: a four-sided double pyramid from the joint to the child, widest just past the joint sphere. */
void addBone(std::vector<render::LineVertex>& out, const glm::dvec3& from, const glm::dvec3& to, double r, const glm::vec4& color)
{
    glm::dvec3 d = to - from;
    double len = glm::length(d);
    if (len < 1e-6)
        return;
    d /= len;
    glm::dvec3 ref = std::abs(d.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 u = glm::normalize(glm::cross(d, ref)), v = glm::cross(d, u);
    double w = std::min(r, len * 0.2);
    glm::dvec3 base = from + d * std::min(r, len * 0.25);
    glm::dvec3 corners[4] = {base + u * w, base + v * w, base - u * w, base - v * w};
    for (int k = 0; k < 4; ++k)
    {
        addLine(out, corners[k], corners[(k + 1) % 4], color);
        addLine(out, corners[k], to, color);
        addLine(out, corners[k], from, color);
    }
}

void addBox(std::vector<render::LineVertex>& out, const glm::dvec3& c, const glm::dmat3& axes, double r, const glm::vec4& color)
{
    for (int a = 0; a < 3; ++a)
        for (int s1 = -1; s1 <= 1; s1 += 2)
            for (int s2 = -1; s2 <= 1; s2 += 2)
            {
                glm::dvec3 off = axes[(a + 1) % 3] * double(s1) * r + axes[(a + 2) % 3] * double(s2) * r;
                addLine(out, c + off - axes[a] * r, c + off + axes[a] * r, color);
            }
}

/** HSV (hue in degrees) to sRGB 0..1. */
glm::vec3 jointHsv(float h, float s, float v)
{
    auto f = [&](float n) {
        float k = std::fmod(n + h / 60.0f, 6.0f);
        return v - v * s * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    };
    return {f(5.0f), f(3.0f), f(1.0f)};
}

float jointLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

/** The selected joint's colour as HSV (hue in degrees) and how far each ancestor steps from it, from the prefs. */
struct ChainStyle
{
    float hue = 140.0f, sat = 0.72f, val = 1.0f;
    float separation = 1.0f;
    bool monotone = false;
};

ChainStyle chainStyle()
{
    const PrefStore& p = PrefStore::get();
    ChainStyle st;
    QColor c(QString::fromStdString(p.value<std::string>("general", kJointSelectedColor, kJointDefaultSelectedColor)));
    if (!c.isValid())
        c = QColor(kJointDefaultSelectedColor);
    float h = 0.0f;
    c.getHsvF(&h, &st.sat, &st.val);
    st.hue = std::max(h, 0.0f) * 360.0f;
    st.separation = float(std::max(0.0, p.value<double>("general", kJointChainSeparation, 1.0)));
    st.monotone = p.value<bool>("general", viewportDisplay::kMonotone, false);
    return st;
}

/**
 * A selected joint (step 0) and its ancestors, one step per joint up the chain. The first ancestor drops well below the
 * selected colour; each step after dims, desaturates and drifts the hue towards blue, all scaled by the separation.
 * Monotone keeps only the dimming.
 */
glm::vec4 chainColor(int step, const ChainStyle& st)
{
    const float k = float(std::min(step, 6)), sep = st.separation;
    float v = st.val, s = st.sat;
    if (step > 0)
    {
        v = std::max(st.val * std::pow(0.72f, sep) * std::pow(0.8f, sep * (k - 1.0f)), std::min(0.26f, st.val));
        s = st.sat * std::pow(0.83f, sep) * std::pow(0.88f, sep * (k - 1.0f));
    }
    glm::vec3 c = st.monotone ? glm::vec3(v) : jointHsv(st.hue + 16.0f * sep * k, s, v);
    return {jointLinear(c.r), jointLinear(c.g), jointLinear(c.b), 1.0f};
}

/** Joint ancestors of the selection by steps up the chain, the nearest selected descendant deciding. */
std::unordered_map<NodeId, int> selectedChainSteps(scene::Graph& g)
{
    std::unordered_map<NodeId, int> steps;
    for (NodeId id : g.selection())
    {
        int step = 0;
        for (NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
        {
            const scene::Node* n = g.node(p);
            if (!n || !scene::rig::isJoint(*n))
                break;
            auto [it, added] = steps.try_emplace(p, ++step);
            if (!added)
                it->second = std::min(it->second, step);
        }
    }
    return steps;
}

std::vector<NodeId> childJoints(scene::Graph& g, NodeId id)
{
    std::vector<NodeId> out;
    for (NodeId c : g.children(id))
        if (jointDrawn(*g.node(c)))
            out.push_back(c);
    return out;
}

void drawJoints(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::unordered_set<NodeId> under;
    std::unordered_map<NodeId, int> chain;
    const bool highlight = viewportDisplay::highlightSelection();
    if (highlight)
    {
        for (NodeId id : g.selection())
            for (NodeId d : g.descendants(id))
                under.insert(d);
        chain = selectedChainSteps(g);
    }
    const ChainStyle chainLook = chainStyle();
    bool xray = PrefStore::get().value<bool>("general", "/rig/xrayJoints", false);
    auto& out = xray ? s.linesXray : s.lines;
    double scale = displayScale();

    // Highlighted joints draw last, nearest the selection latest, so they win over plainer bones at equal depth.
    struct Styled
    {
        NodeId id;
        int order;
        glm::vec4 color;
        bool ancestor = false;
    };
    std::vector<Styled> styled;
    for (scene::Node* jn : scene::sceneIndex::matching(g, "joint", scene::rig::isJoint))
    {
        const NodeId id = jn->id();
        if (!jointDrawn(*jn))
            continue;
        if (highlight && g.isSelected(id))
            styled.push_back({id, 1000, chainColor(0, chainLook)});
        else if (auto it = chain.find(id); it != chain.end())
            styled.push_back({id, 1000 - it->second, chainColor(it->second, chainLook), true});
        else
            styled.push_back({id, under.contains(id) ? 1 : 0, under.contains(id) ? kChildOfSelectedJointColor : kJointColor});
    }
    std::stable_sort(styled.begin(), styled.end(), [](const Styled& a, const Styled& b) { return a.order < b.order; });
    const bool colorBranches = PrefStore::get().value<bool>("general", kJointChainBranches, false);
    auto boneColor = [&](const Styled& from, NodeId child) {
        if (!from.ancestor || colorBranches || chain.contains(child) || (highlight && g.isSelected(child)))
            return from.color;
        return under.contains(child) ? kChildOfSelectedJointColor : kJointColor;
    };

    for (const Styled& st : styled)
    {
        scene::Node* n = g.node(st.id);
        const glm::vec4& color = st.color;
        glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        glm::dvec3 p(w[3]);
        glm::dmat3 axes = axesOf(w);
        double r = n->as<double>(Joint::kRadius) * scale;
        if (n->as<int64_t>(Joint::kDrawStyle) == Joint::kBox)
            addBox(out, p, axes, r, color);
        else
        {
            addCircle(out, p, axes[0], axes[1], r, color);
            addCircle(out, p, axes[1], axes[2], r, color);
            addCircle(out, p, axes[2], axes[0], r, color);
        }
        for (NodeId c : childJoints(g, st.id))
            addBone(out, p, glm::dvec3(g.node(c)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]), r, boneColor(st, c));
    }
}

/** A handle inside a rig element's IK chain: the chain's own controller is what gets drawn and picked instead. */
bool rigElementHandle(scene::Graph& g, NodeId id)
{
    for (NodeId chain : scene::rigel::chains(g))
        if (scene::rigel::handleOf(g, chain) == id)
            return true;
    return false;
}

bool isIkHandle(const scene::Node& n)
{
    scene::Graph& g = scene::Graph::current();
    return n.type().isA("ikHandle") && scene::effectivelyVisible(g, n.id()) && !rigElementHandle(g, n.id());
}

/** IK handles: a cross at the target, a line from the chain's start to it, and the pole vector from the start. */
void drawIkHandles(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    auto& out = PrefStore::get().value<bool>("general", "/rig/xrayJoints", false) ? s.linesXray : s.lines;
    double scale = displayScale();
    for (scene::Node* n : scene::sceneIndex::ofType(g, "ikHandle"))
    {
        const NodeId id = n->id();
        if (!isIkHandle(*n))
            continue;
        const glm::vec4& color = viewportDisplay::highlightSelection() && g.isSelected(id) ? kSelectedJointColor : kChildOfSelectedJointColor;
        glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        std::vector<NodeId> chain = scene::ik::chainOf(g, id);
        double r = 2.0 * scale;
        if (!chain.empty())
            r = g.node(chain.front())->as<double>(Joint::kRadius) * 1.5 * scale;
        for (int a = 0; a < 3; ++a)
        {
            glm::dvec3 d(0.0);
            d[a] = r;
            addLine(out, p - d, p + d, color);
        }
        addCircle(out, p, {1, 0, 0}, {0, 0, 1}, r * 0.6, color, 12);
        if (chain.empty())
            continue;
        glm::dvec3 start(g.node(chain.front())->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        addLine(out, start, p, color);
        glm::dvec3 pole = n->as<glm::dvec3>(scene::IkHandle::kPoleVector);
        if (glm::length(pole) > 1e-9)
            addLine(out, start, start + glm::normalize(pole) * glm::length(p - start) * 0.5, kChildOfSelectedJointColor);
    }
}

std::vector<viewportHooks::PickHit> pickIkHandles(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    scene::Graph& g = scene::Graph::current();
    QRectF box = QRectF(a, b).normalized();
    std::vector<viewportHooks::PickHit> hits;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!isIkHandle(*n))
            continue;
        auto c = view.project(glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]));
        if (!c)
            continue;
        if (!click)
        {
            if (box.contains(*c))
                hits.push_back({n->path(), 0.0});
            continue;
        }
        if (double d = std::hypot(c->x() - a.x(), c->y() - a.y()); d < 10.0)
            hits.push_back({n->path(), d});
    }
    return hits;
}

double segDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t - p;
    return std::hypot(q.x(), q.y());
}

std::vector<viewportHooks::PickHit> pickJoints(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    QRectF box = QRectF(a, b).normalized();
    double scale = displayScale();
    std::vector<viewportHooks::PickHit> hits;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!jointDrawn(*n))
            continue;
        glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        auto c = view.project(p);
        if (!c)
            continue;
        if (!click)
        {
            if (box.contains(*c))
                hits.push_back({n->path(), 0.0});
            continue;
        }
        double rpx = n->as<double>(Joint::kRadius) * scale / view.unitsPerPixel(p);
        double best = std::max(0.0, std::hypot(c->x() - a.x(), c->y() - a.y()) - rpx);
        // A bone picks the joint it starts from, like Maya.
        for (NodeId ch : childJoints(g, id))
            if (auto cc = view.project(glm::dvec3(g.node(ch)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3])))
                best = std::min(best, segDistance(a, *c, *cc));
        if (best < 8.0)
            hits.push_back({n->path(), best});
    }
    return hits;
}

/** Click to place joints in a chain on the view plane through the last joint; Enter starts a new chain. */
class JointTool final : public ViewportTool
{
public:
    const char* name() const override { return "jointTool"; }

    void activate() override { log::info("Joint tool: click to place joints, Enter to start a new chain, Esc to finish"); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        glm::dvec3 anchor(0.0);
        for (NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && scene::rig::isJoint(*n))
                anchor = glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        // The world plane most facing the camera, so front/side views place joints where they're drawn.
        glm::dvec3 vd = glm::abs(view.viewDir(anchor));
        glm::dvec3 normal(0.0);
        normal[vd.x > vd.y && vd.x > vd.z ? 0 : vd.y > vd.z ? 1 : 2] = 1.0;
        double denom = glm::dot(d, normal);
        if (std::abs(denom) < 1e-9)
            return true;
        double t = glm::dot(anchor - o, normal) / denom;
        glm::dvec3 hit = o + d * t;
        actions::run({{"command", "joint"}, {"args", {{"position", {hit.x, hit.y, hit.z}}}}});
        return true;
    }

    bool mouseRelease(QMouseEvent*, const ViewInfo&) override { return true; }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            scene::Graph::current().select({});
            return true;
        }
        if (e->key() == Qt::Key_Escape)
        {
            // Not from inside our own handler: set() destroys this tool.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolStatus(p, view, {.tool = "Joint", .prompt = "click to place", .keys = {{"Enter", "new chain"}, {"Esc", "finish"}}});
    }
};

void command(const char* name, nlohmann::json args = nlohmann::json::object())
{
    actions::run({{"command", name}, {"args", std::move(args)}});
}

void registerRigActions()
{
    auto add = [](const char* id, const char* label, std::function<void()> fn, const char* python, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Rig", .run = std::move(fn), .doc = doc, .python = python});
    };
    add("rig.jointTool", "Joint Tool", [] { viewportTools::set(std::make_unique<JointTool>()); }, "",
        "Click in the viewport to place joints in a chain; Enter starts a new chain, Esc finishes.");
    add("rig.orientJoint", "Orient Joint", [] { command("orientJoint"); }, "orientJoint()",
        "Aims each selected joint's primary axis at its child, with the secondary axis toward a world axis.");
    add("rig.mirrorJoints", "Mirror Joints", [] { command("mirrorJoints"); }, "mirrorJoints()",
        "Copies the selected joint hierarchy to the other side of a plane through the world origin.");
    add("rig.insertJoint", "Insert Joint", [] { command("insertJoint"); }, "insertJoint()", "Splits the bone below the selected joint with evenly spaced joints.");
    add("rig.rerootSkeleton", "Reroot Skeleton", [] { command("rerootSkeleton"); }, "rerootSkeleton()",
        "Makes the selected joint the root by reversing the chain above it, keeping every joint in place.");
    add("rig.bindSkin", "Bind Skin", [] { command("skinCluster"); }, "skinCluster()",
        "Binds the selected mesh to the selected joints, weighting each point by its closest bone.");
    add("rig.bindSkinHeatmap", "Bind Skin (Heat Map)", [] { command("skinCluster", {{"method", "heatmap"}}); },
        "skinCluster(method=\"heatmap\")", "Binds like Bind Skin, but ignores bones hidden behind the surface and smooths the weights.");
    add("rig.unbindSkin", "Unbind Skin", [] { command("unbindSkin"); }, "unbindSkin()",
        "Removes the selected mesh's skinning; the mesh returns to its bind shape.");
    add("rig.goToBindPose", "Go to Bind Pose", [] { command("goToBindPose"); }, "goToBindPose()",
        "Puts the skinned mesh's joints back where they were when bound.");
    add("rig.paintWeights", "Paint Skin Weights", [] { startWeightPaint(); }, "", "Paint how strongly each joint moves the selected skinned mesh.");
    add("rig.mirrorSkinWeights", "Mirror Skin Weights", [] { command("mirrorSkinWeights"); }, "mirrorSkinWeights()",
        "Copies weights from one side of the mesh to the matching joints on the other.");
    add("rig.copySkinWeights", "Copy Skin Weights", [] { command("copySkinWeights"); }, "copySkinWeights()",
        "Copies weights from the first selected skinned mesh to the second by closest surface point.");
    add("rig.normalizeWeights", "Normalize Weights", [] { command("normalizeWeights"); }, "normalizeWeights()",
        "Scales each point's weights to sum to 1; locked joints keep theirs.");
    add("rig.pruneWeights", "Prune Small Weights", [] { command("pruneWeights"); }, "pruneWeights()",
        "Drops tiny weights and any beyond the max influences per point, then renormalizes.");
    add("rig.parentConstraint", "Parent Constraint", [] { command("parentConstraint"); }, "parentConstraint()",
        "The last selected object follows the others' position and rotation as if parented to them.");
    add("rig.pointConstraint", "Point Constraint", [] { command("pointConstraint"); }, "pointConstraint()",
        "The last selected object follows the others' position only.");
    add("rig.orientConstraint", "Orient Constraint", [] { command("orientConstraint"); }, "orientConstraint()",
        "The last selected object follows the others' rotation only.");
    add("rig.aimConstraint", "Aim Constraint", [] { command("aimConstraint"); }, "aimConstraint()",
        "The last selected object turns to point at the others.");
    add("rig.removeConstraint", "Remove Constraint", [] { command("removeConstraint"); }, "removeConstraint()",
        "Deletes the selected constraints, or those on the selected objects, keeping the current pose.");
    add("rig.pin", "Pin", [] { command("pin"); }, "pin()",
        "Pins the selected objects to the last selected curve or mesh: its curve, selected vertex, edge or face, or surface.");
    add("rig.bakePin", "Bake Pins to Joints", [] { command("bakePin"); }, "bakePin()",
        "Turns the selected pins or pinned objects into joints for Unreal: sockets where they ride a bone rigidly, else keyed bones.");
    add("rig.ikHandle", "IK Handle", [] { command("ikHandle"); }, "ikHandle()",
        "Solves the chain from the first selected joint to the last with an IK handle at its end.");
    add("rig.bendBones", "Bind Bend Bones", [] { command("bendBones"); }, "bendBones()",
        "Select a chain's joints from top to end, then the mesh: it bends along a smooth curve through them, the chain one influence.");
    add("rig.splineChain", "Spline Chain", [] { command("splineChain"); }, "splineChain()",
        "A bend-bone chain led by a curve: lays the joints from the first selected to the last along it, with controls (or the selected curve).");
    add("rig.hoseLimb", "Rubber Hose Limb", [] { command("hoseLimb"); }, "hoseLimb()",
        "A bend-bone limb: turns the limb from the selected shoulder or hip to the selected wrist or ankle into a rubber hose.");
    add("rig.hoseFingers", "Rubber Hose Fingers", [] { command("hoseFingers"); }, "hoseFingers()",
        "Bend-bone fingers: makes the selected finger base joints rubber hose fingers with curl, spread and a hand fist.");
    add("rig.bendBoneCV", "Add Bone CV", [] { command("bendBoneCV"); }, "bendBoneCV()",
        "Adds a CV halfway along the selected joint's bend bone or spline chain bone, and on the other side's; on a hose limb, picks its bend control.");
    add("rig.mirrorBoneCVs", "Mirror Bone CVs", [] { command("mirrorBendBoneCVs"); }, "mirrorBendBoneCVs()",
        "Moves the other side's bone CVs to mirror the selected ones.");
    add("rig.bakeBendBones", "Bake Bend Bones", [] { command("bakeBendBones"); }, "bakeBendBones()",
        "Bakes the selected mesh's bend bones to segment joints and a fitted skin for Unreal, checked against Unreal's transforms.");
    actions::add({.id = "rig.softNet",
                  .label = "Soft Net",
                  .category = "Rig",
                  .run = [] { command("softNet"); },
                  .doc = "Turns the selected mesh's Panel Layout lines into a net of curves that shapes the surface after the skin: "
                         "every junction gets a knot control to move, turn or scale.",
                  .python = "softNet()",
                  .keywords = {"curvenet", "curve net", "profile curves", "face rig", "shaping", "knot", "surface curves"}});
    add("rig.softNetRebind", "Rebind Soft Net", [] { command("softNetRebind"); }, "softNetRebind()",
        "Binds the selected Soft Net again after its Panel Layout changed, keeping knot controls and their keys on the junctions that remain.");
    add("rig.mirrorSoftNetKnots", "Mirror Soft Net Knots", [] { command("mirrorSoftNetKnots"); }, "mirrorSoftNetKnots()",
        "Copies the selected Soft Net knots' poses to their partners across the mirror; on a whole net, the + side over the - side.");
    add("rig.softNetShowAllKnots", "Show All Soft Net Knots", [] { command("softNetKnots", {{"toggleShowAll", true}}); }, "softNetKnots(toggleShowAll=True)",
        "Shows or hides the selected Soft Net's knots inside lines; junctions and line ends always show.");
    add("rig.softNetUnreal", "Soft Net to Unreal", [] { command("softNetUnreal"); }, "softNetUnreal()",
        "Sends the selected mesh's Soft Net to Unreal: baked to knot joints, weights and correctives (default), live as a Deformer Graph, or as a geometry cache.");
    add("rig.softNetLimbStations", "Soft Net Limb Stations", [] { command("softNetLimbStations"); }, "softNetLimbStations()",
        "Lays rings and rails for a Soft Net round the selected mesh's limb, along the selected curve or joint chain (select the mesh, then the chain).");
    actions::add({.id = "rig.editSkeleton", .label = "Edit Skeleton", .category = "Rig", .run = [] { command("editSkeleton"); },
                  .doc = "Toggles skeleton editing: the skinned skeleton goes to bind pose so joints can be moved and reoriented "
                         "without deforming the mesh; turning it off rebinds and keeps the animation.",
                  .python = "editSkeleton()", .checked = [] { return scene::rig::editingSkeleton(scene::Graph::current()); }});
    cursorBadges::add({"editSkeleton", "bone", [](const ViewportState&) { return scene::rig::editingSkeleton(scene::Graph::current()); },
                       "Editing skeleton: skins hold their shape", {"rig.editSkeleton"}});
    add("rig.xrayJoints", "X-Ray Joints", [] {
        bool on = PrefStore::get().value<bool>("general", "/rig/xrayJoints", false);
        PrefStore::get().set("general", "/rig/xrayJoints", !on);
        viewportTools::requestRedraw();
    }, "", "Draws joints through meshes so they can be seen and picked inside.");
}
} // namespace

void registerRigViewport()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    viewportHooks::addDrawer(drawJoints);
    viewportHooks::addPicker(pickJoints);
    viewportHooks::addDrawer(drawIkHandles);
    viewportHooks::addPicker(pickIkHandles);
    registerRigActions();
    registerWeightPaint();
    rigHandles::registerRigHandles();
    registerDominantInfluenceUi();
    registerAnimProxyUi();
    registerSpacesUi();
    registerSoftNetDraw();
}
} // namespace lila::ui
