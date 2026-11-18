#include "ui/TexProjectionTool.h"

#include "render/PaintGpu.h"
#include "tex/Layers.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"

#include <QMouseEvent>
#include <QTimer>

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;
namespace rp = render::paint;

/** layerProjection's rotation (degrees, R = Rz Ry Rx) whose z axis is n and whose x axis turns by spin about it. */
glm::dvec3 rotationFacing(const glm::dvec3& n, double spin)
{
    const glm::dvec3 z = glm::normalize(n);
    const glm::dvec3 up = std::abs(z.y) < 0.95 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 x = glm::normalize(glm::cross(up, z));
    glm::dvec3 y = glm::cross(z, x);
    const double c = std::cos(spin), s = std::sin(spin);
    const glm::dvec3 xs = x * c + y * s;
    y = glm::cross(z, xs);
    x = xs;
    // Columns x, y, z; R = Rz Ry Rx gives R20 = -sin(ry), R21 = cos(ry) sin(rx), R22 = cos(ry) cos(rx), R10 = cos(ry) sin(rz), R00 = cos(ry) cos(rz).
    const double ry = std::asin(std::clamp(-x.z, -1.0, 1.0));
    const double rx = std::atan2(y.z, z.z);
    const double rz = std::atan2(x.y, x.x);
    return glm::degrees(glm::dvec3(rx, ry, rz));
}

glm::dmat3 rotationOf(const glm::dvec3& deg)
{
    const glm::dvec3 r = glm::radians(deg);
    const glm::dmat4 m = glm::rotate(glm::dmat4(1.0), r.z, {0, 0, 1}) * glm::rotate(glm::dmat4(1.0), r.y, {0, 1, 0}) *
                         glm::rotate(glm::dmat4(1.0), r.x, {1, 0, 0});
    return glm::dmat3(m);
}

/**
 * Places a fill or procedural layer's (or a mask's) projection on the mesh: a click puts the projector on the surface
 * facing it, a drag moves it along the surface, Shift turns it about the surface, Ctrl scales it. The box shows where it
 * lands; each release is one undo step.
 */
class ProjectionTool final : public ViewportTool
{
public:
    ProjectionTool(std::string set, std::string layer, std::string mask) : mSet(std::move(set)), mLayer(std::move(layer)), mMask(std::move(mask))
    {
        load();
    }

    const char* name() const override { return "texProjection"; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mFrom = e->position();
        mStart = mProj;
        mDrag = (e->modifiers() & Qt::ShiftModifier) ? Drag::Turn : (e->modifiers() & Qt::ControlModifier) ? Drag::Scale : Drag::Move;
        if (mDrag == Drag::Move)
            placeAt(e->position(), view);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mDrag == Drag::None)
            return false;
        const QPointF d = e->position() - mFrom;
        if (mDrag == Drag::Move)
            placeAt(e->position(), view);
        else if (mDrag == Drag::Turn)
        {
            mSpin = mStartSpin + d.x() * 0.01;
            const glm::dvec3 r = rotationFacing(mNormal, mSpin);
            mProj["rotationX"] = r.x;
            mProj["rotationY"] = r.y;
            mProj["rotationZ"] = r.z;
        }
        else
        {
            const double k = std::exp((d.x() - d.y()) / 200.0);
            for (const char* a : {"sizeX", "sizeY", "sizeZ"})
                mProj[a] = std::max(1e-3, mStart.value(a, 100.0) * k);
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent*, const ViewInfo&) override
    {
        if (mDrag == Drag::None)
            return false;
        mDrag = Drag::None;
        mStartSpin = mSpin;
        store();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        ToolStatus status{.tool = "Place Projection"};
        status.subject = QString::fromStdString(mLayer + (mMask.empty() ? std::string() : " " + mMask));
        status.keys = {{"click/drag", "place on surface"}, {"Shift+drag", "turn"}, {"Ctrl+drag", "scale"}, {"Enter", "done"}};
        drawToolStatus(p, view, status);
        scene::Node* mesh = scene::Graph::current().find(mMeshPath);
        if (!mesh)
            return;
        const glm::dmat4 world = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const glm::dvec3 c(mProj.value("positionX", 0.0), mProj.value("positionY", 0.0), mProj.value("positionZ", 0.0));
        const glm::dvec3 half = glm::dvec3(mProj.value("sizeX", 100.0), mProj.value("sizeY", 100.0), mProj.value("sizeZ", 100.0)) * 0.5;
        const glm::dmat3 r = rotationOf({mProj.value("rotationX", 0.0), mProj.value("rotationY", 0.0), mProj.value("rotationZ", 0.0)});
        std::optional<QPointF> corner[8];
        for (int k = 0; k < 8; ++k)
        {
            const glm::dvec3 l((k & 1) ? half.x : -half.x, (k & 2) ? half.y : -half.y, (k & 4) ? half.z : -half.z);
            corner[k] = view.project(glm::dvec3(world * glm::dvec4(c + r * l, 1.0)));
        }
        const glm::vec4 ink = p.ink(0.9f);
        for (int a = 0; a < 8; ++a)
            for (int b = a + 1; b < 8; ++b)
                if (std::popcount(unsigned(a ^ b)) == 1 && corner[a] && corner[b])
                    p.line(*corner[a], *corner[b], (a & 4) && (b & 4) ? 2.0f : 1.0f, ink);
    }

private:
    enum class Drag
    {
        None,
        Move,
        Turn,
        Scale,
    };

    /** The projection as stored, its mesh, and defaults for what it doesn't set yet. */
    void load()
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* set = tex::findSet(g, mSet);
        if (!set)
            return;
        const tex::StackState st = tex::readStack(tex::readGraph(*set), tex::readSettings(*set));
        if (const tex::Layer* l = st.stack.find(mLayer))
        {
            json proj = l->projection;
            if (!mMask.empty())
                for (const tex::Mask& m : l->masks)
                    if (m.id == mMask)
                        proj = m.projection;
            mProj = proj.is_object() ? proj : json::object();
        }
        if (!tex::hasProjection(mProj))
            mProj["mode"] = "planar";
        mMeshPath = meshFor(g, *set);
        mPaint = mMeshPath.empty() ? nullptr : rp::paintMesh(g, mMeshPath, rp::setMaterial(*set));
    }

    static std::string meshFor(scene::Graph& g, const scene::Node& set)
    {
        g.evaluate();
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh); m && m->numFaces())
                {
                    const auto& names = m->materialNames();
                    if (rp::usesSet(names, set))
                        return n->path();
                }
        return {};
    }

    void placeAt(QPointF px, const ViewInfo& view)
    {
        scene::Node* mesh = scene::Graph::current().find(mMeshPath);
        if (!mesh || !mPaint)
            return;
        const glm::dmat4 inv = glm::inverse(mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        glm::dvec3 o, d;
        view.ray(px, o, d);
        geo::BvhHit hit;
        if (!mPaint->bvh->raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit, std::numeric_limits<double>::max(), false))
            return;
        mNormal = glm::dvec3(mPaint->normal(uint32_t(hit.triangle), float(hit.bary.y), float(hit.bary.z)));
        mProj["positionX"] = hit.point.x;
        mProj["positionY"] = hit.point.y;
        mProj["positionZ"] = hit.point.z;
        const glm::dvec3 r = rotationFacing(mNormal, mSpin);
        mProj["rotationX"] = r.x;
        mProj["rotationY"] = r.y;
        mProj["rotationZ"] = r.z;
    }

    void store()
    {
        const json p = mProj;
        try
        {
            if (mMask.empty())
                ops::execute("texSetLayer", {{"args", {mSet, mLayer, {{"projection", p}}}}}, CallSource::UI);
            else
                ops::execute("texSetMask", {{"args", {mSet, mLayer, mMask, {{"projection", p}}}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("Place Projection: {}", e.what());
        }
    }

    std::string mSet, mLayer, mMask, mMeshPath;
    std::shared_ptr<const tex::paint::PaintMesh> mPaint;
    json mProj = json::object();
    json mStart;
    glm::dvec3 mNormal{0.0, 0.0, 1.0};
    double mSpin = 0.0, mStartSpin = 0.0;
    QPointF mFrom;
    Drag mDrag = Drag::None;
};
} // namespace

void startProjectionPlacement(const std::string& set, const std::string& layer, const std::string& mask)
{
    viewportTools::set(std::make_unique<ProjectionTool>(set, layer, mask));
}
} // namespace lila::ui
