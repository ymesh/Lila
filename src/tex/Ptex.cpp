#include "tex/Ptex.h"

#include "bake/Bake.h"
#include "core/ImageIO.h"
#include "core/Paths.h"
#include "geo/Attribute.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "tex/Plan.h"
#include "tex/TextureSet.h"

#include <Ptexture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

namespace lila::tex
{
namespace
{
/**
 * An all-quad or all-triangle mesh's faces in Ptex order, each with its corners from the face's first (the fourth unused
 * for triangles).
 */
struct PtexFaces
{
    int sides = 4;
    std::vector<std::array<geo::Index, 4>> corners;
    std::vector<geo::Index> faces;

    Ptex::MeshType meshType() const { return sides == 3 ? Ptex::mt_triangle : Ptex::mt_quad; }
};

PtexFaces ptexFaces(const geo::Mesh& m, const std::string& command)
{
    PtexFaces q;
    q.sides = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const int n = int(m.faceSize(f));
        if (q.sides == 0 && (n == 3 || n == 4))
            q.sides = n;
        if (n != q.sides)
            throw CommandError(command + ": Ptex needs an all-quad or all-triangle mesh; face " + std::to_string(q.faces.size()) + " has " +
                               std::to_string(n) + " sides" + (q.sides ? " where the first has " + std::to_string(q.sides) : std::string()));
        std::array<geo::Index, 4> c{geo::kInvalid, geo::kInvalid, geo::kInvalid, geo::kInvalid};
        c[0] = m.faceCorner(f);
        for (int k = 1; k < n; ++k)
            c[size_t(k)] = m.cornerNext(c[size_t(k - 1)]);
        q.corners.push_back(c);
        q.faces.push_back(f);
    }
    if (q.faces.empty())
        throw CommandError(command + ": the mesh has no faces");
    return q;
}

/**
 * A triangle face's texel (x, y) of an n x n grid as the barycentric (u, v) of its sub-triangle's centre, per the Ptex
 * triangle layout: texels with x + y < n are the upright sub-triangles at (x, y), the rest the inverted ones at
 * (n - 1 - y, n - 1 - x).
 */
glm::vec2 triangleTexelCentre(int x, int y, int n)
{
    if (x + y < n)
        return {(float(x) + 1.0f / 3.0f) / float(n), (float(y) + 1.0f / 3.0f) / float(n)};
    return {(float(n - 1 - y) + 2.0f / 3.0f) / float(n), (float(n - 1 - x) + 2.0f / 3.0f) / float(n)};
}

const geo::Mesh& meshArg(const Args& a, const std::string& command, scene::MeshPtr& hold)
{
    const std::string path = a.get<std::string>("mesh", "");
    if (path.empty())
        throw CommandError(command + ": give mesh=");
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(command + ": no mesh " + path);
    hold = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!hold || hold->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).empty())
        throw CommandError(command + ": " + path + " has no uvs");
    return *hold;
}

scene::Node& ptexSet(const Args& a, const std::string& command)
{
    const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
    scene::Node* set = findSet(scene::Graph::current(), name);
    if (!set)
        throw CommandError(command + ": no texture set " + name);
    return *set;
}

glm::vec2 bilinear(const std::array<glm::vec2, 4>& q, float s, float t)
{
    return (1.0f - s) * (1.0f - t) * q[0] + s * (1.0f - t) * q[1] + s * t * q[2] + (1.0f - s) * t * q[3];
}

/** The image's colour at a uv, v up, bilinear and clamped. */
glm::vec4 sampleImage(const image::Image& img, glm::vec2 uv)
{
    const float x = uv.x * img.width - 0.5f, y = (1.0f - uv.y) * img.height - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    auto px = [&](int i, int j) {
        i = std::clamp(i, 0, img.width - 1);
        j = std::clamp(j, 0, img.height - 1);
        const float* p = &img.rgba[(size_t(j) * img.width + i) * 4];
        return glm::vec4(p[0], p[1], p[2], p[3]);
    };
    return glm::mix(glm::mix(px(x0, y0), px(x0 + 1, y0), fx), glm::mix(px(x0, y0 + 1), px(x0 + 1, y0 + 1), fx), fy);
}

json exportPtex(const Args& a)
{
    const char* cmd = "texExportPtex";
    scene::Node& set = ptexSet(a, cmd);
    scene::MeshPtr hold;
    const geo::Mesh& m = meshArg(a, cmd, hold);
    const PtexFaces q = ptexFaces(m, cmd);
    const SetSettings settings = readSettings(set);
    const std::string channel = a.get<std::string>("channel", "baseColor");
    const Channel* c = settings.channel(channel);
    if (!c)
        throw CommandError(std::string(cmd) + ": the set has no channel " + channel);
    const int resLog2 = int(a.get<int64_t>("faceResolution", 6));
    const std::string file = a.get<std::string>("file", "");
    if (file.empty())
        throw CommandError(std::string(cmd) + ": give file=");

    const auto images = evaluateOutputs(set, channelRequests(readGraph(set), settings), settings.tiles.front().udim, 0, 0);
    const auto it = images.find(channel);
    if (it == images.end())
        throw CommandError(std::string(cmd) + ": the set's graph has no " + channel + " output");
    const image::Image& img = it->second;

    const int nch = c->type == PortType::Gray ? 1 : 3;
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::vector<int> ptexId(m.faceCapacity(), -1);
    for (size_t i = 0; i < q.faces.size(); ++i)
        ptexId[q.faces[i]] = int(i);

    Ptex::String err;
    PtexWriter* w = PtexWriter::open(paths::toUtf8(bake::resolveFolder(file)).c_str(), q.meshType(), Ptex::dt_float, nch, -1, int(q.faces.size()), err, true);
    if (!w)
        throw CommandError(std::string(cmd) + ": " + err.c_str());
    const int n = 1 << resLog2;
    std::vector<float> data(size_t(n) * n * nch);
    for (size_t i = 0; i < q.faces.size(); ++i)
    {
        const auto& cs = q.corners[i];
        std::array<glm::vec2, 4> quv;
        int adjfaces[4] = {-1, -1, -1, -1}, adjedges[4] = {0, 0, 0, 0};
        for (int k = 0; k < q.sides; ++k)
        {
            quv[k] = uv[cs[k]];
            // Edge k runs from corner k to corner k + 1; its neighbour is the face across it.
            const geo::Index other = m.cornerRadial(cs[k]);
            if (other != cs[k] && other != geo::kInvalid && ptexId[m.cornerFace(other)] >= 0)
            {
                const int j = ptexId[m.cornerFace(other)];
                adjfaces[k] = j;
                adjedges[k] = int(std::find(q.corners[j].begin(), q.corners[j].end(), other) - q.corners[j].begin());
            }
        }
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                glm::vec2 at;
                if (q.sides == 3)
                {
                    const glm::vec2 b = triangleTexelCentre(x, y, n);
                    at = quv[0] * (1.0f - b.x - b.y) + quv[1] * b.x + quv[2] * b.y;
                }
                else
                    at = bilinear(quv, (x + 0.5f) / n, (y + 0.5f) / n);
                const glm::vec4 v = sampleImage(img, at);
                for (int ch = 0; ch < nch; ++ch)
                    data[(size_t(y) * n + x) * nch + ch] = v[ch];
            }
        w->writeFace(int(i), Ptex::FaceInfo(Ptex::Res(int8_t(resLog2), int8_t(resLog2)), adjfaces, adjedges), data.data());
    }
    const bool ok = w->close(err);
    w->release();
    if (!ok)
        throw CommandError(std::string(cmd) + ": " + err.c_str());
    return {{"file", paths::toUtf8(bake::resolveFolder(file))}, {"faces", q.faces.size()}, {"channels", nch}};
}

/** One Ptex face's texels as floats, its resolution and channel count. */
struct PtexFace
{
    int w = 0, h = 0, nch = 0;
    std::vector<float> texels;

    glm::vec4 sample(float s, float t) const
    {
        const float x = s * w - 0.5f, y = t * h - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        auto px = [&](int i, int j) {
            i = std::clamp(i, 0, w - 1);
            j = std::clamp(j, 0, h - 1);
            glm::vec4 v(0.0f, 0.0f, 0.0f, 1.0f);
            for (int ch = 0; ch < std::min(nch, 4); ++ch)
                v[ch] = texels[(size_t(j) * w + i) * nch + ch];
            if (nch == 1)
                v = glm::vec4(v.r, v.r, v.r, 1.0f);
            return v;
        };
        return glm::mix(glm::mix(px(x0, y0), px(x0 + 1, y0), fx), glm::mix(px(x0, y0 + 1), px(x0 + 1, y0 + 1), fx), fy);
    }
};

std::vector<PtexFace> readPtex(const std::string& file, size_t wantFaces, Ptex::MeshType wantType, const char* cmd)
{
    Ptex::String err;
    PtexTexture* t = PtexTexture::open(file.c_str(), err);
    if (!t)
        throw CommandError(std::string(cmd) + ": " + err.c_str());
    std::vector<PtexFace> out;
    try
    {
        if (t->meshType() != wantType)
            throw CommandError(std::string(cmd) + ": " + file + " is for " + (t->meshType() == Ptex::mt_quad ? "quads" : "triangles") +
                               ", the mesh has " + (wantType == Ptex::mt_quad ? "quads" : "triangles"));
        if (size_t(t->numFaces()) != wantFaces)
            throw CommandError(std::string(cmd) + ": " + file + " has " + std::to_string(t->numFaces()) + " faces, the mesh " + std::to_string(wantFaces));
        out.resize(wantFaces);
        for (int f = 0; f < t->numFaces(); ++f)
        {
            const Ptex::FaceInfo& fi = t->getFaceInfo(f);
            PtexFace& p = out[size_t(f)];
            p.w = fi.res.u();
            p.h = fi.res.v();
            p.nch = t->numChannels();
            p.texels.resize(size_t(p.w) * p.h * p.nch);
            for (int y = 0; y < p.h; ++y)
                for (int x = 0; x < p.w; ++x)
                    t->getPixel(f, x, y, &p.texels[(size_t(y) * p.w + x) * p.nch], 0, p.nch);
        }
    }
    catch (...)
    {
        t->release();
        throw;
    }
    t->release();
    return out;
}

/** Grows the covered texels outwards so bilinear filtering at uv borders doesn't pull in the empty background. */
void dilate(image::Image& img, int passes)
{
    for (int p = 0; p < passes; ++p)
    {
        const std::vector<float> src = img.rgba;
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x)
            {
                float* d = &img.rgba[(size_t(y) * img.width + x) * 4];
                if (d[3] > 0.0f)
                    continue;
                glm::vec4 sum(0.0f);
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int i = x + dx, j = y + dy;
                        if (i < 0 || j < 0 || i >= img.width || j >= img.height)
                            continue;
                        const float* s = &src[(size_t(j) * img.width + i) * 4];
                        if (s[3] <= 0.0f)
                            continue;
                        sum += glm::vec4(s[0], s[1], s[2], 1.0f);
                        ++count;
                    }
                if (count)
                {
                    d[0] = sum.r / count;
                    d[1] = sum.g / count;
                    d[2] = sum.b / count;
                    d[3] = 1.0f;
                }
            }
    }
}

/** Fills every texel inside the quad's uvs from the Ptex face, through the two triangles c0 c1 c2 and c0 c2 c3. */
void rasterQuad(image::Image& img, const std::array<glm::vec2, 4>& quv, const PtexFace& face)
{
    static const std::array<glm::vec2, 4> st = {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)};
    for (const std::array<int, 3>& tri : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}})
    {
        glm::vec2 p[3], q[3];
        for (int k = 0; k < 3; ++k)
        {
            p[k] = glm::vec2(quv[tri[k]].x * img.width, (1.0f - quv[tri[k]].y) * img.height);
            q[k] = st[tri[k]];
        }
        const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (std::abs(area) < 1e-12f)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x})))),
                  x1 = std::min(img.width - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
        const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y})))),
                  y1 = std::min(img.height - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const glm::vec2 c(x + 0.5f, y + 0.5f);
                const float w1 = ((c.x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (c.y - p[0].y)) / area;
                const float w2 = ((p[1].x - p[0].x) * (c.y - p[0].y) - (c.x - p[0].x) * (p[1].y - p[0].y)) / area;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f)
                    continue;
                const glm::vec2 s = w0 * q[0] + w1 * q[1] + w2 * q[2];
                const glm::vec4 v = face.sample(s.x, s.y);
                float* d = &img.rgba[(size_t(y) * img.width + x) * 4];
                d[0] = v.r;
                d[1] = v.g;
                d[2] = v.b;
                d[3] = 1.0f;
            }
    }
}

/** The Ptex triangle face's texel holding barycentric (u, v): the upright or inverted sub-triangle it falls in. */
glm::vec4 triangleTexel(const PtexFace& face, float u, float v)
{
    const int n = face.w;
    const float tu = std::clamp(u, 0.0f, 1.0f) * n, tv = std::clamp(v, 0.0f, 1.0f) * n;
    const int ui = std::clamp(int(tu), 0, n - 1), vi = std::clamp(int(tv), 0, n - 1);
    int x = ui, y = vi;
    if ((tu - ui) + (tv - vi) > 1.0f)
    {
        x = n - 1 - vi;
        y = n - 1 - ui;
    }
    glm::vec4 out(0.0f, 0.0f, 0.0f, 1.0f);
    for (int ch = 0; ch < std::min(face.nch, 4); ++ch)
        out[ch] = face.texels[(size_t(y) * face.w + x) * face.nch + ch];
    if (face.nch == 1)
        out = glm::vec4(out.r, out.r, out.r, 1.0f);
    return out;
}

/** Fills every texel inside the triangle's uvs from the Ptex triangle face. */
void rasterTriangle(image::Image& img, const std::array<glm::vec2, 4>& tuv, const PtexFace& face)
{
    glm::vec2 p[3];
    for (int k = 0; k < 3; ++k)
        p[k] = glm::vec2(tuv[k].x * img.width, (1.0f - tuv[k].y) * img.height);
    const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
    if (std::abs(area) < 1e-12f || face.w <= 0)
        return;
    const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x})))),
              x1 = std::min(img.width - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
    const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y})))),
              y1 = std::min(img.height - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const glm::vec2 c(x + 0.5f, y + 0.5f);
            const float w1 = ((c.x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (c.y - p[0].y)) / area;
            const float w2 = ((p[1].x - p[0].x) * (c.y - p[0].y) - (c.x - p[0].x) * (p[1].y - p[0].y)) / area;
            if (w1 < -1e-5f || w2 < -1e-5f || 1.0f - w1 - w2 < -1e-5f)
                continue;
            const glm::vec4 v = triangleTexel(face, w1, w2);
            float* d = &img.rgba[(size_t(y) * img.width + x) * 4];
            d[0] = v.r;
            d[1] = v.g;
            d[2] = v.b;
            d[3] = 1.0f;
        }
}

json importPtex(const Args& a)
{
    const char* cmd = "texImportPtex";
    scene::Node& set = ptexSet(a, cmd);
    scene::MeshPtr hold;
    const geo::Mesh& m = meshArg(a, cmd, hold);
    const PtexFaces q = ptexFaces(m, cmd);
    const SetSettings settings = readSettings(set);
    const std::string channel = a.get<std::string>("channel", "baseColor");
    if (!settings.channel(channel))
        throw CommandError(std::string(cmd) + ": the set has no channel " + channel);
    const std::string file = a.get<std::string>("file", "");
    if (file.empty())
        throw CommandError(std::string(cmd) + ": give file=");
    const std::filesystem::path in = bake::resolveFolder(file);
    const std::vector<PtexFace> faces = readPtex(paths::toUtf8(in), q.faces.size(), q.meshType(), cmd);

    const Tile& tile = settings.tiles.front();
    image::Image img;
    img.width = tile.width > 0 ? tile.width : settings.width;
    img.height = tile.height > 0 ? tile.height : settings.height;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0.0f);
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    for (size_t i = 0; i < q.faces.size(); ++i)
    {
        std::array<glm::vec2, 4> quv{};
        for (int k = 0; k < q.sides; ++k)
            quv[k] = uv[q.corners[i][k]];
        if (q.sides == 3)
            rasterTriangle(img, quv, faces[i]);
        else
            rasterQuad(img, quv, faces[i]);
    }
    dilate(img, 4);
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        img.rgba[i] = 1.0f;

    const std::filesystem::path folder = bake::resolveFolder(a.get<std::string>("folder", "textures/" + set.name()));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path out = folder / (set.name() + "_" + paths::toUtf8(in.stem()) + "_" + channel + ".exr");
    image::writeExr(out, img.width, img.height, img.rgba, 32);
    json add = {{"args", {set.name(), "fill"}}, {"name", a.get<std::string>("name", paths::toUtf8(in.stem()))},
                {"material", {{channel, paths::toUtf8(out)}}}};
    const std::string id = ops::execute("texAddLayer", add).at("id").get<std::string>();
    return {{"id", id}, {"image", paths::toUtf8(out)}, {"faces", q.faces.size()}};
}
} // namespace

void registerPtexCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texExportPtex",
        .doc = "Write one channel of a texture set to a Ptex file (.ptx): every face of the mesh gets its own texels, sampled from the set "
               "through the mesh's uvs. The mesh must be all quads or all triangles. Returns {file, faces, channels}.",
        .positionalDoc = "set",
        .flags = {{"mesh", "", T::String, nullptr, "The mesh the set paints (node path)."},
                  {"file", "", T::String, nullptr, "The .ptx to write; relative paths are under the workspace."},
                  {"channel", "", T::String, "baseColor", "The set's channel."},
                  {"faceResolution", "", T::Int, 6, "Texels per face side as a power of two: 6 is 64 x 64.", {}, {}, 0.0, 12.0}},
        .undoable = false,
        .run = exportPtex,
    });
    ops::registerCommand({
        .name = "texImportPtex",
        .doc = "Bring a Ptex file (.ptx) onto a texture set: its faces are baked through the mesh's uvs into an image, which becomes a new "
               "fill layer on the channel. The mesh must be the quad or triangle mesh the file was painted on. Returns {id, image, faces}.",
        .positionalDoc = "set",
        .flags = {{"mesh", "", T::String, nullptr, "The mesh the file was painted on (node path)."},
                  {"file", "", T::String, nullptr, "The .ptx to read."},
                  {"channel", "", T::String, "baseColor", "The set's channel the layer fills."},
                  {"name", "", T::String, nullptr, "Layer name; the file's name when left out."},
                  {"folder", "", T::String, nullptr, "Where the baked image goes; textures/<set> under the workspace by default."}},
        .undoable = false,
        .run = importPtex,
    });
}
} // namespace lila::tex
