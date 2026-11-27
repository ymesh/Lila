#include "ui/editor/EditorPalette.h"

#include "core/PrefStore.h"
#include "ui/ThemeModel.h"

#include <QApplication>
#include <QPointer>
#include <QTimer>

#include <vector>

namespace lila::ui::palette
{
namespace
{
constexpr const char* kPrefs = "scriptEditor";

struct Listener
{
    QPointer<QObject> owner;
    std::function<void()> fn;
};

struct State
{
    Scheme scheme;
    Look look;
    bool ready = false;
    bool listening = false;
    std::vector<Listener> listeners;
};

State& state()
{
    static State s;
    return s;
}

QColor hex(const char* c)
{
    return QColor(QString::fromLatin1(c));
}

Scheme makeScheme(const Look& look)
{
    Scheme s;
    s.background = gray(30);
    s.text = gray(198);
    s.currentLine = gray(38);
    s.gutterBg = gray(33);
    s.gutterText = gray(92);
    s.gutterCurrent = gray(190);
    s.cellRule = gray(74);
    s.unnecessaryText = gray(112);
    s.outputBg = gray(26);
    s.outputText = gray(208);
    s.outputInfo = gray(135);
    if (look.monotone)
    {
        s.selection = gray(82);
        s.selectedText = gray(250);
        s.bracketMatch = gray(72);
        s.searchMatch = gray(64);
        s.searchCurrent = gray(100);
        s.occurrence = gray(50);
        s.squiggleError = gray(235);
        s.squiggleWarning = gray(150);
        s.squiggleInfo = gray(105);
        s.outputError = gray(250);
        s.outputWarning = gray(250);
        s.outputEcho = gray(160);
        s.outputLink = gray(240);
        return s;
    }
    s.selectedText = s.text;
    s.outputEcho = gray(150);
    if (look.light)
    {
        s.selection = hex("#add6ff");
        s.bracketMatch = hex("#c8d7e8");
        s.searchMatch = hex("#f8c9a9");
        s.searchCurrent = hex("#e2b85c");
        s.occurrence = hex("#d7e3f3");
        s.squiggleError = hex("#e51400");
        s.squiggleWarning = hex("#bf8803");
        s.squiggleInfo = hex("#1a85ff");
        s.outputError = hex("#cd3131");
        s.outputWarning = hex("#a86d00");
        s.outputLink = hex("#006ab1");
    }
    else
    {
        s.selection = hex("#264f78");
        s.bracketMatch = hex("#3e4a5a");
        s.searchMatch = hex("#623315");
        s.searchCurrent = hex("#9e6a03");
        s.occurrence = hex("#3a404a");
        s.squiggleError = hex("#f14c4c");
        s.squiggleWarning = hex("#cca700");
        s.squiggleInfo = hex("#3794ff");
        s.outputError = hex("#f14c4c");
        s.outputWarning = hex("#cca700");
        s.outputLink = hex("#3794ff");
    }
    return s;
}

void refresh(bool notify)
{
    State& st = state();
    Look look = currentLook();
    Scheme next = makeScheme(look);
    // The theme's neutrals can change while the look stays, so compare the colours too.
    bool changed = !st.ready || look != st.look || next.background != st.scheme.background || next.text != st.scheme.text ||
                   next.gutterBg != st.scheme.gutterBg || next.outputBg != st.scheme.outputBg;
    st.look = look;
    st.scheme = next;
    st.ready = true;
    if (!notify || !changed)
        return;
    std::erase_if(st.listeners, [](const Listener& l) { return l.owner.isNull(); });
    std::vector<Listener> listeners = st.listeners;
    for (const Listener& l : listeners)
        if (l.owner)
            l.fn();
}

void listen()
{
    State& st = state();
    if (st.listening || !qApp)
        return;
    st.listening = true;
    PrefStore::get().onChanged([](const std::string& file) {
        if (file != "general" && file != kPrefs)
            return;
        // Deferred: the theme cache drops on this same change, and every listener must have run first.
        QTimer::singleShot(0, qApp, [] { refresh(true); });
    });
}
} // namespace

Look resolveLook(bool editorMonotone, bool followAppMonotone, bool appMonotone, bool lightTheme)
{
    return {editorMonotone || (followAppMonotone && appMonotone), lightTheme};
}

bool appMonotone()
{
    return themes::currentName() == themes::kDefaultTheme || PrefStore::get().value<bool>("general", "/ui/monotone", false);
}

bool monotoneForcedByApp()
{
    return PrefStore::get().value<bool>(kPrefs, kFollowAppMonotonePref, false) && appMonotone();
}

Look currentLook()
{
    PrefStore& prefs = PrefStore::get();
    return resolveLook(prefs.value<bool>(kPrefs, kMonotonePref, false), prefs.value<bool>(kPrefs, kFollowAppMonotonePref, false),
                       appMonotone(), themes::current()->base == "light");
}

void onLookChanged(QObject* owner, std::function<void()> fn)
{
    listen();
    scheme();
    state().listeners.push_back({owner, std::move(fn)});
}

QColor gray(int v, int alpha)
{
    themes::Rgba c = themes::shade(v, uint8_t(alpha));
    return QColor(c.r, c.g, c.b, c.a);
}

const Scheme& scheme()
{
    if (!state().ready)
        refresh(false);
    return state().scheme;
}

namespace detail
{
const Scheme& storage()
{
    return state().scheme;
}
} // namespace detail
} // namespace lila::ui::palette
