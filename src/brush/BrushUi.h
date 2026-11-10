#pragma once

#include "core/Export.h"

#include "brush/Stroke.h"

#include <QPointF>
#include <QWidget>
#include <Qt>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QKeyEvent;
class QMouseEvent;

namespace lila::ui
{
class ImagePicker;
class OverlayPainter;
struct ViewInfo;
} // namespace lila::ui

/**
 * The Qt side of the brush core, shared by every brush tool: pen pressure from tablet events, the held-key radius and
 * strength drags, the brush cursor and the Stroke options group for tool panels.
 */
namespace lila::brush::ui
{
using SettingsRef = std::function<BrushSettings&()>;

/**
 * Held-key brush controls while a brush tool is active, the same in every tool:
 *
 *   B or F held + LMB drag   radius (F tapped still frames the selection); MMB drag starts from the smallest
 *   M or Shift+F + drag      strength; B or F held + RMB drag too (a tapped Shift+F is the tool's, see setStrengthTap)
 *   [ ]                      radius smaller / larger
 *
 * Also reads pen pressure from tablet events, which it leaves unaccepted so Qt still sends the mouse events.
 */
class LILA_UI_API BrushControls
{
public:
    enum class Adjust
    {
        None,
        Radius,
        Strength,
    };

    /** changed runs on every adjustment (redraw, sync panels); finished once a drag or step ends (save prefs). */
    BrushControls(SettingsRef settings, std::function<void()> changed, std::function<void()> finished);
    ~BrushControls();

    /**
     * Installs the key and tablet filter. extraKeys are the tool's own held keys (taken from the hotkeys while a
     * viewport has focus); onKey gets them, down and up, and returns whether it used them.
     */
    void activate(std::vector<int> extraKeys = {}, std::function<bool(int key, Qt::KeyboardModifiers mods, bool down)> onKey = {});
    void deactivate();

    /** True when the event belongs to an adjustment (the tool should do nothing else with it). */
    bool mousePress(const QMouseEvent* e);
    bool mouseMove(const QMouseEvent* e);
    bool mouseRelease(const QMouseEvent* e);
    /** [ and ]. */
    bool keyPress(const QKeyEvent* e);

    Adjust adjusting() const { return mAdjusting; }
    /** The adjustment a held key has armed for the next press, None when no key is held. */
    Adjust armed() const { return mArmed; }
    /** Where the cursor ring belongs: where an adjustment began, so its size reads against the surface there. */
    QPointF anchor(QPointF hover) const { return mAdjusting != Adjust::None ? mStart : hover; }

    /** Runs when Shift+F is tapped without dragging (sculpting toggles the wireframe). */
    void setStrengthTap(std::function<void()> fn) { mStrengthTap = std::move(fn); }

    /** The pen's latest pressure, 1 when no pen event came recently. */
    double pressure() const;
    StrokeInput input(const QMouseEvent* e) const;

    /** For the filter. */
    bool key(int k, Qt::KeyboardModifiers mods, bool down);
    void tablet(double pressure);

private:
    class Filter;

    SettingsRef mSettings;
    std::function<void()> mChanged, mFinished;
    std::function<bool(int, Qt::KeyboardModifiers, bool)> mOnKey;
    std::function<void()> mStrengthTap;
    std::vector<int> mExtraKeys;
    std::unique_ptr<Filter> mFilter;

    Adjust mArmed = Adjust::None;
    int mArmedKey = 0;
    bool mArmedUsed = false;
    Adjust mAdjusting = Adjust::None;
    QPointF mStart;
    double mFrom = 0.0;

    double mPressure = 1.0;
    int64_t mPressureAt = 0;
};

/** The surface under the cursor, world space. */
struct CursorSurface
{
    glm::dvec3 point{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
};

struct CursorOptions
{
    /** Inner ring at the strength fraction (while adjusting strength). */
    bool strengthRing = false;
    /** A tick along the normal (sculpting). */
    bool normalTick = false;
    /** The live stroke: draws the lazy mouse string and the line stroke's line. */
    const Stroke* stroke = nullptr;
    /** The rim colour, sRGB, when the tool marks the brush (a warning while it would refuse). */
    std::optional<glm::vec4> color;
};

/**
 * The brush ring at `at`: laid on the surface (and squeezed by the tip's roundness) when there is one, a dimmer screen
 * circle when not.
 */
LILA_UI_API void drawCursor(lila::ui::OverlayPainter& p, const lila::ui::ViewInfo& view, const BrushSettings& s, QPointF at,
                            const std::optional<CursorSurface>& surface, const CursorOptions& o = {});

/** Where the cursor ring goes: the stabilised brush while a stroke runs, else the mouse. */
LILA_UI_API QPointF ringPosition(const Stroke& stroke, QPointF hover);

/** Local frame helpers for surface queries: screen +x at a world point, in the object space of `world`. */
LILA_UI_API glm::dvec3 screenRight(const lila::ui::ViewInfo& view, const glm::dmat4& world);

/**
 * The Stroke options group for brush tool panels: radius unit, flow, stroke mode, stabiliser, jitter, tip angle,
 * roundness, alpha (from the alpha library or any image), radial symmetry, and pen pressure in its own group. Both
 * start collapsed; their state is remembered.
 */
class LILA_UI_API StrokeOptions : public QWidget
{
public:
    StrokeOptions(QWidget* parent, SettingsRef settings, std::function<void()> changed);
    /** Re-reads the settings into the controls. */
    void sync();

private:
    void edited();

    SettingsRef mSettings;
    std::function<void()> mChanged;
    QWidget* mBody = nullptr;
    QWidget* mWorldRadius = nullptr;
    lila::ui::ImagePicker* mAlpha = nullptr;
    /** Each field's refresh from the settings, without reporting a change. */
    std::vector<std::function<void()>> mSync;
};
} // namespace lila::brush::ui
