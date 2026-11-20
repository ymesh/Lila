#include "ui/ViewportHooks.h"

#include "core/Log.h"
#include "core/Registry.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <map>

namespace lila::ui::viewportHooks
{
namespace
{
/** A hook and when it was first added, so hooks run in the order added and replacing one keeps its place. */
template <class Fn> struct Ordered
{
    Fn fn;
    uint64_t order = 0;
};

template <class Fn> class Hooks
{
public:
    explicit Hooks(const char* kind) : mKind(kind) {}

    void put(std::string id, Fn fn, std::string owner)
    {
        const Ordered<Fn>* had = mRegistry.find(id);
        uint64_t order = had ? had->order : mNext++;
        mRegistry.put(std::move(id), {std::move(fn), order}, std::move(owner));
    }

    ext::Context::Handle add(ext::Context& ctx, std::string id, Fn fn)
    {
        try
        {
            return ctx.add(mRegistry, mKind, std::move(id), Ordered<Fn>{std::move(fn), mNext++});
        }
        catch (const std::exception& e)
        {
            log::error("extension {}: viewport {}: {}", ctx.owner(), mKind, e.what());
            return 0;
        }
    }

    std::string anonymousId() { return std::string("lila.") + mKind + "." + std::to_string(mAnonymous++); }

    bool remove(std::string_view id) { return mRegistry.remove(id); }
    size_t removeOwned(std::string_view owner) { return mRegistry.removeOwned(owner); }

    std::vector<std::pair<uint64_t, std::string>> sorted() const
    {
        std::vector<std::pair<uint64_t, std::string>> out;
        for (const std::string& id : mRegistry.ids())
            out.emplace_back(mRegistry.find(id)->order, id);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<Fn> fns() const
    {
        std::vector<Fn> out;
        for (auto& [order, id] : sorted())
            out.push_back(mRegistry.find(id)->fn);
        return out;
    }

    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        for (auto& [order, id] : sorted())
            out.push_back(id);
        return out;
    }

private:
    const char* mKind;
    Registry<Ordered<Fn>> mRegistry;
    uint64_t mNext = 0;
    int mAnonymous = 0;
};

Hooks<Drawer>& drawerHooks()
{
    static Hooks<Drawer> h("drawer");
    return h;
}

Hooks<Picker>& pickerHooks()
{
    static Hooks<Picker> h("picker");
    return h;
}
} // namespace

void addDrawer(Drawer fn) { drawerHooks().put(drawerHooks().anonymousId(), std::move(fn), {}); }
void addPicker(Picker fn) { pickerHooks().put(pickerHooks().anonymousId(), std::move(fn), {}); }

void addDrawer(std::string id, Drawer fn, std::string owner) { drawerHooks().put(std::move(id), std::move(fn), std::move(owner)); }
void addPicker(std::string id, Picker fn, std::string owner) { pickerHooks().put(std::move(id), std::move(fn), std::move(owner)); }

ext::Context::Handle addDrawer(ext::Context& ctx, std::string id, Drawer fn) { return drawerHooks().add(ctx, std::move(id), std::move(fn)); }
ext::Context::Handle addPicker(ext::Context& ctx, std::string id, Picker fn) { return pickerHooks().add(ctx, std::move(id), std::move(fn)); }

bool removeDrawer(std::string_view id) { return drawerHooks().remove(id); }
bool removePicker(std::string_view id) { return pickerHooks().remove(id); }

size_t removeOwned(std::string_view owner) { return drawerHooks().removeOwned(owner) + pickerHooks().removeOwned(owner); }

std::vector<Drawer> drawers() { return drawerHooks().fns(); }
std::vector<Picker> pickers() { return pickerHooks().fns(); }
std::vector<std::string> drawerIds() { return drawerHooks().ids(); }
std::vector<std::string> pickerIds() { return pickerHooks().ids(); }
namespace
{
std::map<std::string, MaterialHook, std::less<>>& materialHooks()
{
    static std::map<std::string, MaterialHook, std::less<>> h;
    return h;
}
} // namespace

void setMaterialHook(std::string id, MaterialHook fn) { materialHooks()[std::move(id)] = std::move(fn); }

bool removeMaterialHook(std::string_view id)
{
    auto it = materialHooks().find(id);
    if (it == materialHooks().end())
        return false;
    materialHooks().erase(it);
    return true;
}

bool hookedMaterials(scene::NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& materials)
{
    for (const auto& [name, fn] : materialHooks())
        if (fn && fn(id, mesh, materials))
            return true;
    return false;
}
namespace
{
std::map<std::string, MaterialDecorator, std::less<>>& materialDecorators()
{
    static std::map<std::string, MaterialDecorator, std::less<>> h;
    return h;
}
} // namespace

void setMaterialDecorator(std::string id, MaterialDecorator fn) { materialDecorators()[std::move(id)] = std::move(fn); }

bool removeMaterialDecorator(std::string_view id)
{
    auto it = materialDecorators().find(id);
    if (it == materialDecorators().end())
        return false;
    materialDecorators().erase(it);
    return true;
}

void decorateMaterials(scene::NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& materials)
{
    for (const auto& [name, fn] : materialDecorators())
        if (fn)
            fn(id, mesh, materials);
}
namespace
{
std::map<std::string, HideHook, std::less<>>& hideHooks()
{
    static std::map<std::string, HideHook, std::less<>> h;
    return h;
}
} // namespace

void setHideHook(std::string id, HideHook fn) { hideHooks()[std::move(id)] = std::move(fn); }

bool removeHideHook(std::string_view id)
{
    auto it = hideHooks().find(id);
    if (it == hideHooks().end())
        return false;
    hideHooks().erase(it);
    return true;
}

bool hiddenInView(scene::NodeId id)
{
    for (const auto& [name, fn] : hideHooks())
        if (fn && fn(id))
            return true;
    return false;
}
namespace
{
std::map<std::string, ClickHook, std::less<>>& clickHooks()
{
    static std::map<std::string, ClickHook, std::less<>> h;
    return h;
}
} // namespace

void setClickHook(std::string id, ClickHook fn) { clickHooks()[std::move(id)] = std::move(fn); }

bool removeClickHook(std::string_view id)
{
    auto it = clickHooks().find(id);
    if (it == clickHooks().end())
        return false;
    clickHooks().erase(it);
    return true;
}

bool handleClick(QPointF px, const ViewInfo& view)
{
    for (const auto& [name, fn] : clickHooks())
        if (fn && fn(px, view))
            return true;
    return false;
}

namespace
{
std::map<std::string, DropHandler, std::less<>>& dropHandlers()
{
    static std::map<std::string, DropHandler, std::less<>> h;
    return h;
}
} // namespace

void setDropHandler(std::string id, DropHandler fn) { dropHandlers()[std::move(id)] = std::move(fn); }

bool removeDropHandler(std::string_view id)
{
    auto it = dropHandlers().find(id);
    if (it == dropHandlers().end())
        return false;
    dropHandlers().erase(it);
    return true;
}

bool handleDrop(const QMimeData& mime, const DropTarget& target, bool commit)
{
    for (const auto& [name, fn] : dropHandlers())
        if (fn && fn(mime, target, commit))
            return true;
    return false;
}
namespace
{
std::map<std::string, DragHook, std::less<>>& dragHooks()
{
    static std::map<std::string, DragHook, std::less<>> h;
    return h;
}

std::map<std::string, WheelHook, std::less<>>& wheelHooks()
{
    static std::map<std::string, WheelHook, std::less<>> h;
    return h;
}

struct ActiveDrag
{
    std::string id;
    Qt::MouseButton button = Qt::NoButton;
};

ActiveDrag& activeDrag()
{
    static ActiveDrag d;
    return d;
}

const DragHook* activeHook()
{
    auto it = dragHooks().find(activeDrag().id);
    return it == dragHooks().end() ? nullptr : &it->second;
}
} // namespace

void setDragHook(std::string id, DragHook hook) { dragHooks()[std::move(id)] = std::move(hook); }

bool removeDragHook(std::string_view id)
{
    auto it = dragHooks().find(id);
    if (it == dragHooks().end())
        return false;
    if (activeDrag().id == id)
        activeDrag() = {};
    dragHooks().erase(it);
    return true;
}

bool pressDrag(QMouseEvent* e, const ViewInfo& view)
{
    if (dragging())
        return true;
    for (const auto& [id, hook] : dragHooks())
        if (hook.press && hook.press(e, view))
        {
            activeDrag() = {id, e->button()};
            return true;
        }
    return false;
}

bool dragging() { return activeHook() != nullptr; }

void moveDrag(QMouseEvent* e, const ViewInfo& view)
{
    if (const DragHook* h = activeHook(); h && h->move)
        h->move(e, view);
}

bool releaseDrag(QMouseEvent* e, const ViewInfo& view)
{
    const DragHook* h = activeHook();
    if (!h)
        return false;
    if (e->button() != activeDrag().button)
        return true;
    auto release = h->release;
    activeDrag() = {};
    if (release)
        release(e, view);
    return true;
}

void setWheelHook(std::string id, WheelHook fn) { wheelHooks()[std::move(id)] = std::move(fn); }

bool removeWheelHook(std::string_view id)
{
    auto it = wheelHooks().find(id);
    if (it == wheelHooks().end())
        return false;
    wheelHooks().erase(it);
    return true;
}

bool handleWheel(QWheelEvent* e, const ViewInfo& view)
{
    for (const auto& [id, fn] : wheelHooks())
        if (fn && fn(e, view))
            return true;
    return false;
}

} // namespace lila::ui::viewportHooks
