// Physics Animation's UI: marker bodies drawn and picked as proxies for their joints, the live simulation, pins and
// fields in the viewport, Live mode, actions and a menu, a panel, and Attribute Editor sections.

#include "PhysicsAnimation.h"
#include "World.h"

#include <lila/Command.h>
#include <lila/OptionBox.h>
#include <lila/Events.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Prefs.h>
#include <lila/SimPreview.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>
#include <lila/UI.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QGroupBox>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <optional>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace physanim
{
namespace
{
// Bodies are data, coloured by behaviour (linear).
const glm::vec4 kKinematicColor{0.20f, 0.42f, 0.95f, 1.0f};
const glm::vec4 kPoseMatchColor{1.00f, 0.50f, 0.10f, 1.0f};
const glm::vec4 kSimulatedColor{0.95f, 0.18f, 0.15f, 1.0f};
const glm::vec4 kProxyColor{0.25f, 0.75f, 0.70f, 1.0f};
const glm::vec4 kSimColor{0.95f, 0.85f, 0.15f, 1.0f};

json runCommand(const char* name, const json& call = json::object())
{
    if (optionBox::shiftOpens(name, call))
        return nullptr;
    try
    {
        return ops::execute(name, ops::options::callWith(name, call), CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Error, e.what());
        return nullptr;
    }
}

glm::vec4 behaviourColor(Behaviour b)
{
    switch (b)
    {
    case Behaviour::Kinematic: return kKinematicColor;
    case Behaviour::PoseMatch: return kPoseMatchColor;
    case Behaviour::Simulated: return kSimulatedColor;
    case Behaviour::Proxy: return kProxyColor;
    }
    return kProxyColor;
}

glm::vec4 brighter(const glm::vec4& c, float t) { return glm::vec4(glm::mix(glm::vec3(c), glm::vec3(1.0f), t), c.a); }

/** Shaded, see-through triangles and an outline of a shape posed at world. */
void drawShape(render::FrameState& f, const ViewInfo& view, const ShapeGeometry& geo, const glm::dmat4& world, glm::vec4 color, float fill,
               bool onTop)
{
    for (size_t t = 0; t + 2 < geo.triangles.size(); t += 3)
    {
        const glm::dvec3 a(world * glm::dvec4(geo.triangles[t], 1.0)), b(world * glm::dvec4(geo.triangles[t + 1], 1.0)),
            c(world * glm::dvec4(geo.triangles[t + 2], 1.0));
        const glm::dvec3 n = glm::cross(b - a, c - a);
        const double len = glm::length(n);
        const double facing = len > 1e-12 ? std::abs(glm::dot(n / len, view.viewDir((a + b + c) / 3.0))) : 1.0;
        const glm::vec4 col(glm::vec3(color) * float(0.45 + 0.55 * facing), fill);
        for (const glm::dvec3& p : {a, b, c})
            f.tris3d.push_back({glm::dvec3(p), col});
    }
    auto& lines = onTop ? f.linesOnTop : f.lines;
    for (const glm::dvec3& p : geo.lines)
        lines.push_back({glm::dvec3(glm::dvec3(world * glm::dvec4(p, 1.0))), color});
}

glm::dmat4 jointWorld(Graph& g, NodeId joint) { return rigid(g.node(joint)->as<glm::dmat4>(Transform::kWorldMatrix)); }

bool proxySelects() { return PrefStore::get().value<bool>("general", kProxySelectPref, true); }

void drawMarker(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    const NodeId joint = jointOf(g, c.node);
    if (!joint || !flag(c.node, "enabled"))
        return;
    const Behaviour b = Behaviour(std::clamp<int64_t>(integer(c.node, "behaviour"), 0, 3));
    const bool selected = c.highlight == drawOverrides::Highlight::Selected || g.isSelected(joint);
    const bool hovered = c.highlight == drawOverrides::Highlight::Preselected;
    glm::vec4 color = behaviourColor(b);
    if (selected)
        color = brighter(color, 0.45f);
    else if (hovered)
        color = brighter(color, 0.25f);
    drawShape(c.frame, c.view, *geometryOf(shapeOf(c.node)), jointWorld(g, joint), color, selected ? 0.32f : hovered ? 0.24f : 0.14f, false);
}

std::optional<double> pickMarker(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    const NodeId joint = jointOf(g, c.node);
    if (!joint || !flag(c.node, "enabled"))
        return std::nullopt;
    const glm::dmat4 world = jointWorld(g, joint);
    const ShapeDesc s = shapeOf(c.node);
    if (!c.click)
    {
        const glm::dvec3 centre(world * glm::dvec4(s.offset, 1.0));
        std::optional<QPointF> at = c.view.project(centre);
        return at && QRectF(c.a, c.b).normalized().contains(*at) ? std::optional<double>(0.0) : std::nullopt;
    }
    glm::dvec3 o, d;
    c.view.ray(c.a, o, d);
    if (rayHit(s, world, o, d))
        return 0.0;
    return std::nullopt;
}

/** The live simulation from the framework's cache, as outlines over the scene. */
void drawSolver(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    if (!flag(c.node, "preview") || !flag(c.node, "enabled"))
        return;
    std::shared_ptr<const sim::Frame> f;
    try
    {
        f = simPreviewFrame(g, c.node.id());
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!f)
        return;
    const sim::Info info = sim::info(g, c.node.id());
    for (size_t i = 0; i < info.outputs.size() && i < f->world.size(); ++i)
        if (NodeId m = markerFor(g, info.outputs[i]))
            drawShape(c.frame, c.view, *geometryOf(shapeOf(*g.node(m))), rigid(f->world[i]), kSimColor, 0.0f, true);
    for (NodeId p : membersOf(g, c.node.id(), kPinType))
    {
        Node* pin = g.node(p);
        Node* target = named(g, text(*pin, "marker"));
        if (!target || !flag(*pin, "enabled"))
            continue;
        const NodeId joint = isMarker(*target) ? jointOf(g, *target) : target->id();
        for (size_t i = 0; i < info.outputs.size() && i < f->world.size(); ++i)
            if (info.outputs[i] == joint)
            {
                c.frame.linesOnTop.push_back({glm::dvec3(glm::dvec3(pin->as<glm::dmat4>(Transform::kWorldMatrix)[3])), kSimColor});
                c.frame.linesOnTop.push_back({glm::dvec3(glm::dvec3(f->world[i][3])), kSimColor});
            }
    }
    if (info.loop && !info.converged && !f->world.empty())
        if (std::optional<QPointF> at = c.view.project(glm::dvec3(f->world.front()[3])))
            c.painter.label(*at + QPointF(10, -12), "Physics loop did not settle: seam cross-faded");
}

void drawCross(drawOverrides::DrawContext& c, double pixels)
{
    const glm::dvec3 p(c.world[3]);
    const double r = pixels * c.view.unitsPerPixel(p);
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::dvec3 d(0.0);
        d[axis] = r;
        c.frame.lines.push_back({glm::dvec3(p - d), c.color});
        c.frame.lines.push_back({glm::dvec3(p + d), c.color});
    }
}

void drawField(drawOverrides::DrawContext& c)
{
    drawCross(c, 6.0);
    const glm::dvec3 p(c.world[3]);
    glm::dvec3 d = vec(c.node, "direction");
    if (glm::length(d) < 1e-12 || integer(c.node, "kind") >= 2)
        return;
    d = glm::normalize(d) * (40.0 * c.view.unitsPerPixel(p));
    c.frame.lines.push_back({glm::dvec3(p), c.color});
    c.frame.lines.push_back({glm::dvec3(p + d), c.color});
}

std::optional<double> pickPoint(const drawOverrides::PickContext& c)
{
    std::optional<QPointF> at = c.view.project(glm::dvec3(c.world[3]));
    if (!at)
        return std::nullopt;
    if (!c.click)
        return QRectF(c.a, c.b).normalized().contains(*at) ? std::optional<double>(0.0) : std::nullopt;
    return std::max(0.0, std::hypot(at->x() - c.a.x(), at->y() - c.a.y()) - 6.0);
}

/** A click on a marker's body selects the joint behind it: the proxy bodies stand in for the character. */
std::optional<std::vector<std::string>> redirectProxy(const selectionHandlers::Hit& h)
{
    if (!proxySelects() || h.node == kNoNode)
        return std::nullopt;
    Graph& g = Graph::current();
    Node* n = g.node(h.node);
    if (!n || !isMarker(*n))
        return std::nullopt;
    const NodeId joint = jointOf(g, *n);
    if (!joint)
        return std::nullopt;
    return std::vector<std::string>{g.node(joint)->path()};
}

/** The solver of the selection, else the scene's only one. */
NodeId currentSolver()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
    {
        if (NodeId s = solverOf(g, id))
            return s;
        if (NodeId m = markerFor(g, id))
            return solverOf(g, m);
    }
    NodeId only = kNoNode;
    int count = 0;
    for (NodeId id : sim::simulations(g))
        if (Node* n = g.node(id); n && n->type().isA(kSolverType))
            only = id, ++count;
    return count == 1 ? only : kNoNode;
}

/**
 * Live mode: the ragdoll as a puppet. Gravity and pose matching are off and the bodies are heavily damped, so they stay
 * where they are dragged; Enter keys the joints to the pose, R starts again from the animation, Esc leaves.
 */
class LiveTool final : public ViewportTool
{
public:
    const char* name() const override { return "Physics Live"; }

    void activate() override
    {
        mSolver = currentSolver();
        restart();
        mTimer = std::make_unique<QTimer>();
        mTimer->setInterval(16);
        QObject::connect(mTimer.get(), &QTimer::timeout, [this] { tick(); });
        mTimer->start();
    }

    void deactivate() override
    {
        mTimer.reset();
        mWorld.reset();
        mShapes.reset();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mWorld)
            return false;
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        std::optional<double> best;
        int hit = -1;
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            if (auto t = rayHit(mRig.bodies[i].shape, bodyWorld(i), o, d); t && (!best || *t < *best))
                best = t, hit = int(i);
        if (hit < 0)
            return true;
        const glm::dvec3 point = o + d * *best;
        mGrab = hit;
        mGrabLocal = glm::dvec3(glm::inverse(bodyWorld(size_t(hit))) * glm::dvec4(point, 1.0));
        mPlanePoint = point;
        mPlaneNormal = view.viewDir(point);
        mTarget = point;
        mWorld->setGrab(mGrab, mGrabLocal, mTarget);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mGrab < 0 || !mWorld)
            return false;
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        const double den = glm::dot(d, mPlaneNormal);
        if (std::abs(den) > 1e-9)
            mTarget = o + d * (glm::dot(mPlanePoint - o, mPlaneNormal) / den);
        mWorld->setGrab(mGrab, mGrabLocal, mTarget);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mGrab = -1;
        if (mWorld)
            mWorld->setGrab(-1, {}, {});
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: apply(); return true;
        case Qt::Key_R: restart(); return true;
        case Qt::Key_Escape: QTimer::singleShot(0, [] { viewportTools::set(nullptr); }); return true;
        default: return false;
        }
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        if (!mWorld)
        {
            drawToolStatus(p, view, {.tool = "Physics Live", .prompt = "select a ragdoll solver, marker or joint"});
            return;
        }
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
            drawShape(s, view, *geometryOf(mRig.bodies[i].shape), bodyWorld(i), int(i) == mGrab ? brighter(kSimColor, 0.4f) : kSimColor, 0.22f, true);
        if (mGrab >= 0)
        {
            s.linesOnTop.push_back({glm::dvec3(glm::dvec3(bodyWorld(size_t(mGrab)) * glm::dvec4(mGrabLocal, 1.0))), kSimColor});
            s.linesOnTop.push_back({glm::dvec3(mTarget), kSimColor});
        }
        drawToolStatus(p, view,
                       {.tool = "Physics Live",
                        .prompt = "drag the bodies",
                        .keys = {{"Enter", "key pose"}, {"R", "reset to animation"}, {"Esc", "leave"}}});
    }

    std::optional<QCursor> cursor() const override { return QCursor(mGrab >= 0 ? Qt::ClosedHandCursor : Qt::OpenHandCursor); }

private:
    glm::dmat4 bodyWorld(size_t i) const
    {
        const BodyState b = mWorld->body(i);
        glm::dmat4 m = glm::mat4_cast(glm::dquat(b.rotation));
        m[3] = glm::dvec4(glm::dvec3(b.position) * 100.0, 1.0);
        return m;
    }

    void restart()
    {
        mWorld.reset();
        mGrab = -1;
        Graph& g = Graph::current();
        if (!mSolver || !g.node(mSolver))
            return;
        try
        {
            mRig = buildRig(g, mSolver);
            mShapes = std::make_unique<Shapes>(mRig);
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Error, std::string("Physics Live: ") + e.what());
            return;
        }
        g.evaluate();
        std::vector<BodyState> bodies;
        mDrive = {};
        mScale.clear();
        for (const BodyDef& b : mRig.bodies)
        {
            const glm::dmat4 w = g.node(b.joint)->as<glm::dmat4>(Transform::kWorldMatrix);
            const glm::dmat4 r = rigid(w);
            BodyState s;
            s.position = glm::vec3(glm::dvec3(r[3]) / 100.0);
            s.rotation = glm::quat(glm::normalize(glm::quat_cast(glm::dmat3(r))));
            bodies.push_back(s);
            mDrive.bodies.push_back({w, 0.0, 1.0});
            mScale.push_back(glm::dvec3(glm::length(glm::dvec3(w[0])), glm::length(glm::dvec3(w[1])), glm::length(glm::dvec3(w[2]))));
        }
        std::vector<BodyState> colliders;
        for (const ColliderDef& c : mRig.colliders)
        {
            const glm::dmat4 r = rigid(g.node(c.mesh)->as<glm::dmat4>(Transform::kWorldMatrix));
            BodyState s;
            s.position = glm::vec3(glm::dvec3(r[3]) / 100.0);
            s.rotation = glm::quat(glm::normalize(glm::quat_cast(glm::dmat3(r))));
            colliders.push_back(s);
            mDrive.colliders.push_back(r);
        }
        mWorld = std::make_unique<World>(mRig, *mShapes, bodies, colliders, std::vector<std::string>{}, WorldOptions{.live = true});
        viewportTools::requestRedraw();
    }

    void tick()
    {
        if (!mWorld)
            return;
        for (int k = 0; k < 4; ++k)
            mWorld->step(mDrive, 1.0 / 240.0);
        viewportTools::requestRedraw();
    }

    void apply()
    {
        if (!mWorld)
            return;
        Graph& g = Graph::current();
        json pose = json::object();
        for (size_t i = 0; i < mRig.bodies.size(); ++i)
        {
            if (mRig.bodies[i].behaviour == Behaviour::Kinematic)
                continue;
            glm::dmat4 m = bodyWorld(i);
            for (int c = 0; c < 3; ++c)
                m[c] *= mScale[i][c];
            json values = json::array();
            for (int k = 0; k < 16; ++k)
                values.push_back(m[k / 4][k % 4]);
            pose[g.node(mRig.bodies[i].joint)->path()] = values;
        }
        runCommand("physicsApplyPose", {{"pose", pose}});
    }

    NodeId mSolver = kNoNode;
    RigDef mRig;
    std::unique_ptr<Shapes> mShapes;
    std::unique_ptr<World> mWorld;
    std::unique_ptr<QTimer> mTimer;
    Drive mDrive;
    std::vector<glm::dvec3> mScale;
    int mGrab = -1;
    glm::dvec3 mGrabLocal{0.0}, mPlanePoint{0.0}, mPlaneNormal{0.0, 0.0, 1.0}, mTarget{0.0};
};

std::string infoText(NodeId id)
{
    try
    {
        Graph& g = Graph::current();
        const sim::Info i = sim::info(g, id);
        if (!i.error.empty())
            return "Error: " + i.error;
        std::string s = std::to_string(i.outputs.size()) + " bodies, " + std::to_string(i.substeps) + " steps per frame";
        if (i.cachedTo >= i.cachedFrom)
            s += "\nCached frames " + std::to_string(i.cachedFrom) + " to " + std::to_string(i.cachedTo);
        if (i.loop)
            s += "\nLoop " + std::to_string(i.loopStart) + " to " + std::to_string(i.loopEnd) + ": " +
                 (i.converged ? "settled in " + std::to_string(i.passes) + " passes" : "did not settle, seam cross-faded");
        return s;
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
}

/** A labelled button in the app sheet's push style. */
QToolButton* physPush(QWidget* parent, const char* label, const char* tip)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(QString("%1\n%2").arg(label, tip));
    b->setProperty("role", "push");
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

/** Simulate / Record / Snap / Live / Clear buttons and a status line for one solver. */
QWidget* solverControls(NodeId id, QWidget* parent = nullptr)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* row1 = new QHBoxLayout;
    auto* row2 = new QHBoxLayout;
    row1->setSpacing(2);
    row2->setSpacing(2);
    auto* status = new QLabel(w);
    status->setWordWrap(true);
    status->setForegroundRole(QPalette::PlaceholderText);
    auto name = [id] {
        Node* n = Graph::current().node(id);
        return n ? n->path() : std::string();
    };
    auto refresh = [status, id] { status->setText(QString::fromStdString(infoText(id))); };
    auto button = [&](QHBoxLayout* row, const char* label, const char* tip, std::function<void()> fn) {
        QToolButton* b = physPush(w, label, tip);
        QObject::connect(b, &QToolButton::clicked, w, [fn = std::move(fn), refresh] {
            fn();
            refresh();
        });
        row->addWidget(b);
    };
    button(row1, "Simulate", "Simulate up to the current frame from the start or the cache", [name] {
        runCommand("simulate", {{"args", {name()}}, {"frame", sim::frameOf(Graph::current().time())}});
    });
    button(row1, "Record", "Bake to a new additive animation layer", [name] { runCommand("recordPhysics", {{"args", {name()}}}); });
    button(row1, "Replace", "Bake over the joints' own keys across the range", [name] {
        runCommand("recordPhysics", {{"args", {name()}}, {"mode", "replace"}});
    });
    button(row2, "Snap", "Key the joints to the simulated pose at this frame", [name] { runCommand("snapToSimulation", {{"args", {name()}}}); });
    button(row2, "Live", "Drag the ragdoll around in the viewport", [] { viewportTools::activate("physics.live"); });
    button(row2, "Clear Cache", "Throw the cache away; the next frame simulates from the start", [name] {
        runCommand("clearSimCache", {{"args", {name()}}});
    });
    v->addLayout(row1);
    v->addLayout(row2);
    v->addWidget(status);
    refresh();
    return w;
}

/** Fit and select buttons for one marker. */
QWidget* markerControls(NodeId id, QWidget* parent = nullptr)
{
    auto* w = new QWidget(parent);
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    auto name = [id] {
        Node* n = Graph::current().node(id);
        return n ? n->path() : std::string();
    };
    auto button = [&](const char* label, const char* tip, std::function<void()> fn) {
        QToolButton* b = physPush(w, label, tip);
        QObject::connect(b, &QToolButton::clicked, w, std::move(fn));
        row->addWidget(b);
    };
    button("Fit Shape", "Fit the shape to the joint's region of the skinned mesh", [name] {
        runCommand("fitPhysicsMarker", {{"args", {name()}}, {"limits", false}});
    });
    button("Fit Limits", "Fit the swing cones and twist range to the motion over the playback range", [name] {
        runCommand("fitPhysicsMarker", {{"args", {name()}}, {"fitShape", false}});
    });
    button("Select Joint", "Select the joint this marker drives", [id] {
        Graph& g = Graph::current();
        if (Node* m = g.node(id))
            if (NodeId j = jointOf(g, *m))
                runCommand("select", {{"args", {g.node(j)->path()}}});
    });
    return w;
}

QWidget* makePanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    auto* markers = new QGroupBox("Ragdoll", w);
    auto* setup = new QHBoxLayout(markers);
    setup->setContentsMargins(0, 4, 0, 0);
    setup->setSpacing(2);
    auto add = [&](QHBoxLayout* row, const char* label, const char* tip, std::function<void()> fn) {
        QToolButton* b = physPush(w, label, tip);
        QObject::connect(b, &QToolButton::clicked, w, std::move(fn));
        row->addWidget(b);
    };
    add(setup, "Assign Markers", "Physics markers on the selected joints", [] { runCommand("assignPhysicsMarker"); });
    add(setup, "From Template", "A whole ragdoll from the skeleton's semantic labels", [] { runCommand("physicsMarkersFromTemplate"); });
    auto* extras = new QGroupBox("Add", w);
    auto* setup2 = new QHBoxLayout(extras);
    setup2->setContentsMargins(0, 4, 0, 0);
    setup2->setSpacing(2);
    add(setup2, "Pin", "A soft pin on the selected marker or joint", [] { runCommand("createPhysicsPin"); });
    add(setup2, "Collider", "Make the selected meshes environment colliders", [] { runCommand("createPhysicsCollider"); });
    add(setup2, "Wind", "A wind field", [] { runCommand("createPhysicsField", {{"kind", "wind"}}); });
    add(setup2, "Turbulence", "A seeded turbulence field", [] { runCommand("createPhysicsField", {{"kind", "turbulence"}}); });
    v->addWidget(markers);
    v->addWidget(extras);
    auto* list = new QListWidget(w);
    v->addWidget(list, 1);
    auto* empty = new QLabel("No ragdolls. Select the joints and click Assign Markers.", w);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(empty);
    auto* holder = new QVBoxLayout;
    v->addLayout(holder);

    auto fill = [list, empty] {
        Graph& g = Graph::current();
        const QString current = list->currentItem() ? list->currentItem()->text() : QString();
        list->clear();
        for (NodeId id : sim::simulations(g))
            if (Node* n = g.node(id); n && n->type().isA(kSolverType))
                list->addItem(QString::fromStdString(n->path()));
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == current)
                list->setCurrentRow(i);
        list->setVisible(list->count() > 0);
        empty->setVisible(list->count() == 0);
    };
    ui::deletion::claim(list, [list] {
        if (QListWidgetItem* it = list->currentItem())
            runCommand("delete", {{"args", {it->text().toStdString()}}});
    });
    QObject::connect(list, &QListWidget::currentTextChanged, w, [holder, w](const QString& name) {
        while (QLayoutItem* item = holder->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        Graph& g = Graph::current();
        if (Node* n = named(g, name.toStdString()))
        {
            g.select({n->id()});
            holder->addWidget(solverControls(n->id(), w));
        }
    });
    fill();
    std::vector<events::SubscriptionId> subs;
    for (events::Event e : {events::Event::NodeCreated, events::Event::NodeDeleted, events::Event::NodeRenamed, events::Event::SceneNew,
                            events::Event::SceneOpened})
        subs.push_back(events::subscribe(e, [fill](const events::EventData&) { fill(); }, "PhysicsAnimation"));
    QObject::connect(w, &QObject::destroyed, [subs] {
        for (events::SubscriptionId s : subs)
            events::unsubscribe(s);
    });
    return w;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    drawOverrides::add(ctx, {.id = "physics.marker", .nodeType = kMarkerType, .draw = drawMarker, .pick = pickMarker});
    drawOverrides::add(ctx, {.id = "physics.solver", .nodeType = kSolverType, .draw = drawSolver});
    drawOverrides::add(ctx, {.id = "physics.pin", .nodeType = kPinType, .draw = [](drawOverrides::DrawContext& c) { drawCross(c, 8.0); }, .pick = pickPoint});
    drawOverrides::add(ctx, {.id = "physics.field", .nodeType = kFieldType, .draw = drawField, .pick = pickPoint});
    // The mesh it names is what shows.
    drawOverrides::add(ctx, {.id = "physics.collider", .nodeType = kColliderType, .draw = [](drawOverrides::DrawContext&) {}});

    selectionHandlers::add(ctx, {.id = "physics.proxySelect", .label = "Physics body selects its joint", .redirect = redirectProxy, .always = true});

    viewportTools::add(ctx, {.id = "physics.live", .label = "Physics Live Mode", .icon = "hand-grab", .create = [] {
                                 return std::unique_ptr<ViewportTool>(std::make_unique<LiveTool>());
                             }});

    const char* cat = "Animation";
    auto action = [&](const char* id, const char* label, const char* doc, const char* command, json call = nullptr) {
        actions::add(ctx, {.id = id, .label = label, .category = cat, .doc = doc, .command = command,
                           .call = call.is_null() ? json::object() : std::move(call)});
    };
    action("physics.assignMarker", "Assign Physics Marker", "Make the selected joints rigid bodies (physics markers) fitted to the skinned mesh.",
           "assignPhysicsMarker");
    action("physics.fromTemplate", "Ragdoll from Template",
           "Physics markers on every semantically labelled joint of the selected skeleton, with sensible shapes and limits.",
           "physicsMarkersFromTemplate");
    action("physics.fitMarker", "Fit Physics Marker", "Refit the selected markers' shapes to the mesh and limits to the animation.",
           "fitPhysicsMarker");
    action("physics.retarget", "Retarget Physics Marker", "Point the selected marker (first) at the selected joint (second).",
           "retargetPhysicsMarker");
    action("physics.addPin", "Add Physics Pin", "A soft world-space pin on the selected marker or joint.", "createPhysicsPin");
    action("physics.addCollider", "Add Environment Collider", "Make the selected meshes colliders the ragdoll lands on.", "createPhysicsCollider");
    action("physics.addWind", "Add Wind Field", "A wind field on the ragdoll.", "createPhysicsField", {{"kind", "wind"}});
    action("physics.addTurbulence", "Add Turbulence Field", "Seeded turbulence on the ragdoll.", "createPhysicsField", {{"kind", "turbulence"}});
    action("physics.simulate", "Simulate Physics", "Simulate the selected (or every) solver to the current frame.", "simulate");
    action("physics.record", "Record Physics", "Bake the ragdoll to a new additive animation layer.", "recordPhysics");
    action("physics.recordReplace", "Record Physics (Replace)", "Bake the ragdoll over the joints' own keys.", "recordPhysics", {{"mode", "replace"}});
    action("physics.snap", "Snap to Simulation", "Key the joints to the simulated pose at the current frame.", "snapToSimulation");
    action("physics.clearCache", "Clear Physics Cache", "Throw away simulation caches.", "clearSimCache");
    action("physics.hideMeshes", "Hide Skinned Meshes", "Leave only the marker bodies, as a light proxy to animate with.", "physicsProxyDisplay",
           {{"hideMeshes", true}});
    action("physics.showMeshes", "Show Skinned Meshes", "Show the meshes skinned to the ragdoll again.", "physicsProxyDisplay", {{"hideMeshes", false}});
    action("physics.selectMarkers", "Select Physics Markers", "Select the markers of the selected joints.", "selectPhysicsMarkers");
    actions::add(ctx, {.id = "physics.live",
                       .label = "Physics Live Mode",
                       .category = cat,
                       .run = [] { viewportTools::activate("physics.live"); },
                       .doc = "Drag the ragdoll around in the viewport like a puppet; Enter keys the pose."});
    actions::add(ctx, {.id = "physics.proxySelect",
                       .label = "Body Click Selects Joint",
                       .category = cat,
                       .run =
                           [] {
                               PrefStore::get().set("general", kProxySelectPref, !proxySelects());
                               viewportTools::requestRedraw();
                           },
                       .doc = "Toggle whether clicking a physics marker's body selects its joint (on) or the marker itself (off)."});

    panels::add(ctx, {.id = "physicsAnimation", .title = "Physics Animation", .create = makePanel, .icon = "atom", .open = false});

    menus::addItem(ctx, {.id = "physics.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Physics"});
    int sep = 0;
    for (const char* a : {"physics.assignMarker", "physics.fromTemplate", "physics.fitMarker", "physics.retarget", "physics.selectMarkers", "",
                          "physics.addPin", "physics.addCollider", "physics.addWind", "physics.addTurbulence", "", "physics.simulate",
                          "physics.live", "physics.record", "physics.recordReplace", "physics.snap", "physics.clearCache", "",
                          "physics.hideMeshes", "physics.showMeshes", "physics.proxySelect", "", "window.physicsAnimation"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("physics.menu.") + a, .menu = "Animation/Physics", .action = a});
        else
            menus::addItem(ctx, {.id = "physics.menu.sep" + std::to_string(sep++), .menu = "Animation/Physics", .kind = menus::Kind::Separator});
    }

    attributeEditor::addSection(ctx, kSolverType, "Simulation", [](NodeId id) { return solverControls(id); });
    attributeEditor::addSection(ctx, kMarkerType, "Physics Marker", [](NodeId id) { return markerControls(id); });
}
} // namespace physanim
