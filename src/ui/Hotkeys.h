#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include "ui/HotkeyChord.h"

#include <nlohmann/json.hpp>

#include <QObject>
#include <QPoint>
#include <QTimer>

#include <map>
#include <optional>
#include <string>

class QKeyEvent;
class QMouseEvent;
class QWidget;

namespace lila::ui
{
/**
 * Binding layers contributed in code or from an extension's resources/hotkeys. A layer is defaults under the user's
 * hotkeys, shaped like the prefs file's "contexts". A layer with a mode applies only while that mode is active, and
 * sits over each context it names: its keys win over the same context's own, slot by slot (see
 * hotkeys::overlaySlots), so a mode can take a key's tap and keep its hold.
 */
namespace hotkeyLayers
{
struct Layer
{
    std::string id;
    nlohmann::json contexts = nlohmann::json::object(); // {"global": {"Ctrl+K": target, ...}, "viewport": {...}}
    std::string mode;                                   // empty: always on
};
LILA_UI_API Registry<Layer>& registry();
LILA_UI_API void add(Layer layer, std::string owner = kLilaOwner);
LILA_UI_API uint64_t add(ext::Context& ctx, Layer layer);
LILA_UI_API bool remove(const std::string& id);
} // namespace hotkeyLayers

/**
 * App-wide hotkeys from the "hotkeys" prefs file:
 *   { "holdDelayMs": 180,
 *     "contexts": { "global": { "Ctrl+Z": "edit.undo",
 *                               "W": { "press": "tool.move", "LMB": { "markingMenu": "moveTool" } } },
 *                   "viewport": { ... } } }
 * Keys match by physical position (US layout names), so other layouts keep the same hand positions. A widget
 * opts into a context with the dynamic property "hotkeyContext"; lookup walks from the focus widget outwards, then
 * falls back to "global". A focused list or tree with the property "hotkeyPassThrough" set to a context name (see
 * passThrough) also gets that context's keys, Maya style, and swallows typed letters instead of keyboard-searching. A focused widget with the property "numericEntry" gets digits,
 * "." and "-" typed without modifiers, so a value can be typed straight in. Tap vs hold: with a hold target, releasing before holdDelayMs runs press. LMB/MMB/RMB run
 * when that button goes down while the key is held, and then the key's own targets don't run (see HeldKey).
 * Undo and redo (scene and camera) repeat while held, on the /undo/repeatDelayMs and /undo/repeatIntervalMs "general" prefs.
 */
class LILA_UI_API Hotkeys : public QObject
{
    Q_OBJECT

public:
    static Hotkeys& get();
    void install();

    /** "Ctrl+Z" style label for the first key bound to target in any context, or "". */
    QString keyFor(const std::string& actionId) const;
    /** True when e is a key bound to actionId (as its press target) in any context. */
    bool isKeyFor(QKeyEvent* e, const std::string& actionId) const;
    /** Lets the viewport's hotkeys (Q/W/E/R, F, ...) through while view has focus, instead of keyboard search. */
    static void passThrough(QWidget* view, const char* context = "viewport");

    /** The mode whose layer (the hotkeys prefs' /modes/<mode>/contexts) sits over the contexts; empty for none. */
    void setMode(const std::string& mode);
    const std::string& mode() const { return mMode; }

    /** True while an action runs because its key went down, rather than from a menu, a button or a script. */
    static bool fromKey();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Chord
    {
        int mods = 0;      // Qt::KeyboardModifiers
        int qtKey = 0;     // used when no scancode mapping exists
        uint32_t scan = 0; // physical key, 0 if unmapped
        bool operator<(const Chord& o) const { return std::tie(mods, qtKey, scan) < std::tie(o.mods, o.qtKey, o.scan); }
    };
    struct Binding
    {
        hotkeys::Slots targets;
        QString keyText;
    };
    struct Pending
    {
        Binding binding;
        uint32_t scan = 0;
        int qtKey = 0;
        hotkeys::HeldKey state;
    };

    Hotkeys();
    void reload();
    std::optional<Binding> lookup(QKeyEvent* e, QWidget* focus) const;
    /** By physical key when both sides know it, else by Qt key with shifted symbols normalized. */
    static bool chordMatches(const Chord& chord, QKeyEvent* e);
    static bool textInputFocused(QWidget* w);
    void onHoldTimeout();
    bool onMousePress(QObject* obj, QMouseEvent* e);
    bool fireButton(const nlohmann::json& target, QMouseEvent* e);
    void releaseAll();
    void startRepeat(QKeyEvent* e, const nlohmann::json& target);
    void stopRepeat();

    /** A held key whose action repeats on mRepeatTimer (see hotkeys::repeatsWhileHeld). */
    struct Repeat
    {
        uint32_t scan = 0;
        int qtKey = 0;
        nlohmann::json target;
    };

    /** A mode layer's key: its raw value, overlaid on the context's own binding when looked up. */
    struct Over
    {
        nlohmann::json value;
        QString keyText;
    };

    std::map<std::string, std::vector<std::pair<Chord, Binding>>> mContexts;
    std::map<std::string, std::vector<std::pair<Chord, Over>>> mModeContexts;
    std::string mMode;
    std::optional<Pending> mPending;
    std::vector<Pending> mHeldKeys;
    std::optional<Repeat> mRepeat;
    QTimer mHoldTimer;
    QTimer mRepeatTimer;
    int mSwallowRelease = 0; // Qt::MouseButtons whose release follows a press this filter ate
    int mHoldDelay = 180;
};
} // namespace lila::ui
