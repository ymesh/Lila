#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace lila::ui::hotkeys
{
/** JSON keys of the mouse buttons a hotkey pairs with, indexed left, middle, right. */
inline constexpr const char* kButtonKeys[3] = {"LMB", "MMB", "RMB"};

/** general.json: "labels" matches keys by the character printed on them, "positions" by where they sit on a US keyboard. */
inline constexpr const char* kKeysFollowPref = "/hotkeys/keysFollow";

/**
 * Whether the key with this Windows set-1 scancode matches by its US position. The number row always does, so AZERTY's
 * unshifted digits still reach the number hotkeys.
 */
inline bool matchesByPosition(uint32_t scan, bool positions)
{
    if (scan >= 0x02 && scan <= 0x0B)
        return true;
    return positions && scan >= 0x02 && scan <= 0x35;
}

/** Qt::ShiftModifier, spelled out so this header stays free of Qt. */
inline constexpr int kShiftModifier = 0x02000000;

/** The US key a shifted symbol sits on ('>' is on '.'), else key. Qt::Key codes of printable ASCII are the characters. */
inline int unshiftedUsKey(int key)
{
    switch (key)
    {
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    case '_': return '-';
    case '+': return '=';
    case '{': return '[';
    case '}': return ']';
    case '|': return '\\';
    case ':': return ';';
    case '"': return '\'';
    case '<': return ',';
    case '>': return '.';
    case '?': return '/';
    case '~': return '`';
    default: return key;
    }
}

/** {mods, key} with a shifted symbol turned into Shift plus its key, so "Shift+>", ">" and "Shift+." are one chord. */
inline std::pair<int, int> normalizeShifted(int mods, int key)
{
    int base = unshiftedUsKey(key);
    return base == key ? std::pair{mods, key} : std::pair{mods | kShiftModifier, base};
}

/**
 * The targets of one hotkey value:
 *   "edit.undo"
 *   { "press": ..., "hold": ..., "release": ..., "LMB": ..., "MMB": ..., "RMB": ... }
 * LMB/MMB/RMB run when that mouse button goes down while the key is held (W+LMB opens the move tool's marking menu).
 */
struct Slots
{
    nlohmann::json press, hold, release;
    std::array<nlohmann::json, 3> buttons;

    bool hasButtons() const { return !buttons[0].is_null() || !buttons[1].is_null() || !buttons[2].is_null(); }
    bool empty() const { return press.is_null() && hold.is_null() && release.is_null() && !hasButtons(); }
};

inline Slots splitSlots(const nlohmann::json& value)
{
    Slots s;
    bool multi = value.is_object() && (value.contains("press") || value.contains("hold") || value.contains("release") ||
                                       value.contains("LMB") || value.contains("MMB") || value.contains("RMB"));
    if (!multi)
    {
        s.press = value;
        return s;
    }
    s.press = value.value("press", nlohmann::json());
    s.hold = value.value("hold", nlohmann::json());
    s.release = value.value("release", nlohmann::json());
    for (int i = 0; i < 3; ++i)
        s.buttons[i] = value.value(kButtonKeys[i], nlohmann::json());
    // A marking menu never opens on a bare hold: as in Maya it needs key + mouse button, so it moves to the left button.
    if (s.hold.is_object() && s.hold.contains("markingMenu") && s.buttons[0].is_null())
    {
        s.buttons[0] = s.hold;
        s.hold = nullptr;
    }
    return s;
}

inline nlohmann::json joinSlots(const Slots& s)
{
    if (s.hold.is_null() && s.release.is_null() && !s.hasButtons())
        return s.press;
    nlohmann::json v = nlohmann::json::object();
    if (!s.press.is_null())
        v["press"] = s.press;
    if (!s.hold.is_null())
        v["hold"] = s.hold;
    if (!s.release.is_null())
        v["release"] = s.release;
    for (int i = 0; i < 3; ++i)
        if (!s.buttons[i].is_null())
            v[kButtonKeys[i]] = s.buttons[i];
    return v;
}

/**
 * A mode layer's value over the binding beneath it, so a mode can rebind a key's tap and leave its hold alone (or the
 * other way round). The slots the value names replace those below, a null one clearing it; the rest come through. A
 * plain value names the press slot alone.
 */
inline Slots overlaySlots(Slots below, const nlohmann::json& over)
{
    bool multi = over.is_object() && (over.contains("press") || over.contains("hold") || over.contains("release") ||
                                      over.contains("LMB") || over.contains("MMB") || over.contains("RMB"));
    if (!multi)
    {
        below.press = over;
        return below;
    }
    for (auto [key, slot] : {std::pair{"press", &below.press}, {"hold", &below.hold}, {"release", &below.release}})
        if (over.contains(key))
            *slot = over[key];
    for (int i = 0; i < 3; ++i)
        if (over.contains(kButtonKeys[i]))
            below.buttons[i] = over[kButtonKeys[i]];
    if (below.hold.is_object() && below.hold.contains("markingMenu") && below.buttons[0].is_null())
    {
        below.buttons[0] = below.hold;
        below.hold = nullptr;
    }
    return below;
}

/**
 * A key whose press runs at once and whose release runs at key up (hold D for pivot mode), as opposed to one whose
 * press waits for key up to tell a tap from a hold or a click. Its mouse-button targets fire while it is down and its
 * unbound buttons pass through, so D+LMB still drags the pivot while D+RMB opens a menu.
 */
inline bool isMomentary(const Slots& s)
{
    return s.hold.is_null() && (!s.release.is_null() || !s.hasButtons());
}

/**
 * State of a key that is down and has a hold or mouse-button targets, so its press target waits for key up. Using a
 * mouse button while it is down replaces the tap: the press, hold and release targets then never run.
 */
class HeldKey
{
public:
    enum class Mouse
    {
        Pass,    // the key pairs with no buttons, so the click goes through (B held + drag adjusts soft select)
        Swallow, // paired key, but nothing on this button: eaten so the viewport doesn't select or orbit
        Fire     // run the button's target
    };
    enum class Up
    {
        Tap,       // run press, then release
        AfterHold, // hold already ran; run release
        Used       // a mouse button was used; run nothing
    };

    explicit HeldKey(const Slots& s) : mHasHold(!s.hold.is_null())
    {
        for (int i = 0; i < 3; ++i)
            mButtons[i] = !s.buttons[i].is_null();
    }

    /** The hold delay passed. True when the hold target should run now. */
    bool holdTimeout()
    {
        if (!mHasHold || mHeld || mUsed)
            return false;
        mHeld = true;
        return true;
    }

    Mouse mouseDown(int button)
    {
        if (!mButtons[0] && !mButtons[1] && !mButtons[2])
            return Mouse::Pass;
        mUsed = true;
        return button >= 0 && button < 3 && mButtons[button] ? Mouse::Fire : Mouse::Swallow;
    }

    Up keyUp() const { return mUsed ? Up::Used : mHeld ? Up::AfterHold : Up::Tap; }

private:
    bool mHasHold;
    std::array<bool, 3> mButtons{};
    bool mHeld = false;
    bool mUsed = false;
};

/**
 * Plain bindings whose action repeats while the key is held (scene and camera undo and redo), on the cadence of the
 * /undo/repeatDelayMs and /undo/repeatIntervalMs prefs rather than the OS key repeat.
 */
inline bool repeatsWhileHeld(const nlohmann::json& press)
{
    if (!press.is_string())
        return false;
    const std::string& id = press.get_ref<const std::string&>();
    return id == "edit.undo" || id == "edit.redo" || id == "viewport.cameraUndo" || id == "viewport.cameraRedo";
}
} // namespace lila::ui::hotkeys
