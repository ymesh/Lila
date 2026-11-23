// Python first: its headers use `slots` as a member name, which Qt defines as a macro.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "core/Log.h"
#include "python/Python.h"
#include "scene/Graph.h"
#include "ui/CursorBadges.h"
#include "ui/DrawOverrides.h"
#include "ui/Gizmo.h"
#include "ui/NodeLookup.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <memory>
#include <stdexcept>

/**
 * _lila_viewport: the viewport registries for Python, behind lila.viewport. One entry point, call(op, args_json,
 * callables), shaped like _lila_sdk's: arguments and results are JSON text, and each callable takes and returns JSON
 * text. Geometry callbacks return {"lines": [{"a": [x,y,z], "b": [x,y,z], "color": [r,g,b,a]}], "labels": [{"at":
 * [x,y], "text": "..."}]}: lines in world space with linear colors, labels in widget pixels.
 */
namespace lila::ui::pyviewport
{
namespace
{
using json = nlohmann::json;
using PyCall = std::function<json(const json& args)>;

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

/** Runs a callback for the viewport; an exception is logged once per message, not per frame, and yields null. */
json guarded(const PyCall& fn, const json& args, const std::string& what)
{
    try
    {
        return fn(args);
    }
    catch (const std::exception& e)
    {
        static std::string last;
        std::string msg = what + ": " + e.what();
        if (msg != last)
            log::error("{}", msg);
        last = msg;
        return nullptr;
    }
}

std::string str(const json& a, const char* key, const std::string& fallback = {})
{
    auto it = a.find(key);
    return it != a.end() && it->is_string() ? it->get<std::string>() : fallback;
}

json mat(const glm::dmat4& m)
{
    json out = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out.push_back(m[c][r]);
    return out;
}

glm::dmat4 matFrom(const json& j)
{
    glm::dmat4 m(1.0);
    if (j.is_array() && j.size() == 16)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = j[c * 4 + r].get<double>();
    return m;
}

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

json viewJson(const ViewInfo& v)
{
    return {{"view", mat(v.view)}, {"proj", mat(v.proj)}, {"eye", vec(v.eye)}, {"width", v.width}, {"height", v.height},
            {"camera", v.camera}, {"ortho", v.ortho}};
}

ViewInfo viewFrom(const json& j)
{
    ViewInfo v;
    v.view = matFrom(j.value("view", json()));
    v.proj = matFrom(j.value("proj", json()));
    if (auto e = j.value("eye", json()); e.is_array() && e.size() == 3)
        v.eye = {e[0].get<double>(), e[1].get<double>(), e[2].get<double>()};
    v.width = j.value("width", 1.0);
    v.height = j.value("height", 1.0);
    v.camera = j.value("camera", std::string());
    v.ortho = j.value("ortho", false);
    return v;
}

glm::vec4 color(const json& c, const glm::vec4& fallback)
{
    if (!c.is_array() || c.size() < 3)
        return fallback;
    return {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c.size() > 3 ? c[3].get<float>() : 1.0f};
}

glm::vec3 point(const json& p)
{
    return p.is_array() && p.size() == 3 ? glm::vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>()) : glm::vec3(0.0f);
}

/** Adds a geometry callback's result to the frame. */
void apply(const json& out, render::FrameState& s, OverlayPainter& p, const glm::vec4& fallback)
{
    if (!out.is_object())
        return;
    for (const json& l : out.value("lines", json::array()))
    {
        glm::vec4 c = color(l.value("color", json()), fallback);
        s.lines.push_back({point(l.value("a", json())), c});
        s.lines.push_back({point(l.value("b", json())), c});
    }
    for (const json& t : out.value("labels", json::array()))
        if (json at = t.value("at", json()); at.is_array() && at.size() == 2)
            p.label(QPointF(at[0].get<double>(), at[1].get<double>()), QString::fromStdString(str(t, "text")));
}

json hitJson(const selectionHandlers::Hit& h)
{
    auto id = [](uint32_t v) { return v == selectionHandlers::kNone ? json(nullptr) : json(v); };
    return {{"node", h.node}, {"name", h.name}, {"face", id(h.face)}, {"point", id(h.point)}, {"position", vec(h.position)},
            {"px", {h.px.x(), h.px.y()}}, {"click", h.click}};
}

json mods(Qt::KeyboardModifiers m)
{
    json out = json::array();
    if (m & Qt::ShiftModifier)
        out.push_back("shift");
    if (m & Qt::ControlModifier)
        out.push_back("ctrl");
    if (m & Qt::AltModifier)
        out.push_back("alt");
    return out;
}

const char* buttonName(Qt::MouseButton b)
{
    return b == Qt::LeftButton ? "left" : b == Qt::MiddleButton ? "middle" : b == Qt::RightButton ? "right" : "";
}

/** A viewport tool whose events go to one Python callable as {"type": ..., ...}; truthy results consume them. */
class PyTool final : public ViewportTool
{
public:
    PyTool(std::string name, PyCall event, PyCall draw) : mName(std::move(name)), mEvent(std::move(event)), mDraw(std::move(draw)) {}
    const char* name() const override { return mName.c_str(); }

    void activate() override { send({{"type", "activate"}}); }
    void deactivate() override { send({{"type", "deactivate"}}); }
    bool mousePress(QMouseEvent* e, const ViewInfo& v) override { return mouse("press", e, v); }
    bool mouseMove(QMouseEvent* e, const ViewInfo& v) override { return mouse("move", e, v); }
    bool mouseRelease(QMouseEvent* e, const ViewInfo& v) override { return mouse("release", e, v); }
    bool wheel(QWheelEvent* e, const ViewInfo& v) override
    {
        return send({{"type", "wheel"}, {"x", e->position().x()}, {"y", e->position().y()}, {"delta", e->angleDelta().y()},
                     {"modifiers", mods(e->modifiers())}, {"view", viewJson(v)}});
    }
    bool keyPress(QKeyEvent* e, const ViewInfo& v) override
    {
        return send({{"type", "key"}, {"key", e->key()}, {"text", e->text().toStdString()}, {"modifiers", mods(e->modifiers())}, {"view", viewJson(v)}});
    }
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& v) override
    {
        if (mDraw)
            apply(guarded(mDraw, json::array({viewJson(v)}), "viewport tool " + mName), s, p, {0.9f, 0.9f, 0.9f, 1.0f});
    }

private:
    bool mouse(const char* type, QMouseEvent* e, const ViewInfo& v)
    {
        return send({{"type", type}, {"x", e->position().x()}, {"y", e->position().y()}, {"button", buttonName(e->button())},
                     {"modifiers", mods(e->modifiers())}, {"view", viewJson(v)}});
    }
    bool send(const json& event)
    {
        json r = mEvent ? guarded(mEvent, json::array({event}), "viewport tool " + mName) : json();
        return r.is_boolean() && r.get<bool>();
    }

    std::string mName;
    PyCall mEvent, mDraw;
};

size_t removeOwned(const std::string& owner)
{
    return gizmos::registry().removeOwned(owner) + selectionHandlers::registry().removeOwned(owner) +
           drawOverrides::registry().removeOwned(owner) + renderPasses::registry().removeOwned(owner) +
           viewportTools::registry().removeOwned(owner) + viewportHooks::removeOwned(owner) + cursorBadges::removeOwned(owner) +
           overlayIcons::removeOwned(owner);
}

json dispatch(const std::string& op, const json& a, const std::vector<PyCall>& fns)
{
    const std::string owner = str(a, "owner", "python");
    const std::string id = str(a, "id");
    auto fn = [&](size_t i) { return i < fns.size() ? fns[i] : PyCall(); };

    if (op == "gizmo.list")
    {
        json out = json::array();
        for (const gizmos::Provider* p : gizmos::registry().all())
            out.push_back({{"id", p->id}, {"label", p->label.toStdString()}, {"owner", *gizmos::registry().ownerOf(p->id)}});
        return out;
    }
    if (op == "gizmo.setActive")
        return gizmos::setActive(id);
    if (op == "gizmo.active")
        return gizmos::activeId();

    if (op == "icon.add")
    {
        overlayIcons::add(id, {.tabler = QString::fromStdString(str(a, "tabler")), .file = QString::fromStdString(str(a, "file"))}, owner);
        return true;
    }
    if (op == "icon.remove")
        return overlayIcons::remove(id);
    if (op == "icon.list")
        return overlayIcons::ids();

    if (op == "badge.add")
    {
        PyCall active = fn(0);
        cursorBadges::add({.id = id,
                           .icon = str(a, "icon"),
                           .active = [active, id](const ViewportState& v) {
                               if (!active)
                                   return false;
                               json st = {{"panel", v.panel}, {"cursor", {v.cursor.x(), v.cursor.y()}}, {"tool", v.tool},
                                          {"navigating", v.navigating}, {"gizmo", v.gizmo != nullptr},
                                          {"dragging", v.gizmo && v.gizmo->dragging()}};
                               json r = guarded(active, json::array({st}), "cursor badge " + id);
                               return r.is_boolean() && r.get<bool>();
                           },
                           .message = QString::fromStdString(str(a, "message")),
                           .actions = a.value("actions", std::vector<std::string>()),
                           .key = QString::fromStdString(str(a, "key"))},
                          owner);
        return true;
    }
    if (op == "badge.remove")
        return cursorBadges::remove(id);
    if (op == "badge.list")
        return cursorBadges::ids();

    if (op == "drawer.add")
    {
        PyCall draw = fn(0);
        viewportHooks::addDrawer(id,
                                 [draw, id](render::FrameState& s, OverlayPainter& p, const ViewInfo& v) {
                                     apply(guarded(draw, json::array({viewJson(v)}), "drawer " + id), s, p, {0.9f, 0.9f, 0.9f, 1.0f});
                                 },
                                 owner);
        return true;
    }
    if (op == "drawer.remove")
        return viewportHooks::removeDrawer(id);
    if (op == "picker.add")
    {
        PyCall pick = fn(0);
        viewportHooks::addPicker(id,
                                 [pick, id](QPointF pa, QPointF pb, bool click, const ViewInfo& v) {
                                     std::vector<viewportHooks::PickHit> hits;
                                     json r = guarded(pick, json::array({{{"a", {pa.x(), pa.y()}}, {"b", {pb.x(), pb.y()}}, {"click", click}, {"view", viewJson(v)}}}),
                                                      "picker " + id);
                                     if (r.is_array())
                                         for (const json& h : r)
                                             if (h.is_array() && h.size() == 2 && h[0].is_string() && h[1].is_number())
                                                 hits.push_back({h[0].get<std::string>(), h[1].get<double>()});
                                     return hits;
                                 },
                                 owner);
        return true;
    }
    if (op == "picker.remove")
        return viewportHooks::removePicker(id);
    if (op == "hooks.list")
        return {{"drawers", viewportHooks::drawerIds()}, {"pickers", viewportHooks::pickerIds()}};

    if (op == "drawOverride.add")
    {
        PyCall draw = fn(0), pick = fn(1);
        drawOverrides::DrawOverride o{.id = id, .nodeType = str(a, "nodeType")};
        auto nodeJson = [](const scene::Node& n, const glm::dmat4& world) { return json{{"node", n.path()}, {"world", mat(world)}}; };
        if (draw)
            o.draw = [draw, id, nodeJson](drawOverrides::DrawContext& c) {
                json args = nodeJson(c.node, c.world);
                args["highlight"] = int(c.highlight);
                args["color"] = {c.color.r, c.color.g, c.color.b, c.color.a};
                args["view"] = viewJson(c.view);
                apply(guarded(draw, json::array({args}), "draw override " + id), c.frame, c.painter, c.color);
            };
        if (pick)
            o.pick = [pick, id, nodeJson](const drawOverrides::PickContext& c) -> std::optional<double> {
                json args = nodeJson(c.node, c.world);
                args["a"] = {c.a.x(), c.a.y()};
                args["b"] = {c.b.x(), c.b.y()};
                args["click"] = c.click;
                args["view"] = viewJson(c.view);
                json r = guarded(pick, json::array({args}), "draw override " + id);
                return r.is_number() ? std::optional<double>(r.get<double>()) : std::nullopt;
            };
        if (drawOverrides::registry().contains(id))
            throw std::invalid_argument("draw override '" + id + "' is already registered");
        drawOverrides::registry().add(id, std::move(o), owner);
        return true;
    }
    if (op == "drawOverride.remove")
        return drawOverrides::remove(id);
    if (op == "drawOverride.list")
        return drawOverrides::registry().ids();

    if (op == "selectionHandler.add")
    {
        PyCall redirect = fn(0), hover = fn(1);
        selectionHandlers::Handler h{.id = id, .label = QString::fromStdString(str(a, "label", id))};
        if (redirect)
            h.redirect = [redirect, id](const selectionHandlers::Hit& hit) -> std::optional<std::vector<std::string>> {
                json r = guarded(redirect, json::array({hitJson(hit)}), "selection handler " + id);
                if (!r.is_array())
                    return std::nullopt;
                std::vector<std::string> out;
                for (const json& n : r)
                    if (n.is_string())
                        out.push_back(n.get<std::string>());
                return out;
            };
        if (hover)
            h.hover = [hover, id](const selectionHandlers::Hit& hit) -> std::optional<selectionHandlers::Highlight> {
                json r = guarded(hover, json::array({hitJson(hit)}), "selection handler " + id);
                if (!r.is_object())
                    return std::nullopt;
                selectionHandlers::Highlight hl;
                std::string mesh = str(r, "mesh");
                if (scene::Node* n = findNode(mesh))
                    hl.mesh = n->id();
                hl.domain = str(r, "domain", "face") == "point" ? selectionHandlers::Highlight::Domain::Point : selectionHandlers::Highlight::Domain::Face;
                hl.weights = r.value("weights", std::vector<float>());
                hl.color = color(r.value("color", json()), hl.color);
                return hl;
            };
        if (selectionHandlers::registry().contains(id))
            throw std::invalid_argument("selection handler '" + id + "' is already registered");
        selectionHandlers::registry().add(id, std::move(h), owner);
        return true;
    }
    if (op == "selectionHandler.remove")
        return selectionHandlers::remove(id);
    if (op == "selectionHandler.setActive")
        return selectionHandlers::setActive(id);
    if (op == "selectionHandler.active")
        return selectionHandlers::activeId();
    if (op == "selectionHandler.list")
        return selectionHandlers::registry().ids();

    if (op == "renderPass.add")
    {
        PyCall run = fn(0);
        std::string stage = str(a, "stage", "overlay");
        renderPasses::Pass pass{.id = id,
                                .stage = stage == "scene" ? renderPasses::Stage::Scene : stage == "final" ? renderPasses::Stage::Final : renderPasses::Stage::Overlay,
                                .order = a.value("order", 0),
                                .run = [run, id](renderPasses::Context& c) {
                                    json args = {{"view", viewJson(c.view)}, {"panel", c.panel}};
                                    apply(guarded(run, json::array({args}), "render pass " + id), c.frame, c.painter, {0.9f, 0.9f, 0.9f, 1.0f});
                                }};
        if (renderPasses::registry().contains(id))
            throw std::invalid_argument("render pass '" + id + "' is already registered");
        renderPasses::registry().add(id, std::move(pass), owner);
        return true;
    }
    if (op == "renderPass.remove")
        return renderPasses::remove(id);
    if (op == "renderPass.list")
        return renderPasses::registry().ids();

    if (op == "tool.add")
    {
        PyCall event = fn(0), draw = fn(1);
        std::string name = str(a, "name", id);
        if (viewportTools::registry().contains(id))
            throw std::invalid_argument("viewport tool '" + id + "' is already registered");
        viewportTools::registry().add(id,
                                      {.id = id,
                                       .label = QString::fromStdString(str(a, "label", id)),
                                       .icon = str(a, "icon"),
                                       .create = [name, event, draw] { return std::make_unique<PyTool>(name, event, draw); }},
                                      owner);
        return true;
    }
    if (op == "tool.remove")
        return viewportTools::remove(id);
    if (op == "tool.activate")
        return viewportTools::activate(id);
    if (op == "tool.deactivate")
    {
        viewportTools::set(nullptr);
        return true;
    }
    if (op == "tool.active")
        return viewportTools::activeId();
    if (op == "tool.list")
        return viewportTools::registry().ids();

    if (op == "project")
    {
        json p = a.value("point", json());
        if (!p.is_array() || p.size() != 3)
            throw std::invalid_argument("project: point must be [x, y, z]");
        std::optional<QPointF> px = viewFrom(a.value("view", json::object())).project({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
        return px ? json::array({px->x(), px->y()}) : json(nullptr);
    }
    if (op == "owner.removeAll")
        return removeOwned(owner);
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
    // Registries run viewport and Qt code, which may call back into Python; this thread needn't hold the GIL meanwhile.
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
    {"call", call, METH_VARARGS, "call(op, args_json, callables=()) -> result_json. Use lila.viewport instead."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT, "_lila_viewport", "Lila's viewport registries for Python.", -1, kMethods};

PyObject* initModule()
{
    return PyModule_Create(&kModule);
}

/** Registered while the library loads, which both hosts do before starting Python. */
const bool kRegistered = [] {
    if (!Py_IsInitialized())
        PyImport_AppendInittab("_lila_viewport", &initModule);
    return true;
}();
} // namespace
} // namespace lila::ui::pyviewport
