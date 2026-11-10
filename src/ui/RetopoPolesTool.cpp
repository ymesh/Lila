#include "ui/RetopoPolesTool.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/ToolColors.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
using geo::Index;

constexpr const char* kFacesPref = "/retopoPoles/targetFaces";

// Overlay colours are sRGB; the 3D streams take linear. Poles are data, so they are coloured by valence.
const glm::vec4 kThree(0.25f, 0.72f, 0.95f, 1.0f);
const glm::vec4 kFive(0.98f, 0.58f, 0.2f, 1.0f);
const glm::vec4 kOther(0.9f, 0.35f, 0.9f, 1.0f);
const glm::vec4 kPreviewLinear(0.3f, 0.3f, 0.3f, 1.0f);

glm::vec4 poleColor(int valence) { return valence == 3 ? kThree : valence == 5 ? kFive : kOther; }

glm::dvec3 transformPoint(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }
} // namespace

RetopoPolesTool::RetopoPolesTool(scene::NodeId source) : mSource(source) {}

RetopoPolesTool::~RetopoPolesTool()
{
    if (mJob.valid())
        mJob.wait();
}

int RetopoPolesTool::targetFaces() { return std::clamp(PrefStore::get().value<int>("general", kFacesPref, 3000), 50, 200000); }

void RetopoPolesTool::setTargetFaces(int faces) { PrefStore::get().set("general", kFacesPref, std::clamp(faces, 50, 200000)); }

void RetopoPolesTool::activate()
{
    mTimer = std::make_unique<QTimer>();
    QObject::connect(mTimer.get(), &QTimer::timeout, [this] { poll(); });
    mTimer->start(30);
    refresh();
    solve(true);
    log::info("Retopo Poles: drag poles, Ctrl-click adds a 3/5 pair, Ctrl+Alt-click removes a pair, Ctrl-wheel sets the face "
              "count, Backspace resets, Enter builds, Esc exits");
}

void RetopoPolesTool::deactivate()
{
    if (mTimer)
        mTimer->stop();
    mTimer.reset();
}

bool RetopoPolesTool::refresh()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.node(mSource);
    scene::MeshPtr mesh = n && n->type().isA("mesh") ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
    glm::dmat4 world = n ? n->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    int axis = n && n->type().isA("mesh") ? int(n->as<int64_t>(scene::MeshNode::kSymmetry)) - 1 : -1;
    if (mesh == mMesh && world == mWorld && axis == mSymmetryAxis)
        return false;
    mMesh = mesh;
    mWorld = world;
    mSymmetryAxis = axis >= 0 && axis < 3 ? axis : -1;
    if (mesh)
    {
        geo::Mesh w = *mesh;
        w.compact();
        for (glm::dvec3& p : w.positions())
            p = transformPoint(world, p);
        mBvh.build(w);
    }
    else
        mBvh = geo::MeshBvh();
    return true;
}

retopo::QuadRemeshOptions RetopoPolesTool::options() const
{
    retopo::QuadRemeshOptions o;
    o.targetFaces = targetFaces();
    o.symmetryAxis = mSymmetryAxis;
    o.timeout = 20.0;
    return o;
}

void RetopoPolesTool::solve(bool automatic)
{
    if (!mMesh)
        return;
    if (mJob.valid())
    {
        // One solve at a time; the newest request runs when this one is done.
        mQueued = true;
        mQueuedAutomatic = automatic;
        return;
    }
    retopo::QuadRemeshOptions o = options();
    if (!automatic)
    {
        glm::dmat4 toLocal = glm::inverse(mWorld);
        std::vector<retopo::Pole> local;
        for (const retopo::Pole& p : mPoles)
            local.push_back({transformPoint(toLocal, p.position), p.valence});
        o.poles = std::move(local);
    }
    uint64_t generation = ++mGeneration;
    mJob = std::async(std::launch::async, [mesh = mMesh, o, automatic, generation]() mutable {
        Solved r;
        r.generation = generation;
        if (automatic)
        {
            retopo::PoleResult found = retopo::findPoles(*mesh, o);
            if (!found.ok())
            {
                r.error = found.error;
                return r;
            }
            r.poles = found.poles;
            r.shellTotals = found.shellTotals;
            o.poles = std::move(found.poles);
        }
        retopo::QuadRemeshResult q = retopo::quadRemesh(*mesh, o);
        if (q.ok())
            r.preview = std::move(q.mesh);
        else
            r.error = q.error;
        return r;
    });
    viewportTools::requestRedraw();
}

void RetopoPolesTool::poll()
{
    if (!mJob.valid() || mJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    Solved r = mJob.get();
    mError = r.error;
    // Poles found automatically only replace the user's while they haven't touched them since.
    const bool editedSince = mDrag >= 0 || (mQueued && !mQueuedAutomatic);
    if (r.poles && r.generation == mGeneration && !editedSince)
    {
        mPoles.clear();
        for (const retopo::Pole& p : *r.poles)
            mPoles.push_back({transformPoint(mWorld, p.position), p.valence});
        mShellTotals = r.shellTotals;
    }
    if (r.error.empty())
    {
        mPreviewEdges.clear();
        const geo::Mesh& m = r.preview;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
                mPreviewEdges.emplace_back(transformPoint(mWorld, m.position(m.edgePoint(e, 0))),
                                           transformPoint(mWorld, m.position(m.edgePoint(e, 1))));
    }
    if (mQueued)
    {
        mQueued = false;
        solve(mQueuedAutomatic);
    }
    viewportTools::requestRedraw();
}

bool RetopoPolesTool::surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const
{
    if (mBvh.empty())
        return false;
    glm::dvec3 o, d;
    view.ray(px, o, d);
    geo::BvhHit hit;
    if (!mBvh.raycast(o, d, hit))
        return false;
    out = hit.point;
    return true;
}

bool RetopoPolesTool::visible(const glm::dvec3& p, const ViewInfo& view) const
{
    if (mBvh.empty())
        return true;
    glm::dvec3 d = p - view.eye;
    double len = glm::length(d);
    // A little slack so points lying on the surface aren't hidden by it.
    double slack = view.unitsPerPixel(p) * 3.0;
    return len <= slack || !mBvh.occluded(view.eye, d / len, len - slack);
}

int RetopoPolesTool::pick(QPointF px, const ViewInfo& view) const
{
    int best = -1;
    double bestD = kPickRadiusPx;
    for (size_t i = 0; i < mPoles.size(); ++i)
        if (auto s = view.project(mPoles[i].position); s && distPx(*s, px) < bestD && visible(mPoles[i].position, view))
        {
            bestD = distPx(*s, px);
            best = int(i);
        }
    return best;
}

int RetopoPolesTool::mirrorOf(int i) const
{
    if (mSymmetryAxis < 0 || i < 0)
        return -1;
    glm::dvec3 local = transformPoint(glm::inverse(mWorld), mPoles[i].position);
    local[mSymmetryAxis] = -local[mSymmetryAxis];
    glm::dvec3 target = transformPoint(mWorld, local);
    int best = -1;
    double bestD = std::max(1e-6, glm::length(mPoles[i].position - target) * 0.25);
    for (size_t j = 0; j < mPoles.size(); ++j)
        if (int(j) != i && mPoles[j].valence == mPoles[i].valence && glm::length(mPoles[j].position - target) < bestD)
        {
            bestD = glm::length(mPoles[j].position - target);
            best = int(j);
        }
    return best;
}

void RetopoPolesTool::addPair(QPointF px, const ViewInfo& view)
{
    glm::dvec3 at;
    if (!surfaceHit(px, view, at) || !mMesh)
        return;
    // Two lattice cells apart, sideways on screen.
    double area = 0.0;
    for (Index f = 0; f < mMesh->faceCapacity(); ++f)
        if (mMesh->faceValid(f))
            area += mMesh->faceArea(f);
    double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(mWorld))));
    double spacing = 2.0 * scale * std::sqrt(area / targetFaces());
    glm::dvec3 other = at;
    if (!surfaceHit(px + QPointF(spacing / std::max(view.unitsPerPixel(at), 1e-12), 0.0), view, other))
        return;
    std::vector<retopo::Pole> added = {{at, 5}, {other, 3}};
    if (mSymmetryAxis >= 0)
    {
        glm::dmat4 toLocal = glm::inverse(mWorld);
        for (const retopo::Pole& p : std::vector<retopo::Pole>(added))
        {
            glm::dvec3 local = transformPoint(toLocal, p.position);
            if (std::abs(local[mSymmetryAxis]) < 1e-6)
                continue;
            local[mSymmetryAxis] = -local[mSymmetryAxis];
            added.push_back({transformPoint(mWorld, local), p.valence});
        }
    }
    mPoles.insert(mPoles.end(), added.begin(), added.end());
    solve(false);
}

void RetopoPolesTool::removePair(int i)
{
    if (i < 0)
        return;
    int j = -1;
    double best = std::numeric_limits<double>::max();
    for (size_t k = 0; k < mPoles.size(); ++k)
        if (int(k) != i && (4 - mPoles[k].valence) == -(4 - mPoles[i].valence) && glm::length(mPoles[k].position - mPoles[i].position) < best)
        {
            best = glm::length(mPoles[k].position - mPoles[i].position);
            j = int(k);
        }
    if (j < 0)
    {
        log::warn("Retopo Poles: no pole of the other kind to remove with it; the count has to stay balanced");
        return;
    }
    std::vector<int> gone = {i, j, mirrorOf(i), mirrorOf(j)};
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    for (auto it = gone.rbegin(); it != gone.rend(); ++it)
        if (*it >= 0)
            mPoles.erase(mPoles.begin() + *it);
    mHover = -1;
    solve(false);
}

void RetopoPolesTool::build()
{
    scene::Node* n = scene::Graph::current().node(mSource);
    if (!n)
        return;
    json poles = json::array();
    for (const retopo::Pole& p : mPoles)
        poles.push_back({{"position", {p.position.x, p.position.y, p.position.z}}, {"valence", p.valence}});
    json call = {{"args", {n->path()}}, {"targetFaces", targetFaces()}, {"poles", poles}};
    call["symmetry"] = mSymmetryAxis >= 0 ? std::string(1, char('x' + mSymmetryAxis)) : std::string("off");
    try
    {
        json r = ops::execute("quadRemesh", call, CallSource::UI);
        log::info("Retopo Poles: built {}", r.value("name", std::string()));
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

ToolStatus RetopoPolesTool::status() const
{
    int three = 0, five = 0;
    for (const retopo::Pole& p : mPoles)
    {
        three += p.valence == 3;
        five += p.valence == 5;
    }
    return {.tool = "Retopo Poles",
            .fields = {{"three-edge", QString::number(three)}, {"five-edge", QString::number(five)}, {"faces", QString("~%1").arg(targetFaces())}},
            .flags = {{"solving", mJob.valid()}},
            .keys = {{"drag pole", "move"},
                     {"Ctrl-click", "add a 3/5 pair"},
                     {"Ctrl+Alt-click", "remove a pair"},
                     {"Ctrl-wheel", "faces"},
                     {"Backspace", "reset"},
                     {"Enter", "build"},
                     {"Esc", "exit"}}};
}

bool RetopoPolesTool::mousePress(QMouseEvent* e, const ViewInfo& view)
{
    const bool ctrl = e->modifiers() & Qt::ControlModifier, alt = e->modifiers() & Qt::AltModifier;
    if (e->button() != Qt::LeftButton || (alt && !ctrl))
        return false;
    if (ctrl && alt)
    {
        removePair(pick(e->position(), view));
        return true;
    }
    if (ctrl)
    {
        addPair(e->position(), view);
        return true;
    }
    mDrag = pick(e->position(), view);
    mDragMirror = mirrorOf(mDrag);
    return mDrag >= 0;
}

bool RetopoPolesTool::mouseMove(QMouseEvent* e, const ViewInfo& view)
{
    mCursorView = view.camera;
    if (mDrag < 0)
    {
        int h = pick(e->position(), view);
        if (h != mHover)
        {
            mHover = h;
            viewportTools::requestRedraw();
        }
        return false;
    }
    glm::dvec3 hit;
    if (surfaceHit(e->position(), view, hit))
    {
        mPoles[mDrag].position = hit;
        if (mDragMirror >= 0)
        {
            glm::dvec3 local = transformPoint(glm::inverse(mWorld), hit);
            local[mSymmetryAxis] = -local[mSymmetryAxis];
            mPoles[mDragMirror].position = transformPoint(mWorld, local);
        }
        viewportTools::requestRedraw();
    }
    return true;
}

bool RetopoPolesTool::mouseRelease(QMouseEvent* e, const ViewInfo& view)
{
    (void)e, (void)view;
    if (mDrag < 0)
        return false;
    mDrag = mDragMirror = -1;
    solve(false);
    return true;
}

bool RetopoPolesTool::wheel(QWheelEvent* e, const ViewInfo& view)
{
    (void)view;
    if (!(e->modifiers() & Qt::ControlModifier))
        return false;
    int faces = targetFaces();
    setTargetFaces(int(std::lround(e->angleDelta().y() > 0 ? faces * 1.15 : faces / 1.15)));
    solve(mPoles.empty());
    return true;
}

bool RetopoPolesTool::keyPress(QKeyEvent* e, const ViewInfo& view)
{
    (void)view;
    switch (e->key())
    {
    case Qt::Key_Escape:
        // Leaving deletes the tool, so it has to wait until this event is done with it.
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        build();
        return true;
    case Qt::Key_Backspace:
        solve(true);
        return true;
    default:
        return false;
    }
}

void RetopoPolesTool::draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    if (refresh())
        solve(true);
    for (const auto& [a, b] : mPreviewEdges)
    {
        s.lines.push_back({glm::vec3(a), kPreviewLinear});
        s.lines.push_back({glm::vec3(b), kPreviewLinear});
    }
    for (size_t i = 0; i < mPoles.size(); ++i)
    {
        const retopo::Pole& pole = mPoles[i];
        if (!visible(pole.position, view))
            continue;
        if (auto sp = view.project(pole.position))
        {
            bool hot = int(i) == mHover || int(i) == mDrag || int(i) == mDragMirror;
            p.disc(*sp, hot ? 8.0f : 6.5f, hot ? kHotColor : kHandleRim, 20);
            p.disc(*sp, hot ? 6.0f : 4.5f, poleColor(pole.valence), 20);
        }
    }
    if (!mMesh)
        drawToolStatus(p, view, {.tool = "Retopo Poles", .prompt = "the mesh is gone"});
    else
    {
        drawToolStatus(p, view, status());
        if (!mError.empty())
            drawToolMessage(p, view, QString::fromStdString(mError));
    }
}

void registerRetopoPolesActions()
{
    actions::add({
        .id = "retopo.poles",
        .label = "Retopo Poles",
        .category = "Mesh",
        .run =
            [] {
                scene::Graph& g = scene::Graph::current();
                for (scene::NodeId id : g.selection())
                    if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                    {
                        viewportTools::set(std::make_unique<RetopoPolesTool>(id));
                        return;
                    }
                log::warn("Retopo Poles: select the mesh to remesh");
            },
        .doc = "Shows where an auto quad remesh of the selected mesh would turn its edge flow (three- and five-edge poles) with a "
               "live preview, and lets you move them: drag a pole, Ctrl-click to add a 3/5 pair, Ctrl+Alt-click to remove a "
               "pair, Ctrl-wheel for the face count, Backspace to reset, Enter to build the mesh.",
        .python = "lila.ui.run_action(\"retopo.poles\")",
    });
}
} // namespace lila::ui
