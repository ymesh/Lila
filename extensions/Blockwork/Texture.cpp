#include "Blockwork.h"

#include <lila/Components.h>
#include <lila/Deformer.h>
#include <lila/Prefs.h>
#include <lila/Transform.h>

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QImageReader>
#include <QPainter>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace blockwork
{
using namespace lila;
using namespace lila::scene;
using geo::Index;

namespace
{
constexpr const char* kDensityPref = "/blockwork/texelDensity";

std::vector<Index> facesOf(const Args& a, NodeId mesh, const geo::Mesh& m, bool allWhenNone = true)
{
    std::vector<Index> out;
    if (a.has("faces"))
        for (const json& f : a.flags["faces"])
            out.push_back(f.get<Index>());
    else
        for (uint32_t f : components::selected(mesh, ComponentMode::Face))
            out.push_back(f);
    if (out.empty() && allWhenNone)
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                out.push_back(f);
    return out;
}

glm::dvec2 vec2Flag(const Args& a, const char* name)
{
    const json& v = a.flags[name];
    if (!v.is_array() || v.size() != 2)
        throw CommandError(std::string(name) + ": give two numbers, [u, v]");
    return {v[0].get<double>(), v[1].get<double>()};
}

/** Faces' u and v extents (cm) along their mapping axes, before scale and offset. */
void extents(const geo::Mesh& m, Index f, const FaceMapping& fm, glm::dvec2& lo, glm::dvec2& hi)
{
    glm::dvec3 u, v;
    mappingAxes(fm, m.faceNormal(f), u, v);
    lo = glm::dvec2(1e300), hi = glm::dvec2(-1e300);
    for (Index p : m.facePoints(f))
    {
        const glm::dvec2 q(glm::dot(m.position(p), u), glm::dot(m.position(p), v));
        lo = glm::min(lo, q), hi = glm::max(hi, q);
    }
}

double tileOf(Graph& g, NodeId mesh)
{
    for (NodeId id : modifiers::stack(g, mesh))
        if (Node* n = g.node(id); n && n->type().name == kTextureLockType)
            return n->as<double>(n->type().attrIndex("tileSize"));
    return 100.0;
}

NodeId textureLockOf(Graph& g, NodeId mesh)
{
    for (NodeId id : modifiers::stack(g, mesh))
        if (Node* n = g.node(id); n && n->type().name == kTextureLockType)
            return id;
    return kNoNode;
}

/** The face's mapping with its axes made explicit (rotation folded in), for editing them. */
FaceMapping explicitMapping(const geo::Mesh& m, Index f)
{
    FaceMapping fm = faceMapping(m, f);
    glm::dvec3 u, v;
    mappingAxes(fm, m.faceNormal(f), u, v);
    fm.mode = kMapAxes;
    fm.rotate = 0.0f;
    fm.uAxis = glm::vec3(u);
    fm.vAxis = glm::vec3(v);
    return fm;
}

/** The edge two faces share, kInvalid when none. */
Index sharedEdge(const geo::Mesh& m, Index a, Index b)
{
    Index found = geo::kInvalid;
    m.forEachFaceCorner(a, [&](Index c) {
        m.forEachEdgeCorner(m.cornerEdge(c), [&](Index k) {
            if (m.cornerFace(k) == b)
                found = m.cornerEdge(c);
        });
    });
    return found;
}

/** source's mapping carried onto target across the edge they share, so the texture runs on round the corner. */
FaceMapping wrapped(const geo::Mesh& m, Index source, Index target, Index edge, const FaceMapping& src)
{
    FaceMapping fm = src;
    glm::dvec3 u, v;
    mappingAxes(src, m.faceNormal(source), u, v);
    const glm::dvec3 a = m.position(m.edgePoint(edge, 0)), b = m.position(m.edgePoint(edge, 1));
    const glm::dvec3 axis = glm::normalize(b - a);
    const glm::dvec3 ns = m.faceNormal(source), nt = m.faceNormal(target);
    const double angle = std::atan2(glm::dot(glm::cross(ns, nt), axis), glm::dot(ns, nt));
    const glm::dmat3 r(glm::rotate(glm::dmat4(1.0), angle, axis));
    const glm::dvec3 ru = r * u, rv = r * v;
    const double su = src.scale.x != 0.0f ? src.scale.x : 100.0, sv = src.scale.y != 0.0f ? src.scale.y : 100.0;
    fm.mode = kMapAxes;
    fm.rotate = 0.0f;
    fm.uAxis = glm::vec3(ru);
    fm.vAxis = glm::vec3(rv);
    // Points on the shared edge keep their uv.
    fm.offset = src.offset + glm::vec2(float(glm::dot(a, u - ru) / su), float(glm::dot(a, v - rv) / sv));
    return fm;
}

json runEach(const Args& a, const char* cmd, const std::function<void(Graph&, NodeId, geo::Mesh&, const std::vector<Index>&)>& fn)
{
    Graph& g = graph();
    json out = json::array();
    for (NodeId id : blockArgs(a, cmd))
    {
        geo::Mesh m = baseMesh(g, id);
        const std::vector<Index> faces = facesOf(a, id, m);
        fn(g, id, m, faces);
        setBaseMesh(g, id, std::move(m));
        out.push_back(g.fullPath(id));
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select blocks or their faces");
    return out;
}
} // namespace

double texelDensity()
{
    const double d = PrefStore::get().value<double>("general", kDensityPref, 5.12);
    return d > 1e-6 ? d : 5.12;
}

std::optional<glm::dvec2> materialRepeat(Graph& g, const std::string& material)
{
    Node* mat = materials::find(g, material);
    if (!mat)
        return std::nullopt;
    for (const auto& [name, attr] : materials::textureChannels())
        if (name == "baseColor")
        {
            const std::string path = materials::resolvePath(mat->as<std::string>(attr));
            if (path.empty())
                return std::nullopt;
            const QSize s = QImageReader(QString::fromStdString(path)).size();
            if (!s.isValid() || s.isEmpty())
                return std::nullopt;
            return glm::dvec2(s.width(), s.height()) / texelDensity();
        }
    return std::nullopt;
}

std::string materialForImage(Graph& g, const std::string& path)
{
    const QFileInfo info(QString::fromStdString(path));
    if (!info.exists())
        throw CommandError("no image at " + path);
    // Reuse a material already showing this image.
    for (Node* m : materials::all(g))
        for (const auto& [name, attr] : materials::textureChannels())
            if (name == "baseColor" && !m->as<std::string>(attr).empty() &&
                QFileInfo(QString::fromStdString(materials::resolvePath(m->as<std::string>(attr)))).canonicalFilePath() == info.canonicalFilePath())
                return m->name();
    Node* m = materials::create(g, info.completeBaseName().toStdString());
    ops::execute("setMaterialTexture", {{"args", {m->name()}}, {"channel", "baseColor"}, {"path", path}});
    return m->name();
}

void registerTextureCommands(ext::Context& ctx)
{
    using T = ArgType;

    ctx.registerCommand({
        .name = "blockworkTexture",
        .doc = "Change how textures lie on blocks' faces: shift, scale, rotate, fit, justify, or align to the world or the face. Acts on "
               "the selected faces, or every face of the selected blocks. Returns the blocks.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"faces", "f", T::List, nullptr, "Face ids; default the selected faces, or all."},
                  {"shift", "sh", T::List, nullptr, "Add [u, v] to the offset, in repeats."},
                  {"offset", "o", T::List, nullptr, "Set the offset [u, v], in repeats."},
                  {"scale", "s", T::List, nullptr, "Set cm per repeat [u, v]; 0 uses the block's tile size."},
                  {"rotate", "r", T::Float, nullptr, "Set the rotation, degrees."},
                  {"turn", "t", T::Float, nullptr, "Add to the rotation, degrees."},
                  {"align", "a", T::String, nullptr, "face (on the face's own plane) or world (along the nearest world axis).", {"face", "world"}},
                  {"fit", "", T::String, nullptr, "Make one repeat cover the face: width, height or both.", {"width", "height", "both"}},
                  {"justify", "j", T::String, nullptr, "Line the texture's edge up with the face's: left, right, top, bottom or center.",
                   {"left", "right", "top", "bottom", "center"}},
                  {"reset", "", T::Bool, false, "Back to face aligned at the tile size, no offset or rotation."}},
        .run = [](const Args& a) -> json {
            return runEach(a, "blockworkTexture", [&](Graph& g, NodeId id, geo::Mesh& m, const std::vector<Index>& faces) {
                const double tile = tileOf(g, id);
                for (Index f : faces)
                {
                    if (!m.faceValid(f))
                        continue;
                    FaceMapping fm = a.get<bool>("reset", false) ? FaceMapping() : faceMapping(m, f);
                    if (a.has("align"))
                        fm.mode = a.get<std::string>("align") == "world" ? kMapWorld : kMapFace;
                    if (fm.mode == kMapStored)
                        fm.mode = kMapFace;
                    if (a.has("rotate"))
                        fm.rotate = float(a.get<double>("rotate"));
                    if (a.has("turn"))
                        fm.rotate = float(std::fmod(double(fm.rotate) + a.get<double>("turn"), 360.0));
                    if (a.has("scale"))
                        fm.scale = glm::vec2(vec2Flag(a, "scale"));
                    if (a.has("offset"))
                        fm.offset = glm::vec2(vec2Flag(a, "offset"));
                    if (a.has("shift"))
                        fm.offset += glm::vec2(vec2Flag(a, "shift"));
                    glm::dvec2 lo, hi;
                    if (a.has("fit"))
                    {
                        extents(m, f, fm, lo, hi);
                        const std::string fit = a.get<std::string>("fit");
                        const double su = fm.scale.x != 0.0f ? fm.scale.x : tile, sv = fm.scale.y != 0.0f ? fm.scale.y : tile;
                        const double aspect = sv / su;
                        if (fit == "width" || fit == "both")
                            fm.scale.x = float(hi.x - lo.x), fm.scale.y = float(fit == "both" ? hi.y - lo.y : (hi.x - lo.x) * aspect);
                        else
                            fm.scale.y = float(hi.y - lo.y), fm.scale.x = float((hi.y - lo.y) / aspect);
                        // Fitting also lines the repeat up with the face.
                        fm.offset = glm::vec2(float(-lo.x / fm.scale.x), float(-lo.y / fm.scale.y));
                    }
                    if (a.has("justify"))
                    {
                        extents(m, f, fm, lo, hi);
                        const double su = fm.scale.x != 0.0f ? fm.scale.x : tile, sv = fm.scale.y != 0.0f ? fm.scale.y : tile;
                        const std::string j = a.get<std::string>("justify");
                        if (j == "left")
                            fm.offset.x = float(-lo.x / su);
                        else if (j == "right")
                            fm.offset.x = float(1.0 - hi.x / su);
                        else if (j == "bottom")
                            fm.offset.y = float(-lo.y / sv);
                        else if (j == "top")
                            fm.offset.y = float(1.0 - hi.y / sv);
                        else
                            fm.offset = glm::vec2(float(0.5 - 0.5 * (lo.x + hi.x) / su), float(0.5 - 0.5 * (lo.y + hi.y) / sv));
                    }
                    setFaceMapping(m, f, fm);
                }
            });
        },
        .adjustable = true,
        .examples = {"blockworkTexture('room', faces=[0, 1], shift=[0.25, 0])", "blockworkTexture('door', fit='both')"},
    });

    ctx.registerCommand({
        .name = "blockworkMaterial",
        .doc = "Put a material, or an image (made into a material once and reused), on blocks' faces, scaled to the texel density "
               "from its base colour image. Returns the material.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"material", "m", T::String, nullptr, "A material in the scene."},
                  {"image", "i", T::String, nullptr, "An image file; a material showing it is made or reused."},
                  {"faces", "f", T::List, nullptr, "Face ids; default the selected faces, or all."},
                  {"keepScale", "", T::Bool, false, "Leave the faces' scale as it is."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::string mat = a.get<std::string>("material", "");
            if (a.has("image"))
                mat = materialForImage(g, a.get<std::string>("image"));
            if (mat.empty() || !materials::find(g, mat))
                throw CommandError("blockworkMaterial: give a material in the scene or an image");
            const std::optional<glm::dvec2> repeat = materialRepeat(g, mat);
            runEach(a, "blockworkMaterial", [&](Graph&, NodeId, geo::Mesh& m, const std::vector<Index>& faces) {
                materials::assign(m, mat, std::vector<uint32_t>(faces.begin(), faces.end()));
                if (repeat && !a.get<bool>("keepScale", false))
                    for (Index f : faces)
                    {
                        FaceMapping fm = faceMapping(m, f);
                        fm.scale = glm::vec2(*repeat);
                        setFaceMapping(m, f, fm);
                    }
            });
            return mat;
        },
        .examples = {"blockworkMaterial('room', image='X:/Textures/T_Brick_D.png')", "blockworkMaterial(material='Concrete')"},
    });

    ctx.registerCommand({
        .name = "blockworkCopyTexture",
        .doc = "Copy one face's material and mapping onto other faces. A face sharing an edge with the source carries the texture on "
               "round the corner. Returns the blocks changed.",
        .positionalDoc = "blocks to copy onto (default the selection)",
        .flags = {{"source", "src", T::String, nullptr, "The block the texture comes from."},
                  {"sourceFace", "sf", T::Int, nullptr, "The face on it."},
                  {"faces", "f", T::List, nullptr, "Face ids to copy onto; default the selected faces, or all."},
                  {"what", "w", T::String, "both", "material, mapping or both.", {"material", "mapping", "both"}},
                  {"wrap", "", T::Bool, true, "Carry the texture round shared edges instead of copying the mapping as is."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* src = a.has("source") ? g.find(a.get<std::string>("source")) : nullptr;
            if (!src || !a.has("sourceFace"))
                throw CommandError("blockworkCopyTexture: give the source block and face");
            const geo::Mesh sm = baseMesh(g, src->id());
            const Index sf = Index(a.get<int>("sourceFace"));
            if (!sm.faceValid(sf))
                throw CommandError("blockworkCopyTexture: " + src->name() + " has no face " + std::to_string(sf));
            const FaceMapping fm = faceMapping(sm, sf);
            std::string mat;
            if (std::span<const int32_t> ids = sm.faceAttrs().get<int32_t>(geo::attr::kMaterial); sf < ids.size() && ids[sf] >= 0 &&
                size_t(ids[sf]) < sm.materialNames().size())
                mat = sm.materialNames()[size_t(ids[sf])];
            const std::string what = a.get<std::string>("what", "both");
            const NodeId srcId = src->id();
            return runEach(a, "blockworkCopyTexture", [&](Graph&, NodeId id, geo::Mesh& m, const std::vector<Index>& faces) {
                std::vector<uint32_t> tf;
                for (Index f : faces)
                {
                    if (!m.faceValid(f) || (id == srcId && f == sf))
                        continue;
                    tf.push_back(f);
                    if (what == "material")
                        continue;
                    const Index e = id == srcId && a.get<bool>("wrap", true) ? sharedEdge(m, sf, f) : geo::kInvalid;
                    setFaceMapping(m, f, e != geo::kInvalid ? wrapped(m, sf, f, e, fm) : fm);
                }
                if (what != "mapping" && !mat.empty())
                    materials::assign(m, mat, tf);
            });
        },
        .examples = {"blockworkCopyTexture('room', source='room', sourceFace=2, faces=[3, 4])"},
    });

    ctx.registerCommand({
        .name = "blockworkAlignEdge",
        .doc = "Line a texture's side up with one of a face's edges: the texture runs along the edge and starts at it. Returns the block.",
        .positionalDoc = "block (default the selection)",
        .flags = {{"face", "f", T::Int, nullptr, "The face."},
                  {"edge", "e", T::Int, 0, "Which of the face's edges, counted round it from its first corner."},
                  {"side", "s", T::String, "bottom", "The texture side on the edge: bottom, top, left or right.", {"bottom", "top", "left", "right"}},
                  {"fit", "", T::String, "none", "Also make one repeat span the face: none, width or height.", {"none", "width", "height"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> ids = blockArgs(a, "blockworkAlignEdge");
            if (ids.empty() || !a.has("face"))
                throw CommandError("blockworkAlignEdge: give the block and face");
            geo::Mesh m = baseMesh(g, ids[0]);
            const Index f = Index(a.get<int>("face"));
            if (!m.faceValid(f))
                throw CommandError("blockworkAlignEdge: no face " + std::to_string(f));
            const std::vector<Index> pts = m.facePoints(f);
            const size_t k = size_t(std::max(0, a.get<int>("edge", 0))) % pts.size();
            const glm::dvec3 pa = m.position(pts[k]), pb = m.position(pts[(k + 1) % pts.size()]);
            const glm::dvec3 n = m.faceNormal(f), along = glm::normalize(pb - pa), into = glm::normalize(glm::cross(n, along));
            const std::string side = a.get<std::string>("side", "bottom");
            glm::dvec3 u = side == "bottom" ? along : side == "top" ? -along : side == "left" ? into : -into;
            glm::dvec3 v = glm::cross(n, u);
            FaceMapping fm = faceMapping(m, f);
            const double tile = tileOf(g, ids[0]);
            fm.mode = kMapAxes, fm.rotate = 0.0f, fm.uAxis = glm::vec3(u), fm.vAxis = glm::vec3(v);
            glm::dvec2 lo, hi;
            extents(m, f, fm, lo, hi);
            const std::string fit = a.get<std::string>("fit", "none");
            const double aspect = (fm.scale.y != 0.0f ? fm.scale.y : tile) / (fm.scale.x != 0.0f ? fm.scale.x : tile);
            if (fit == "width")
                fm.scale = glm::vec2(float(hi.x - lo.x), float((hi.x - lo.x) * aspect));
            else if (fit == "height")
                fm.scale = glm::vec2(float((hi.y - lo.y) / aspect), float(hi.y - lo.y));
            const double su = fm.scale.x != 0.0f ? fm.scale.x : tile, sv = fm.scale.y != 0.0f ? fm.scale.y : tile;
            // The edge's start sits at a repeat's corner; the named texture side lies on the edge.
            fm.offset.x = float(side == "right" ? 1.0 - glm::dot(pa, u) / su : -glm::dot(pa, u) / su);
            fm.offset.y = float(side == "top" ? 1.0 - glm::dot(pa, v) / sv : -glm::dot(pa, v) / sv);
            if (side == "left" || side == "right")
                fm.offset.y = float(-lo.y / sv);
            if (side == "bottom" || side == "top")
                fm.offset.x = float(-glm::dot(pa, u) / su);
            setFaceMapping(m, f, fm);
            setBaseMesh(g, ids[0], std::move(m));
            return g.fullPath(ids[0]);
        },
        .adjustable = true,
        .examples = {"blockworkAlignEdge('wall', face=2, edge=0, side='bottom', fit='height')"},
    });

    ctx.registerCommand({
        .name = "blockworkTextureStrip",
        .doc = "Lay a texture along a strip of connected faces (a road, a curved trim), running on from face to face and across "
               "the strip once. Returns the block.",
        .positionalDoc = "block (default the selection)",
        .flags = {{"faces", "f", T::List, nullptr, "The strip's faces; default the selected faces."},
                  {"fitWidth", "", T::Bool, true, "One repeat spans the strip's width; off keeps the scale."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> ids = blockArgs(a, "blockworkTextureStrip");
            if (ids.empty())
                throw CommandError("blockworkTextureStrip: select a strip of faces");
            geo::Mesh m = baseMesh(g, ids[0]);
            const std::vector<Index> faces = facesOf(a, ids[0], m, false);
            if (faces.size() < 2)
                throw CommandError("blockworkTextureStrip: select two or more connected faces");
            const std::set<Index> in(faces.begin(), faces.end());
            std::map<Index, std::vector<std::pair<Index, Index>>> next; // face -> (neighbour, shared edge)
            for (Index f : faces)
                m.forEachFaceCorner(f, [&](Index c) {
                    m.forEachEdgeCorner(m.cornerEdge(c), [&](Index k) {
                        const Index o = m.cornerFace(k);
                        if (o != f && in.count(o))
                            next[f].push_back({o, m.cornerEdge(c)});
                    });
                });
            Index start = faces.front();
            for (Index f : faces)
                if (next[f].size() <= 1)
                {
                    start = f;
                    break;
                }
            const double tile = tileOf(g, ids[0]);
            auto mid = [&](Index e) { return (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5; };
            std::set<Index> done;
            Index cur = start, entry = geo::kInvalid;
            double run = 0.0;
            while (cur != geo::kInvalid && !done.count(cur))
            {
                done.insert(cur);
                Index exit = geo::kInvalid, nextFace = geo::kInvalid;
                for (auto [o, e] : next[cur])
                    if (!done.count(o))
                    {
                        exit = e, nextFace = o;
                        break;
                    }
                const glm::dvec3 n = m.faceNormal(cur), c = m.faceCentroid(cur);
                glm::dvec3 from = entry != geo::kInvalid ? mid(entry) : c - (mid(exit) - c);
                glm::dvec3 to = exit != geo::kInvalid ? mid(exit) : c + (c - mid(entry));
                glm::dvec3 u = to - from;
                u -= n * glm::dot(u, n);
                if (glm::length(u) < 1e-9)
                    break;
                u = glm::normalize(u);
                const glm::dvec3 v = glm::cross(n, u);
                FaceMapping fm = faceMapping(m, cur);
                fm.mode = kMapAxes, fm.rotate = 0.0f, fm.uAxis = glm::vec3(u), fm.vAxis = glm::vec3(v);
                glm::dvec2 lo, hi;
                extents(m, cur, fm, lo, hi);
                if (a.get<bool>("fitWidth", true))
                    fm.scale = glm::vec2(float(hi.y - lo.y), float(hi.y - lo.y));
                const double su = fm.scale.x != 0.0f ? fm.scale.x : tile, sv = fm.scale.y != 0.0f ? fm.scale.y : tile;
                // u carries on from the distance run so far; v centres the strip.
                fm.offset = glm::vec2(float(run / su - glm::dot(from, u) / su), float(0.5 - 0.5 * (lo.y + hi.y) / sv));
                setFaceMapping(m, cur, fm);
                run += glm::length(to - from);
                entry = exit;
                cur = nextFace;
            }
            setBaseMesh(g, ids[0], std::move(m));
            return g.fullPath(ids[0]);
        },
        .examples = {"blockworkTextureStrip('road', faces=[4, 5, 6, 7, 8])"},
    });

    ctx.registerCommand({
        .name = "blockworkHotspot",
        .doc = "Map blocks' faces onto the current trim sheet: each face region gets the trim whose shape and size fit it best. live "
               "keeps whole blocks hotspotted as they change; otherwise the selected faces are mapped once and keep those uvs. "
               "Needs the Trim Sheets extension. Returns what the trim command returned.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"live", "l", T::Bool, true, "Refit whenever the block changes (whole blocks)."},
                  {"faces", "f", T::List, nullptr, "Face ids for a one-off fit; default the selected faces."},
                  {"seed", "s", T::Int, 0, "Variation seed."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (!ops::find("trimAuto") || !ops::find("trimLive"))
                throw CommandError("blockworkHotspot: turn on the Trim Sheets extension");
            std::vector<NodeId> ids = blockArgs(a, "blockworkHotspot");
            if (ids.empty())
                throw CommandError("blockworkHotspot: select blocks");
            const std::vector<NodeId> before = g.selection();
            json result;
            if (a.get<bool>("live", true) && !a.has("faces"))
            {
                g.select(ids);
                result = ops::execute("trimLive", {{"seed", a.get<int>("seed", 0)}});
            }
            else
            {
                for (NodeId id : ids)
                {
                    geo::Mesh m = baseMesh(g, id);
                    std::vector<Index> faces = facesOf(a, id, m);
                    components::select(id, ComponentMode::Face, components::Ids(faces.begin(), faces.end()));
                    // Hotspotted faces keep the uvs the trim command writes.
                    for (Index f : faces)
                    {
                        FaceMapping fm = faceMapping(m, f);
                        fm.mode = kMapStored;
                        setFaceMapping(m, f, fm);
                    }
                    setBaseMesh(g, id, std::move(m));
                }
                g.select(ids);
                result = ops::execute("trimAuto", {{"seed", a.get<int>("seed", 0)}});
            }
            g.select(before);
            return result;
        },
        .examples = {"blockworkHotspot('room')", "blockworkHotspot('room', live=False, faces=[2, 3])"},
    });

    ctx.registerCommand({
        .name = "blockworkTextureLock",
        .doc = "Turn blocks' Texture Lock on or off. Off keeps the uvs as they are now, which then stretch with edits; on lays them "
               "out from the faces' mappings again. Returns the blocks.",
        .positionalDoc = "blocks (default the selection)",
        .flags = {{"on", "", T::Bool, true, "On or off."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            const bool on = a.get<bool>("on", true);
            for (NodeId id : blockArgs(a, "blockworkTextureLock"))
            {
                const NodeId lock = textureLockOf(g, id);
                if (lock == kNoNode)
                    continue;
                Node* n = g.node(lock);
                const int enabled = n->type().attrIndex("enabled");
                if (n->as<bool>(enabled) == on)
                    continue;
                if (!on)
                {
                    // Keep the uvs the lock laid out, so turning it off changes nothing until the next edit.
                    geo::Mesh m = baseMesh(g, id);
                    applyMapping(m, tileOf(g, id));
                    setBaseMesh(g, id, std::move(m));
                }
                g.setValue({lock, enabled, -1}, on);
                out.push_back(g.fullPath(id));
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "blockworkPlaceholder",
        .doc = "Make a placeholder texture, a flat colour with a label and a grid, saved as a PNG in folder, and a material showing "
               "it. Returns the material.",
        .flags = {{"label", "l", T::String, "", "Text written on it; default the name."},
                  {"name", "n", T::String, "Placeholder", "File and material name."},
                  {"color", "c", T::List, json::array({0.5, 0.5, 0.5}), "[r, g, b], 0 to 1."},
                  {"folder", "d", T::String, nullptr, "Where the PNG goes; default the workspace's textures folder, or the temp folder."},
                  {"size", "s", T::Int, 512, "Pixels square.", {}, {}, 16, 4096}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const json c = a.get<json>("color", json::array({0.5, 0.5, 0.5}));
            const QColor col = QColor::fromRgbF(float(c.at(0).get<double>()), float(c.at(1).get<double>()), float(c.at(2).get<double>()));
            const int size = a.get<int>("size", 512);
            const std::string name = a.get<std::string>("name", "Placeholder");
            const QString label = QString::fromStdString(a.get<std::string>("label", "").empty() ? name : a.get<std::string>("label"));
            QImage img(size, size, QImage::Format_RGB32);
            img.fill(col);
            {
                QPainter p(&img);
                const QColor ink = col.lightnessF() > 0.5 ? col.darker(160) : col.lighter(160);
                p.setPen(QPen(ink, std::max(1, size / 256)));
                for (int i = 0; i <= 4; ++i)
                {
                    p.drawLine(i * size / 4, 0, i * size / 4, size);
                    p.drawLine(0, i * size / 4, size, i * size / 4);
                }
                QFont f;
                f.setPixelSize(std::max(8, size / 10));
                f.setBold(true);
                p.setFont(f);
                p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, label);
            }
            QString folder = a.has("folder") ? QString::fromStdString(a.get<std::string>("folder")) : QString();
            if (folder.isEmpty())
                folder = QDir::temp().filePath("LilaPlaceholders");
            QDir().mkpath(folder);
            const QString file = QDir(folder).filePath(QString::fromStdString(name) + ".png");
            if (!img.save(file))
                throw CommandError("blockworkPlaceholder: could not write " + file.toStdString());
            return materialForImage(g, file.toStdString());
        },
        .examples = {"blockworkPlaceholder(name='Floor_Dev', label='FLOOR', color=[0.35, 0.4, 0.45])"},
    });
}
} // namespace blockwork
