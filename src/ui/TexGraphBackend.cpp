#include "ui/GraphBackend.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "tex/Evaluate.h"
#include "tex/GraphProfile.h"
#include "tex/NodeDef.h"
#include "tex/TextureSet.h"
#include "tex/PaintStrokes.h"
#include "render/PaintGpu.h"
#pragma pop_macro("slots")
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Menus.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/TexGraphPreview.h"
#include "ui/TexParams.h"
#include "ui/TexPreview.h"
#include "ui/TextureGraphUi.h"
#include "ui/TextureSetPanel.h"
#include "ui/TexturePaintTool.h"
#include "ui/Theme.h"
#include "ui/FileDialog.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/PainterImportUi.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QMenu>
#include <QThreadPool>
#include <QTimer>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QImageReader>
#include <QBoxLayout>
#include <QFileDialog>
#include <QPointer>
#include <QToolButton>

#include <algorithm>
#include <format>
#include <set>

namespace lila::ui
{
namespace
{

constexpr int kThumbnail = 96;
/** The square size Node Timings evaluates the set at. */
constexpr int kProfileSize = 256;
/** The Parameters panel's id for the graph seed: "@" never starts a graph parameter's name. */
constexpr const char* kSeedParam = "@seed";

std::vector<NodeEditor*>& texEditors()
{
    static std::vector<NodeEditor*> list;
    return list;
}

/** A param's value on a node panel's json, else its default. */
json paramValue(const json& node, const std::string& id)
{
    for (const json& p : node.value("params", json::array()))
        if (p.value("id", "") == id)
            return p.contains("value") ? p["value"] : p.value("default", json());
    return {};
}

/** The smallest set size (a power of two, 512 to 16384) holding n texels. */
int setSizeFor(int n)
{
    int s = 512;
    while (s < n && s < 16384)
        s *= 2;
    return s;
}

/**
 * An Image node's file size against its texture set's, with Match Set when the set would sample it smaller. Null when
 * the node is outside a set or its file has no readable size.
 */
QWidget* imageSizeRow(QWidget* parent, const std::string& set, const json& node)
{
    scene::Node* n = tex::findSet(scene::Graph::current(), set);
    const json file = paramValue(node, "file");
    if (!n || !file.is_string() || file.get<std::string>().empty())
        return nullptr;
    const std::filesystem::path path = tex::resolveImagePath(file.get<std::string>(), texPreview::imageRoot());
    const QSize size = QImageReader(QString::fromStdU16String(path.u16string())).size();
    if (!size.isValid())
        return nullptr;
    const tex::SetSettings s = tex::readSettings(*n);
    const json tilesValue = paramValue(node, "tiles");
    const int tiles = std::max(tilesValue.is_number() ? int(tilesValue.get<double>()) : 1, 1);
    const int wantW = setSizeFor(size.width() * tiles), wantH = setSizeFor(size.height() * tiles);
    const bool smaller = s.width < wantW || s.height < wantH;

    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    QString text = QString("%1 \u00d7 %2").arg(size.width()).arg(size.height());
    if (smaller)
        text += s.width == s.height ? QString("\nset %1").arg(s.width) : QString("\nset %1 \u00d7 %2").arg(s.width).arg(s.height);
    auto* label = new QLabel(text, row);
    label->setToolTip(smaller ? "The set samples this image below its own resolution" : "Image Size");
    h->addWidget(label, 1);
    if (smaller)
    {
        auto* match = new QPushButton("Match Set", row);
        match->setToolTip(QString("Set the texture set's resolution to %1 \u00d7 %2").arg(wantW).arg(wantH));
        QObject::connect(match, &QPushButton::clicked, row, [set, wantW, wantH, row, label, size] {
            runUi("texSetSettings", argsCall({set, json{{"width", wantW}, {"height", wantH}}}));
            label->setText(QString("%1 \u00d7 %2").arg(size.width()).arg(size.height()));
            label->setToolTip("Image Size");
            for (QPushButton* b : row->findChildren<QPushButton*>())
                b->hide();
        });
        h->addWidget(match);
    }
    return row;
}

/** texGraphParams for a graph address, or null when it can't be read. */
json queryGraphParams(const std::string& group)
{
    try
    {
        return ops::execute("texGraphParams", argsCall({group}));
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** A node's parameters that drive graph parameters: parameter id to graph parameter name. */
std::map<std::string, std::string> exposedParamsOf(const json& graphParams, const std::string& node)
{
    std::map<std::string, std::string> out;
    if (!graphParams.is_object())
        return out;
    for (const json& p : graphParams.value("params", json::array()))
        for (const json& t : p.value("targets", json::array()))
            if (const std::string plug = t.get<std::string>(); plug.starts_with(node + "."))
                out[plug.substr(node.size() + 1)] = p.value("name", "");
    return out;
}

/** Runs a UI command on the next turn of the event loop, for menu actions living in the panel it rebuilds. */
void deferTexCommand(std::string command, json call)
{
    QTimer::singleShot(0, qApp, [command = std::move(command), call = std::move(call)] { runUi(command, call); });
}

QLabel* texGraphHeading(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setStyleSheet("color: rgb(225,225,225); padding-top: 6px;");
    return l;
}

/** Preset picker, Save and a menu with Rename and Delete, over the presets of the graph at group. */
QWidget* presetRow(QWidget* parent, const std::string& group, const json& graphParams)
{
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    auto* picker = new QComboBox(row);
    picker->setObjectName("graphPresets");
    for (const json& name : graphParams.value("presets", json::array()))
        picker->addItem(qs(name.get<std::string>()));
    picker->setPlaceholderText(picker->count() ? "Custom" : "No presets");
    picker->setCurrentIndex(picker->findText(qs(graphParams.value("active", std::string()))));
    picker->setToolTip("Preset\nPick one to set every parameter to its values. Custom: the values match no preset");
    QObject::connect(picker, &QComboBox::activated, picker, [group, picker](int i) {
        if (i >= 0)
            runUi("texApplyPreset", argsCall({group, picker->itemText(i).toStdString()}));
    });
    h->addWidget(picker, 1);

    auto* save = new QToolButton(row);
    save->setObjectName("savePreset");
    save->setIcon(icon("device-floppy"));
    save->setToolTip("Save Preset\nStore every parameter's value under a name; a preset's own name replaces it");
    QObject::connect(save, &QToolButton::clicked, picker, [group, picker] {
        const QString start = picker->currentIndex() >= 0 ? picker->currentText() : QString("Preset %1").arg(picker->count() + 1);
        controls::editInPlace(picker, start, [group](const QString& name) { deferTexCommand("texSavePreset", argsCall({group, name.toStdString()})); });
    });
    h->addWidget(save);

    auto* more = new QToolButton(row);
    more->setIcon(icon("dots"));
    more->setToolTip("Preset Actions");
    more->setPopupMode(QToolButton::InstantPopup);
    controls::setRole(more, "overflow");
    auto* menu = new QMenu(more);
    QAction* rename = menu->addAction(icon("pencil"), "Rename Preset", picker, [group, picker] {
        const QString from = picker->currentText();
        controls::editInPlace(picker, from, [group, from](const QString& to) {
            deferTexCommand("texRenamePreset", argsCall({group, from.toStdString(), to.toStdString()}));
        });
    });
    QAction* remove = menu->addAction(icon("trash"), "Delete Preset", picker, [group, picker] {
        deferTexCommand("texDeletePreset", argsCall({group, picker->currentText().toStdString()}));
    });
    QObject::connect(menu, &QMenu::aboutToShow, menu, [picker, rename, remove] {
        rename->setEnabled(picker->currentIndex() >= 0);
        remove->setEnabled(picker->currentIndex() >= 0);
    });
    more->setMenu(menu);
    h->addWidget(more);
    return row;
}

/**
 * The graph's own section of the side panel. A set's graph: its presets and parameters, each right-click removable. A node
 * group lists its inputs under Parameter Defaults already, so it gets only the presets over them.
 */
QWidget* graphParamsPanel(QWidget* parent, NodeEditor& editor, const std::string& group, bool setGraph)
{
    const json gp = queryGraphParams(group);
    if (!gp.is_object())
        return nullptr;
    const json params = gp.value("params", json::array());
    if (!setGraph && params.empty())
        return nullptr;
    auto* box = new QWidget(parent);
    box->setObjectName("graphParams");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    v->addWidget(texGraphHeading(box, setGraph ? "Parameters" : "Presets"));
    v->addWidget(presetRow(box, group, gp));
    if (!setGraph)
        return box;
    QPointer<QComboBox> presets = box->findChild<QComboBox*>("graphPresets");
    QPointer<NodeEditor> ed(&editor);
    // The panel stays up under a field being edited, so the picker follows on its own.
    auto followPreset = [group, presets] {
        if (const json now = queryGraphParams(group); presets && now.is_object())
            presets->setCurrentIndex(presets->findText(qs(now.value("active", std::string()))));
    };

    json shown = {{"title", ""}, {"params", json::array()}};
    shown["params"].push_back({{"id", kSeedParam},
                               {"label", "Seed"},
                               {"type", "seed"},
                               {"value", gp.value("seed", 0)},
                               {"doc", "Added to every seed parameter in the set, groups included: one number re-rolls the whole material"}});
    for (const json& p : params)
    {
        json e = {{"id", p.value("name", "")}, {"type", p.value("type", "float")}, {"value", p.value("value", json())}};
        if (const std::string label = p.value("label", ""); !label.empty())
            e["label"] = label;
        std::string doc = p.value("description", "");
        for (const json& t : p.value("targets", json::array()))
            doc += (doc.empty() ? "" : "\n") + std::string("Drives ") + t.get<std::string>();
        e["doc"] = doc;
        for (const char* k : {"options", "min", "max", "softMin", "softMax", "step"})
            if (p.contains(k))
                e[k] = p[k];
        shown["params"].push_back(e);
    }
    v->addWidget(texParams::nodePanel(
        box, shown,
        [ed, group, followPreset](const std::string& id, const json& value) {
            if (!ed)
                return;
            if (id == kSeedParam)
                ed->runFromField("texSetGraphSeed", argsCall({group, value}));
            else
                ed->runFromField("texSetGraphParam", argsCall({group, id, value}));
            followPreset();
        },
        {},
        [group](const std::string& id, QWidget* owner) -> QAction* {
            if (id == kSeedParam)
                return nullptr;
            auto* a = new QAction("Remove Graph Parameter", owner);
            QObject::connect(a, &QAction::triggered, qApp, [group, id] { deferTexCommand("texUnexposeParam", argsCall({group, id})); });
            return a;
        }));
    if (params.empty())
    {
        auto* none = new QLabel("No parameters. Right-click a node's parameter to expose it.", box);
        none->setWordWrap(true);
        none->setStyleSheet("color: rgb(150,150,150);");
        v->addWidget(none);
    }
    return box;
}

/** Texture graphs: one per texture set, edited through the tex* commands. */
class TexBackend final : public GraphBackend
{
public:
    std::string id() const override { return kTextureGraphBackend; }
    QString title() const override { return "Texture Graph"; }
    std::string commandPrefix() const override { return "tex"; }

    std::string command(std::string_view verb) const override
    {
        if (verb == "GroupInfo")
            return "texGraphInfo";
        if (verb == "ExportGroup")
            return "texExportGraph";
        if (verb == "ImportGroup")
            return "texImportGraph";
        return GraphBackend::command(verb);
    }

    bool supports(std::string_view verb) const override
    {
        static const std::set<std::string_view> kVerbs = {"GroupInfo",   "NodeTypes",   "AddNode",      "DeleteNode",  "Connect",     "Disconnect",
                                                          "MoveNodes",   "SetValue",    "CopyNodes",    "PasteNodes",  "DuplicateNodes",
                                                          "GroupNodes",  "Ungroup",     "SaveGroup",    "ExportGroup", "ImportGroup", "SetTies",
                                                          "SetFrames",   "SetNodeFlags", "SetNodeLabel", "SetBookmarks", "AddInput",  "RemoveInput",
                                                          "RenameInput", "MoveInput",   "SetInputInfo", "ExposeInput", "RemoveOutput"};
        return kVerbs.contains(verb);
    }

    void adaptCall(std::string_view verb, json& call) const override
    {
        if (verb == "NodeTypes")
            call.erase("group");
    }

    std::vector<std::string> groupNames(scene::Graph& g) const override { return tex::setNames(g); }
    bool hasGroup(scene::Graph& g, const std::string& group) const override { return tex::hasGraphAt(g, group); }
    std::string subgroupNodeType() const override { return tex::kGroupType; }
    /** A set's node groups share one namespace, so a Group node anywhere in the set runs "Set/<its group>". */
    std::string childGroup(const std::string& current, const std::string& child) const override
    {
        return tex::joinAddress(tex::splitAddress(current).set, child);
    }
    std::string parentGroupOf(const std::string& group) const override
    {
        const tex::GraphAddress at = tex::splitAddress(group);
        return at.group.empty() ? std::string() : at.set;
    }
    bool hasInterface(const std::string& group) const override { return !tex::splitAddress(group).group.empty(); }
    bool isGroupNode(const scene::Node& n) const override { return n.type().name == tex::kTextureSetType; }
    std::optional<size_t> groupStamp(scene::Graph& g, const std::string& group) const override
    {
        if (scene::Node* n = tex::findSet(g, tex::splitAddress(group).set))
            return tex::setStamp(*n);
        return std::nullopt;
    }
    std::string newGroupAction() const override { return "texture.newSet"; }
    std::vector<std::string> fileSuffixes() const override { return {"lilamat", "lilatexnodes", "lilatexnode"}; }

    /** A selected texture set, else the set bound to a material slot of a selected mesh or a mesh under a selected group. */
    Owner followSelection(scene::Graph& g) const override
    {
        std::vector<scene::NodeId> ids;
        for (scene::NodeId id : g.selection())
        {
            ids.push_back(id);
            for (scene::NodeId d : g.descendants(id))
                ids.push_back(d);
        }
        for (scene::NodeId id : ids)
        {
            scene::Node* n = g.node(id);
            if (!n)
                continue;
            if (n->type().name == tex::kTextureSetType)
                return {n->path(), n->name()};
            if (!n->type().isA("mesh"))
                continue;
            scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            if (!mesh)
                continue;
            const size_t slotCount = std::max<size_t>(mesh->materialNames().size(), 1);
            for (size_t i = 0; i < slotCount; ++i)
                if (std::string set = textureSetForSlot(g, scene::materials::slotName(*mesh, int32_t(i))); !set.empty())
                    if (scene::Node* s = tex::findSet(g, set))
                        return {s->path(), set};
        }
        return {};
    }

    std::string groupOfOwner(scene::Graph& g, const std::string& path) const override
    {
        scene::Node* n = path.empty() ? nullptr : g.find(path);
        return n && n->type().name == tex::kTextureSetType ? n->name() : std::string();
    }

    QString ownerNoun() const override { return "texture set"; }
    QString groupNoun() const override { return "Texture Set"; }
    bool canRenameGroup() const override { return true; }

    std::string renameGroup(const std::string& group, const std::string& to) const override
    {
        const tex::GraphAddress at = tex::splitAddress(group);
        if (at.group.empty())
            return renameTextureSet(group, to);
        json r = runUi("texRenameGroup", argsCall({group, to}));
        return r.is_string() ? tex::joinAddress(at.set, r.get<std::string>()) : std::string();
    }
    bool canDeleteGroup() const override { return true; }
    bool deleteGroup(const std::string& group) const override
    {
        if (tex::splitAddress(group).group.empty())
            return deleteTextureSet(group);
        return tryRunUi("texDeleteGroup", argsCall({group}));
    }

    bool canViewNodes() const override { return true; }

    void viewNode(const ViewRequest& r) const override
    {
        texture2d::Source s{r.group, r.node, {}, {}};
        if (r.nodeType == tex::kMaterialOutputType)
            s = {r.group, {}, {}, "baseColor"};
        else if (r.outputs.empty())
            return;
        texture2d::view(s);
        panels::show(kTexture2dDock);
    }

    SocketStyle socketStyle(const std::string& type) const override
    {
        if (type == "gray")
            return {themeColor("socketFloat"), SocketShape::Circle, SocketMark::Dot};
        if (type == "sdf2d")
            return {themeColor("socketRotation"), SocketShape::Circle, SocketMark::Hole};
        if (type == "material")
            return {themeColor("socketGeometry"), SocketShape::Square, SocketMark::None, 6.0};
        return GraphBackend::socketStyle(type);
    }

    bool valueless(const std::string& type) const override { return type == "material"; }

    double wireWidth(const std::string& type) const override { return type == "material" ? 3.5 : 2.0; }

    std::optional<MergeNode> mergeNode(const std::string& type, const std::string& op) const override
    {
        if (type == "material")
            return MergeNode{"materialBlend", json::object(), "a", "b", "material"};
        // Blend has no divide; the rest map onto its modes.
        if (op == "divide")
            return std::nullopt;
        return MergeNode{"blend", {{"mode", op}}, "background", "foreground", "out"};
    }

    bool canLink(const std::string& from, const std::string& to) const override
    {
        tex::PortType a, b;
        return tex::parsePortType(from, &a) && tex::parsePortType(to, &b) && tex::linkable(a, b);
    }

    QColor headerColor(const std::string& type, const std::string& category) const override
    {
        static const std::pair<std::string_view, const char*> kTokens[] = {
            {"Noise", "nodeTexture"},      {"Pattern", "nodeTexture"},  {"Filter", "nodeColor"},   {"Transform", "nodeGeometry"},
            {"Input", "nodeInput"},        {"Normal", "nodeAttribute"}, {"Material", "nodeMesh"},  {"Output", "nodeOutput"},
            {"Utility", "nodeUtilities"},  {"Graphs", "nodeGroupIo"},
        };
        if (type != tex::kGroupInputType && type != tex::kGroupOutputType)
            for (auto& [name, token] : kTokens)
                if (category == name)
                    return themeColor(token);
        return GraphBackend::headerColor(type, category);
    }

    std::vector<std::string> inputTypes() const override { return {"float", "gray", "color", "vector"}; }

    QString canvasHint() const override
    {
        return GraphBackend::canvasHint() +
               " Ctrl+Shift+click a node to show it in the 2D View and on the meshes this set textures. Drop a .lilamat or "
               ".lilatexnodes file here to import it.";
    }

    int thumbnailSize(const std::string& type) const override
    {
        return type == tex::kMaterialOutputType || type == tex::kGroupInputType || type == tex::kGroupOutputType ? 0 : kThumbnail;
    }

    QImage thumbnail(const std::string& group, const json&, const std::string& node, std::function<void()> ready) const override
    {
        texPreview::Result r = texPreview::get({group, node, {}, {}, 128}, std::move(ready));
        if (!r.image)
            return {};
        return texPreview::toQImage(*r.image, r.type, texPreview::isColor(r.type, node));
    }

    bool thumbnailsFollowTime(const std::string& group) const override { return texPreview::animated(group); }

    QWidget* nodePanel(QWidget* parent, NodeEditor& editor, const json& node) const override
    {
        if (node.value("type", "") == tex::kGroupInputType)
            return nullptr;
        std::string group = editor.group().toStdString();
        std::string name = node.value("name", "");
        QPointer<NodeEditor> ed(&editor);
        json withSet = node;
        withSet["set"] = group;
        texParams::ExposeFn expose;
        if (hasInterface(group))
            expose = [group, name](const std::string& id) { runUi("texExposeInput", argsCall({group, name + "." + id})); };
        const std::string type = node.value("type", "");
        texParams::ParamActionFn paramAction;
        if (type != tex::kGroupOutputType && type != tex::kMaterialOutputType)
            paramAction = [group, name, exposed = exposedParamsOf(queryGraphParams(group), name)](const std::string& id, QWidget* owner) -> QAction* {
                auto it = exposed.find(id);
                if (it == exposed.end())
                {
                    auto* a = new QAction("Expose as Graph Parameter", owner);
                    QObject::connect(a, &QAction::triggered, qApp, [group, plug = name + "." + id] { deferTexCommand("texExposeParam", argsCall({group, plug})); });
                    return a;
                }
                QFont f = owner->font();
                f.setBold(true);
                owner->setFont(f);
                owner->setToolTip("Graph Parameter: " + qs(it->second) + (owner->toolTip().isEmpty() ? QString() : "\n" + owner->toolTip()));
                auto* a = new QAction("Remove Graph Parameter", owner);
                QObject::connect(a, &QAction::triggered, qApp, [group, param = it->second] { deferTexCommand("texUnexposeParam", argsCall({group, param})); });
                return a;
            };
        QWidget* panel = texParams::nodePanel(
            parent, withSet,
            [ed, group, name](const std::string& id, const json& v) {
                if (ed)
                    ed->setValueFromField(argsCall({group, name + "." + id, v}));
            },
            expose, paramAction);
        if (node.value("type", "") == "image")
            if (QWidget* row = imageSizeRow(panel, group, node))
            {
                if (auto* form = panel->findChild<QFormLayout*>())
                    form->addRow("Size", row);
                else
                    panel->layout()->addWidget(row);
            }
        return panel;
    }

    QWidget* pinnedPanel(QWidget* parent, NodeEditor& editor) const override { return new TexGraphPreview(editor, parent); }

    bool canProfile() const override { return true; }

    /** The set's passes timed on the CPU (the GPU evaluator doesn't time passes), on a pool thread. */
    void profile(const std::string& group, std::function<void(json)> done) const override
    {
        const tex::GraphAddress at = tex::splitAddress(group);
        scene::Node* n = tex::findSet(scene::Graph::current(), at.set);
        if (!n)
        {
            done({{"error", "no texture set named " + at.set}});
            return;
        }
        tex::Graph graph;
        tex::SetSettings settings;
        try
        {
            graph = tex::readGraph(*n);
            settings = tex::readSettings(*n);
        }
        catch (const std::exception& e)
        {
            done({{"error", e.what()}});
            return;
        }
        QThreadPool::globalInstance()->start([graph = std::move(graph), settings = std::move(settings), sub = at.group, done = std::move(done)] {
            json r;
            try
            {
                r = tex::profileGraph(graph, settings, sub, kProfileSize);
                r["label"] = std::format("CPU, {} px", kProfileSize);
            }
            catch (const std::exception& e)
            {
                r = {{"error", e.what()}};
            }
            mainq::post([done, r] { done(r); });
        });
    }

    QWidget* graphPanel(QWidget* parent, NodeEditor& editor, const json&) const override
    {
        const std::string group = editor.group().toStdString();
        return graphParamsPanel(parent, editor, group, !hasInterface(group));
    }

    void addToolbarButtons(QBoxLayout* bar, NodeEditor& editor) const override
    {
        auto* view = new QToolButton(&editor);
        view->setText("2D View");
        view->setToolTip("2D View: show it; Ctrl+Shift+click a node to show its output there");
        QObject::connect(view, &QToolButton::clicked, &editor, [] { panels::show(kTexture2dDock); });
        bar->addWidget(view);
        auto* exportMaps = new QToolButton(&editor);
        exportMaps->setText("Export Maps");
        exportMaps->setToolTip("Export Maps: write this set's maps through its export preset (texExport)");
        QObject::connect(exportMaps, &QToolButton::clicked, &editor, [&editor] {
            if (!editor.group().isEmpty())
                if (json r = runUi("texExport", argsCall({editor.group().toStdString()})); r.is_object())
                    log::info("exported {} maps", r.value("files", json::array()).size());
        });
        bar->addWidget(exportMaps);
    }
};

NodeEditor* texEditor()
{
    if (NodeEditor* e = NodeEditor::active(); e && e->backend().id() == kTextureGraphBackend)
        return e;
    for (NodeEditor* e : texEditors())
        if (e->isVisible())
            return e;
    return texEditors().empty() ? nullptr : texEditors().front();
}

std::string uniqueName(std::string base)
{
    std::string clean;
    for (char c : base)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            clean += c;
    if (clean.empty() || std::isdigit(static_cast<unsigned char>(clean.front())))
        clean = "TextureSet";
    std::string name = clean;
    for (int i = 1; !scene::Graph::current().findAll(name).empty(); ++i)
        name = clean + std::to_string(i);
    return name;
}

void openSet(const std::string& set)
{
    panels::show(kTextureGraphDock);
    for (NodeEditor* e : texEditors())
        e->setGroup(qs(set));
}

/** The texture size the smallest of a material's maps fills, per side; invalid when it has no map that can be read. */
QSize sizeFromMaps(const std::string& slot)
{
    scene::Node* material = scene::materials::find(scene::Graph::current(), slot);
    if (!material)
        return {};
    QSize least;
    for (const auto& [channel, attr] : scene::materials::textureChannels())
    {
        const std::string path = material->as<std::string>(attr);
        // The stroke sheet is a brush pattern, not a map of the surface.
        if (path.empty() || channel == "stroke")
            continue;
        const QSize size = QImageReader(qs(scene::materials::resolvePath(path))).size();
        if (size.isValid())
            least = least.isValid() ? least.boundedTo(size) : size;
    }
    if (!least.isValid())
        return {};
    auto snap = [](int side) { return std::clamp(int(std::bit_floor(unsigned(std::max(side, 1)))), 512, 16384); };
    return {snap(least.width()), snap(least.height())};
}

std::string joinNames(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

/** A paint layer in a new set, wired to every channel and the mask and made active, so painting starts at once. */
void addFirstPaintLayer(const std::string& set)
{
    const std::string id = ops::execute("texAddLayer", {{"args", {set, "paint"}}}).value("id", std::string());
    scene::Node* n = tex::findSet(scene::Graph::current(), set);
    if (id.empty() || !n)
        return;
    std::vector<std::string> channels;
    for (const tex::Channel& c : tex::readSettings(*n).channels)
        channels.push_back(c.name);
    channels.push_back(tex::paint::kMaskChannel);
    render::paint::wireLayer(*n, id, channels);
    textureSets::setActiveLayer(set, id);
}

/** A new texture set the materials pick, sized to the smallest map of the first. Returns its name. */
std::string createSetFor(const std::vector<std::string>& materials)
{
    const std::string first = materials.empty() ? std::string() : materials.front();
    json call = argsCall({uniqueName(first.empty() ? "TextureSet" : tex::defaultSetName(first))});
    if (!materials.empty())
        call["materials"] = materials;
    if (const QSize size = first.empty() ? QSize() : sizeFromMaps(first); size.isValid())
    {
        call["settings"] = {{"width", size.width()}, {"height", size.height()}};
        log::info("{}: {} x {}, the size of {}'s smallest map", call["args"][0].get<std::string>(), size.width(), size.height(), first);
    }
    const std::string made = ops::execute("texCreateSet", call).get<std::string>();
    addFirstPaintLayer(made);
    return made;
}

/** M_<mesh name>, made unused. */
std::string newMaterialName(const scene::Node& mesh)
{
    const std::string base = "M_" + mesh.name();
    std::string name = base;
    for (int i = 1; !scene::Graph::current().findAll(name).empty(); ++i)
        name = base + std::to_string(i);
    return name;
}

/** Creates a material and assigns it to the meshes at paths. Returns its name. */
std::string assignNewMaterial(const scene::Node& namedAfter, const std::vector<std::string>& paths)
{
    const std::string material = ops::execute("createMaterial", argsCall({newMaterialName(namedAfter)})).get<std::string>();
    json call = argsCall({material});
    for (const std::string& p : paths)
        call["args"].push_back(p);
    ops::execute("assignMaterial", call);
    return material;
}

/**
 * The materials of the meshes (the selected ones, else with everyMesh every mesh) that use no texture set, in scene
 * order. Meshes only on Default first share a new material.
 */
std::vector<std::string> materialsWithoutSet(bool everyMesh)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<scene::Node*> meshes;
    for (scene::NodeId id : g.selection().empty() && !everyMesh ? std::vector<uint64_t>() : texturePaintTargets())
        if (scene::Node* n = g.node(id))
            meshes.push_back(n);

    std::vector<std::string> materials, unassigned, kept;
    scene::Node* firstUnassigned = nullptr;
    for (scene::Node* n : meshes)
    {
        scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        std::vector<std::string> names = mesh ? mesh->materialNames() : std::vector<std::string>{};
        // A mesh only on Default gets a material; a Default slot beside real ones is left alone.
        if (names.empty() || (names.size() == 1 && names[0] == scene::materials::kDefaultSlot))
        {
            unassigned.push_back(n->path());
            firstUnassigned = firstUnassigned ? firstUnassigned : n;
            continue;
        }
        for (const std::string& m : names)
        {
            if (m == scene::materials::kDefaultSlot || std::ranges::find(materials, m) != materials.end() || std::ranges::find(kept, m) != kept.end())
                continue;
            // A material that already uses a set keeps it.
            (tex::setForMaterial(g, m).empty() ? materials : kept).push_back(m);
        }
    }
    if (!unassigned.empty())
        materials.push_back(assignNewMaterial(*firstUnassigned, unassigned));
    if (!kept.empty())
        log::info("New Texture Set: {} keep the texture sets they use", joinNames(kept));
    return materials;
}

/** See the texCreateSetForSelection doc. */
std::string createSetForSelection() { return createSetFor(materialsWithoutSet(false)); }

/** See the texCreateSetPerMaterial doc. */
json createSetPerMaterial()
{
    json out = json::array();
    for (const std::string& m : materialsWithoutSet(true))
        out.push_back(createSetFor({m}));
    if (out.empty())
        log::info("New Texture Set per Material: every material already uses a texture set");
    return out;
}

void newSetPerMaterial()
{
    try
    {
        json r = ops::execute("texCreateSetPerMaterial", json::object(), CallSource::UI);
        if (r.is_array() && !r.empty())
            openSet(r.front().get<std::string>());
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
}

void newSet()
{
    try
    {
        json r = ops::execute("texCreateSetForSelection", json::object(), CallSource::UI);
        if (r.is_string())
            openSet(r.get<std::string>());
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
}

QString fileFilter()
{
    return "Texture graphs (*.lilamat *.lilatexnodes *.lilatexnode);;Materials (*.lilamat);;Node libraries (*.lilatexnodes *.lilatexnode)";
}

void importFiles()
{
    QStringList files = ui::fileDialog::getOpenFileNames(QApplication::activeWindow(), "Import Texture Graphs", QString(), fileFilter());
    UndoChunk chunk("Import Texture Graphs");
    std::string last;
    for (const QString& f : files)
        if (json r = runUi("texImportGraph", argsCall({f.toStdString()})); r.is_array() && !r.empty())
            last = r.back().get<std::string>();
    if (!last.empty())
        openSet(last);
}

void exportFile(const char* suffix)
{
    std::string set = currentTextureSet();
    if (set.empty())
    {
        log::warn("No texture set to export");
        return;
    }
    QString path = ui::fileDialog::getSaveFileName(QApplication::activeWindow(), "Export Texture Graph", qs(set) + "." + suffix,
                                                std::string_view(suffix) == "lilamat" ? "Materials (*.lilamat)" : "Node libraries (*.lilatexnodes)");
    if (path.isEmpty())
        return;
    if (json r = runUi("texExportGraph", argsCall({set, path.toStdString()})); r.is_string())
        log::info("exported {}", r.get<std::string>());
}
} // namespace

void openTextureSet(const std::string& set)
{
    openSet(set);
}

void registerTexGraphBackend()
{
    if (graphBackends::find(kTextureGraphBackend))
        return;
    graphBackends::add(std::make_shared<TexBackend>());
}

std::string currentTextureSet()
{
    scene::Graph& g = scene::Graph::current();
    if (NodeEditor* e = texEditor(); e && tex::findSet(g, e->group().toStdString()))
        return e->group().toStdString();
    if (std::string s = texture2d::current().set; !s.empty() && tex::findSet(g, s))
        return s;
    std::vector<std::string> names = tex::setNames(g);
    return names.empty() ? std::string() : names.front();
}

std::string renameTextureSet(const std::string& set, const std::string& to)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = tex::findSet(g, set);
    if (!n)
        return {};
    json path = runUi("rename", {{"args", {n->path(), to}}});
    scene::Node* renamed = path.is_string() ? g.find(path.get<std::string>()) : nullptr;
    return renamed ? renamed->name() : std::string();
}

bool deleteTextureSet(const std::string& set)
{
    scene::Node* n = tex::findSet(scene::Graph::current(), set);
    return n && tryRunUi("delete", {{"args", {n->path()}}});
}

void registerTextureGraphUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    registerTexGraphBackend();

    panels::add({.id = kTextureGraphDock,
                 .title = "Texture Graph",
                 .create =
                     [](QWidget* p) -> QWidget* {
                         auto* e = new NodeEditor(p, kTextureGraphBackend);
                         texEditors().push_back(e);
                         QObject::connect(e, &QObject::destroyed, [e] { std::erase(texEditors(), e); });
                         return e;
                     },
                 .area = Qt::BottomDockWidgetArea,
                 .icon = "texture",
                 .open = false,
                 .toggleAction = "window.textureGraph",
                 .docs = "node-editor"});
    panels::add({.id = kTexture2dDock,
                 .title = "2D View",
                 .create = [](QWidget* p) -> QWidget* { return new Texture2DView(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "photo",
                 .open = false,
                 .toggleAction = "window.texture2d"});

    ops::registerCommand({
        .name = "texCreateSetPerMaterial",
        .doc = "Make a texture set, with a paint layer, for each material of the selected meshes (every mesh when nothing is "
               "selected) that uses no set yet, and pick it in that material. Meshes only on Default first share a new "
               "material. Returns the sets made.",
        .run = [](const Args&) -> json { return createSetPerMaterial(); },
    });
    actions::add({.id = "texture.newSetPerMaterial",
                  .label = "New Texture Set per Material",
                  .category = "Texture",
                  .run = [] { newSetPerMaterial(); },
                  .doc = "Make a texture set with a paint layer for each material of the selected meshes (every mesh when nothing "
                         "is selected) that has none.",
                  .python = "texCreateSetPerMaterial()",
                  .keywords = {"texture sets", "all materials", "batch"}});
    ops::registerCommand({
        .name = "texCreateSetForSelection",
        .doc = "Make a texture set with a paint layer and pick it in the materials of the selected meshes (and the meshes under "
               "selected groups) that use no set yet. Meshes only on Default first share a new material. Returns the set.",
        .run = [](const Args&) -> json { return createSetForSelection(); },
    });
    actions::add({.id = "texture.newSet",
                  .label = "New Texture Set",
                  .category = "Texture",
                  .run = [] { newSet(); },
                  .doc = "Make a texture set with a paint layer, picked by the selected meshes' materials that have none, and open it "
                         "in the Texture Graph.",
                  .python = "texCreateSet(\"HullTex\", materials=[\"M_Hull\", \"M_Canopy\"])"});
    actions::add({.id = "texture.renameSet",
                  .label = "Rename Texture Set",
                  .category = "Texture",
                  .run =
                      [] {
                          NodeEditor* e = texEditor();
                          if (e && e->isVisible())
                              e->startGroupRename();
                          else if (!startSetRename())
                              log::warn("Rename Texture Set: open the Texture Set panel or the Texture Graph");
                      },
                  .doc = "Rename the texture set shown, in place in its picker.",
                  .python = "lila.ops.rename(\"RockTex\", \"BoulderTex\")"});
    actions::add({.id = "texture.deleteSet",
                  .label = "Delete Texture Set",
                  .category = "Texture",
                  .run =
                      [] {
                          if (std::string set = currentTextureSet(); set.empty())
                              log::warn("Delete Texture Set: no texture set");
                          else
                              deleteTextureSet(set);
                      },
                  .doc = "Delete the texture set shown in the Texture Graph, with its graph and layers. Undo brings it back.",
                  .python = "lila.ops.delete(\"RockTex\")"});
    actions::add({.id = "texture.import",
                  .label = "Import Texture Graph...",
                  .category = "Texture",
                  .run = [] { importFiles(); },
                  .doc = "Read .lilamat materials as new texture sets, or add a .lilatexnodes node library to your node library.",
                  .python = "lila.ops.texImportGraph(\"rock.lilamat\")"});
    actions::add({.id = "texture.importPainterSmart",
                  .label = "Import from Painter...",
                  .category = "Texture",
                  .run = [] { importPainterSmartMaterialsDialog(); },
                  .doc = "Convert Substance 3D Painter smart materials, smart masks and brush presets (.spsm, .spmsk, .sppr) for the shelf. "
                         "What can't come across exactly is listed in the log.",
                  .python = "lila.ops.texImportPainterSmartMaterials(\"C:/path/to/smart-materials\")",
                  .keywords = {"substance", "spsm", "adobe", "smart material", "convert"}});
    actions::add({.id = "texture.exportMaterial",
                  .label = "Export Material...",
                  .category = "Texture",
                  .run = [] { exportFile("lilamat"); },
                  .doc = "Write the current texture set's graph and settings to a .lilamat file to share or reuse.",
                  .python = "lila.ops.texExportGraph(\"RockTex\", \"rock.lilamat\")"});
    actions::add({.id = "texture.exportNodes",
                  .label = "Export Node Library...",
                  .category = "Texture",
                  .run = [] { exportFile("lilatexnodes"); },
                  .doc = "Write the current texture set's node groups to a .lilatexnodes library, each as a node others can add.",
                  .python = "lila.ops.texExportGraph(\"RockTex\", \"rock.lilatexnodes\")"});
    actions::add({.id = "texture.exportMaps",
                  .label = "Export Maps",
                  .category = "Texture",
                  .run =
                      [] {
                          if (std::string set = currentTextureSet(); !set.empty())
                              if (json r = runUi("texExport", argsCall({set})); r.is_object())
                                  log::info("exported {} maps", r.value("files", json::array()).size());
                      },
                  .doc = "Evaluate the current texture set and write its maps through its export preset (Unreal by default).",
                  .python = "lila.ops.texExport(\"RockTex\")"});
    actions::add({.id = "texture.exportVariants",
                  .label = "Export All Variants",
                  .category = "Texture",
                  .run =
                      [] {
                          if (std::string set = currentTextureSet(); !set.empty())
                          {
                              json call = argsCall({set});
                              call["variants"] = true;
                              if (json r = runUi("texExport", call); r.is_object())
                                  log::info("exported {} variants", r.value("variants", json::object()).size());
                          }
                      },
                  .doc = "Write the current texture set's maps once for the base and once for each variant, told apart by the {variant} "
                         "token in the export preset's names (added when the preset has none).",
                  .python = "lila.ops.texExport(\"RockTex\", variants=True)",
                  .keywords = {"variants", "export", "colourways", "skins"}});
    actions::add({.id = "texture.exportFlipbook",
                  .label = "Export Flipbook",
                  .category = "Texture",
                  .run =
                      [] {
                          if (std::string set = currentTextureSet(); !set.empty())
                              if (json r = runUi("texExportFlipbook", argsCall({set})); r.is_object())
                                  log::info("exported {} frames, {} x {} at {} fps", r.value("frames", 0), r.value("columns", 0), r.value("rows", 0),
                                            r.value("fps", 0.0));
                      },
                  .doc = "Write the current texture set over the playback range as flipbook sheets, one per channel: its Image Sequence "
                         "nodes and keyed parameters at every frame, for a game material to play.",
                  .python = "texExportFlipbook(\"FireTex\", startFrame=1, endFrame=16, cellSize=256)",
                  .keywords = {"flipbook", "subuv", "sprite sheet", "animated texture", "image sequence", "atlas"}});
    actions::add({.id = "texture.flipbookMaterials",
                  .label = "Flipbook Materials in Unreal",
                  .category = "Texture",
                  .run =
                      [] {
                          if (json r = runUi("texFlipbookMaterials", json::object()); r.is_object())
                              log::info("flipbook materials: {}", r.value("function", std::string()));
                      },
                  .doc = "Create MF_LilaFlipbook in the connected Unreal project, the material function that plays an exported flipbook, "
                         "with the materials M_LilaFlipbook and M_LilaFlipbookTranslucent built on it.",
                  .python = "texFlipbookMaterials()",
                  .keywords = {"flipbook", "subuv", "unreal", "material function", "animated texture"}});
    actions::add({.id = "texture.clearPreview",
                  .label = "Clear Mesh Preview",
                  .category = "Texture",
                  .run = [] { texture2d::clearMeshPreview(); },
                  .doc = "Stop showing the 2D View's output on meshes; they draw their materials again.",
                  .python = "lila.ui.run_action(\"texture.clearPreview\")"});

    menus::addMenu({.id = "Texture", .title = "Texture", .before = "Rig", .sets = {"Texture"}});
    menus::addActions("Texture", {"texture.newSet", "texture.newSetPerMaterial", "texture.renameSet", "texture.deleteSet", "tex.paint", "tex.stencil", "tex.quickMaskOverlay", "tex.paintAlongCurve", "tex.patchFill", "tex.transferSet", "", "window.textureGraph", "window.texture2d", "", "texture.import", "tex.materialFromMaps", "tex.materialsFromFolder", "texture.importPainterSmart", "tex.importPtex", "tex.exportPtex", "texture.exportMaterial",
                                  "texture.exportNodes", "texture.exportMaps", "texture.exportVariants", "texture.exportFlipbook", "texture.flipbookMaterials", "", "texture.clearPreview", "#View", "viewport.textureView3d",
                                  "viewport.textureViewSplit", "viewport.textureView2d"});
}
} // namespace lila::ui
