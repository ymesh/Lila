#include "ui/AutoBevelUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/AutoBevel.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/NodeLookup.h"
#include "ui/UiCommands.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "scene/SceneIndex.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;

const glm::vec4 kEdgeColor(0.8f, 0.8f, 0.8f, 1.0f);
const glm::vec4 kLimitingColor(0.95f, 0.22f, 0.18f, 1.0f);
const glm::vec4 kFreeColor(0.25f, 0.5f, 1.0f, 1.0f);

/** Whether the clamp held the bevel's offset back. */
bool clamped(const scene::BevelEdgeSet& set)
{
    return !set.limits.empty() && set.applied < set.offset * (1.0 - 1e-6);
}

/**
 * How much an edge holds a clamped bevel back, 0 for the edges setting the applied offset to 1 for those leaving the asked
 * offset alone, on a log scale since limits span orders of magnitude.
 */
float freedom(const scene::BevelEdgeSet& set, geo::Index e)
{
    const double limit = e < set.limits.size() ? set.limits[e] : std::numeric_limits<double>::infinity();
    if (!(limit < set.offset) || set.applied <= 0.0)
        return 1.0f;
    return float(std::clamp(std::log(limit / set.applied) / std::log(set.offset / set.applied), 0.0, 1.0));
}

void drawBevelEdges(render::FrameState& s, OverlayPainter&, const ViewInfo& view)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<NodeId>& sel = g.selection();
    for (scene::Node* n : scene::sceneIndex::matching(g, "autoBevel", scene::autoBevel::isAutoBevel))
    {
        const NodeId id = n->id();
        const bool editing = scene::autoBevel::editing(*n);
        bool picked = std::find(sel.begin(), sel.end(), id) != sel.end();
        if (!editing && !picked && !n->as<bool>(n->type().attrIndex("showEdges")))
            continue;
        NodeId meshId = scene::autoBevel::meshOf(g, id);
        scene::Node* mesh = g.node(meshId);
        if (!mesh || !scene::effectivelyVisible(g, meshId))
            continue;
        auto set = scene::autoBevel::chosen(g, id);
        if (!set || !set->mesh)
            continue;
        const geo::Mesh& m = *set->mesh;
        // A clamped bevel colours its edges from the ones holding the offset back (red, thick) to free ones (blue). While
        // editing, normal selection shows the edges, so only the ones holding it back are drawn.
        const bool heat = clamped(*set);
        if (editing && !heat)
            continue;
        glm::dmat4 world = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index e : set->edges)
        {
            if (!m.edgeValid(e))
                continue;
            const float t = heat ? freedom(*set, e) : 1.0f;
            if (editing && t >= 1.0f)
                continue;
            const glm::vec4 color = heat ? glm::mix(kLimitingColor, kFreeColor, t) : kEdgeColor;
            const int half = heat && t < 0.5f ? 2 : 1;
            glm::dvec3 a = glm::dvec3(world * glm::dvec4(m.position(m.edgePoint(e, 0)), 1.0));
            glm::dvec3 b = glm::dvec3(world * glm::dvec4(m.position(m.edgePoint(e, 1)), 1.0));
            glm::dvec3 mid = (a + b) * 0.5;
            glm::dvec3 side = glm::cross(b - a, view.viewDir(mid));
            double len = glm::length(side);
            // Three lines a pixel apart read as one thick one.
            glm::dvec3 step = len > 0.0 ? side / len * view.unitsPerPixel(mid) : glm::dvec3(0.0);
            for (int k = -half; k <= half; ++k)
            {
                s.linesOnTop.push_back({glm::dvec3(a + step * double(k)), color});
                s.linesOnTop.push_back({glm::dvec3(b + step * double(k)), color});
            }
        }
    }
}

QPushButton* pushButton(const QString& text, const QString& tip, std::function<void()> fn)
{
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
    return b;
}

/** Status line and Edit Edges buttons for one modifier, kept current by a graph listener. */
QWidget* modifierPanel(NodeId mod, bool withSelect)
{
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    v->setSpacing(3);
    auto* status = new QLabel;
    status->setStyleSheet("color: #9a9a9a;");
    v->addWidget(status);

    auto name = [mod] {
        scene::Node* n = Graph::current().node(mod);
        return n ? n->path() : std::string();
    };
    auto call = [name](const char* cmd) { return [name, cmd] { runUi(cmd, {{"args", {name()}}}); }; };

    auto* row1 = new QHBoxLayout;
    auto* edit = pushButton("Edit Edges", "Show the input mesh with these edges selected; change them with normal selection. Red edges hold a clamped offset back.", call("bevelEditEdges"));
    auto* apply = pushButton("Apply", "Store the selected edges and finish editing.", call("bevelApplyEdges"));
    auto* cancel = pushButton("Cancel", "Finish editing without changing the edges.", call("bevelCancelEdges"));
    row1->addWidget(edit);
    row1->addWidget(apply);
    row1->addWidget(cancel);
    v->addLayout(row1);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(pushButton("Measure Thin", "Set thin to 1.5 times the selected edge's length.", call("bevelMeasureThin")));
    auto* clear = pushButton("Clear Overrides", "Drop the hand-picked edges.", call("bevelClearOverrides"));
    row2->addWidget(clear);
    if (withSelect)
        row2->addWidget(pushButton("Select", "Select the modifier to edit its parameters and see its edges.", [name] {
            runUi("select", {{"args", {name()}}});
        }));
    v->addLayout(row2);

    QPointer<QWidget> guard(w);
    auto refresh = [guard, status, edit, apply, cancel, clear, mod] {
        if (!guard)
            return;
        Graph& g = Graph::current();
        scene::Node* n = g.node(mod);
        if (!n)
            return;
        auto set = scene::autoBevel::chosen(g, mod);
        bool editing = scene::autoBevel::editing(*n);
        QString text = QString("%1 edges").arg(set ? int(set->edges.size()) : 0);
        if (set && clamped(*set))
            text += QString(", offset %1 of %2, clamped").arg(set->applied, 0, 'g', 3).arg(set->offset, 0, 'g', 3);
        if (set && set->lost > 0)
            text += QString(", %1 hand-picked edges lost").arg(set->lost);
        if (editing)
            text += ", editing";
        status->setText(text);
        edit->setEnabled(!editing);
        apply->setEnabled(editing);
        cancel->setEnabled(editing);
        clear->setEnabled(!editing);
    };
    refresh();
    // Deferred: listeners fire mid-edit, when the graph must not be evaluated.
    auto queued = std::make_shared<bool>(false);
    int listener = Graph::current().addListener([w, refresh, queued](scene::Change, NodeId) {
        if (*queued)
            return;
        *queued = true;
        QTimer::singleShot(0, w, [refresh, queued] {
            *queued = false;
            refresh();
        });
    });
    QObject::connect(w, &QObject::destroyed, [listener] { Graph::current().removeListener(listener); });
    return w;
}

QWidget* meshPanel(NodeId mesh)
{
    Graph& g = Graph::current();
    std::vector<NodeId> mods;
    for (NodeId id : scene::modifiers::stack(g, mesh))
        if (scene::autoBevel::isAutoBevel(*g.node(id)))
            mods.push_back(id);
    if (mods.empty())
        return nullptr;
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    for (NodeId id : mods)
    {
        auto* title = new QLabel(QString::fromStdString(g.node(id)->name()));
        title->setToolTip(QString::fromStdString(g.node(id)->fullPath()));
        title->setContentsMargins(8, 2, 4, 0);
        v->addWidget(title);
        v->addWidget(modifierPanel(id, true));
    }
    return w;
}

void registerAutoBevelActions()
{
    auto add = [](const char* id, const char* label, const char* cmd, const char* doc) {
        actions::add({.id = id,
                      .label = label,
                      .category = "Modifiers",
                      .run = [cmd] { runUi(cmd); },
                      .doc = doc,
                      .python = std::string(cmd) + "()"});
    };
    add("modifier.autoBevel", "Auto Bevel", "autoBevel", scene::modifiers::kindDoc("autoBevel").c_str());
    add("select.foldEdges", "Select Fold Edges", "selectFoldEdges", "Select fold edges by the auto bevel rule.");
    add("bevel.editEdges", "Edit Bevel Edges", "bevelEditEdges", "Edit the selected auto bevel's edges with normal selection.");
    add("bevel.applyEdges", "Apply Bevel Edges", "bevelApplyEdges", "Store the selected edges in the auto bevel being edited.");
    add("bevel.cancelEdges", "Cancel Bevel Edges", "bevelCancelEdges", "Stop editing auto bevel edges without changes.");
    add("bevel.measureThin", "Measure Bevel Thin", "bevelMeasureThin", "Set the auto bevel's thin from the selected edge.");
    add("bevel.clearOverrides", "Clear Bevel Overrides", "bevelClearOverrides", "Drop the auto bevel's hand-picked edges.");
    actions::add({.id = "bevel.showEdges",
                  .label = "Show Bevel Edges",
                  .category = "Modifiers",
                  .run =
                      [] {
                          json names = json::array();
                          bool on = true;
                          for (const std::string& name : Args{}.objects())
                          {
                              scene::Node* n = findNode(name);
                              if (n && scene::autoBevel::isAutoBevel(*n))
                              {
                                  names.push_back(name);
                                  on = !n->as<bool>(n->type().attrIndex("showEdges"));
                              }
                          }
                          if (names.empty())
                          {
                              log::error("Show Bevel Edges: select an autoBevel modifier");
                              return;
                          }
                          for (const json& name : names)
                              runUi("setAttr", {{"args", {name.get<std::string>() + ".showEdges", on}}});
                          viewportTools::requestRedraw();
                      },
                  .doc = "Toggle the viewport highlight of the selected auto bevel modifiers' edges."});
}
} // namespace

void registerAutoBevelUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerAutoBevelActions();
    viewportHooks::addDrawer(drawBevelEdges);
    attributeEditor::addSection("autoBevelModifier", "Bevel Edges", [](NodeId id) { return modifierPanel(id, false); });
    attributeEditor::addSection("mesh", "Auto Bevel", [](NodeId id) { return meshPanel(id); });
}
} // namespace lila::ui
