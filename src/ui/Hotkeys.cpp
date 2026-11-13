#include "ui/Hotkeys.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Hotbox.h"
#include "ui/HotkeyBindings.h"
#include "ui/MarkingMenu.h"
#include "ui/ViewportTool.h"

#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCursor>
#include <QApplication>
#include <QMenu>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lila::ui
{
namespace
{
constexpr int kModMask = Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;
static_assert(hotkeys::kShiftModifier == int(Qt::ShiftModifier));

/** Windows set-1 scancodes for the US positions of the keys whose labels move between layouts. */
uint32_t usScancode(int qtKey)
{
#ifdef _WIN32
    static const std::map<int, uint32_t> table = {
        {Qt::Key_Q, 0x10}, {Qt::Key_W, 0x11}, {Qt::Key_E, 0x12}, {Qt::Key_R, 0x13}, {Qt::Key_T, 0x14},
        {Qt::Key_Y, 0x15}, {Qt::Key_U, 0x16}, {Qt::Key_I, 0x17}, {Qt::Key_O, 0x18}, {Qt::Key_P, 0x19},
        {Qt::Key_BracketLeft, 0x1A}, {Qt::Key_BracketRight, 0x1B}, {Qt::Key_A, 0x1E}, {Qt::Key_S, 0x1F},
        {Qt::Key_D, 0x20}, {Qt::Key_F, 0x21}, {Qt::Key_G, 0x22}, {Qt::Key_H, 0x23}, {Qt::Key_J, 0x24},
        {Qt::Key_K, 0x25}, {Qt::Key_L, 0x26}, {Qt::Key_Semicolon, 0x27}, {Qt::Key_Apostrophe, 0x28},
        {Qt::Key_QuoteLeft, 0x29}, {Qt::Key_Backslash, 0x2B}, {Qt::Key_Z, 0x2C}, {Qt::Key_X, 0x2D},
        {Qt::Key_C, 0x2E}, {Qt::Key_V, 0x2F}, {Qt::Key_B, 0x30}, {Qt::Key_N, 0x31}, {Qt::Key_M, 0x32},
        {Qt::Key_Comma, 0x33}, {Qt::Key_Period, 0x34}, {Qt::Key_Slash, 0x35}, {Qt::Key_1, 0x02},
        {Qt::Key_2, 0x03}, {Qt::Key_3, 0x04}, {Qt::Key_4, 0x05}, {Qt::Key_5, 0x06}, {Qt::Key_6, 0x07},
        {Qt::Key_7, 0x08}, {Qt::Key_8, 0x09}, {Qt::Key_9, 0x0A}, {Qt::Key_0, 0x0B}, {Qt::Key_Minus, 0x0C},
        {Qt::Key_Equal, 0x0D},
    };
    auto it = table.find(qtKey);
    return it == table.end() ? 0 : it->second;
#else
    (void)qtKey;
    return 0;
#endif
}

uint32_t eventScancode(QKeyEvent* e)
{
#ifdef _WIN32
    uint32_t s = e->nativeScanCode();
    // Only the remappable block; anything else (arrows, F-keys) matches by Qt key.
    return hotkeys::matchesByPosition(s, hotkeys::keysByPosition()) ? s : 0;
#else
    (void)e;
    return 0;
#endif
}

/**
 * The key pressed, by its label. Windows reports Ctrl with a symbol key by the control character it types, so Ctrl+`
 * arrives as '@' and would read as Ctrl+Shift+2; the layout's own character for the virtual key is the key pressed.
 */
int eventKey(QKeyEvent* e)
{
#ifdef _WIN32
    const auto mods = e->modifiers();
    if ((mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier) && hotkeys::unshiftedUsKey(e->key()) != e->key())
    {
        UINT vk = e->nativeVirtualKey();
        if (!vk && e->nativeScanCode())
            vk = MapVirtualKeyW(e->nativeScanCode(), MAPVK_VSC_TO_VK);
        if (UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF; ch > 0x20)
            return QChar(char16_t(ch)).toUpper().unicode();
    }
#endif
    return e->key();
}

bool isStandardTextEdit(int mods, int key)
{
    if (mods == 0 || mods == Qt::ShiftModifier)
        return true;
    if (mods & Qt::ControlModifier)
        switch (key)
        {
        case Qt::Key_Z: case Qt::Key_Y: case Qt::Key_X: case Qt::Key_C: case Qt::Key_V: case Qt::Key_A:
        case Qt::Key_Backspace: case Qt::Key_Delete: case Qt::Key_Left: case Qt::Key_Right:
        case Qt::Key_Home: case Qt::Key_End:
            return true;
        default: break;
        }
    return false;
}

bool sFromKey = false;

/** Runs a key's action with Hotkeys::fromKey() true. */
void runFromKey(const json& target)
{
    const bool outer = sFromKey;
    sFromKey = true;
    actions::run(target);
    sFromKey = outer;
}

/** The hotbox shows on key down rather than after the hold delay, as in Maya. */
bool isHotbox(const json& target)
{
    return target.is_string() && target.get<std::string>() == "hotbox.show";
}

int buttonIndex(Qt::MouseButton b)
{
    switch (b)
    {
    case Qt::LeftButton: return 0;
    case Qt::MiddleButton: return 1;
    case Qt::RightButton: return 2;
    default: return -1;
    }
}

bool isBareModifier(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta;
}

bool isArrowKey(int key)
{
    return key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right;
}

/** Lists, trees, combo boxes and sliders keep their own arrow keys, unless they opt into pickwalking. */
bool startsNumber(const QString& text)
{
    return text.size() == 1 && (text[0].isDigit() || text[0] == '.' || text[0] == '-');
}

bool listNavigationFocused(QWidget* w)
{
    for (QWidget* p = w; p; p = p->parentWidget())
        if (p->property("pickWalk").toBool())
            return false;
    return w && (qobject_cast<QAbstractItemView*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSlider*>(w));
}

/** F2 renames in place in a list whose current item is editable, unless a hotkey context there binds the key itself. */
bool itemRenameFocused(QWidget* w, int mods, int key)
{
    auto* view = qobject_cast<QAbstractItemView*>(w);
    if (mods != 0 || key != Qt::Key_F2 || !view || !(view->editTriggers() & QAbstractItemView::EditKeyPressed))
        return false;
    for (QWidget* p = w; p; p = p->parentWidget())
        if (!p->property("hotkeyContext").toString().isEmpty())
            return false;
    const QModelIndex index = view->currentIndex();
    return index.isValid() && (index.flags() & Qt::ItemIsEditable);
}
} // namespace

Hotkeys& Hotkeys::get()
{
    static Hotkeys h;
    return h;
}

Hotkeys::Hotkeys()
{
    mHoldTimer.setSingleShot(true);
    connect(&mHoldTimer, &QTimer::timeout, this, &Hotkeys::onHoldTimeout);
    mRepeatTimer.setSingleShot(true);
    connect(&mRepeatTimer, &QTimer::timeout, this, [this] {
        if (!mRepeat)
            return;
        actions::run(mRepeat->target);
        mRepeatTimer.start(std::max(10, PrefStore::get().value<int>("general", "/undo/repeatIntervalMs", 100)));
    });
}

void Hotkeys::install()
{
    reload();
    PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "hotkeys")
            reload();
    });
    qApp->installEventFilter(this);
}

Registry<hotkeyLayers::Layer>& hotkeyLayers::registry()
{
    static Registry<Layer> r;
    static int listener = r.addListener([](RegistryChange change, const std::string& id, const std::string&) {
        std::string layer = "layer:" + id;
        if (change == RegistryChange::Removed)
        {
            PrefStore::get().removeDefaults("hotkeys", layer);
            return;
        }
        const Layer* l = r.find(id);
        if (!l)
            return;
        json contexts = l->contexts.is_object() ? l->contexts : json::object();
        PrefStore::get().setDefaults("hotkeys", layer,
                                     l->mode.empty() ? json{{"contexts", contexts}} : json{{"modes", {{l->mode, {{"contexts", contexts}}}}}});
    });
    (void)listener;
    return r;
}

void hotkeyLayers::add(Layer layer, std::string owner)
{
    std::string id = layer.id;
    registry().put(std::move(id), std::move(layer), std::move(owner));
}

uint64_t hotkeyLayers::add(ext::Context& ctx, Layer layer)
{
    if (const std::string* owner = registry().ownerOf(layer.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "hotkey layer '" + layer.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = layer.id;
    add(std::move(layer), ctx.owner());
    return ctx.track("hotkeyLayer:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool hotkeyLayers::remove(const std::string& id)
{
    return registry().remove(id);
}

void Hotkeys::reload()
{
    json def = PrefStore::get().merged("hotkeys");
    mHoldDelay = def.value("holdDelayMs", 180);
    mContexts.clear();
    mModeContexts.clear();
    // Calls fn(context, chord, key text, value) for each parseable key of a {"context": {"key": value}} object.
    auto eachKey = [](const json& contexts, auto fn) {
        if (!contexts.is_object())
            return;
        for (auto& [context, keys] : contexts.items())
        {
            if (!keys.is_object())
                continue;
            for (auto& [keyText, value] : keys.items())
            {
                QKeySequence seq = QKeySequence::fromString(QString::fromStdString(keyText), QKeySequence::PortableText);
                if (seq.isEmpty())
                {
                    log::warn("hotkeys: can't parse key '{}'", keyText);
                    continue;
                }
                QKeyCombination combo = seq[0];
                auto [mods, key] = hotkeys::normalizeShifted(int(combo.keyboardModifiers()) & kModMask, int(combo.key()));
                fn(context, Chord{mods, key, usScancode(key)}, seq.toString(QKeySequence::NativeText), value);
            }
        }
    };
    eachKey(def.value("contexts", json::object()), [this](const std::string& context, Chord chord, QString keyText, const json& value) {
        Binding b;
        b.keyText = std::move(keyText);
        b.targets = hotkeys::splitSlots(value);
        if (!b.targets.empty()) // null unbinds a default
            mContexts[context].emplace_back(chord, std::move(b));
    });
    if (!mMode.empty())
        eachKey(def.value(json::json_pointer("/modes/" + mMode + "/contexts"), json::object()),
                [this](const std::string& context, Chord chord, QString keyText, const json& value) {
                    mModeContexts[context].emplace_back(chord, Over{value, std::move(keyText)});
                });
}

bool Hotkeys::fromKey()
{
    return sFromKey;
}

void Hotkeys::setMode(const std::string& mode)
{
    if (mode == mMode)
        return;
    mMode = mode;
    releaseAll();
    reload();
}

namespace
{
/**
 * With several keys on one action, the one hints name: a letter, digit or function key before punctuation, which sits
 * under the right hand while it holds the mouse and moves between keyboard layouts.
 */
int hintRank(const QString& keyText)
{
    const QString key = keyText.section('+', -1);
    if (key.size() == 1)
        return key[0].isLetterOrNumber() ? 1 : 0;
    return key.startsWith('F') ? 1 : 0;
}

/** The best-ranked key in keys, the first of its rank winning. */
QString bestHint(const std::vector<QString>& keys)
{
    QString best;
    for (const QString& k : keys)
        if (best.isEmpty() || hintRank(k) > hintRank(best))
            best = k;
    return best;
}
} // namespace

QString Hotkeys::keyFor(const std::string& actionId) const
{
    std::vector<QString> found;
    for (const char* ctx : {"global", "viewport"})
        if (auto it = mModeContexts.find(ctx); it != mModeContexts.end())
            for (auto& [chord, over] : it->second)
                if (hotkeys::Slots s = hotkeys::overlaySlots({}, over.value); s.press.is_string() && s.press.get<std::string>() == actionId)
                    found.push_back(over.keyText);
    if (!found.empty())
        return bestHint(found);
    for (const char* ctx : {"global", "viewport"})
    {
        auto it = mContexts.find(ctx);
        if (it == mContexts.end())
            continue;
        for (auto& [chord, b] : it->second)
            if (b.targets.press.is_string() && b.targets.press.get<std::string>() == actionId)
                found.push_back(b.keyText);
    }
    if (!found.empty())
        return bestHint(found);
    // Panel contexts last (graph editor, UV editor...), so a panel's own keys show in its tooltips.
    for (auto& [ctx, bindings] : mContexts)
        for (auto& [chord, b] : bindings)
            if (b.targets.press.is_string() && b.targets.press.get<std::string>() == actionId)
                found.push_back(b.keyText);
    return bestHint(found);
}

bool Hotkeys::chordMatches(const Chord& chord, QKeyEvent* e)
{
    const int held = int(e->modifiers()) & kModMask;
    // By position the modifiers are the ones held: Windows reports Ctrl+` as '@', which normalizing would read as Shift+2.
    if (uint32_t scan = eventScancode(e); chord.scan && scan)
        return chord.scan == scan && chord.mods == held;
    auto [mods, key] = hotkeys::normalizeShifted(held, eventKey(e));
    return chord.mods == mods && chord.qtKey == key;
}

bool Hotkeys::isKeyFor(QKeyEvent* e, const std::string& actionId) const
{
    for (auto& [ctx, list] : mContexts)
        for (auto& [chord, b] : list)
            if (b.targets.press.is_string() && b.targets.press.get<std::string>() == actionId && chordMatches(chord, e))
                return true;
    return false;
}

void Hotkeys::passThrough(QWidget* view, const char* context)
{
    view->setProperty("hotkeyPassThrough", context);
}

bool Hotkeys::textInputFocused(QWidget* w)
{
    return w && (qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) ||
                 qobject_cast<QAbstractSpinBox*>(w) || w->property("hotkeysOff").toBool() ||
                 // An editable combo keeps focus itself and forwards keys to its line edit.
                 (qobject_cast<QComboBox*>(w) && static_cast<QComboBox*>(w)->isEditable()));
}

std::optional<Hotkeys::Binding> Hotkeys::lookup(QKeyEvent* e, QWidget* focus) const
{
    std::vector<std::string> chain;
    QWidget* hovered = QApplication::widgetAt(QCursor::pos());
    for (QWidget* w = hovered; w; w = w->parentWidget())
        if (w->property("hotkeyHover").toBool())
            if (QString ctx = w->property("hotkeyContext").toString(); !ctx.isEmpty())
                chain.push_back(ctx.toStdString());
    for (QWidget* w = focus; w; w = w->parentWidget())
    {
        QString ctx = w->property("hotkeyContext").toString();
        // Hover contexts (the outliner) only apply under the cursor, so F there doesn't steal the viewport's F.
        if (!ctx.isEmpty() && !w->property("hotkeyHover").toBool())
            chain.push_back(ctx.toStdString());
    }
    // Only the view itself, not an inline editor inside it, so typing a name never fires a tool.
    if (focus)
        if (QString ctx = focus->property("hotkeyPassThrough").toString(); !ctx.isEmpty())
            chain.push_back(ctx.toStdString());
    // The panel under the cursor answers keys the focused one leaves unbound, so Space over the viewport opens the
    // hotbox after a click in the channel box or a torn-off menu.
    if (!textInputFocused(focus))
        for (QWidget* w = hovered; w; w = w->parentWidget())
            if (QString ctx = w->property("hotkeyContext").toString(); !ctx.isEmpty() && !w->property("hotkeyHover").toBool())
                if (std::find(chain.begin(), chain.end(), ctx.toStdString()) == chain.end())
                    chain.push_back(ctx.toStdString());
    chain.push_back("global");

    auto matches = [&](const Chord& chord) { return chordMatches(chord, e); };
    for (const std::string& ctx : chain)
    {
        const Binding* own = nullptr;
        if (auto it = mContexts.find(ctx); it != mContexts.end())
            for (auto& [chord, b] : it->second)
                if (!own && matches(chord))
                    own = &b;
        const Over* over = nullptr;
        if (auto it = mModeContexts.find(ctx); it != mModeContexts.end())
            for (auto& [chord, o] : it->second)
                if (!over && matches(chord))
                    over = &o;
        if (!over)
        {
            if (own)
                return *own;
            continue;
        }
        Binding b = own ? *own : Binding{{}, over->keyText};
        b.targets = hotkeys::overlaySlots(b.targets, over->value);
        // A mode that nulls every slot of a key unbinds it while the mode is on.
        if (b.targets.empty())
            return std::nullopt;
        return b;
    }
    return std::nullopt;
}

namespace
{
/** A key bound only with mouse buttons (Tab+RMB): its own events and its unbound buttons still reach the widget, so the
 * viewport keeps Tab as its drag-select modifier. */
bool buttonsOnly(const hotkeys::Slots& t) { return t.press.is_null() && t.hold.is_null() && t.release.is_null(); }
} // namespace

bool Hotkeys::eventFilter(QObject* obj, QEvent* e)
{
    // Key-ups are lost when the app loses focus mid-hold, so release everything then.
    if (e->type() == QEvent::ApplicationDeactivate)
        releaseAll();
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick)
        return onMousePress(obj, static_cast<QMouseEvent*>(e));
    if (e->type() == QEvent::MouseButtonRelease && obj->isWidgetType())
        if (auto* m = static_cast<QMouseEvent*>(e); mSwallowRelease & int(m->button()))
        {
            mSwallowRelease &= ~int(m->button());
            return true;
        }
    if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease)
        return false;
    auto* k = static_cast<QKeyEvent*>(e);

    // The held key's own events are handled wherever they land: an open hotbox menu would otherwise take the key's
    // repeats as presses of its highlighted item.
    if (mPending && (mPending->scan ? mPending->scan == eventScancode(k) : mPending->qtKey == eventKey(k)))
    {
        const bool pass = buttonsOnly(mPending->binding.targets);
        if (e->type() == QEvent::KeyPress || k->isAutoRepeat())
            return !pass;
        Pending p = std::move(*mPending);
        mPending.reset();
        mHoldTimer.stop();
        const hotkeys::Slots& t = p.binding.targets;
        hotkeys::HeldKey::Up up = p.state.keyUp();
        if (MarkingMenu* mm = MarkingMenu::active())
        {
            Qt::MouseButton button = mm->button();
            // The menu's button may still be down; its release then belongs to nothing.
            if (mm->keyReleased() && (QGuiApplication::mouseButtons() & button))
                mSwallowRelease |= int(button);
        }
        if (isHotbox(t.hold))
            if (Hotbox* hb = Hotbox::instance())
            {
                if (up == hotkeys::HeldKey::Up::Tap && hb->interacted())
                    up = hotkeys::HeldKey::Up::AfterHold;
                hb->keyReleased(up == hotkeys::HeldKey::Up::Tap);
            }
        if (up == hotkeys::HeldKey::Up::Tap)
            runFromKey(t.press);
        if (up != hotkeys::HeldKey::Up::Used)
            actions::run(t.release);
        return !pass;
    }

    // The OS key repeat is eaten so the repeat cadence comes from the prefs alone.
    if (mRepeat)
    {
        bool same = mRepeat->scan ? mRepeat->scan == eventScancode(k) : mRepeat->qtKey == eventKey(k);
        if (same)
        {
            if (e->type() == QEvent::KeyRelease && !k->isAutoRepeat())
                stopRepeat();
            return true;
        }
        if (e->type() == QEvent::KeyPress && !isBareModifier(k->key()))
            stopRepeat();
    }

    // An unhandled key event propagates up the parent chain and passes through here again; only act on its first
    // receiver.
    QWidget* first = QWidget::keyboardGrabber() ? QWidget::keyboardGrabber() : QApplication::focusWidget();
    if (!first)
        first = QApplication::activeWindow();
    if (obj != first)
        return false;

    if (e->type() == QEvent::KeyRelease)
    {
        if (k->isAutoRepeat())
            return false;
        for (auto it = mHeldKeys.begin(); it != mHeldKeys.end(); ++it)
            if (it->scan ? it->scan == eventScancode(k) : it->qtKey == eventKey(k))
            {
                json release = it->binding.targets.release;
                mHeldKeys.erase(it);
                actions::run(release);
                return true;
            }
        return false;
    }

    // A modal dialog (file dialog, message box) owns the keyboard; app hotkeys would act on the window behind it.
    if (QApplication::activeModalWidget())
        return false;
    QWidget* focus = QApplication::focusWidget();
    // A widget mid-gesture (a node editor grab) owns every key until the gesture ends.
    if (focus && focus->property("keysCaptured").toBool())
        return false;
    // An open menu takes typing as a search (MenuSearch).
    if (MarkingMenu::active() || qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        return false;
    int mods = int(k->modifiers()) & kModMask;
    if (textInputFocused(focus) && isStandardTextEdit(mods, k->key()))
        return false;
    // Code and text editors keep Ctrl+F for their own find bar.
    if ((qobject_cast<QPlainTextEdit*>(focus) || qobject_cast<QTextEdit*>(focus)) && mods == Qt::ControlModifier && k->key() == Qt::Key_F)
        return false;
    if (listNavigationFocused(focus) && mods == 0 && isArrowKey(k->key()))
        return false;
    if (itemRenameFocused(focus, mods, k->key()))
        return false;
    if (mods == 0 && startsNumber(k->text()) && focus && focus->property("numericEntry").toBool())
        return false;

    std::optional<Binding> found = lookup(k, focus);
    const Binding* b = found ? &*found : nullptr;
    if (!b)
    {
        // Eats unbound letters so the view doesn't jump to the first item starting with one.
        bool typed = (mods & ~Qt::ShiftModifier) == 0 && !k->text().isEmpty() && k->text().front().isPrint() &&
                     k->key() != Qt::Key_Space;
        return typed && focus && focus->property("hotkeyPassThrough").isValid();
    }
    if (k->isAutoRepeat())
        return true;

    const hotkeys::Slots& t = b->targets;
    if (ViewportTool* tool = viewportTools::active();
        tool && tool->hotkey(*k, t.press.is_string() ? std::string_view(t.press.get_ref<const std::string&>()) : std::string_view()))
    {
        viewportTools::requestRedraw();
        return true;
    }
    if (t.hold.is_null() && t.release.is_null() && !t.hasButtons())
    {
        runFromKey(t.press);
        if (hotkeys::repeatsWhileHeld(t.press))
            startRepeat(k, t.press);
        return true;
    }
    if (hotkeys::isMomentary(t))
    {
        // Press/release pairs (hold D for pivot mode) are tracked apart so other keys can come and go meanwhile.
        mHeldKeys.push_back(Pending{*b, eventScancode(k), eventKey(k), hotkeys::HeldKey(t)});
        if (!t.press.is_null())
            runFromKey(t.press);
        return true;
    }
    mPending.emplace(Pending{*b, eventScancode(k), eventKey(k), hotkeys::HeldKey(t)});
    if (!t.hold.is_null())
        mHoldTimer.start(mHoldDelay);
    if (isHotbox(t.hold))
        if (Hotbox* hb = Hotbox::instance())
            hb->showAt(QCursor::pos(), true);
    return !buttonsOnly(t);
}

bool Hotkeys::onMousePress(QObject* obj, QMouseEvent* e)
{
    if (!obj->isWidgetType() || MarkingMenu::active() || QApplication::activePopupWidget())
        return false;
    int index = buttonIndex(e->button());
    if (!mPending)
    {
        for (auto it = mHeldKeys.rbegin(); it != mHeldKeys.rend(); ++it)
            if (index >= 0 && index < 3 && !it->binding.targets.buttons[index].is_null())
                return fireButton(it->binding.targets.buttons[index], e);
        return false;
    }
    switch (mPending->state.mouseDown(index))
    {
    case hotkeys::HeldKey::Mouse::Pass: return false;
    case hotkeys::HeldKey::Mouse::Swallow:
        if (buttonsOnly(mPending->binding.targets))
            return false;
        mSwallowRelease |= int(e->button());
        return true;
    case hotkeys::HeldKey::Mouse::Fire: break;
    }
    mHoldTimer.stop();
    return fireButton(mPending->binding.targets.buttons[index], e);
}

bool Hotkeys::fireButton(const json& target, QMouseEvent* e)
{
    if (target.is_object() && target.contains("markingMenu"))
    {
        std::string name = target["markingMenu"].get<std::string>();
        if (!MarkingMenu::popup(name, MarkingMenu::Trigger::MouseButton, e->button(), e->globalPosition().toPoint()))
            log::warn("no marking menu '{}'", name);
        return true;
    }
    mSwallowRelease |= int(e->button());
    actions::run(target);
    return true;
}

void Hotkeys::startRepeat(QKeyEvent* e, const json& target)
{
    // An interval of 0 turns the repeat off.
    if (PrefStore::get().value<int>("general", "/undo/repeatIntervalMs", 100) <= 0)
        return;
    mRepeat = Repeat{eventScancode(e), eventKey(e), target};
    mRepeatTimer.start(std::max(0, PrefStore::get().value<int>("general", "/undo/repeatDelayMs", 180)));
}

void Hotkeys::stopRepeat()
{
    mRepeat.reset();
    mRepeatTimer.stop();
}

void Hotkeys::releaseAll()
{
    stopRepeat();
    mPending.reset();
    mHoldTimer.stop();
    mSwallowRelease = 0;
    auto held = std::move(mHeldKeys);
    mHeldKeys.clear();
    for (const Pending& p : held)
        actions::run(p.binding.targets.release);
}

void Hotkeys::onHoldTimeout()
{
    mHoldTimer.stop();
    if (!mPending || !mPending->state.holdTimeout() || isHotbox(mPending->binding.targets.hold))
        return;
    actions::run(mPending->binding.targets.hold);
}
} // namespace lila::ui
