#include <doctest/doctest.h>

#include "ui/HotboxZone.h"
#include "ui/HotkeyChord.h"

using namespace lila::ui;
using nlohmann::json;
using hotbox::Zone;
using hotkeys::HeldKey;

TEST_CASE("hotbox zones split by the diagonals from the centre rectangle's corners")
{
    const double hw = 200, hh = 40;
    CHECK(hotbox::zoneAt(0, 0, hw, hh) == Zone::Center);
    CHECK(hotbox::zoneAt(-199, 39, hw, hh) == Zone::Center);
    CHECK(hotbox::zoneAt(0, -41, hw, hh) == Zone::North);
    CHECK(hotbox::zoneAt(0, 41, hw, hh) == Zone::South);
    CHECK(hotbox::zoneAt(201, 0, hw, hh) == Zone::East);
    CHECK(hotbox::zoneAt(-201, 0, hw, hh) == Zone::West);
    // Above the rectangle's wide top edge it is north even far to the side, until the corner's diagonal.
    CHECK(hotbox::zoneAt(190, -60, hw, hh) == Zone::North);
    CHECK(hotbox::zoneAt(250, -60, hw, hh) == Zone::East);
    CHECK(hotbox::zoneAt(250, -100, hw, hh) == Zone::North);
    CHECK(hotbox::zoneAt(-250, 80, hw, hh) == Zone::West);
    CHECK(hotbox::zoneAt(-250, 100, hw, hh) == Zone::South);
    CHECK(hotbox::markingMenuName(Zone::North, "lmb") == "hotbox_north_lmb");
    CHECK(hotbox::markingMenuName(Zone::Center, "rmb") == "hotbox_center_rmb");
}

TEST_CASE("hotkey values split into press, hold, release and mouse buttons")
{
    hotkeys::Slots plain = hotkeys::splitSlots(json("edit.undo"));
    CHECK(plain.press == "edit.undo");
    CHECK_FALSE(plain.hasButtons());
    CHECK(hotkeys::joinSlots(plain) == json("edit.undo"));

    json w = json::parse(R"({"press": "tool.move", "LMB": {"markingMenu": "moveTool"}, "RMB": "edit.undo"})");
    hotkeys::Slots s = hotkeys::splitSlots(w);
    CHECK(s.press == "tool.move");
    CHECK(s.buttons[0]["markingMenu"] == "moveTool");
    CHECK(s.buttons[1].is_null());
    CHECK(s.buttons[2] == "edit.undo");
    CHECK(hotkeys::joinSlots(s) == w);

    // A marking menu on a bare hold moves to key + left button.
    hotkeys::Slots old = hotkeys::splitSlots(json::parse(R"({"press": "tool.move", "hold": {"markingMenu": "moveTool"}})"));
    CHECK(old.hold.is_null());
    CHECK(old.buttons[0]["markingMenu"] == "moveTool");

    // An action hold stays a hold.
    hotkeys::Slots b = hotkeys::splitSlots(json::parse(R"({"press": "a", "hold": "b", "release": "c"})"));
    CHECK(b.hold == "b");
    CHECK_FALSE(b.hasButtons());
}

TEST_CASE("momentary keys run their press at once, even with mouse buttons")
{
    CHECK(hotkeys::isMomentary(hotkeys::splitSlots(json::parse(R"("tool.move")"))));
    CHECK(hotkeys::isMomentary(hotkeys::splitSlots(json::parse(R"({"press": "tool.pivotEditOn", "release": "tool.pivotEditOff"})"))));
    CHECK(hotkeys::isMomentary(hotkeys::splitSlots(json::parse(R"({"press": "tool.pivotEditOn", "release": "tool.pivotEditOff", "RMB": {"markingMenu": "pivotTool"}})"))));
    CHECK_FALSE(hotkeys::isMomentary(hotkeys::splitSlots(json::parse(R"({"press": "tool.move", "LMB": {"markingMenu": "moveTool"}})"))));
    CHECK_FALSE(hotkeys::isMomentary(hotkeys::splitSlots(json::parse(R"({"press": "a", "hold": "b", "release": "c"})"))));
}

TEST_CASE("held key: tap, hold and key + mouse button")
{
    auto parse = [](const char* text) { return hotkeys::splitSlots(json::parse(text)); };

    SUBCASE("tap without a click runs the press")
    {
        HeldKey k(parse(R"({"press": "tool.move", "LMB": {"markingMenu": "moveTool"}})"));
        CHECK(k.keyUp() == HeldKey::Up::Tap);
    }
    SUBCASE("a click on a bound button fires it and replaces the tap")
    {
        HeldKey k(parse(R"({"press": "tool.move", "LMB": {"markingMenu": "moveTool"}})"));
        CHECK(k.mouseDown(0) == HeldKey::Mouse::Fire);
        CHECK(k.keyUp() == HeldKey::Up::Used);
    }
    SUBCASE("other buttons are swallowed and still count as used")
    {
        HeldKey k(parse(R"({"press": "tool.move", "LMB": {"markingMenu": "moveTool"}})"));
        CHECK(k.mouseDown(2) == HeldKey::Mouse::Swallow);
        CHECK(k.mouseDown(-1) == HeldKey::Mouse::Swallow);
        CHECK(k.keyUp() == HeldKey::Up::Used);
    }
    SUBCASE("keys without buttons let clicks through")
    {
        HeldKey k(parse(R"({"press": "softSelect.toggle", "hold": "softSelect.adjustOn", "release": "softSelect.adjustOff"})"));
        CHECK(k.mouseDown(0) == HeldKey::Mouse::Pass);
        CHECK(k.keyUp() == HeldKey::Up::Tap);
        CHECK(k.holdTimeout());
        CHECK_FALSE(k.holdTimeout());
        CHECK(k.mouseDown(0) == HeldKey::Mouse::Pass);
        CHECK(k.keyUp() == HeldKey::Up::AfterHold);
    }
    SUBCASE("no hold after a button was used")
    {
        HeldKey k(parse(R"({"press": "a", "hold": "b", "MMB": "c"})"));
        CHECK(k.mouseDown(1) == HeldKey::Mouse::Fire);
        CHECK_FALSE(k.holdTimeout());
        CHECK(k.keyUp() == HeldKey::Up::Used);
    }
    SUBCASE("a key with buttons but no hold never times out")
    {
        HeldKey k(parse(R"({"press": "a", "LMB": "b"})"));
        CHECK_FALSE(k.holdTimeout());
        CHECK(k.keyUp() == HeldKey::Up::Tap);
    }
}

TEST_CASE("hotbox key release picks the hovered menu item")
{
    using hotbox::KeyUp;
    CHECK(hotbox::keyUpAction(true, false, false) == KeyUp::Dismiss);
    CHECK(hotbox::keyUpAction(true, true, false) == KeyUp::Dismiss);
    CHECK(hotbox::keyUpAction(false, true, false) == KeyUp::TriggerAndDismiss);
    CHECK(hotbox::keyUpAction(false, false, false) == KeyUp::Dismiss);
    CHECK(hotbox::keyUpAction(false, false, true) == KeyUp::Keep);

    // With a menu open, pressing another menu name switches to it; not the centre button or the menu already open.
    CHECK(hotbox::pressSwitchesMenu(3, false, 2));
    CHECK_FALSE(hotbox::pressSwitchesMenu(3, false, 3));
    CHECK_FALSE(hotbox::pressSwitchesMenu(-1, false, 2));
    CHECK_FALSE(hotbox::pressSwitchesMenu(5, true, 2));
}

TEST_CASE("shifted symbols in hotkey text mean Shift plus the key they sit on")
{
    using hotkeys::kShiftModifier;
    using hotkeys::normalizeShifted;
    using P = std::pair<int, int>;
    // Qt reports Shift+. on a US layout as Shift with Key_Greater; the binding may be written either way.
    CHECK(normalizeShifted(kShiftModifier, '>') == P{kShiftModifier, '.'});
    CHECK(normalizeShifted(0, '>') == P{kShiftModifier, '.'});
    CHECK(normalizeShifted(kShiftModifier, '<') == P{kShiftModifier, ','});
    CHECK(normalizeShifted(kShiftModifier, '.') == P{kShiftModifier, '.'});
    CHECK(normalizeShifted(0, '[') == P{0, '['});
    CHECK(normalizeShifted(0, ']') == P{0, ']'});
    CHECK(normalizeShifted(0, '{') == P{kShiftModifier, '['});
    CHECK(normalizeShifted(0, 'R') == P{0, 'R'});
    CHECK(hotkeys::unshiftedUsKey('?') == '/');
}

TEST_CASE("undo and redo repeat while their key is held")
{
    CHECK(hotkeys::repeatsWhileHeld(json("edit.undo")));
    CHECK(hotkeys::repeatsWhileHeld(json("edit.redo")));
    CHECK(hotkeys::repeatsWhileHeld(json("viewport.cameraUndo")));
    CHECK(hotkeys::repeatsWhileHeld(json("viewport.cameraRedo")));
    CHECK_FALSE(hotkeys::repeatsWhileHeld(json("edit.delete")));
    CHECK_FALSE(hotkeys::repeatsWhileHeld(json()));
}
