#include "render/PaintGpu.h"

#include "ops/Command.h"
#include "scene/Node.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <limits>

/**
 * Stroke editing: a paint layer's strokes are records, so each can be changed after the fact (colour, channel values,
 * size, strength, brush), deleted or reordered, and the layer re-rasterised from the records. Picking finds the topmost
 * stroke with a dab over a surface point.
 */
namespace lila::render::paint
{
// Named, not anonymous: unity builds put this beside the other paint command files' helpers.
namespace strokeedit
{
using namespace tex::paint;
using scene::Node;
using json = nlohmann::json;
using T = ArgType;

scene::Graph& graph() { return scene::Graph::current(); }

struct Target
{
    Node* set = nullptr;
    std::string layer;
    PaintPtr data;
    const Buffer* buffer = nullptr;
};

Target target(const Args& a, const char* cmd)
{
    if (a.positional.size() < 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
        throw CommandError(std::string(cmd) + ": texture set and paint layer required");
    Target t;
    t.set = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!t.set)
        throw CommandError(std::string(cmd) + ": no texture set named '" + a.positional[0].get<std::string>() + "'");
    t.layer = a.positional[1].get<std::string>();
    t.data = readPaint(*t.set);
    t.buffer = t.data->buffer(t.layer);
    if (!t.buffer)
        throw CommandError(std::string(cmd) + ": no paint layer '" + t.layer + "' in " + t.set->name());
    return t;
}

size_t indexOf(const Target& t, uint64_t id, const char* cmd)
{
    auto it = std::find_if(t.buffer->strokes.begin(), t.buffer->strokes.end(), [&](const StrokePtr& s) { return s->id == id; });
    if (it == t.buffer->strokes.end())
        throw CommandError(std::string(cmd) + ": " + t.layer + " has no stroke " + std::to_string(id));
    return size_t(it - t.buffer->strokes.begin());
}

uint64_t topId(const Buffer& b)
{
    uint64_t top = 0;
    for (const StrokePtr& s : b.strokes)
        top = std::max(top, s->id);
    return top;
}

/** Stores the edited stroke list and re-rasterises the layer from it. */
void commit(Target& t, Buffer b, const char* cmd)
{
    auto nd = std::make_shared<PaintData>(*t.data);
    nd->buffers[t.layer] = std::move(b);
    writePaint(graph(), *t.set, nd);
    try
    {
        rasterize(graph(), *t.set, t.layer);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

json strokeJson(const Stroke& s, size_t index)
{
    json channels = json::object();
    for (const auto& [name, v] : s.payload.channels)
        channels[name] = {v.x, v.y, v.z};
    float radius = 0.0f;
    for (const Dab& d : s.dabs)
        radius = std::max(radius, d.radius);
    return {{"id", s.id},
            {"index", index},
            {"kind", s.payload.kind},
            {"mesh", s.mesh},
            {"channels", channels},
            {"radius", radius},
            {"strength", s.brush.value("strength", 1.0)},
            {"dabs", s.dabs.size()},
            {"path", !s.path.is_null()}};
}

json listStrokes(const Args& a)
{
    const Target t = target(a, "texStrokes");
    json out = json::array();
    for (size_t i = 0; i < t.buffer->strokes.size(); ++i)
        out.push_back(strokeJson(*t.buffer->strokes[i], i));
    return out;
}

glm::vec3 color(const json& j, const char* cmd)
{
    if (j.is_number())
        return glm::vec3(j.get<float>());
    if (!j.is_array() || j.size() < 3)
        throw CommandError(std::string(cmd) + ": a value is a number or [r, g, b]");
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

json editStroke(const Args& a)
{
    const char* cmd = "texEditStroke";
    Target t = target(a, cmd);
    const size_t i = indexOf(t, uint64_t(a.get<int64_t>("stroke", 0)), cmd);
    Stroke next = *t.buffer->strokes[i];
    if (a.has("color"))
        next.payload.channels[next.payload.colorChannel.empty() ? std::string("baseColor") : next.payload.colorChannel] = color(a.flags["color"], cmd);
    if (a.has("channels"))
    {
        if (!a.flags["channels"].is_object())
            throw CommandError(std::string(cmd) + ": channels is {name: value}");
        for (const auto& [name, v] : a.flags["channels"].items())
            if (v.is_null())
                next.payload.channels.erase(name);
            else
                next.payload.channels[name] = color(v, cmd);
    }
    if (a.has("radius") || a.has("sizeScale"))
    {
        float widest = 0.0f;
        for (const Dab& d : next.dabs)
            widest = std::max(widest, d.radius);
        const float scale = a.has("radius") ? (widest > 0.0f ? float(a.get<double>("radius")) / widest : 1.0f) : float(a.get<double>("sizeScale"));
        if (scale <= 0.0f)
            throw CommandError(std::string(cmd) + ": the size must stay above zero");
        for (Dab& d : next.dabs)
            d.radius *= scale;
    }
    if (a.has("strength"))
        next.brush["strength"] = std::clamp(a.get<double>("strength"), 0.0, 1.0);
    if (a.has("brush"))
    {
        if (!a.flags["brush"].is_object())
            throw CommandError(std::string(cmd) + ": brush is a dict of brush settings");
        next.brush.merge_patch(a.flags["brush"]);
    }
    // A new id: the pages see a changed stroke list and rebuild, and undo rebuilds them back.
    next.id = topId(*t.buffer) + 1;
    Buffer b = *t.buffer;
    b.strokes[i] = std::make_shared<const Stroke>(std::move(next));
    const json out = strokeJson(*b.strokes[i], i);
    commit(t, std::move(b), cmd);
    return out;
}

json deleteStrokes(const Args& a)
{
    const char* cmd = "texDeleteStrokes";
    Target t = target(a, cmd);
    if (!a.has("strokes") || !a.flags["strokes"].is_array())
        throw CommandError(std::string(cmd) + ": strokes is a list of stroke ids");
    std::vector<uint64_t> ids;
    for (const json& j : a.flags["strokes"])
    {
        ids.push_back(j.get<uint64_t>());
        indexOf(t, ids.back(), cmd);
    }
    Buffer b = *t.buffer;
    std::erase_if(b.strokes, [&](const StrokePtr& s) { return std::find(ids.begin(), ids.end(), s->id) != ids.end(); });
    const size_t left = b.strokes.size();
    commit(t, std::move(b), cmd);
    return {{"deleted", ids.size()}, {"strokes", left}};
}

json moveStroke(const Args& a)
{
    const char* cmd = "texMoveStroke";
    Target t = target(a, cmd);
    const size_t from = indexOf(t, uint64_t(a.get<int64_t>("stroke", 0)), cmd);
    const int64_t to = a.get<int64_t>("index", 0);
    if (to < 0 || size_t(to) >= t.buffer->strokes.size())
        throw CommandError(std::string(cmd) + ": index is 0 to " + std::to_string(t.buffer->strokes.size() - 1));
    Buffer b = *t.buffer;
    StrokePtr s = b.strokes[from];
    b.strokes.erase(b.strokes.begin() + std::ptrdiff_t(from));
    b.strokes.insert(b.strokes.begin() + std::ptrdiff_t(to), s);
    commit(t, std::move(b), cmd);
    return {{"stroke", s->id}, {"index", to}};
}

json pickStroke(const Args& a)
{
    const char* cmd = "texPickStroke";
    const Target t = target(a, cmd);
    if (!a.has("position") || !a.flags["position"].is_array() || a.flags["position"].size() != 3)
        throw CommandError(std::string(cmd) + ": position is [x, y, z] in the mesh's object space");
    const json& pj = a.flags["position"];
    const glm::vec3 p(pj[0].get<float>(), pj[1].get<float>(), pj[2].get<float>());
    const std::string mesh = a.get<std::string>("mesh", "");
    // Topmost first: the stroke painted last shows on top.
    for (size_t i = t.buffer->strokes.size(); i-- > 0;)
    {
        const Stroke& s = *t.buffer->strokes[i];
        if (!mesh.empty() && s.mesh != mesh)
            continue;
        std::shared_ptr<const PaintMesh> pm = paintMesh(graph(), s.mesh, setMaterial(*t.set));
        if (!pm)
            continue;
        for (const Dab& d : s.dabs)
            if (d.triangle < pm->triangleCount() && glm::distance(pm->point(d.triangle, d.u, d.v), p) <= d.radius)
                return strokeJson(s, i);
    }
    return nullptr;
}
} // namespace strokeedit

void registerStrokeEditCommands()
{
    using namespace strokeedit;
    ops::registerCommand({
        .name = "texStrokes",
        .doc = "A paint layer's strokes, bottom first: [{id, index, kind, mesh, channels, radius, strength, dabs, path}].",
        .positionalDoc = "set layer",
        .undoable = false,
        .run = listStrokes,
    });
    ops::registerCommand({
        .name = "texEditStroke",
        .doc = "Change a painted stroke after the fact and re-rasterise its layer: color (the colour channel), channels {name: value, "
               "null drops one}, radius (the widest dab's, the rest in proportion) or sizeScale, strength, brush (a patch over its "
               "brush settings). The stroke gets a new id, returned with the rest of it.",
        .positionalDoc = "set layer",
        .flags = {{"stroke", "", T::Int, nullptr, "The stroke's id (texStrokes)."},
                  {"color", "", T::Any, nullptr, "[r, g, b], linear."},
                  {"channels", "", T::Any, nullptr, "{channel: value}."},
                  {"radius", "", T::Float, nullptr, "Object units.", {}, {}, 0.0},
                  {"sizeScale", "", T::Float, nullptr, "Multiplies every dab's radius.", {}, {}, 0.0},
                  {"strength", "", T::Float, nullptr, "0 to 1.", {}, {}, 0.0, 1.0},
                  {"brush", "", T::Any, nullptr, "Brush settings to change (falloff, roundness, alpha...)."}},
        .run = editStroke,
    });
    ops::registerCommand({
        .name = "texDeleteStrokes",
        .doc = "Delete strokes from a paint layer and re-rasterise it.",
        .positionalDoc = "set layer",
        .flags = {{"strokes", "", T::List, nullptr, "Stroke ids."}},
        .run = deleteStrokes,
    });
    ops::registerCommand({
        .name = "texMoveStroke",
        .doc = "Move a stroke to another place in its layer's order (0 the bottom) and re-rasterise the layer.",
        .positionalDoc = "set layer",
        .flags = {{"stroke", "", T::Int, nullptr, "The stroke's id."}, {"index", "", T::Int, nullptr, "Its new place, 0 the bottom."}},
        .run = moveStroke,
    });
    ops::registerCommand({
        .name = "texPickStroke",
        .doc = "The topmost stroke of a paint layer with a dab over a surface point, as texStrokes lists it; None when no stroke covers it.",
        .positionalDoc = "set layer",
        .flags = {{"position", "", T::List, nullptr, "[x, y, z] in the mesh's object space."},
                  {"mesh", "", T::String, nullptr, "Only strokes on this mesh."}},
        .undoable = false,
        .run = pickStroke,
    });
}
} // namespace lila::render::paint
