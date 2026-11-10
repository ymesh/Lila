#include "ui/ViewportTool.h"

#include "core/Events.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Hotkeys.h"
#include "ui/Layouts.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
struct QueuedMessage
{
    QString text;
    glm::vec4 color;
};
std::vector<QueuedMessage> gMessages;
std::vector<ToolStatus> gStatuses;
/** A tool drew its own key list this frame, so the layout's keys stay out of the corner. */
bool gCornerKeysDrawn = false;
std::map<std::string, std::vector<viewportKeys::Provider>>& layoutKeys()
{
    static std::map<std::string, std::vector<viewportKeys::Provider>> keys;
    return keys;
}

/** The widest each part of a tool's readout has been, by tool then part, so the readout only ever grows. */
std::map<QString, std::map<QString, double>>& rememberedWidths()
{
    static std::map<QString, std::map<QString, double>> widths;
    return widths;
}

constexpr double kStatusPadX = 10.0, kStatusPadY = 4.0, kStatusRow = 20.0, kStatusBaseline = 14.0;
constexpr double kStatusGroupGap = 20.0, kStatusItemGap = 14.0, kStatusPairGap = 5.0;
constexpr float kStatusBright = 0.95f, kStatusValue = 0.92f, kStatusPrompt = 0.78f, kStatusSubject = 0.62f, kStatusLabel = 0.55f, kStatusOff = 0.38f;
const glm::vec4 kStatusPlate(0.08f, 0.08f, 0.08f, 0.8f);

glm::vec4 gray(float g) { return {g, g, g, 1.0f}; }

/** One run of text in a readout row, with the gap before it. */
struct Run
{
    QString text;
    float shade;
    double gapBefore = 0.0;
};

/** A slot holds runs that share a remembered width. */
struct Slot
{
    QString key;
    std::vector<Run> runs;
    double gapBefore = 0.0;
    bool dividerBefore = false;
};

double runsWidth(const std::vector<Run>& runs)
{
    double w = 0.0;
    for (const Run& r : runs)
        w += r.gapBefore + OverlayPainter::textWidth(r.text);
    return w;
}

std::vector<Slot> settingsRow(const ToolStatus& s)
{
    std::vector<Slot> row;
    row.push_back({"tool", {{s.tool, kStatusBright}}});
    if (!s.subject.isEmpty())
        row.push_back({"subject", {{s.subject, kStatusSubject}}, kStatusItemGap * 0.6});
    bool first = true;
    auto add = [&](QString key, std::vector<Run> runs) {
        row.push_back({std::move(key), std::move(runs), first ? kStatusGroupGap : kStatusItemGap, first});
        first = false;
    };
    if (!s.mode.isEmpty())
        add("mode", {{s.mode, kStatusBright}});
    for (const ToolField& f : s.fields)
        add("f:" + f.label, {{f.label, kStatusLabel}, {f.value, kStatusValue, kStatusPairGap}});
    for (const ToolFlag& f : s.flags)
        add("g:" + f.label, {{f.label, f.on ? kStatusBright : kStatusOff}});
    return row;
}

/** The prompt and key hints, wrapped into lines no wider than maxWidth. */
std::vector<std::vector<Run>> hintLines(const ToolStatus& s, double maxWidth)
{
    std::vector<std::vector<Run>> lines(1);
    double x = 0.0;
    auto place = [&](std::vector<Run> item) {
        const double w = runsWidth(item);
        if (!lines.back().empty() && x + kStatusItemGap + w > maxWidth)
        {
            lines.emplace_back();
            x = 0.0;
        }
        if (!lines.back().empty())
        {
            item.front().gapBefore = kStatusItemGap;
            x += kStatusItemGap;
        }
        x += w;
        for (Run& r : item)
            lines.back().push_back(std::move(r));
    };
    if (!s.prompt.isEmpty())
        place({{s.prompt, kStatusPrompt}});
    if (lines.back().empty())
        lines.pop_back();
    return lines;
}

/** Moves r down until it clears every occupied rect. */
QRectF clearOf(QRectF r, const std::vector<QRectF>& occupied)
{
    for (bool moved = true; moved;)
    {
        moved = false;
        for (const QRectF& o : occupied)
            if (o.intersects(r))
            {
                r.moveTop(o.bottom() + 4.0);
                moved = true;
            }
    }
    return r;
}

void drawRuns(OverlayPainter& p, double x, double baseline, const std::vector<Run>& runs)
{
    for (const Run& r : runs)
    {
        x += r.gapBefore;
        x += p.text(QPointF(x, baseline), r.text, gray(r.shade));
    }
}

/** Draws s with its top at or below top; returns the bottom of its plate. */
double drawStatus(OverlayPainter& p, const ViewInfo& view, const ToolStatus& s, double top, std::vector<QRectF>& occupied)
{
    std::map<QString, double>& widths = rememberedWidths()[s.tool];
    std::vector<Slot> row = settingsRow(s);
    double rowWidth = 0.0;
    for (const Slot& slot : row)
    {
        double& w = widths[slot.key];
        w = std::max(w, runsWidth(slot.runs));
        rowWidth += slot.gapBefore + w;
    }
    const double maxInner = std::max(200.0, view.width - 24.0 - 2.0 * kStatusPadX);
    const std::vector<std::vector<Run>> hints = hintLines(s, std::max(maxInner, rowWidth));
    double inner = rowWidth;
    for (const std::vector<Run>& line : hints)
        inner = std::max(inner, runsWidth(line));
    double& plateWidth = widths[QString()];
    plateWidth = std::max(plateWidth, inner + 2.0 * kStatusPadX);

    const double height = kStatusRow * double(1 + hints.size()) + 2.0 * kStatusPadY;
    QRectF plate(std::max(12.0, (view.width - plateWidth) / 2.0), top, plateWidth, height);
    plate = clearOf(plate, occupied);
    p.rect(plate, kStatusPlate);

    double x = plate.left() + kStatusPadX, baseline = plate.top() + kStatusPadY + kStatusBaseline;
    for (const Slot& slot : row)
    {
        x += slot.gapBefore;
        if (slot.dividerBefore)
            p.rect(QRectF(x - kStatusGroupGap / 2.0, plate.top() + kStatusPadY + 4.0, 1.0, kStatusRow - 8.0), {1.0f, 1.0f, 1.0f, 0.14f});
        drawRuns(p, x, baseline, slot.runs);
        x += widths[slot.key];
    }
    if (!hints.empty())
        p.rect(QRectF(plate.left() + kStatusPadX, plate.top() + kStatusPadY + kStatusRow, plate.width() - 2.0 * kStatusPadX, 1.0), {1.0f, 1.0f, 1.0f, 0.08f});
    for (const std::vector<Run>& line : hints)
    {
        baseline += kStatusRow;
        drawRuns(p, plate.left() + kStatusPadX, baseline, line);
    }
    occupied.push_back(plate);
    return plate.bottom();
}
} // namespace

const QCursor& deleteCursor()
{
    static const QCursor cursor = [] {
        const int size = 24;
        const qreal dpr = qApp->devicePixelRatio();
        QPixmap pm(int(size * dpr), int(size * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing);
        for (auto [width, color] : {std::pair{4.5, QColor(20, 20, 20)}, std::pair{2.0, QColor(240, 240, 240)}})
        {
            painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(6, 6), QPointF(18, 18));
            painter.drawLine(QPointF(18, 6), QPointF(6, 18));
        }
        painter.end();
        return QCursor(pm, size / 2, size / 2);
    }();
    return cursor;
}

namespace
{
std::optional<ToolSymmetry> gSelectionSymmetry;

/** The symmetry chip centred at top; returns its bottom. Lit and tinted when on, so the state reads at a glance. */
double drawSymmetry(OverlayPainter& p, const ViewInfo& view, const ToolSymmetry& sym, double top, std::vector<QRectF>& occupied)
{
    static double widest = 0.0;
    constexpr double dot = 14.0;
    const QString value = !sym.on ? QString("off") : sym.axis.isEmpty() ? QString("on") : sym.axis;
    const double labelWidth = OverlayPainter::textWidth("Symmetry");
    widest = std::max(widest, dot + labelWidth + kStatusPairGap + OverlayPainter::textWidth(value));
    QRectF plate((view.width - widest) / 2.0 - kStatusPadX, top, widest + 2.0 * kStatusPadX, kStatusRow + 2.0 * kStatusPadY);
    plate = clearOf(plate, occupied);
    const bool colored = viewportDisplay::accentColors(viewportDisplay::kComponentColors);
    const glm::vec4 accent = colored ? themeSrgb("accent", glm::vec4(0.3f, 0.75f, 1.0f, 1.0f)) : gray(kStatusBright);
    p.rect(plate, kStatusPlate);
    if (sym.on && colored)
        p.rect(plate, {accent.r, accent.g, accent.b, 0.18f});

    double x = plate.left() + kStatusPadX;
    const double baseline = plate.top() + kStatusPadY + kStatusBaseline;
    const QPointF centre(x + 4.0, baseline - 4.5);
    if (sym.on)
        p.disc(centre, 3.5f, accent);
    else
    {
        std::vector<QPointF> ring;
        for (int k = 0; k < 16; ++k)
            ring.push_back(centre + QPointF(std::cos(k * 0.3927), std::sin(k * 0.3927)) * 3.0);
        p.polyline(ring, 1.0f, gray(kStatusOff), true);
    }
    x += dot;
    x += p.text(QPointF(x, baseline), "Symmetry", gray(sym.on ? kStatusBright : kStatusLabel));
    p.text(QPointF(x + kStatusPairGap, baseline), value, sym.on ? accent : gray(kStatusOff));
    occupied.push_back(plate);
    return plate.bottom();
}
} // namespace

void drawSelectionSymmetry(const ToolSymmetry& symmetry) { gSelectionSymmetry = symmetry; }

void drawToolStatus(OverlayPainter&, const ViewInfo&, const ToolStatus& status) { gStatuses.push_back(status); }

void drawToolMessage(OverlayPainter&, const ViewInfo&, const QString& text, const glm::vec4& color)
{
    gMessages.push_back({text, color});
}

void flushToolMessages(OverlayPainter& p, const ViewInfo& view, const std::vector<QRectF>& hud)
{
    std::vector<QRectF> occupied = hud;
    double top = 8.0;
    std::optional<ToolSymmetry> sym = gSelectionSymmetry;
    for (const ToolStatus& s : gStatuses)
        if (s.symmetry)
        {
            sym = s.symmetry;
            break;
        }
    gSelectionSymmetry.reset();
    if (sym)
        top = drawSymmetry(p, view, *sym, top, occupied) + 4.0;
    // The corner key list follows what is active: the running tool's keys, which change with the modifiers held.
    std::vector<KeyHint> toolKeys;
    for (const ToolStatus& s : gStatuses)
    {
        top = drawStatus(p, view, s, top, occupied) + 4.0;
        if (toolKeys.empty())
            toolKeys = s.keys;
    }
    const bool toolActive = !gStatuses.empty();
    gStatuses.clear();
    double y = top + 16.0;
    for (const QueuedMessage& m : gMessages)
    {
        const double w = OverlayPainter::textWidth(m.text);
        const double x = std::max(12.0, (view.width - w) / 2.0);
        for (bool moved = true; moved;)
        {
            moved = false;
            QRectF r(x - 6.0, y - 16.0, w + 12.0, 22.0);
            for (const QRectF& o : occupied)
                if (o.intersects(r))
                {
                    y = o.bottom() + 20.0;
                    moved = true;
                }
        }
        p.label(QPointF(x, y), m.text, m.color);
        y += 22.0;
    }
    gMessages.clear();
    if (!gCornerKeysDrawn && keysShown())
        if (const std::vector<KeyHint> keys = toolActive ? toolKeys : viewportKeys::forLayout(layouts::current()); !keys.empty())
            drawKeyList(p, QPointF(16.0, view.height - 84.0), keys);
    gCornerKeysDrawn = false;
}

bool keysShown() { return PrefStore::get().value<bool>("general", kShowKeysPref, true); }

namespace viewportKeys
{
void add(const std::string& layout, Provider provider) { layoutKeys()[layout].push_back(std::move(provider)); }

std::vector<KeyHint> forLayout(const std::string& layout)
{
    std::vector<KeyHint> out;
    if (auto it = layoutKeys().find(layout); it != layoutKeys().end())
        for (const Provider& p : it->second)
            for (KeyHint& k : p())
                out.push_back(std::move(k));
    return out;
}

void addAction(std::vector<KeyHint>& out, const std::string& actionId, const QString& what)
{
    if (const QString key = Hotkeys::get().keyFor(actionId); !key.isEmpty())
        out.push_back({key, what});
}

void registerBuiltins()
{
    using Keys = std::vector<std::pair<const char*, const char*>>;
    // Read at draw time, so the list follows the user's bindings.
    auto fromActions = [](Keys keys) {
        return [keys] {
            std::vector<KeyHint> out;
            for (const auto& [id, what] : keys)
                addAction(out, id, what);
            addAction(out, "window.searchMenus", "Search");
            return out;
        };
    };
    const Keys transform = {{"tool.move", "Move"}, {"tool.rotate", "Rotate"}, {"tool.scale", "Scale"}};
    Keys model = transform;
    model.insert(model.end(), {{"mesh.extrude", "Extrude"}, {"mesh.bevel", "Bevel"}, {"mesh.insertLoop", "Insert Loop"}, {"display.smoothOn", "Smooth preview"}});
    add("Model", fromActions(model));
    add("UV", fromActions({{"uv.unwrap", "Unwrap"}, {"uv.pack", "Pack"}, {"uv.islandMode", "Islands"}}));
    Keys rig = transform;
    rig.insert(rig.end(), {{"edit.parent", "Parent"}, {"edit.unparent", "Unparent"}});
    add("Rig", fromActions(rig));
    add("Animate", fromActions({{"anim.setKey", "Set key"}, {"anim.play", "Play"}, {"anim.nextKey", "Next key"}, {"anim.prevKey", "Previous key"}}));
    Keys level = transform;
    level.insert(level.end(), {{"edit.duplicateWithTransform", "Duplicate again"}, {"viewport.frameSelected", "Frame"}});
    add("Level", fromActions(level));
}
} // namespace viewportKeys

namespace
{
constexpr double kKeyLine = 17.0, kKeyGap = 12.0;

double widestKey(const std::vector<KeyHint>& hints)
{
    double w = 0.0;
    for (const KeyHint& h : hints)
        w = std::max(w, double(OverlayPainter::textWidth(h.keys)));
    return w;
}
} // namespace

double keyListHeight(size_t rows) { return kKeyLine * double(rows); }

double keyListWidth(const std::vector<KeyHint>& hints)
{
    double what = 0.0;
    for (const KeyHint& h : hints)
        what = std::max(what, double(OverlayPainter::textWidth(h.what)));
    return widestKey(hints) + kKeyGap + what;
}

void drawKeyList(OverlayPainter& p, QPointF bottomLeft, const std::vector<KeyHint>& hints, float alpha)
{
    gCornerKeysDrawn = true;
    if (!keysShown())
        return;
    const double keyWidth = widestKey(hints);
    double y = bottomLeft.y() - keyListHeight(hints.size()) + 4.0;
    const glm::vec4 key(0.3f, 0.75f, 1.0f, alpha), white(0.95f, 0.95f, 0.95f, alpha);
    p.setShadow(true);
    for (const KeyHint& h : hints)
    {
        p.text(QPointF(bottomLeft.x(), y), h.keys, key);
        p.text(QPointF(bottomLeft.x() + keyWidth + kKeyGap, y), h.what, white);
        y += kKeyLine;
    }
    p.setShadow(false);
}
} // namespace lila::ui

namespace lila::ui::viewportTools
{
namespace
{
std::unique_ptr<ViewportTool> gTool;
std::string gToolId; // registered id gTool came from
std::optional<glm::dvec3> gOrbitPoint;
std::map<int, std::function<void()>> gListeners;
int gNext = 1;

void install(std::unique_ptr<ViewportTool> tool, std::string id)
{
    if (gTool)
        gTool->deactivate();
    gOrbitPoint.reset();
    gTool = std::move(tool);
    gToolId = gTool ? std::move(id) : std::string();
    if (gTool)
        gTool->activate();
    requestRedraw();
    events::send({.type = events::Event::ViewportToolChanged, .name = gTool ? gTool->name() : std::string()});
}
} // namespace

Registry<ToolDef>& registry()
{
    static Registry<ToolDef>* r = [] {
        auto* reg = new Registry<ToolDef>();
        // The active tool's code may belong to whoever registered it, so it ends before its definition goes.
        reg->addListener([](RegistryChange change, const std::string& id, const std::string&) {
            if (change == RegistryChange::Removed && gTool && gToolId == id)
                install(nullptr, {});
        });
        return reg;
    }();
    return *r;
}

const ToolDef& add(ToolDef def)
{
    std::string id = def.id;
    return registry().add(std::move(id), std::move(def));
}

const ToolDef* add(ext::Context& ctx, ToolDef def)
{
    std::string id = def.id;
    try
    {
        ctx.add(registry(), "viewportTool", id, std::move(def));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: viewport tool: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

bool activate(std::string_view id)
{
    const ToolDef* def = registry().find(id);
    if (!def || !def->create)
        return false;
    std::unique_ptr<ViewportTool> tool = def->create();
    if (!tool)
        return false;
    install(std::move(tool), std::string(id));
    return true;
}

const std::string& activeId() { return gToolId; }

void set(std::unique_ptr<ViewportTool> tool) { install(std::move(tool), {}); }

ViewportTool* active() { return gTool.get(); }

namespace
{
scene::NodeId gHoverHighlight = scene::kNoNode;
}

void setHoverHighlight(scene::NodeId id)
{
    if (id == gHoverHighlight)
        return;
    gHoverHighlight = id;
    requestRedraw();
}

scene::NodeId hoverHighlight() { return gHoverHighlight; }

void requestRedraw()
{
    auto copy = gListeners;
    for (auto& [id, fn] : copy)
        fn();
}

int addListener(std::function<void()> fn)
{
    gListeners[gNext] = std::move(fn);
    return gNext++;
}

void removeListener(int id) { gListeners.erase(id); }

void setOrbitPoint(std::optional<glm::dvec3> point) { gOrbitPoint = point; }

std::optional<glm::dvec3> orbitPoint() { return gOrbitPoint; }
} // namespace lila::ui::viewportTools
