#include "ui/CursorBadges.h"

#include "core/PrefStore.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "scene/Components.h"
#include "scene/SoftSelect.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"
#include "ui/PaintSelect.h"
#include "ui/SurfaceSnap.h"
#include "ui/Tools.h"

#include <QPainter>
#include <QStringList>

#include <algorithm>

namespace lila::ui::cursorBadges
{
namespace
{
constexpr double kIcon = 16.0;
constexpr double kGap = 4.0;
const QPointF kOffset(16.0, 20.0); // clear of the arrow cursor's tip and body

struct Entry
{
    Badge badge;
    uint64_t order = 0; // when first added, so replacing a badge keeps its place
};

struct Badges
{
    Registry<Entry> registry;
    uint64_t next = 0;
    std::vector<const Badge*> ordered; // display order, rebuilt on change

    Badges()
    {
        registry.addListener([this](RegistryChange, const std::string&, const std::string&) {
            std::vector<Entry*> all = registry.all();
            std::sort(all.begin(), all.end(), [](const Entry* a, const Entry* b) { return a->order < b->order; });
            ordered.clear();
            for (const Entry* e : all)
                ordered.push_back(&e->badge);
        });
    }
};

Badges& badges()
{
    static Badges b;
    return b;
}

/** Badges name Tabler icons freely; the overlay atlas only draws registered ones. */
void ensureIcon(const std::string& icon)
{
    if (!icon.empty() && !overlayIcons::contains(icon))
        overlayIcons::add(icon, {.tabler = QString::fromStdString(icon)});
}

struct Status
{
    const void* owner = nullptr;
    QString text;
    std::function<void(const QString&)> sink;
};

Status& status()
{
    static Status s;
    return s;
}

QString lineFor(const Badge& b)
{
    QStringList keys;
    if (!b.key.isEmpty())
        keys << b.key;
    for (const std::string& id : b.actions)
        if (QString k = Hotkeys::get().keyFor(id); !k.isEmpty() && !keys.contains(k))
            keys << k;
    return keys.isEmpty() ? b.message : b.message + " (" + keys.join(" / ") + ")";
}
} // namespace

void add(Badge badge, std::string owner)
{
    Badges& b = badges();
    ensureIcon(badge.icon);
    const Entry* had = b.registry.find(badge.id);
    uint64_t order = had ? had->order : b.next++;
    std::string id = badge.id;
    b.registry.put(std::move(id), {std::move(badge), order}, std::move(owner));
}

ext::Context::Handle add(ext::Context& ctx, Badge badge)
{
    Badges& b = badges();
    ensureIcon(badge.icon);
    std::string id = badge.id;
    try
    {
        return ctx.add(b.registry, "cursorBadge", std::move(id), Entry{std::move(badge), b.next++});
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: cursor badge: {}", ctx.owner(), e.what());
        return 0;
    }
}

bool remove(std::string_view id) { return badges().registry.remove(id); }
size_t removeOwned(std::string_view owner) { return badges().registry.removeOwned(owner); }

std::vector<std::string> ids()
{
    std::vector<std::string> out;
    for (const Badge* b : badges().ordered)
        out.push_back(b->id);
    return out;
}

std::vector<const Badge*> active(const ViewportState& view)
{
    std::vector<const Badge*> out;
    if (!PrefStore::get().value<bool>("general", kEnabledPref, true))
        return out;
    // A copy: a predicate may add or remove badges.
    const std::vector<const Badge*> all = badges().ordered;
    for (const Badge* b : all)
        if (b->active && b->active(view))
            out.push_back(b);
    return out;
}

void draw(OverlayPainter& p, QPointF cursor, const std::vector<const Badge*>& list)
{
    if (list.empty())
        return;
    QPointF at = cursor + kOffset;
    QRectF all(at, QSizeF(list.size() * (kIcon + kGap), kIcon));
    glm::vec4 color = p.ink(0.88f, 1.0f, all);
    p.setShadow(true);
    double x = at.x();
    for (const Badge* b : list)
    {
        p.icon(QRectF(x, at.y(), kIcon, kIcon), b->icon, color);
        x += kIcon + kGap;
    }
    p.setShadow(false);
}

void draw(QPainter& p, QPointF cursor, const std::vector<const Badge*>& list)
{
    QPointF at = cursor + kOffset;
    for (const Badge* b : list)
        if (QIcon i = icon(QString::fromStdString(b->icon)); !i.isNull())
        {
            i.paint(&p, QRectF(at, QSizeF(kIcon, kIcon)).toRect(), Qt::AlignCenter, QIcon::Active);
            at.rx() += kIcon + kGap;
        }
}

void publish(const void* view, const std::vector<const Badge*>& list)
{
    Status& s = status();
    if (list.empty() && s.owner != view)
        return;
    QStringList lines;
    for (const Badge* b : list)
        lines << lineFor(*b);
    QString text = lines.join("   ");
    s.owner = list.empty() ? nullptr : view;
    if (text == s.text)
        return;
    s.text = text;
    if (s.sink)
        s.sink(text);
}

void setStatusSink(std::function<void(const QString&)> shown)
{
    status().sink = std::move(shown);
}

void registerDefaults()
{
    auto snap = [](unsigned flag) { return [flag](const ViewportState&) { return (tools::snaps() & flag) != 0; }; };
    add({"snap.grid", "grid-pattern", snap(SnapGrid), "Grid snapping enabled", {"snap.gridOn"}});
    add({"snap.edge", "line", snap(SnapEdge), "Edge snapping enabled", {"snap.edgeOn"}});
    add({"snap.vertex", "point", snap(SnapVertex), "Vertex snapping enabled", {"snap.vertexOn"}});
    add({"snap.surface", "magnet", snap(SnapSurface), "Surface snapping enabled", {"snap.surfaceOn"}});
    add({"snap.rotateHold", "angle", snap(SnapRotateHold), "Rotate snap toggled", {"snap.rotateHoldOn"}});
    add({"pivotEdit", "target", [](const ViewportState&) { return tools::pivotEdit(); }, "Pivot edit",
         {"tool.pivotEditOn", "tool.pivotEditToggle"}});
    add({"cloneDrag", "copy", [](const ViewportState&) { return tools::cloneDrag(); }, "Drag a handle to clone", {"tool.cloneKeyDown", "tool.cloneDragOn"}});
    add({"softSelect.adjust", "circle-dotted", [](const ViewportState&) { return tools::softSelectAdjusting(); },
         "Drag to set the soft select radius", {"softSelect.keyDown", "softSelect.adjustOn"}});
    add({"softSelect.on", "ripple",
         [](const ViewportState&) {
             return !tools::softSelectAdjusting() && scene::components::mode() != scene::ComponentMode::Object &&
                    PrefStore::get().value<bool>("general", scene::softSelect::kEnabled, false);
         },
         "Soft select on", {"softSelect.toggle"}});
    add({"tabSelect.add", "circle-plus", [](const ViewportState& v) { return v.tabSelects == true; }, "Drag to select", {}, "Tab"});
    add({"tabSelect.remove", "circle-minus", [](const ViewportState& v) { return v.tabSelects == false; }, "Drag to deselect", {}, "Tab"});
    add({"select.lassoHold", "lasso", [](const ViewportState&) { return paintSelect::lassoHeld(); }, "Drag to lasso select",
         {"select.lassoHoldOn"}});
    add({"manipulator.precision", "feather", [](const ViewportState& v) { return v.gizmo && v.gizmo->precisionDrag(); },
         "Precision drag", {}, "Shift"});
    add({"manipulator.ctrlPlane", "perspective",
         [](const ViewportState& v) {
             auto* m = dynamic_cast<const Manipulator*>(v.gizmo);
             return m && m->ctrlPlaneDrag();
         },
         "Dragging in the plane of the other two axes", {}, "Ctrl"});
    add({"manipulator.surfaceSlide", "magnet",
         [](const ViewportState& v) {
             auto* m = dynamic_cast<const Manipulator*>(v.gizmo);
             return m && m->surfaceSlideDrag();
         },
         "Sliding along the surface under the cursor", {}, "Ctrl+MMB"});
    add({"snapSurface", "layers-intersect",
         [](const ViewportState&) { return tools::current() == Tool::Move && surfaceSnap::active(); },
         "Moves conform to the snap surface", {"snapSurface.clear"}});
}
} // namespace lila::ui::cursorBadges
