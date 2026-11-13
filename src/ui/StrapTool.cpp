#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/StrapNode.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Isolate.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <array>

namespace lila::ui
{
namespace
{
using namespace curveTools;

constexpr const char* kStrapPrefs = "/strapTool/";
constexpr size_t kMaxStrapMeshes = 8;
const std::array<const char*, 5> kStrapPresets = {"strap", "rope", "tape", "metalBand", "zipTie"};
const std::array<const char*, 5> kStrapPresetLabels = {"strap", "rope", "tape", "metal band", "zip tie"};

double strapPrefD(const char* key, double def) { return prefs().value<double>("general", std::string(kStrapPrefs) + key, def); }
bool strapPrefB(const char* key, bool def) { return prefs().value<bool>("general", std::string(kStrapPrefs) + key, def); }
void setStrapPref(const char* key, const json& v) { prefs().set("general", std::string(kStrapPrefs) + key, v); }

/** What the selection and visibility say to strap, as many as a slash takes. */
std::vector<curve::StrapTarget> strapTargets()
{
    std::vector<curve::StrapTarget> out = curve::strapTargets(scene::Graph::current());
    if (out.size() > kMaxStrapMeshes)
        out.resize(kMaxStrapMeshes);
    return out;
}

/**
 * Strap: drag a slash across the meshes and a strap goes round what it crosses, in the plane of the slash edge on to the view,
 * previewed as you drag. Several loops crossed are bundled into one strap. P cycles the presets (strap, rope, tape, metal band,
 * zip tie), Ctrl+scroll sets the width, Shift+scroll how wide a hollow it spans, T pulls it taut. Straps what was selected
 * when the tool started (selected faces only, in component mode), or every visible mesh, so each new strap can go on the
 * same things. Hidden meshes and faces isolation hides are left out.
 */
class StrapTool final : public ViewportTool
{
public:
    const char* name() const override { return "Strap"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        mPreset = int(strapPrefD("preset", 0.0)) % int(kStrapPresets.size());
        mWidth = strapPrefD("width", presetWidth());
        mBridge = strapPrefD("bridge", 0.0);
        mTaut = strapPrefB("taut", false);
        mTargets = strapTargets();
        log::info("Strap: drag a slash across what to strap, P: preset, Ctrl+scroll: width, Shift+scroll: bridge, T: taut");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        scene::Graph& g = scene::Graph::current();
        std::erase_if(mTargets, [&](const curve::StrapTarget& t) { return !g.node(t.node) || !scene::effectivelyVisible(g, t.node); });
        if (mTargets.empty())
            mTargets = strapTargets();
        if (mTargets.empty())
        {
            log::warn("Strap: no mesh to strap");
            return true;
        }
        mMeshes.clear();
        for (const curve::StrapTarget& t : mTargets)
            if (scene::Node* node = g.node(t.node))
                if (scene::MeshPtr m = curve::strapFaces(node->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), t.faces))
                    mMeshes.push_back({m, node->as<glm::dmat4>(scene::Transform::kWorldMatrix)});
        mFrom = mTo = e->position();
        mDragging = true;
        mView = view.camera;
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDragging)
            return false;
        mTo = e->position();
        updatePreview(view);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        mTo = e->position();
        if (distPx(mFrom, mTo) >= 4.0)
            build(view);
        mPreview = {};
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo& view) override
    {
        const double steps = e->angleDelta().y() / 120.0;
        if (e->modifiers() & Qt::ControlModifier)
        {
            mWidth = std::clamp(mWidth * std::pow(1.1, steps), 0.01, 1e4);
            setStrapPref("width", mWidth);
        }
        else if (e->modifiers() & Qt::ShiftModifier)
        {
            mBridge = std::max(0.0, std::round((mBridge + steps * std::max(0.5, mBridge * 0.1)) * 10.0) / 10.0);
            setStrapPref("bridge", mBridge);
        }
        else
            return false;
        if (mDragging)
            updatePreview(view);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        switch (e->key())
        {
        case Qt::Key_P:
            mPreset = (mPreset + 1) % int(kStrapPresets.size());
            setStrapPref("preset", mPreset);
            mWidth = presetWidth();
            setStrapPref("width", mWidth);
            break;
        case Qt::Key_T:
            mTaut = !mTaut;
            setStrapPref("taut", mTaut);
            break;
        case Qt::Key_Escape:
            if (!mDragging)
                exitTool();
            mDragging = false;
            mPreview = {};
            break;
        default: return false;
        }
        if (mDragging)
            updatePreview(view);
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        // P and T are app hotkeys too; the tool takes them while active.
        return plainKey(e) && (e.key() == Qt::Key_P || e.key() == Qt::Key_T) && keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{});
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        if (mDragging && view.camera == mView)
        {
            p.line(mFrom, mTo, 1.5f, kPreview);
            if (mPreview.path)
                drawSpline(f, *mPreview.path, kGhost);
            const geo::Mesh& m = mPreviewMesh;
            for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
                if (m.edgeValid(e))
                {
                    f.lines.push_back({glm::dvec3(m.position(m.edgePoint(e, 0))), kPreview});
                    f.lines.push_back({glm::dvec3(m.position(m.edgePoint(e, 1))), kPreview});
                }
            if (!mPreview.warning.empty())
                drawToolMessage(p, view, QString::fromStdString("Strap: " + mPreview.warning));
        }
        drawToolStatus(p, view,
                       {.tool = "Strap",
                        .mode = kStrapPresetLabels[size_t(mPreset)],
                        .fields = {{"width", QString("%1 cm").arg(mWidth, 0, 'f', 2)}, {"bridge", QString("%1 cm").arg(mBridge, 0, 'f', 1)}},
                        .flags = {{"taut", mTaut}},
                        .prompt = "drag a slash across what to strap",
                        .keys = {{"P", "preset"}, {"Ctrl+scroll", "width"}, {"Shift+scroll", "bridge"}, {"T", "taut"}, {"Esc", "exit"}}});
    }

private:
    double presetWidth() const { return curve::strapPreset(kStrapPresets[size_t(mPreset)]).value("width", 4.0); }

    /** The preset's values with the tool's own settings on top: what a strap is built with. */
    json values() const
    {
        json v = curve::strapPreset(kStrapPresets[size_t(mPreset)]);
        v["width"] = mWidth;
        v["bridge"] = mBridge;
        v["taut"] = mTaut;
        return v;
    }

    curve::StrapSlash slashOf(const ViewInfo& view) const
    {
        curve::StrapSlash s;
        view.ray(mFrom, s.start, s.startDir);
        view.ray(mTo, s.end, s.endDir);
        return s;
    }

    void updatePreview(const ViewInfo& view)
    {
        LILA_ZONE();

        std::vector<geo::SectionMesh> meshes;
        for (const auto& [m, world] : mMeshes)
            meshes.push_back({m.get(), world});
        const json v = values();
        mPreview = curve::strapPath(meshes, slashOf(view), {.gap = v.value("gap", 0.0), .bridge = mBridge, .taut = mTaut, .tolerance = 0.01, .width = mWidth});
        mPreviewMesh = {};
        if (mPreview.path)
        {
            curve::StrapOptions o = curve::strapOptionsFrom(v);
            o.up = mPreview.up;
            mPreviewMesh = curve::makeStrap(*mPreview.path, o).mesh;
        }
    }

    void build(const ViewInfo& view)
    {
        const curve::StrapSlash s = slashOf(view);
        json meshes = json::array(), faces = json::array();
        for (const curve::StrapTarget& t : mTargets)
            if (const scene::Node* n = scene::Graph::current().node(t.node))
            {
                meshes.push_back(n->path());
                faces.push_back(t.faces ? json(*t.faces) : json());
            }
        json call = values();
        call["args"] = meshes;
        call["faces"] = faces;
        call["start"] = vecJson(s.start);
        call["end"] = vecJson(s.end);
        call["startDir"] = vecJson(s.startDir);
        call["endDir"] = vecJson(s.endDir);
        try
        {
            ops::execute("strap", call, CallSource::UI);
        }
        catch (const CommandError& err)
        {
            log::error("{}", err.what());
        }
    }

    std::vector<curve::StrapTarget> mTargets;
    /** The targets' meshes trimmed to the faces that count, taken when the slash starts. */
    std::vector<std::pair<scene::MeshPtr, glm::dmat4>> mMeshes;
    QPointF mFrom, mTo;
    bool mDragging = false;
    std::string mView;
    curve::StrapPathResult mPreview;
    geo::Mesh mPreviewMesh;
    int mPreset = 0;
    double mWidth = 4.0, mBridge = 0.0;
    bool mTaut = false;
};
} // namespace

void registerStrapTool()
{
    if (!viewportTools::registry().find("curve.strapTool"))
        viewportTools::add({.id = "curve.strapTool", .label = "Strap Tool", .icon = "gift", .create = [] { return std::make_unique<StrapTool>(); }});
    actions::add({.id = "curve.strapTool",
                  .label = "Strap Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.strapTool"); },
                  .doc = "Straps the selected meshes: drag a slash across them and a strap, rope or band goes round what it crosses, "
                         "kept live. P cycles the presets.",
                  .python = "lila.ui.run_action(\"curve.strapTool\")"});
    for (size_t k = 0; k < kStrapPresets.size(); ++k)
    {
        std::string label = kStrapPresetLabels[k];
        for (size_t c = 0; c < label.size(); ++c)
            if (c == 0 || label[c - 1] == ' ')
                label[c] = char(std::toupper(label[c]));
        std::string id = kStrapPresets[k];
        id[0] = char(std::toupper(id[0]));
        actions::add({.id = "curve.strapPreset" + id,
                      .label = "Strap Preset: " + label,
                      .category = "Curves",
                      .doc = "Applies the " + std::string(kStrapPresetLabels[k]) + " preset to the selected straps.",
                      .python = ops::toPython("strapPreset", {{"preset", kStrapPresets[k]}}),
                      .command = "strapPreset",
                      .call = {{"preset", kStrapPresets[k]}}});
    }
}
} // namespace lila::ui
