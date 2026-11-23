// Python first: its headers use `slots` as a member name, which Qt defines as a macro.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "core/Events.h"
#include "core/Log.h"
#include "python/Python.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/ExtensionUi.h"
#include "ui/FileDialog.h"
#include "ui/Hotkeys.h"
#include "ui/IconBars.h"
#include "ui/Layouts.h"
#include "ui/MarkingMenu.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/Panels.h"
#include "ui/PreferencesDialog.h"
#include "ui/Shelf.h"

#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <stdexcept>

/**
 * _lila_sdk: the UI registries and the event bus for Python extensions, behind lila.ui and lila.events. One entry
 * point, call(op, args_json, callables): arguments and results travel as JSON text, and each callable is wrapped so it
 * takes and returns JSON text as well (lila/ui.py does the wrapping on its side).
 */
namespace lila::ui::pysdk
{
namespace
{
using json = nlohmann::json;
using PyCall = std::function<json(const json& args)>;

/** Calls a Python callable under the GIL with the args as JSON text; its JSON text result comes back parsed. */
PyCall wrap(PyObject* fn)
{
    Py_INCREF(fn);
    std::shared_ptr<PyObject> held(fn, [](PyObject* o) {
        if (!Py_IsInitialized())
            return;
        python::Gil gil;
        Py_DECREF(o);
    });
    return [held](const json& args) -> json {
        python::Gil gil;
        std::string text = args.dump();
        PyObject* result = PyObject_CallFunction(held.get(), "s#", text.data(), Py_ssize_t(text.size()));
        if (!result)
        {
            throw std::runtime_error(python::takeError());
        }
        json out;
        if (PyUnicode_Check(result))
        {
            Py_ssize_t size = 0;
            const char* s = PyUnicode_AsUTF8AndSize(result, &size);
            out = s ? json::parse(std::string_view(s, size_t(size)), nullptr, false) : json();
            if (out.is_discarded())
                out = nullptr;
        }
        Py_DECREF(result);
        return out;
    };
}

/** A Python callable that is allowed to be absent. */
std::function<void()> optionalCall(const std::vector<PyCall>& fns, size_t i)
{
    if (i >= fns.size() || !fns[i])
        return {};
    PyCall fn = fns[i];
    return [fn] { fn(json::array()); };
}

std::string str(const json& a, const char* key, const std::string& fallback = {})
{
    auto it = a.find(key);
    return it != a.end() && it->is_string() ? it->get<std::string>() : fallback;
}

/** A menu path whose first part names a menu by its title instead of its id, as its id; warns when it names no menu yet. */
std::string menuPath(const std::string& path)
{
    const size_t slash = path.find('/');
    const std::string head = path.substr(0, slash);
    if (menus::menuRegistry().contains(head))
        return path;
    for (const menus::Menu* m : menus::menuRegistry().allInOrder())
    {
        QString title = QString::fromStdString(m->title);
        title.remove('&');
        if (title.toStdString() == head)
            return slash == std::string::npos ? m->id : m->id + path.substr(slash);
    }
    log::warn("ui.add_menu_item: no menu '{}' yet, so its items stay hidden until one is added; paths start with the id add_menu was given", head);
    return path;
}

Qt::DockWidgetArea area(const std::string& name)
{
    if (name == "left")
        return Qt::LeftDockWidgetArea;
    if (name == "bottom")
        return Qt::BottomDockWidgetArea;
    if (name == "top")
        return Qt::TopDockWidgetArea;
    return Qt::RightDockWidgetArea;
}

/** A container whose pointer the Python side wraps with shiboken6 and fills, so the widget belongs to C++ from the start. */
QWidget* container(const PyCall& fill, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    fill(json::array({uint64_t(reinterpret_cast<uintptr_t>(w))}));
    return w;
}

void prefRows(prefPages::PageBuilder& b, const json& rows, const PyCall& custom)
{
    for (const json& r : rows)
    {
        if (!r.is_object())
            continue;
        if (r.contains("section"))
        {
            b.section(str(r, "section"));
            continue;
        }
        std::string type = str(r, "type"), label = str(r, "label"), key = str(r, "key");
        json def = r.value("default", json());
        if (type == "toggle")
            b.toggle(label, key, def.is_boolean() && def.get<bool>());
        else if (type == "number")
            b.number(label, key, def.is_number() ? def.get<double>() : 0.0, r.value("min", 0.0), r.value("max", 100.0), r.value("step", 1.0),
                     r.value("decimals", 2));
        else if (type == "choice")
        {
            std::vector<std::pair<std::string, std::string>> options;
            for (const json& o : r.value("options", json::array()))
                if (o.is_array() && o.size() == 2)
                    options.emplace_back(o[0].get<std::string>(), o[1].get<std::string>());
                else if (o.is_string())
                    options.emplace_back(o.get<std::string>(), o.get<std::string>());
            b.choice(label, key, options, def.is_string() ? def.get<std::string>() : std::string());
        }
        else if (type == "text")
            b.text(label, key, def.is_string() ? def.get<std::string>() : std::string(), r.value("file", false));
        else if (type == "color")
            b.color(label, key, def.is_string() ? def.get<std::string>() : std::string("#808080"));
    }
    if (custom)
        b.row("", container(custom, nullptr), {});
}

json dispatch(const std::string& op, const json& a, const std::vector<PyCall>& fns)
{
    const std::string owner = str(a, "owner", "python");
    auto fn = [&](size_t i) { return i < fns.size() ? fns[i] : PyCall(); };

    if (op == "action.add")
    {
        PyCall run = fn(0);
        Action action{.id = str(a, "id"), .label = str(a, "label", str(a, "id")), .category = str(a, "category", "Python"),
                      .run = [run] {
                          if (run)
                              run(json::array());
                      },
                      .doc = str(a, "doc"), .python = str(a, "python"), .icon = str(a, "icon")};
        if (PyCall checked = fn(1))
            action.checked = [checked] {
                try
                {
                    const json on = checked(json::array());
                    return on.is_boolean() && on.get<bool>();
                }
                catch (const std::exception&)
                {
                    return false;
                }
            };
        actions::add(std::move(action), owner);
        return true;
    }
    if (op == "action.remove")
        return actions::remove(str(a, "id"));
    if (op == "file.reveal")
    {
        fileOps::reveal(QString::fromStdString(str(a, "path")));
        return true;
    }
    if (op == "action.list")
    {
        json out = json::array();
        for (const Action* x : actions::all())
            out.push_back({{"id", x->id}, {"label", x->label}, {"category", x->category}, {"doc", x->doc},
                           {"owner", *actions::registry().ownerOf(x->id)}});
        return out;
    }
    if (op == "menu.add")
    {
        menus::addMenu({.id = str(a, "id"), .title = str(a, "title"), .area = str(a, "area") == "hotbox" ? menus::Area::Hotbox : menus::Area::MenuBar,
                        .before = str(a, "before"), .docs = str(a, "docs"),
                        .sets = a.contains("sets") && a["sets"].is_array() ? a["sets"].get<std::vector<std::string>>() : std::vector<std::string>{}},
                       owner);
        return true;
    }
    if (op == "menu.addItem")
    {
        std::string kind = str(a, "kind", "action");
        menus::Kind k = kind == "separator" ? menus::Kind::Separator
                       : kind == "section" ? menus::Kind::Section
                       : kind == "submenu" ? menus::Kind::Submenu
                                           : menus::Kind::Action;
        return menus::addItem({.id = str(a, "id"), .menu = menuPath(str(a, "menu")), .kind = k, .action = str(a, "action"), .label = str(a, "label"),
                               .after = str(a, "after"), .before = str(a, "before")},
                              owner);
    }
    if (op == "iconBar.addItem")
    {
        std::string kind = str(a, "kind", "action");
        std::vector<std::string> rows;
        for (const json& r : a.value("actions", json::array()))
            if (r.is_string())
                rows.push_back(r.get<std::string>());
        return iconBars::add({.id = str(a, "id"),
                              .bar = str(a, "bar", iconBars::kToolBox),
                              .kind = kind == "separator" ? iconBars::Kind::Separator : kind == "menu" ? iconBars::Kind::Menu : iconBars::Kind::Action,
                              .action = str(a, "action"),
                              .icon = str(a, "icon"),
                              .label = str(a, "label"),
                              .menuActions = std::move(rows),
                              .after = str(a, "after"),
                              .before = str(a, "before")},
                             owner);
    }
    if (op == "iconBar.removeItem")
        return iconBars::remove(str(a, "id"));
    if (op == "iconBar.list")
    {
        json out = json::array();
        for (const iconBars::Item* i : iconBars::itemsIn(str(a, "bar", iconBars::kToolBox)))
            out.push_back({{"id", i->id}, {"action", i->action}, {"icon", i->icon}, {"label", i->label}, {"owner", *iconBars::registry().ownerOf(i->id)}});
        return out;
    }
    if (op == "menu.remove")
        return menus::removeMenu(str(a, "id"));
    if (op == "menu.removeItem")
        return menus::removeItem(str(a, "id"));
    if (op == "panel.add")
    {
        PyCall fill = fn(0);
        if (!fill)
            throw std::invalid_argument("a panel needs a function that builds its widget");
        std::string id = str(a, "id");
        panels::add({.id = id, .title = str(a, "title", id), .create = [fill](QWidget* parent) { return container(fill, parent); },
                     .area = area(str(a, "area", "right")), .icon = str(a, "icon"), .open = a.value("open", true),
                     .toggleAction = str(a, "action", "window." + id), .docs = str(a, "docs")},
                    owner);
        return true;
    }
    if (op == "panel.remove")
    {
        std::string id = str(a, "id");
        if (const panels::Panel* p = panels::registry().find(id); p && !p->toggleAction.empty())
            if (const std::string* o = actions::registry().ownerOf(p->toggleAction); o && *o == owner)
                actions::remove(p->toggleAction);
        return panels::remove(id);
    }
    if (op == "panel.show")
    {
        panels::show(str(a, "id"));
        return true;
    }
    if (op == "mode.add")
    {
        modes::add({.id = str(a, "id"), .label = str(a, "label"), .doc = str(a, "doc"), .icon = str(a, "icon"), .layout = str(a, "layout"),
                    .hotkeys = a.value("hotkeys", json::object()), .markingMenuSet = str(a, "markingMenuSet"), .shelf = str(a, "shelf"),
                    .gizmoProvider = str(a, "gizmoProvider"), .selectionHandler = str(a, "selectionHandler"),
                    .onEnter = optionalCall(fns, 0), .onExit = optionalCall(fns, 1)},
                   owner);
        return true;
    }
    if (op == "mode.remove")
        return modes::remove(str(a, "id"));
    if (op == "mode.activate")
        return modes::activate(str(a, "id"));
    if (op == "mode.current")
        return modes::current();
    if (op == "mode.list")
    {
        json out = json::array();
        for (const modes::Mode* m : modes::registry().allInOrder())
            out.push_back({{"id", m->id}, {"label", m->label}, {"doc", m->doc}, {"layout", m->layout}, {"shelf", m->shelf},
                           {"markingMenuSet", m->markingMenuSet}, {"gizmoProvider", m->gizmoProvider},
                           {"selectionHandler", m->selectionHandler}});
        return out;
    }
    if (op == "layout.add")
    {
        layouts::add(str(a, "name"), a.value("layout", json::object()), owner);
        return true;
    }
    if (op == "layout.remove")
        return layouts::contributed().remove(str(a, "name"));
    if (op == "markingMenu.add")
    {
        markingMenus::add(str(a, "name"), a.value("definition", json::object()), owner);
        return true;
    }
    if (op == "markingMenu.remove")
        return markingMenus::remove(str(a, "name"));
    if (op == "markingMenu.addSet")
    {
        markingMenus::addSet(str(a, "set"), a.value("menus", json::object()), owner);
        return true;
    }
    if (op == "markingMenu.removeSet")
        return markingMenus::removeSet(str(a, "set"));
    if (op == "markingMenu.addItem")
        return markingMenus::addItem({str(a, "menu"), str(a, "slot", "overflow"), a.value("item", json::object())}, owner);
    if (op == "markingMenu.removeItem")
        return markingMenus::removeItem(str(a, "id"));
    if (op == "hotkeyLayer.add")
    {
        hotkeyLayers::add({str(a, "id"), a.value("contexts", json::object()), str(a, "mode")}, owner);
        return true;
    }
    if (op == "hotkeyLayer.remove")
        return hotkeyLayers::remove(str(a, "id"));
    if (op == "shelf.add")
    {
        shelves::add({str(a, "name"), a.value("items", json::array())}, owner);
        return true;
    }
    if (op == "shelf.remove")
        return shelves::registry().remove(str(a, "name"));
    if (op == "prefPage.add")
    {
        json rows = a.value("rows", json::array());
        PyCall custom = fn(0);
        prefPages::add({.id = str(a, "id"), .title = str(a, "title", str(a, "id")), .file = str(a, "file", "general"),
                        .build = [rows, custom](prefPages::PageBuilder& b) { prefRows(b, rows, custom); }},
                       owner);
        return true;
    }
    if (op == "prefPage.remove")
        return prefPages::remove(str(a, "id"));
    if (op == "event.subscribe")
    {
        std::optional<events::Event> e = events::fromName(str(a, "event"));
        if (!e)
            throw std::invalid_argument("no event named '" + str(a, "event") + "'");
        PyCall handler = fn(0);
        return events::subscribe(
            *e,
            [handler](const events::EventData& d) {
                auto utf8 = [](const std::filesystem::path& p) {
                    std::u8string s = p.generic_u8string();
                    return std::string(s.begin(), s.end());
                };
                json paths = json::array();
                for (const std::filesystem::path& p : d.paths)
                    paths.push_back(utf8(p));
                handler(json::array({json{{"event", events::name(d.type)},
                                          {"path", utf8(d.path)},
                                          {"paths", paths},
                                          {"name", d.name},
                                          {"node", d.node},
                                          {"time", d.time}}}));
            },
            owner);
    }
    if (op == "event.unsubscribe")
        return events::unsubscribe(a.value("id", uint64_t(0)));
    if (op == "event.names")
    {
        json out = json::array();
        for (int i = 0; i < int(events::Event::Count); ++i)
            out.push_back(events::name(events::Event(i)));
        return out;
    }
    if (op == "owner.removeAll")
        return extensionUi::removeOwned(owner) + events::unsubscribeOwned(owner);
    throw std::invalid_argument("unknown operation '" + op + "'");
}

PyObject* call(PyObject*, PyObject* args)
{
    const char* op = nullptr;
    const char* text = nullptr;
    PyObject* callables = nullptr;
    if (!PyArg_ParseTuple(args, "ss|O", &op, &text, &callables))
        return nullptr;
    std::vector<PyCall> fns;
    if (callables && callables != Py_None)
    {
        PyObject* seq = PySequence_Fast(callables, "callables must be a sequence");
        if (!seq)
            return nullptr;
        for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); ++i)
        {
            PyObject* f = PySequence_Fast_GET_ITEM(seq, i);
            fns.push_back(f == Py_None ? PyCall() : wrap(f));
        }
        Py_DECREF(seq);
    }
    json argsJson = json::parse(text, nullptr, false);
    if (argsJson.is_discarded() || !argsJson.is_object())
    {
        PyErr_SetString(PyExc_ValueError, "arguments must be a JSON object");
        return nullptr;
    }
    std::string out;
    // The registries run Qt code and other Python callbacks; neither needs this thread to hold the GIL meanwhile.
    PyThreadState* state = PyEval_SaveThread();
    std::string error;
    try
    {
        out = dispatch(op, argsJson, fns).dump();
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    PyEval_RestoreThread(state);
    if (!error.empty())
    {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    return PyUnicode_FromStringAndSize(out.data(), Py_ssize_t(out.size()));
}

PyMethodDef kMethods[] = {
    {"call", call, METH_VARARGS, "call(op, args_json, callables=()) -> result_json. Use lila.ui and lila.events instead."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT, "_lila_sdk", "Lila's UI registries and event bus for Python.", -1, kMethods};

PyObject* initModule()
{
    return PyModule_Create(&kModule);
}

/** Registered while the library loads, which both hosts do before starting Python. */
const bool kRegistered = [] {
    if (!Py_IsInitialized())
        PyImport_AppendInittab("_lila_sdk", &initModule);
    return true;
}();
} // namespace
} // namespace lila::ui::pysdk
