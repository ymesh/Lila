#include "render/PaintGpu.h"

#include "geo/Bvh.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "ops/Undo.h"
#include "tex/Layers.h"
#include "scene/Components.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

#include <cmath>

/**
 * The channel picker: values of the texture set itself (every channel as the stack evaluates it) or of one paint layer,
 * read at a texel, never from the screen, so lighting and display transforms don't colour what is picked.
 */
namespace lila::render::paint
{
// Named, not anonymous: unity builds put this beside the other paint command files.
namespace pick
{
using namespace tex::paint;
using scene::Node;
using json = nlohmann::json;
using T = ArgType;

scene::Graph& graph() { return scene::Graph::current(); }

/** The set's evaluated channels per UDIM tile, kept while its graph, settings and paint stay the same. */
struct Evaluated
{
    size_t stamp = 0;
    const void* paint = nullptr;
    int udim = 0;
    std::map<std::string, image::Image> images;
};
std::map<scene::NodeId, Evaluated> gEvaluated;

const std::map<std::string, image::Image>& evaluated(Node& set, int udim)
{
    PaintPtr paint = readPaint(set);
    Evaluated& e = gEvaluated[set.id()];
    const size_t stamp = tex::setStamp(set);
    if (e.stamp == stamp && e.paint == paint.get() && e.udim == udim && !e.images.empty())
        return e.images;
    const tex::SetSettings s = tex::readSettings(set);
    std::pair<int, int> size{s.width, s.height};
    for (const tex::Tile& t : s.tiles)
        if (t.udim == udim)
            size = s.tileSize(t);
    e.images = tex::evaluateOutputs(set, tex::channelRequests(tex::readGraph(set), s), udim, size.first, size.second);
    e.stamp = stamp;
    e.paint = paint.get();
    e.udim = udim;
    return e.images;
}

/** The texel of uv (in its UDIM tile) of an image: rgba. */
glm::vec4 texel(const image::Image& img, glm::dvec2 uv)
{
    const double fu = uv.x - std::floor(uv.x), fv = uv.y - std::floor(uv.y);
    const int x = std::clamp(int(fu * img.width), 0, img.width - 1), y = std::clamp(int((1.0 - fv) * img.height), 0, img.height - 1);
    const float* t = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
    return {t[0], t[1], t[2], t[3]};
}

json valueJson(const tex::Channel* c, glm::vec4 v)
{
    if (c && c->type == tex::PortType::Gray)
        return v.x;
    return {v.x, v.y, v.z};
}

json sampleChannels(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("texSampleChannels: texture set required");
    Node* set = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!set)
        throw CommandError("texSampleChannels: no texture set named '" + a.positional[0].get<std::string>() + "'");
    glm::dvec2 uv;
    if (a.has("uv"))
    {
        const json u = a.flags["uv"];
        if (!u.is_array() || u.size() != 2)
            throw CommandError("texSampleChannels: uv is [u, v]");
        uv = {u[0].get<double>(), u[1].get<double>()};
    }
    else if (a.has("position") && a.has("mesh"))
    {
        std::shared_ptr<const PaintMesh> pm = paintMesh(graph(), a.get<std::string>("mesh"), setMaterial(*set));
        if (!pm)
            throw CommandError("texSampleChannels: " + a.get<std::string>("mesh") + " isn't a mesh with faces");
        const json p = a.flags["position"];
        if (!p.is_array() || p.size() != 3)
            throw CommandError("texSampleChannels: position is [x, y, z] in the mesh's object space");
        geo::BvhClosest hit;
        if (!pm->bvh->closestPoint({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}, hit, std::numeric_limits<double>::max()) ||
            hit.triangle >= pm->triangleCount())
            throw CommandError("texSampleChannels: no surface near " + p.dump());
        uv = glm::dvec2(pm->uv(uint32_t(hit.triangle), float(hit.bary.y), float(hit.bary.z)));
    }
    else
        throw CommandError("texSampleChannels: give uv, or mesh and position");

    const int udim = 1001 + int(std::floor(uv.x)) + 10 * int(std::floor(uv.y));
    const tex::SetSettings settings = tex::readSettings(*set);
    const std::string source = a.get<std::string>("source", "set");
    json values = json::object();
    if (source == "layer")
    {
        std::string layer = a.get<std::string>("layer", "");
        if (layer.empty())
            layer = readPaint(*set)->active;
        if (!readPaint(*set)->buffer(layer))
            throw CommandError("texSampleChannels: no paint layer '" + layer + "' in " + set->name());
        syncBuffer(graph(), *set, layer);
        // Only what the layer painted: a channel it never covered here has no value to pick.
        for (const tex::Channel& c : settings.channels)
        {
            const image::Image img = readChannel(set->id(), layer, c.name, udim);
            if (img.width <= 0)
                continue;
            const glm::vec4 v = texel(img, uv);
            if (v.w > 0.0f)
                values[c.name] = valueJson(&c, v);
        }
    }
    else if (source == "set")
    {
        try
        {
            for (const auto& [name, img] : evaluated(*set, udim))
                if (img.width > 0)
                    values[name] = valueJson(settings.channel(name), texel(img, uv));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string("texSampleChannels: ") + e.what());
        }
    }
    else
        throw CommandError("texSampleChannels: source is set or layer");
    return {{"uv", {uv.x, uv.y}}, {"udim", udim}, {"source", source}, {"channels", values}};
}
/** Even-odd: whether p is inside the polygon. */
bool inPolygon(const glm::dvec2& p, const std::vector<glm::dvec2>& poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < poly[j].x + (p.y - poly[j].y) / (poly[i].y - poly[j].y) * (poly[i].x - poly[j].x))
            in = !in;
    return in;
}

std::vector<glm::dvec2> pointsArg(const json& pts, const char* what)
{
    std::vector<glm::dvec2> out;
    if (!pts.is_array())
        throw CommandError(std::string("texSelectionMask: ") + what + " must be [[x, y], ...]");
    for (const json& p : pts)
    {
        if (!p.is_array() || p.size() < 2)
            throw CommandError(std::string("texSelectionMask: ") + what + " must be [[x, y], ...]");
        out.push_back({p[0].get<double>(), p[1].get<double>()});
    }
    return out;
}

/**
 * The paint buffer a layer's selection mask paints: a paint layer's own mask, else a paint mask on the layer reading
 * buffer sel_<layer>, added the first time.
 */
std::string selectionBuffer(Node& set, const std::string& layer)
{
    const tex::StackState st = tex::readStack(tex::readGraph(set), tex::readSettings(set));
    const tex::Layer* l = st.stack.find(layer);
    if (!l)
        throw CommandError("texSelectionMask: no layer " + layer);
    if (l->kind == tex::LayerKind::Paint)
        return layer;
    const std::string buffer = "sel_" + layer;
    const std::string ref = imageRef(buffer, kMaskChannel);
    for (const tex::Mask& m : l->masks)
        if (m.kind == tex::MaskKind::Paint && m.image == ref)
            return buffer;
    ops::execute("texAddMask", {{"args", {set.name(), layer, "paint"}}, {"image", ref}, {"name", "Selection"}}, CallSource::Script);
    return buffer;
}

json selectionMask(const Args& a)
{
    if (a.positional.size() < 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
        throw CommandError("texSelectionMask: give the texture set and the layer");
    Node* set = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!set)
        throw CommandError("texSelectionMask: no texture set named '" + a.positional[0].get<std::string>() + "'");
    const std::string layer = a.positional[1].get<std::string>();
    const std::string mode = a.get<std::string>("mode", "replace");
    if (mode != "replace" && mode != "add" && mode != "subtract")
        throw CommandError("texSelectionMask: mode is replace, add or subtract");
    const std::string buffer = selectionBuffer(*set, layer);
    const json brush = {{"radius", 1.0}};
    auto stroke = [&](json payload, json extra) {
        json st = {{"payload", std::move(payload)}, {"brush", brush}};
        if (a.has("mesh"))
            st["mesh"] = a.flags["mesh"];
        st.update(extra);
        return ops::execute("texPaintStroke", {{"args", {set->name(), buffer}}, {"stroke", st}}, CallSource::Script);
    };
    const char* blend = mode == "subtract" ? "erase" : "normal";
    // Replace starts from an empty selection.
    if (mode == "replace")
        stroke({{"kind", "fill"}, {"fillBy", "mesh"}, {"channels", json::object()}, {"mask", true}, {"blend", "erase"}}, json::object());

    if (a.has("group"))
        return stroke({{"kind", "fill"}, {"fillBy", "group"}, {"group", a.flags["group"]}, {"channels", json::object()}, {"mask", true}, {"blend", blend}},
                      json::object());
    if (a.has("uvPoints"))
    {
        // In uv space (the UV editor): the triangles whose middle is inside.
        const std::vector<glm::dvec2> poly = pointsArg(a.flags["uvPoints"], "uvPoints");
        std::string mesh = a.get<std::string>("mesh", "");
        if (mesh.empty())
            for (scene::NodeId id : graph().nodes())
                if (Node* n = graph().node(id); n && n->type().isA("mesh") && mesh.empty())
                    if (auto pm = paintMesh(graph(), n->path(), setMaterial(*set)); pm && pm->triangleCount())
                        mesh = n->path();
        std::shared_ptr<const PaintMesh> pm = mesh.empty() ? nullptr : paintMesh(graph(), mesh, setMaterial(*set));
        if (!pm)
            throw CommandError("texSelectionMask: no mesh uses " + set->name() + "'s material; pass mesh=");
        json tris = json::array();
        for (uint32_t t = 0; t < pm->triangleCount(); ++t)
            if (inPolygon(glm::dvec2(pm->uv(t, 1.0f / 3.0f, 1.0f / 3.0f)), poly))
                tris.push_back(t);
        if (tris.empty())
            return json{{"layer", buffer}, {"triangles", 0}};
        stroke({{"kind", "fill"}, {"triangles", tris}, {"fillBy", "uv shape"}, {"channels", json::object()}, {"mask", true}, {"blend", blend}},
               {{"mesh", mesh}});
        return json{{"layer", buffer}, {"triangles", tris.size()}};
    }
    // On screen: the shape drawn into a picture the size of the view, projected into the visible texels.
    if (!a.has("view"))
        throw CommandError("texSelectionMask: a screen shape needs view {width, height, camera | ...}");
    const json view = a.flags["view"];
    std::vector<glm::dvec2> poly;
    if (a.has("rect"))
    {
        const json r = a.flags["rect"];
        if (!r.is_array() || r.size() != 4)
            throw CommandError("texSelectionMask: rect is [x0, y0, x1, y1] screen pixels");
        const double x0 = r[0].get<double>(), y0 = r[1].get<double>(), x1 = r[2].get<double>(), y1 = r[3].get<double>();
        poly = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    else if (a.has("points"))
        poly = pointsArg(a.flags["points"], "points");
    else
        throw CommandError("texSelectionMask: give points (lasso or polygon), rect, uvPoints or group");
    if (poly.size() < 3)
        throw CommandError("texSelectionMask: a shape needs at least three points");
    const int w = std::clamp(view.value("width", 1), 1, 8192), h = std::clamp(view.value("height", 1), 1, 8192);
    std::vector<float> rgba(size_t(w) * size_t(h) * 4, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (inPolygon({x + 0.5, y + 0.5}, poly))
                for (int c = 0; c < 4; ++c)
                    rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = 1.0f;
    const json picture = PaintImage::fromRgba(w, h, rgba)->toJson();
    return stroke({{"kind", "project"}, {"image", picture}, {"colorChannel", ""}, {"channels", json::object()}, {"mask", true}, {"blend", blend}},
                  {{"view", view}, {"occlusion", a.get<bool>("occlusion", true)}});
}

json faceGroup(const Args& a)
{
    if (a.positional.size() < 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
        throw CommandError("texFaceGroup: give the texture set and the group's name");
    Node* set = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!set)
        throw CommandError("texFaceGroup: no texture set named '" + a.positional[0].get<std::string>() + "'");
    const std::string name = a.positional[1].get<std::string>();
    tex::SetSettings s = tex::readSettings(*set);
    if (a.get<bool>("remove", false))
        s.faceGroups.erase(name);
    else
    {
        std::string mesh = a.get<std::string>("mesh", "");
        if (mesh.empty())
            for (scene::NodeId id : graph().selection())
                if (Node* n = graph().node(id); n && n->type().isA("mesh") && mesh.empty())
                    mesh = n->path();
        Node* m = mesh.empty() ? nullptr : graph().find(mesh);
        if (!m)
            throw CommandError("texFaceGroup: give mesh= or select the mesh");
        std::vector<uint32_t> faces = a.has("faces") ? a.flags["faces"].get<std::vector<uint32_t>>()
                                                     : scene::components::selected(m->id(), scene::ComponentMode::Face);
        if (faces.empty())
            throw CommandError("texFaceGroup: no faces given or selected on " + m->path());
        std::sort(faces.begin(), faces.end());
        s.faceGroups[name] = {{"mesh", m->path()}, {"faces", faces}};
    }
    tex::writeSettings(graph(), *set, s);
    return s.faceGroups;
}
} // namespace pick

void registerPickCommands()
{
    ops::registerCommand({
        .name = "texSampleChannels",
        .doc = "Pick a texture set's channel values at a point: uv=[u, v], or mesh= and position=[x, y, z] in its object space. source=set "
               "reads every channel as the stack evaluates it; source=layer reads one paint layer's own paint (layer=, else the "
               "active one), leaving out channels it didn't paint there. Values are linear: [r, g, b] for colour and vector channels, "
               "a number for gray ones. Returns {uv, udim, source, channels}.",
        .positionalDoc = "set",
        .flags = {{"uv", "", ArgType::List, nullptr, "[u, v]."},
                  {"mesh", "", ArgType::String, nullptr, "The mesh position is on."},
                  {"position", "", ArgType::List, nullptr, "[x, y, z] in the mesh's object space."},
                  {"source", "", ArgType::String, "set", "set or layer.", {"set", "layer"}},
                  {"layer", "", ArgType::String, nullptr, "The paint layer, for source=layer."}},
        .undoable = false,
        .run = pick::sampleChannels,
    });
    ops::registerCommand({
        .name = "texSelectionMask",
        .doc = "Make a layer's selection mask from a shape: a lasso or polygon (points, screen pixels) or a rectangle (rect [x0, y0, x1, y1]) "
               "through view, projected into the texels the view sees (occlusion=False reaches hidden ones too); a polygon in uv space "
               "(uvPoints, the triangles whose middle is inside); or a face group (group). mode replace, add or subtract. A paint layer's "
               "own mask takes it; any other layer gets a paint mask named Selection. Returns texPaintStroke's result.",
        .positionalDoc = "set layer",
        .flags = {{"points", "", ArgType::List, nullptr, "Lasso or polygon, [[x, y], ...] screen pixels."},
                  {"rect", "", ArgType::List, nullptr, "[x0, y0, x1, y1] screen pixels."},
                  {"view", "", ArgType::Any, nullptr, "The screen view, as texPaintStroke takes it."},
                  {"uvPoints", "", ArgType::List, nullptr, "A polygon in uv space."},
                  {"group", "", ArgType::String, nullptr, "A face group (texFaceGroup)."},
                  {"mode", "", ArgType::String, "replace", "replace, add or subtract.", {"replace", "add", "subtract"}},
                  {"occlusion", "", ArgType::Bool, true, "Only what the view sees."},
                  {"mesh", "", ArgType::String, nullptr, "The painted mesh."}},
        .run = pick::selectionMask,
    });
    ops::registerCommand({
        .name = "texFaceGroup",
        .doc = "Save a named face set on a texture set for painting (the faces given, else those selected on the mesh), or remove it. "
               "Groups make selection masks (texSelectionMask group=), fill targets (fillBy group) and locks (the paint tool's Lock). "
               "Returns every group.",
        .positionalDoc = "set name",
        .flags = {{"faces", "", ArgType::List, nullptr, "Face indices; else the mesh's selected faces."},
                  {"mesh", "", ArgType::String, nullptr, "The mesh; else the selected one."},
                  {"remove", "", ArgType::Bool, false, "Remove the group."}},
        .run = pick::faceGroup,
    });
}
} // namespace lila::render::paint
