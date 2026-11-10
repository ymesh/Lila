#include "ui/SelectionHandlers.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/SelectAs.h"
#include "ui/NodeLookup.h"

#include <algorithm>
#include <map>

namespace lila::ui::selectionHandlers
{
namespace
{
std::string gActive;
bool gSelectAsHeld = false;
std::map<int, std::function<void()>> gListeners;
int gNextListener = 1;

void notify()
{
    auto copy = gListeners;
    for (auto& [id, fn] : copy)
        fn();
}
} // namespace

Registry<Handler>& registry()
{
    static Registry<Handler>* r = [] {
        auto* reg = new Registry<Handler>();
        reg->addListener([](RegistryChange change, const std::string& id, const std::string&) {
            if (change == RegistryChange::Removed && id == gActive)
            {
                gActive.clear();
                notify();
            }
        });
        return reg;
    }();
    return *r;
}

const Handler& add(Handler handler)
{
    std::string id = handler.id;
    return registry().add(std::move(id), std::move(handler));
}

const Handler* add(ext::Context& ctx, Handler handler)
{
    std::string id = handler.id;
    try
    {
        ctx.add(registry(), "selectionHandler", id, std::move(handler));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: selection handler: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

const Handler* find(std::string_view id) { return registry().find(id); }

bool setActive(std::string_view id)
{
    if (!id.empty() && !registry().contains(id))
        return false;
    if (gActive == id)
        return true;
    gActive = std::string(id);
    notify();
    return true;
}

const std::string& activeId() { return gActive; }

const Handler* active() { return gActive.empty() ? nullptr : registry().find(gActive); }

bool anyRedirect()
{
    if (active())
        return true;
    for (const Handler* h : registry().all())
        if (h->always && h->redirect)
            return true;
    return false;
}

std::vector<std::string> redirect(const std::vector<Hit>& hits)
{
    std::vector<const Handler*> handlers;
    for (const Handler* h : registry().allInOrder())
        if (h->always && h->redirect)
            handlers.push_back(h);
    if (const Handler* h = active(); h && !h->always)
        handlers.push_back(h);
    std::vector<std::string> out;
    auto push = [&](const std::string& name) {
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end())
            out.push_back(name);
    };
    // Only the selection mask blocks a redirect: joints hidden from view are still what a mesh click picks.
    auto maskedOut = [](const std::string& name) {
        const scene::Node* n = findNode(name);
        return n && !scene::selectionMask::enabled(scene::selectionMask::kindOf(*n));
    };
    for (const Hit& hit : hits)
    {
        std::optional<std::vector<std::string>> to;
        for (const Handler* h : handlers)
        {
            try
            {
                if (h->redirect)
                    to = h->redirect(hit);
            }
            catch (const std::exception& e)
            {
                log::error("selection handler {}: {}", h->id, e.what());
            }
            if (to)
                break;
        }
        std::vector<std::string> picked;
        if (to)
            for (const std::string& name : *to)
                if (std::string as = applySelectAs(name); !maskedOut(as))
                    picked.push_back(as);
        // A redirect onto a masked kind (the joint mask off) selects what was clicked instead; an empty one drops the hit.
        if (!to || (!to->empty() && picked.empty()))
            push(hit.name);
        for (const std::string& name : picked)
            push(name);
    }
    return out;
}

std::vector<Candidate> candidates(const Hit& hit)
{
    const Handler* h = active();
    if (!h || !h->candidates)
        return {};
    try
    {
        return h->candidates(hit);
    }
    catch (const std::exception& e)
    {
        log::error("selection handler {}: {}", h->id, e.what());
        return {};
    }
}

json candidateMenu(const std::vector<Candidate>& cands, const json& fallback)
{
    if (cands.empty())
        return nullptr;
    static const char* kSlots[] = {"N", "S", "W", "E", "NE", "NW", "SE", "SW"};
    json menu = json::object();
    json overflow = json::array();
    for (size_t i = 0; i < cands.size(); ++i)
    {
        json item = {{"label", cands[i].label.isEmpty() ? cands[i].name : cands[i].label.toStdString()},
                     {"action", {{"command", "select"}, {"args", {{"args", {cands[i].name}}}}}}};
        if (i < std::size(kSlots))
            menu[kSlots[i]] = std::move(item);
        else
            overflow.push_back(std::move(item));
    }
    if (fallback.is_object() && !fallback.empty())
    {
        // A cascade has no compass, so the fallback's compass items join its overflow list in compass order.
        json rows = json::array();
        for (const char* slot : {"N", "NE", "E", "SE", "S", "SW", "W", "NW"})
            if (fallback.contains(slot) && fallback[slot].is_object())
                rows.push_back(fallback[slot]);
        if (fallback.contains("overflow") && fallback["overflow"].is_array())
            for (const json& r : fallback["overflow"])
                rows.push_back(r);
        if (!rows.empty())
        {
            if (!overflow.empty())
                overflow.push_back({{"separator", true}});
            overflow.push_back({{"label", "Object Menu"}, {"children", {{"overflow", rows}}}});
        }
    }
    if (!overflow.empty())
        menu["overflow"] = std::move(overflow);
    return menu;
}

bool selectAsBypassed() { return gSelectAsHeld || PrefStore::get().value<bool>("general", "/rig/selectAsBypass", false); }

void setSelectAsHeld(bool held) { gSelectAsHeld = held; }

std::string applySelectAs(const std::string& name)
{
    if (selectAsBypassed())
        return name;
    const scene::Graph& g = scene::Graph::current();
    const scene::Node* n = findNode(name);
    if (!n || !scene::rig::isJoint(*n))
        return name;
    scene::NodeId t = scene::rig::selectAsTarget(g, n->id());
    return t != n->id() && g.node(t) ? g.node(t)->path() : name;
}

int addListener(std::function<void()> fn)
{
    gListeners[gNextListener] = std::move(fn);
    return gNextListener++;
}

void removeListener(int id) { gListeners.erase(id); }
} // namespace lila::ui::selectionHandlers
