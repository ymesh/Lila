#include "ui/GraphBackend.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "gn/Gn.h"
// TBB, under NodeLib.h, has members named like Qt's emit macro.
#pragma push_macro("emit")
#undef emit
#include "gn/NodeLib.h"
#pragma pop_macro("emit")
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "ui/GnSpreadsheet.h"
#include "ui/GnViewerDraw.h"
#include "ui/NodeEditor.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"

#include <QBoxLayout>
#include <QToolButton>

#include <algorithm>
#include <utility>

namespace lila::ui
{
namespace
{

json runGn(const std::string& name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

class GnBackend final : public GraphBackend
{
public:
    std::string id() const override { return kGeometryNodesBackend; }
    QString title() const override { return "Geometry Nodes"; }
    std::string commandPrefix() const override { return "gn"; }

    std::vector<std::string> groupNames(scene::Graph& g) const override { return gn::groupNames(g); }
    bool hasGroup(scene::Graph& g, const std::string& group) const override { return gn::findGroupNode(g, group) != nullptr; }
    bool isGroupNode(const scene::Node& n) const override { return n.type().name == gn::kGroupNodeType; }
    std::string newGroupAction() const override { return "gn.newGroup"; }
    std::string subgroupNodeType() const override { return gn::kGroupNodeDeclType; }
    bool runsSubgroup(const std::string& type) const override { return gn::runsGroup(type); }
    std::vector<std::string> fileSuffixes() const override { return {"lilanodes", "json"}; }
    bool canRenameGroup() const override { return true; }
    std::string renameGroup(const std::string& group, const std::string& to) const override
    {
        json r = runUi("gnRenameGroup", {{"args", {group, to}}});
        return r.is_string() ? r.get<std::string>() : std::string();
    }
    bool canDeleteGroup() const override { return true; }
    bool deleteGroup(const std::string& group) const override { return tryRunUi("gnDeleteGroup", {{"args", {group}}}); }

    Owner followSelection(scene::Graph& g) const override
    {
        for (scene::NodeId id : g.selection())
        {
            scene::Node* n = g.node(id);
            if (!n)
                continue;
            if (gn::isGnModifier(*n))
            {
                if (scene::Node* grp = gn::groupOfModifier(g, *n))
                    return {n->path(), grp->name()};
                return {};
            }
            if (!n->type().isA("mesh"))
                continue;
            std::vector<scene::NodeId> stack = scene::modifiers::stack(g, id);
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                if (scene::Node* m = g.node(*it); m && gn::isGnModifier(*m))
                    if (scene::Node* grp = gn::groupOfModifier(g, *m))
                        return {m->path(), grp->name()};
        }
        return {};
    }

    std::string groupOfOwner(scene::Graph& g, const std::string& path) const override
    {
        scene::Node* m = path.empty() ? nullptr : g.find(path);
        if (!m || !gn::isGnModifier(*m))
            return {};
        scene::Node* grp = gn::groupOfModifier(g, *m);
        return grp ? grp->name() : std::string();
    }

    QString ownerNoun() const override { return "modifier"; }

    std::string viewerType() const override { return "Viewer"; }

    void viewNode(const ViewRequest& r) const override
    {
        if (r.nodeType == "Viewer" || r.nodeType == "GroupOutput")
            return;
        std::string geometry, field, fieldType;
        for (auto& [id, t] : r.outputs)
        {
            if (t == "geometry" && geometry.empty())
                geometry = id;
            else if (t != "geometry" && t != "string" && field.empty())
            {
                field = id;
                fieldType = t;
            }
        }
        if (geometry.empty() && field.empty())
            return;
        UndoChunk chunk("View Node");
        std::string viewer;
        json info = runGn("gnGroupInfo", argsCall({r.group}));
        if (info.is_object())
            for (const json& n : info["definition"].value("nodes", json::array()))
                if (n.value("type", "") == "Viewer")
                {
                    viewer = n.value("name", "");
                    break;
                }
        if (viewer.empty())
        {
            QPointF at = r.position + QPointF(r.nodeWidth + 60.0, 0.0);
            json added = runGn("gnAddNode", {{"args", {r.group, "Viewer"}}, {"position", {at.x(), at.y()}}});
            if (!added.is_string())
                return;
            viewer = added.get<std::string>();
        }
        if (!field.empty())
        {
            std::string dt = fieldType == "rotation" ? "vector" : fieldType;
            runGn("gnSetValue", argsCall({r.group, viewer + ".dataType", dt}));
            runGn("gnConnect", argsCall({r.group, r.node + "." + field, viewer + ".value"}));
        }
        if (!geometry.empty())
            runGn("gnConnect", argsCall({r.group, r.node + "." + geometry, viewer + ".geometry"}));
        runGn("gnSetActiveViewer", argsCall({r.group, viewer}));
    }

    bool canSend() const override { return true; }

    /** The selected meshes running this group, else the mesh of the modifier in view, else every mesh running it. */
    std::vector<std::string> sendTargets(scene::Graph& g, const std::string& group, const std::string& ownerInView) const override
    {
        scene::Node* grp = group.empty() ? nullptr : gn::findGroupNode(g, group);
        if (!grp)
            return {};
        std::vector<scene::NodeId> meshes;
        for (scene::NodeId m : gn::instancesOf(g, grp->id()))
            if (scene::NodeId mesh = scene::modifiers::meshOf(g, m); g.node(mesh) && std::find(meshes.begin(), meshes.end(), mesh) == meshes.end())
                meshes.push_back(mesh);
        std::vector<std::string> out;
        for (scene::NodeId id : g.selection())
            if (std::find(meshes.begin(), meshes.end(), id) != meshes.end())
                out.push_back(g.node(id)->path());
        if (!out.empty())
            return out;
        if (scene::Node* m = ownerInView.empty() ? nullptr : g.find(ownerInView))
            if (scene::Node* mesh = g.node(scene::modifiers::meshOf(g, m->id())))
                return {mesh->path()};
        for (scene::NodeId id : meshes)
            out.push_back(g.node(id)->path());
        return out;
    }

    SocketStyle socketStyle(const std::string& type) const override
    {
        if (type == "geometry")
            return {themeColor("socketGeometry"), SocketShape::Circle, SocketMark::None, 6.0};
        if (type == "rotation")
            return {themeColor("socketRotation"), SocketShape::Circle, SocketMark::Hole};
        if (type == "matrix")
            return {themeColor("socketMatrix"), SocketShape::Square, SocketMark::Hole};
        if (type == "bundle")
            return {themeColor("socketBundle"), SocketShape::Circle, SocketMark::Ring};
        if (type == "closure")
            return {themeColor("socketClosure"), SocketShape::Circle, SocketMark::Dot};
        return GraphBackend::socketStyle(type);
    }

    bool valueless(const std::string& type) const override { return type == "geometry" || type == "matrix" || type == "bundle" || type == "closure"; }

    double wireWidth(const std::string& type) const override { return type == "geometry" ? 3.5 : 2.0; }

    std::optional<MergeNode> mergeNode(const std::string& type, const std::string& op) const override
    {
        if (type == "geometry")
            return MergeNode{"JoinGeometry", json::object(), "geometry", {}, "geometry", true};
        if (type == "float" || type == "int" || type == "bool")
            return MergeNode{"Math", {{"operation", op}}, "a", "b", "value"};
        if (type == "vector" || type == "color" || type == "rotation")
            return MergeNode{"VectorMath", {{"operation", op}}, "a", "b", "vector"};
        return std::nullopt;
    }

    bool canLink(const std::string& from, const std::string& to) const override
    {
        gn::SocketType a, b;
        return gn::parseSocketType(from, &a) && gn::parseSocketType(to, &b) && gn::linkable(a, b);
    }

    QColor headerColor(const std::string& type, const std::string& category) const override
    {
        static const std::pair<std::string_view, const char*> kTokens[] = {
            {"Geometry", "nodeGeometry"}, {"Mesh", "nodeMesh"},           {"Mesh Primitives", "nodeMesh"}, {"Curve", "nodeCurve"},
            {"Curve Primitives", "nodeCurve"}, {"Point", "nodePoint"},    {"Instances", "nodeInstances"}, {"Attribute", "nodeAttribute"},
            {"Input", "nodeInput"},       {"Output", "nodeOutput"},       {"Utilities", "nodeUtilities"}, {"Color", "nodeColor"},
            {"Texture", "nodeTexture"},   {"Group", "nodeGroupIo"},       {"Zones", "nodeGroupIo"},
        };
        if (type != "GroupInput" && type != "GroupOutput")
            for (auto& [name, token] : kTokens)
                if (category == name)
                    return themeColor(token);
        return GraphBackend::headerColor(type, category);
    }

    std::vector<std::string> inputTypes() const override
    {
        return {"float", "int", "bool", "vector", "rotation", "color", "string", "matrix", "bundle", "closure"};
    }

    QString canvasHint() const override
    {
        return GraphBackend::canvasHint() +
               QString(" Repeat, For Each, Simulation and Closure zones run the nodes between their input and output. %1 groups the selection, %2 ungroups, "
                       "double-click a Group node to open its group, Ctrl+Shift+click a node to preview it in a Viewer. Drop a .lilanodes file here to import it.")
                   .arg(hintKey("nodeEditor.groupNodes"), hintKey("nodeEditor.ungroup"));
    }

    void addToolbarButtons(QBoxLayout* bar, NodeEditor& editor) const override;
};

void GnBackend::addToolbarButtons(QBoxLayout* bar, NodeEditor& editor) const
{
    auto* apply = new QToolButton(&editor);
    apply->setText("Apply to Selection");
    apply->setToolTip("Add this group as a modifier on the selected meshes");
    bar->addWidget(apply);
    QObject::connect(apply, &QToolButton::clicked, &editor, [&editor] {
        QString group = editor.group();
        if (group.isEmpty())
            return;
        scene::Graph& g = scene::Graph::current();
        UndoChunk chunk("Apply Geometry Nodes");
        for (scene::NodeId id : std::vector<scene::NodeId>(g.selection()))
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                runGn("gnApply", argsCall({n->path(), group.toStdString()}));
    });
}
} // namespace

void registerGnGraphBackend()
{
    if (graphBackends::find(kGeometryNodesBackend))
        return;
    graphBackends::add(std::make_shared<GnBackend>());
    registerGnViewerDrawer();
    registerGnSpreadsheet();
}
} // namespace lila::ui
