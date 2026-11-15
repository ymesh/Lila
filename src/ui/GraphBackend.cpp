#include "ui/GraphBackend.h"

#include "core/ExtensionContext.h"
#include "scene/Graph.h"
#include "ui/Theme.h"

#include <algorithm>

namespace lila::ui
{
GraphBackend::~GraphBackend() = default;

bool GraphBackend::supports(std::string_view) const { return true; }

bool GraphBackend::isQuery(std::string_view verb) const { return verb == "GroupInfo" || verb == "NodeTypes"; }

bool GraphBackend::hasGroup(scene::Graph& g, const std::string& group) const
{
    std::vector<std::string> names = groupNames(g);
    return std::find(names.begin(), names.end(), group) != names.end();
}

GraphBackend::Owner GraphBackend::followSelection(scene::Graph&) const { return {}; }

std::string GraphBackend::groupOfOwner(scene::Graph&, const std::string&) const { return {}; }

std::vector<std::string> GraphBackend::sendTargets(scene::Graph&, const std::string&, const std::string&) const { return {}; }

GraphBackend::SocketStyle GraphBackend::socketStyle(const std::string& type) const
{
    if (type == "float")
        return {themeColor("socketFloat")};
    if (type == "int")
        return {themeColor("socketInt"), SocketShape::Square};
    if (type == "bool")
        return {themeColor("socketBool"), SocketShape::Square, SocketMark::Dot};
    if (type == "string")
        return {themeColor("socketString"), SocketShape::Pill};
    if (type == "color")
        return {themeColor("socketColor"), SocketShape::Circle, SocketMark::Ring};
    return {themeColor("socketVector"), SocketShape::Circle, type == "vector" ? SocketMark::Dot : SocketMark::None};
}

QString GraphBackend::wireSummary(const nlohmann::json& info, const std::string& node, const std::string&) const
{
    if (!info.is_object())
        return {};
    for (const nlohmann::json& n : info.value("nodes", nlohmann::json::array()))
    {
        if (n.value("name", "") != node)
            continue;
        QStringList parts;
        for (const nlohmann::json& in : n.value("inputs", nlohmann::json::array()))
        {
            if (in.value("linked", false))
                return {};
            const nlohmann::json& v = in.value("value", nlohmann::json());
            if (v.is_number() || v.is_boolean() || v.is_string())
                parts << QString::fromStdString(in.value("id", "")) + " " + QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
            else if (v.is_array() && v.size() <= 4)
                parts << QString::fromStdString(in.value("id", "")) + " " + QString::fromStdString(v.dump());
        }
        if (parts.isEmpty() || parts.size() > 4)
            return {};
        return "From constants: " + parts.join(", ");
    }
    return {};
}

QColor GraphBackend::headerColor(const std::string& type, const std::string&) const
{
    if (type == "GroupInput" || type == "GroupOutput")
        return themeColor("nodeGroupIo");
    return themeColor("nodeHeader");
}

QString GraphBackend::canvasHint() const
{
    return "Select a node to edit its values.\n\nTab / Shift+A add a node under the cursor; while dragging a wire, or letting one go "
           "on empty canvas, it lists only nodes that connect. Ctrl+drag cuts links, Alt+click a socket disconnects it, Delete / X deletes, Ctrl+C / Ctrl+V copy "
           "and paste as text, Ctrl+D duplicates, F frames the selection, Home frames all. Hold Z and drag across wires to zip-tie "
           "them; drag a tie to move it, Shift+drag to turn it, right-click it for colour and removal. Alt+right-drag from one node to "
           "another connects them; Ctrl+drag a linked input moves its link, Shift+drag copies it.";
}

Registry<GraphBackendPtr>& graphBackends::registry()
{
    static Registry<GraphBackendPtr> r;
    return r;
}

void graphBackends::add(GraphBackendPtr backend, std::string owner)
{
    std::string id = backend->id();
    registry().put(std::move(id), std::move(backend), std::move(owner));
}

uint64_t graphBackends::add(ext::Context& ctx, GraphBackendPtr backend)
{
    std::string id = backend->id();
    if (const std::string* owner = registry().ownerOf(id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "graph backend '" + id + "' already belongs to " + *owner);
        return 0;
    }
    add(std::move(backend), ctx.owner());
    return ctx.track("graphBackend:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool graphBackends::remove(const std::string& id) { return registry().remove(id); }

GraphBackendPtr graphBackends::find(const std::string& id)
{
    GraphBackendPtr* b = registry().find(id);
    return b ? *b : nullptr;
}

std::vector<std::string> graphBackends::ids() { return registry().idsInOrder(); }
} // namespace lila::ui
