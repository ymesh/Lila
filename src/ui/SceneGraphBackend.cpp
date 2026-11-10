#include "ui/GraphBackend.h"

#include "scene/Graph.h"
#include "scene/SceneGraph.h"
#include "ops/Command.h"
#include "ui/NodeEditor.h"
#include "ui/Theme.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>

#include <set>

namespace lila::ui
{
namespace
{

/** The scene's dependency graph: each group is a view of real nodes around the selection, edited through sceneGraph* commands. */
class SceneBackend final : public GraphBackend
{
public:
    std::string id() const override { return kSceneGraphBackend; }
    QString title() const override { return "Scene"; }
    std::string commandPrefix() const override { return "sceneGraph"; }

    std::string command(std::string_view verb) const override
    {
        if (verb == "GroupInfo")
            return "sceneGraphInfo";
        return GraphBackend::command(verb);
    }

    bool supports(std::string_view verb) const override
    {
        static const std::set<std::string_view> kVerbs = {"GroupInfo", "NodeTypes",  "AddNode",  "DeleteNode",  "Connect",
                                                          "Disconnect", "MoveNodes", "SetValue", "SetNodeFlags"};
        return kVerbs.contains(verb);
    }

    void adaptCall(std::string_view verb, json& call) const override
    {
        if (verb == "NodeTypes")
            call.erase("group");
        if (verb == "GroupInfo")
        {
            call["depth"] = mDepth;
            call["all"] = mAll;
        }
    }

    std::vector<std::string> groupNames(scene::Graph&) const override
    {
        return std::vector<std::string>(scene::sceneGraph::kViews.begin(), scene::sceneGraph::kViews.end());
    }
    bool isGroupNode(const scene::Node&) const override { return false; }

    /** Every view is built from the selection, so a selection change rebuilds it rather than switching views. */
    bool rebuildsOn(scene::Change change, const scene::Node* n) const override
    {
        switch (change)
        {
        case scene::Change::Selection:
        case scene::Change::Connection:
        case scene::Change::Attrs:
        case scene::Change::Reparented:
            return true;
        case scene::Change::Value:
            return n != nullptr;
        default:
            return false;
        }
    }
    bool rebuildsOnUndo() const override { return true; }

    QString ownerNoun() const override { return "node"; }
    QString groupNoun() const override { return "View"; }

    SocketStyle socketStyle(const std::string& type) const override
    {
        if (type.ends_with("[]"))
        {
            SocketStyle s = socketStyle(type.substr(0, type.size() - 2));
            if (s.mark == SocketMark::None)
                s.mark = SocketMark::Ring;
            return s;
        }
        if (type == scene::sceneGraph::kDagSocketType)
            return {themeColor("socketRotation"), SocketShape::Circle, SocketMark::Hole};
        if (type == "matrix")
            return {themeColor("socketMatrix"), SocketShape::Square};
        if (type == "mesh")
            return {themeColor("socketGeometry"), SocketShape::Square, SocketMark::None, 6.0};
        if (type == "data")
            return {themeColor("socketBundle"), SocketShape::Square};
        return GraphBackend::socketStyle(type);
    }

    double wireWidth(const std::string& type) const override
    {
        if (type == "mesh")
            return 3.5;
        return type == scene::sceneGraph::kDagSocketType ? 1.5 : 2.0;
    }

    bool canLink(const std::string& from, const std::string& to) const override { return scene::sceneGraph::linkable(from, to); }

    bool valueless(const std::string& type) const override
    {
        return type == "matrix" || type == "mesh" || type == "data" || type == scene::sceneGraph::kDagSocketType || type.ends_with("[]");
    }

    QColor headerColor(const std::string& type, const std::string& category) const override
    {
        static const std::pair<std::string_view, const char*> kTokens[] = {
            {"Math", "nodeUtilities"}, {"Animation", "nodeCurve"}, {"Mesh", "nodeMesh"}, {"Object", "nodeGeometry"}, {"Utility", "nodeAttribute"},
        };
        for (auto& [name, token] : kTokens)
            if (category == name)
                return themeColor(token);
        return GraphBackend::headerColor(type, category);
    }

    QString canvasHint() const override
    {
        return "Select a node to edit its values.\n\nEvery view shows the scene's own nodes around the selection: Selection with what is "
               "wired straight in and out of it, History with each mesh's whole modifier stack, Hierarchy with parents and children, Upstream and Downstream to the depth set in the "
               "toolbar. Nodes show only their connected plugs until Hide Unused Sockets is turned off for them. Drag from an output to "
               "an input to connect, Ctrl+drag across wires to cut them. Tab / Shift+A adds a node, which joins the selection. In "
               "Hierarchy, wire Children into Parent to parent.";
    }

    void addToolbarButtons(QBoxLayout* bar, NodeEditor& editor) const override
    {
        auto* depthLabel = new QLabel("Depth", &editor);
        auto* depth = new QSpinBox(&editor);
        depth->setRange(1, 20);
        depth->setValue(mDepth);
        depth->setToolTip("How many steps Upstream and Downstream follow from the selection");
        QObject::connect(depth, &QSpinBox::valueChanged, &editor, [this, &editor](int v) {
            mDepth = v;
            editor.setGroup(editor.group());
        });
        auto* all = new QCheckBox("Conversions", &editor);
        all->setChecked(mAll);
        all->setToolTip("Show anim curves and unit conversions; otherwise views look through them");
        QObject::connect(all, &QCheckBox::toggled, &editor, [this, &editor](bool on) {
            mAll = on;
            editor.setGroup(editor.group());
        });
        auto syncDepth = [depth, depthLabel](const QString& view) {
            const bool used = view == "Upstream" || view == "Downstream";
            depth->setEnabled(used);
            depthLabel->setEnabled(used);
        };
        syncDepth(editor.group());
        const int listener = NodeEditor::addGroupListener([syncDepth, owner = &editor](NodeEditor& e) {
            if (&e == owner)
                syncDepth(e.group());
        });
        QObject::connect(depth, &QObject::destroyed, [listener] { NodeEditor::removeGroupListener(listener); });
        bar->addWidget(depthLabel);
        bar->addWidget(depth);
        bar->addWidget(all);
    }

private:
    mutable int mDepth = 2;
    mutable bool mAll = false;
};
} // namespace

void registerSceneGraphBackend()
{
    if (graphBackends::find(kSceneGraphBackend))
        return;
    graphBackends::add(std::make_shared<SceneBackend>());
}
} // namespace lila::ui
