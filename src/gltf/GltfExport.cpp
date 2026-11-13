#include "gltf/Gltf.h"
#include "scene/Georef.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "core/TextMatch.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"
#include "gltf/GltfDraco.h"
#include "gltf/GltfInternal.h"
#include "gltf/GltfMaterials.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace lila::gltf
{
namespace
{
using fbx::ExportNode;
using fbx::ExportScene;
using fbx::ExportTake;
using geo::Index;
using geo::kInvalid;
namespace anim = scene::anim;
namespace lights = scene::lights;

constexpr int kFloat = 5126, kUByte = 5121, kUShort = 5123, kUInt = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

/** The binary buffer and the views and accessors into it. */
struct Bin
{
    std::string data;
    json views = json::array();
    json accessors = json::array();

    int view(const void* p, size_t bytes, int target = 0, size_t stride = 0)
    {
        while (data.size() % 4)
            data.push_back('\0');
        json v = {{"buffer", 0}, {"byteOffset", data.size()}, {"byteLength", bytes}};
        if (target)
            v["target"] = target;
        if (stride)
            v["byteStride"] = stride;
        data.append(static_cast<const char*>(p), bytes);
        views.push_back(std::move(v));
        return int(views.size()) - 1;
    }

    int accessor(json a)
    {
        accessors.push_back(std::move(a));
        return int(accessors.size()) - 1;
    }

    static const char* typeName(size_t comps)
    {
        switch (comps)
        {
        case 1: return "SCALAR";
        case 2: return "VEC2";
        case 3: return "VEC3";
        case 4: return "VEC4";
        case 16: return "MAT4";
        default: return "SCALAR";
        }
    }

    static std::pair<json, json> bounds(const std::vector<float>& v, size_t comps)
    {
        json lo = json::array(), hi = json::array();
        for (size_t c = 0; c < comps; ++c)
        {
            float a = std::numeric_limits<float>::max(), b = std::numeric_limits<float>::lowest();
            for (size_t i = c; i < v.size(); i += comps)
            {
                a = std::min(a, v[i]);
                b = std::max(b, v[i]);
            }
            if (v.empty())
                a = b = 0.0f;
            lo.push_back(a);
            hi.push_back(b);
        }
        return {lo, hi};
    }

    int floats(const std::vector<float>& v, size_t comps, int target, bool minMax)
    {
        json a = {{"bufferView", view(v.data(), v.size() * sizeof(float), target)},
                  {"componentType", kFloat},
                  {"count", v.size() / comps},
                  {"type", typeName(comps)}};
        if (minMax)
            std::tie(a["min"], a["max"]) = bounds(v, comps);
        return accessor(std::move(a));
    }

    /** A float accessor that stores only its nonzero elements (sparse over zeros). */
    int sparseFloats(const std::vector<float>& v, size_t comps, bool minMax)
    {
        std::vector<uint32_t> idx;
        std::vector<float> vals;
        const size_t count = v.size() / comps;
        for (size_t i = 0; i < count; ++i)
        {
            bool zero = true;
            for (size_t c = 0; c < comps && zero; ++c)
                zero = v[i * comps + c] == 0.0f;
            if (zero)
                continue;
            idx.push_back(uint32_t(i));
            vals.insert(vals.end(), v.begin() + std::ptrdiff_t(i * comps), v.begin() + std::ptrdiff_t((i + 1) * comps));
        }
        if (idx.size() * 3 >= count)
            return floats(v, comps, kArrayBuffer, minMax);
        json a = {{"componentType", kFloat}, {"count", count}, {"type", typeName(comps)}};
        if (minMax)
            std::tie(a["min"], a["max"]) = bounds(v, comps);
        if (!idx.empty())
            a["sparse"] = {{"count", idx.size()},
                           {"indices", {{"bufferView", view(idx.data(), idx.size() * sizeof(uint32_t))}, {"componentType", kUInt}}},
                           {"values", {{"bufferView", view(vals.data(), vals.size() * sizeof(float))}}}};
        return accessor(std::move(a));
    }

    template <class T> int ints(const std::vector<T>& v, size_t comps, int componentType, int target)
    {
        return accessor({{"bufferView", view(v.data(), v.size() * sizeof(T), target)},
                         {"componentType", componentType},
                         {"count", v.size() / comps},
                         {"type", typeName(comps)}});
    }
};

std::string uriEncode(std::string_view s)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            out.push_back(char(c));
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}


/** The images and textures of an export, embedded or written next to it. */
class Textures final : public TextureSink
{
public:
    Textures(Bin& bin, const ExportOptions& opts, const fs::path& outDir, std::vector<std::string>& warnings)
        : mBin(bin), mOpts(opts), mDir(outDir), mWarnings(warnings)
    {
    }

    json images = json::array();
    json textures = json::array();
    /** Index 0 is the default (trilinear, repeat), which most textures share. */
    json samplers = json::array({samplerJson({})});

    int file(const std::string& path, const scene::material::TextureSampling& s) override { return texture(fileImage(path), s); }

    int made(const std::string& key, const std::string& name, const std::function<bool(Pixels&)>& make,
             const scene::material::TextureSampling& s) override
    {
        return texture(madeImage(key, name, make), s);
    }

private:
    /** The image index of a source file, added on first use; -1 when it can't be written. */
    int fileImage(const std::string& path)
    {
        const std::string key = "file|" + path;
        if (auto it = mByKey.find(key); it != mByKey.end())
            return it->second;
        const fs::path src = utf8Path(path);
        std::error_code ec;
        if (!fs::exists(src, ec))
        {
            mWarnings.push_back(std::format("texture {} not found; left out", path));
            return mByKey[key] = -1;
        }
        const std::string mime = gltfMimeForPath(src);
        if (mime.empty())
        {
            // glTF carries PNG and JPEG only.
            Pixels p;
            std::string err;
            if (!loadImage(src, p, &err))
            {
                mWarnings.push_back(std::format("texture {} can't be converted to PNG: {}", path, err));
                return mByKey[key] = -1;
            }
            return mByKey[key] = addBytes(utf8(src.stem()), "image/png", encodePng(p));
        }
        if (mOpts.embedImages)
        {
            std::string bytes;
            if (!readFile(src, bytes))
            {
                mWarnings.push_back(std::format("can't read texture {}", path));
                return mByKey[key] = -1;
            }
            return mByKey[key] = addBytes(utf8(src.stem()), mime, std::move(bytes));
        }
        std::string uri;
        if (!mOpts.copyTextures)
        {
            const fs::path rel = paths::relativeTo(src, mDir);
            if (!rel.empty())
                uri = uriEncode(utf8(rel));
        }
        if (uri.empty())
        {
            const std::string name = uniqueName(utf8(src.filename()), key);
            const fs::path dest = mDir / utf8Path(name);
            if (!fs::exists(dest, ec) || !fs::equivalent(src, dest, ec))
            {
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    mWarnings.push_back(std::format("can't copy texture {} to {}: {}", path, utf8(dest), ec.message()));
                    return mByKey[key] = -1;
                }
            }
            uri = uriEncode(name);
        }
        images.push_back({{"name", utf8(src.stem())}, {"uri", uri}});
        return mByKey[key] = int(images.size()) - 1;
    }

    int madeImage(const std::string& key, const std::string& name, const std::function<bool(Pixels&)>& make)
    {
        if (auto it = mByKey.find(key); it != mByKey.end())
            return it->second;
        Pixels p;
        if (!make(p) || p.empty())
            return mByKey[key] = -1;
        std::string png = encodePng(p);
        if (png.empty())
            return mByKey[key] = -1;
        return mByKey[key] = addBytes(name, "image/png", std::move(png));
    }

    static json samplerJson(const scene::material::TextureSampling& s)
    {
        using scene::material::TextureWrap;
        auto wrap = [](TextureWrap w) { return w == TextureWrap::Mirror ? 33648 : w == TextureWrap::Repeat ? 10497 : 33071; };
        const bool nearest = s.filter == scene::material::TextureFilter::Nearest;
        return {{"magFilter", nearest ? 9728 : 9729}, {"minFilter", nearest ? 9986 : 9987}, {"wrapS", wrap(s.wrapU)}, {"wrapT", wrap(s.wrapV)}};
    }

    /** The texture of an image with a sampler, shared by every map asking for the same pair; -1 for no image. */
    int texture(int image, const scene::material::TextureSampling& s)
    {
        if (image < 0)
            return -1;
        if ((s.wrapU == scene::material::TextureWrap::Black || s.wrapV == scene::material::TextureWrap::Black) && !mWarnedBlack)
        {
            mWarnedBlack = true;
            mWarnings.push_back("glTF has no black texture border; those textures clamp to their edge");
        }
        const json sj = samplerJson(s);
        int sampler = -1;
        for (size_t k = 0; k < samplers.size() && sampler < 0; ++k)
            if (samplers[k] == sj)
                sampler = int(k);
        if (sampler < 0)
        {
            samplers.push_back(sj);
            sampler = int(samplers.size()) - 1;
        }
        auto [it, added] = mTextures.try_emplace({image, sampler}, int(textures.size()));
        if (added)
            textures.push_back({{"source", image}, {"sampler", sampler}});
        return it->second;
    }

    std::string uniqueName(const std::string& wanted, const std::string& key)
    {
        const fs::path w = utf8Path(wanted);
        std::string name = wanted;
        for (int k = 1; mNames.count(textMatch::lower(name)) && mNames[textMatch::lower(name)] != key; ++k)
            name = utf8(w.stem()) + "_" + std::to_string(k) + utf8(w.extension());
        mNames[textMatch::lower(name)] = key;
        return name;
    }

    int addBytes(const std::string& stem, const std::string& mime, std::string bytes)
    {
        json img = {{"name", stem}};
        if (mOpts.embedImages && mOpts.binary)
        {
            img["bufferView"] = mBin.view(bytes.data(), bytes.size());
            img["mimeType"] = mime;
        }
        else if (mOpts.embedImages)
            img["uri"] = dataUri(mime, bytes);
        else
        {
            const std::string name = uniqueName(stem + extensionForMime(mime), stem + "|" + mime + "|" + std::to_string(bytes.size()));
            const fs::path dest = mDir / utf8Path(name);
            if (!writeFile(dest, bytes))
            {
                mWarnings.push_back(std::format("can't write {}", utf8(dest)));
                return -1;
            }
            img["uri"] = uriEncode(name);
        }
        images.push_back(std::move(img));
        return int(images.size()) - 1;
    }

    Bin& mBin;
    const ExportOptions& mOpts;
    fs::path mDir;
    std::vector<std::string>& mWarnings;
    std::map<std::string, int> mByKey; // image index per source
    std::map<std::pair<int, int>, int> mTextures; // (image, sampler) -> texture
    std::map<std::string, std::string> mNames; // lowercase file name -> key that took it
    bool mWarnedBlack = false;
};

struct FileTrs
{
    glm::dvec3 t{0.0}, s{1.0};
    glm::dquat q{1.0, 0.0, 0.0, 0.0};
    bool sheared = false;
};

FileTrs fileTrs(const glm::dmat4& m)
{
    FileTrs out;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, out.q, out.t, skew, persp))
        return out;
    out.q = glm::normalize(out.q);
    out.sheared = glm::length(skew) > 1e-6;
    return out;
}

json vec(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

double tidy(double v) { return std::abs(v) < 1e-12 ? 0.0 : v; }

/** Samples kept so that linear (or slerp) interpolation between them reproduces every sample within the tolerance. */
std::vector<size_t> reduceKeys(size_t n, const std::function<bool(size_t a, size_t b, size_t i)>& fits)
{
    std::vector<size_t> kept;
    if (n == 0)
        return kept;
    kept.push_back(0);
    for (size_t i = 1; i + 1 < n; ++i)
    {
        const size_t a = kept.back();
        bool ok = true;
        for (size_t j = a + 1; j <= i && ok; ++j)
            ok = fits(a, i + 1, j);
        if (!ok)
            kept.push_back(i);
    }
    if (n > 1)
        kept.push_back(n - 1);
    return kept;
}

/** One animation channel before it is written. */
/** One vertex attribute of a mesh: floats, or the unsigned bytes or shorts of joint indices. */
struct Stream
{
    std::string semantic;
    DracoAttribute::Kind kind = DracoAttribute::Generic;
    int comps = 3;
    int componentType = 5126;
    std::vector<float> f;
    std::vector<uint8_t> u8;
    std::vector<uint16_t> u16;
};

using TargetsFn = std::function<json(const std::vector<uint32_t>*)>;

struct Channel
{
    int node = -1;
    const char* path = "translation";
    const char* interp = "LINEAR";
    std::vector<float> times;
    std::vector<float> values;
    size_t comps = 3;
};

struct Writer
{
    const ExportSnapshot& snap;
    const ExportScene& scene;
    const ExportOptions& opts;
    std::vector<std::string>& warnings;
    fs::path outDir;
    Bin bin;
    json doc = json::object();
    std::set<std::string> extensionsUsed;
    std::set<std::string> extensionsRequired;
    glm::dmat3 B{1.0};       // Lila axes to file axes
    glm::dmat4 K{1.0}, Kinv; // B with the unit scale
    std::map<std::string, int> materialIndex;
    std::vector<int> nodeCamera; // per node: index into cameras, -1

    /** The file-space local matrix of node i for a Lila-space local. */
    glm::dmat4 fileLocal(size_t i, const glm::dmat4& local) const
    {
        const ExportNode& n = scene.nodes[i];
        glm::dmat4 m = K * local * Kinv;
        // A camera's or light's frame is turned so it looks down file -Z wherever the scene's up axis puts Lila's -Z.
        auto turned = [](const ExportNode& e) { return e.camera || e.light; };
        if (turned(n))
            m = m * glm::dmat4(B);
        if (n.parent >= 0 && turned(scene.nodes[size_t(n.parent)]))
            m = glm::transpose(glm::dmat4(B)) * m;
        return m;
    }

    int writeMesh(size_t i, const std::vector<int>& jointSlots);
    json dracoPrimitive(const std::vector<Stream>& streams, const std::vector<uint32_t>& idx, const TargetsFn* targets, const std::string& name);
    json cameraJson(const fbx::ExportCamera& c) const;
    std::optional<json> lightJson(const ExportNode& n);
    void writeTake(const ExportTake& take, json& animations);
};

/**
 * A KHR_lights_punctual light: candela for point and spot, lux for directional, converted from lumens or watts when
 * the light uses them. Area, disc and dome lights have no punctual form, so they are left out with a warning.
 */
std::optional<json> Writer::lightJson(const ExportNode& n)
{
    using lights::Kind;
    const lights::Settings& l = *n.light;
    if (l.kind == Kind::Area || l.kind == Kind::Disc || l.kind == Kind::Dome)
    {
        warnings.push_back(std::format("{}: {} light not written (glTF only has point, spot and directional lights)", n.name, lights::kindName(l.kind)));
        return std::nullopt;
    }
    double intensity = l.intensity;
    if (l.units == lights::Units::Watts)
        intensity *= 683.0;
    if ((l.units == lights::Units::Lumens || l.units == lights::Units::Watts) && l.kind != Kind::Directional)
        intensity /= 4.0 * glm::pi<double>();
    const glm::dvec3 c = lights::effectiveColor(l);
    json j = {{"name", n.name},
              {"type", l.kind == Kind::Directional ? "directional" : l.kind == Kind::Spot ? "spot" : "point"},
              {"color", {c.x, c.y, c.z}},
              {"intensity", intensity}};
    if (l.range > 0.0 && l.kind != Kind::Directional)
        j["range"] = l.range / opts.gather.unitScale;
    if (l.kind == Kind::Spot)
        j["spot"] = {{"innerConeAngle", glm::radians(std::min(l.innerConeAngle, l.outerConeAngle))},
                     {"outerConeAngle", glm::radians(std::clamp(l.outerConeAngle, 0.0, 90.0))}};
    extensionsUsed.insert("KHR_lights_punctual");
    return j;
}

json Writer::cameraJson(const fbx::ExportCamera& c) const
{
    const double k = 1.0 / opts.gather.unitScale;
    if (c.ortho)
    {
        const double xmag = std::max(c.orthoWidth * 0.5 * k, 1e-9);
        const double ymag = xmag * (c.filmWidth > 0.0 ? c.filmHeight / c.filmWidth : 1.0);
        return {{"type", "orthographic"},
                {"orthographic", {{"xmag", xmag}, {"ymag", ymag}, {"znear", std::max(c.nearClip * k, 0.0)}, {"zfar", std::max(c.farClip * k, c.nearClip * k + 1e-6)}}}};
    }
    const double fl = std::max(c.focalLength, 1e-6);
    json p = {{"yfov", 2.0 * std::atan(c.filmHeight * 25.4 * 0.5 / fl)}, {"znear", std::max(c.nearClip * k, 1e-9)}};
    if (c.filmHeight > 0.0)
        p["aspectRatio"] = c.filmWidth / c.filmHeight;
    if (c.farClip > c.nearClip)
        p["zfar"] = c.farClip * k;
    return {{"type", "perspective"}, {"perspective", p}};
}

/**
 * A KHR_draco_mesh_compression primitive of the vertices idx uses. Morph targets stay uncompressed accessors, as the
 * extension requires, so such meshes are encoded keeping their point order. Empty when encoding fails.
 */
json Writer::dracoPrimitive(const std::vector<Stream>& streams, const std::vector<uint32_t>& idx, const TargetsFn* targets, const std::string& name)
{
    LILA_ZONE();

    std::vector<uint32_t> local, tris;
    std::unordered_map<uint32_t, uint32_t> localOf;
    tris.reserve(idx.size());
    for (uint32_t v : idx)
    {
        auto [it, added] = localOf.emplace(v, uint32_t(local.size()));
        if (added)
            local.push_back(v);
        tris.push_back(it->second);
    }
    std::vector<std::vector<uint8_t>> data(streams.size());
    std::vector<DracoAttribute> attrs;
    for (size_t s = 0; s < streams.size(); ++s)
    {
        const Stream& st = streams[s];
        const size_t size = st.componentType == kFloat ? 4 : st.componentType == kUShort ? 2 : 1;
        const auto* src = st.componentType == kFloat ? reinterpret_cast<const uint8_t*>(st.f.data())
                          : st.componentType == kUShort ? reinterpret_cast<const uint8_t*>(st.u16.data())
                                                         : st.u8.data();
        const size_t stride = size * size_t(st.comps);
        data[s].resize(local.size() * stride);
        for (size_t v = 0; v < local.size(); ++v)
            std::memcpy(&data[s][v * stride], src + size_t(local[v]) * stride, stride);
        attrs.push_back({st.semantic, st.kind, st.comps, st.componentType, data[s].data()});
    }
    DracoSettings settings;
    settings.positionBits = opts.dracoPositionBits;
    settings.normalBits = opts.dracoNormalBits;
    settings.uvBits = opts.dracoUvBits;
    settings.colorBits = opts.dracoColorBits;
    settings.genericBits = opts.dracoGenericBits;
    settings.level = opts.dracoLevel;
    DracoEncoded enc;
    std::string err;
    if (!encodeDraco(attrs, local.size(), tris, settings, targets != nullptr, enc, &err))
    {
        warnings.push_back(std::format("{}: Draco compression failed ({}); written uncompressed", name, err));
        return json::object();
    }
    json attributes = json::object(), ids = json::object();
    for (const Stream& st : streams)
    {
        json a = {{"componentType", st.componentType}, {"count", enc.points}, {"type", Bin::typeName(size_t(st.comps))}};
        if (st.semantic == "POSITION")
        {
            a["min"] = {enc.minPos[0], enc.minPos[1], enc.minPos[2]};
            a["max"] = {enc.maxPos[0], enc.maxPos[1], enc.maxPos[2]};
        }
        attributes[st.semantic] = bin.accessor(std::move(a));
        ids[st.semantic] = enc.ids.at(st.semantic);
    }
    const int indices = bin.accessor({{"componentType", enc.points <= 65535 ? kUShort : kUInt}, {"count", enc.indices}, {"type", "SCALAR"}});
    json out = {{"attributes", attributes}, {"indices", indices}};
    out["extensions"]["KHR_draco_mesh_compression"] = {{"bufferView", bin.view(enc.bytes.data(), enc.bytes.size())}, {"attributes", ids}};
    if (targets)
        out["targets"] = (*targets)(&local);
    extensionsUsed.insert("KHR_draco_mesh_compression");
    extensionsRequired.insert("KHR_draco_mesh_compression");
    return out;
}

/** jointSlots: the skin's joint order index of each cluster (cluster i uses slot jointSlots[i]). */
int Writer::writeMesh(size_t ni, const std::vector<int>& jointSlots)
{
    LILA_ZONE();

    const ExportNode& e = scene.nodes[ni];
    const geo::Mesh& m = *e.mesh;
    const glm::dmat4 toFile = K * e.bake;
    const glm::dmat3 lin(toFile);
    const glm::dmat3 nm = glm::inverse(glm::transpose(lin));
    const bool mirrored = glm::determinant(lin) < 0.0;

    std::vector<int32_t> pointIndex(m.pointCapacity(), -1);
    int32_t points = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            pointIndex[p] = points++;

    std::vector<glm::vec3> computed;
    std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (normals.size() != m.cornerCapacity())
    {
        computed = geo::cornerNormals(m);
        normals = computed;
    }
    const geo::Triangulation tris = geo::triangulate(m);
    std::vector<glm::vec4> tangents;
    if (opts.gather.tangents && m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).size() == m.cornerCapacity())
        tangents = geo::cornerTangents(m, tris, normals);
    std::vector<std::span<const glm::vec2>> uvs;
    for (int s = 0; s < 8; ++s)
    {
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(s));
        if (uv.size() != m.cornerCapacity())
            break;
        uvs.push_back(uv);
    }
    std::vector<std::span<const glm::vec4>> colors;
    for (const std::string& set : geo::colors::sets(m))
        if (std::span<const glm::vec4> c = geo::colors::values(m, set); c.size() == m.cornerCapacity())
            colors.push_back(c);

    // Vertices: one per distinct (point, corner attributes).
    std::vector<float> pos, nrm, tan;
    std::vector<std::vector<float>> uvOut(uvs.size()), colOut(colors.size());
    std::vector<int32_t> vertexPoint;
    std::vector<Index> vertexCorner;
    std::unordered_map<std::string, uint32_t> vertexOf;
    std::string key;
    auto vertexFor = [&](Index c) {
        const int32_t p = pointIndex[m.cornerPoint(c)];
        key.assign(reinterpret_cast<const char*>(&p), sizeof(p));
        key.append(reinterpret_cast<const char*>(&normals[c]), sizeof(glm::vec3));
        if (!tangents.empty())
            key.append(reinterpret_cast<const char*>(&tangents[c]), sizeof(glm::vec4));
        for (const auto& uv : uvs)
            key.append(reinterpret_cast<const char*>(&uv[c]), sizeof(glm::vec2));
        for (const auto& col : colors)
            key.append(reinterpret_cast<const char*>(&col[c]), sizeof(glm::vec4));
        auto [it, added] = vertexOf.emplace(key, uint32_t(vertexPoint.size()));
        if (!added)
            return it->second;
        vertexPoint.push_back(p);
        vertexCorner.push_back(c);
        const glm::dvec3 v = glm::dvec3(toFile * glm::dvec4(m.position(m.cornerPoint(c)), 1.0));
        pos.insert(pos.end(), {float(v.x), float(v.y), float(v.z)});
        glm::dvec3 n = nm * glm::dvec3(normals[c]);
        n = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
        nrm.insert(nrm.end(), {float(n.x), float(n.y), float(n.z)});
        if (!tangents.empty())
        {
            const glm::vec4& t4 = tangents[c];
            glm::dvec3 t = lin * glm::dvec3(t4);
            t = glm::length(t) > 0.0 ? glm::normalize(t) : glm::dvec3(1, 0, 0);
            const float w = (t4.w < 0.0f ? -1.0f : 1.0f) * (mirrored ? -1.0f : 1.0f);
            tan.insert(tan.end(), {float(t.x), float(t.y), float(t.z), w});
        }
        for (size_t s = 0; s < uvs.size(); ++s)
            uvOut[s].insert(uvOut[s].end(), {uvs[s][c].x, 1.0f - uvs[s][c].y});
        for (size_t s = 0; s < colors.size(); ++s)
            colOut[s].insert(colOut[s].end(), {colors[s][c].r, colors[s][c].g, colors[s][c].b, colors[s][c].a});
        return it->second;
    };

    std::span<const int32_t> faceMat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    const bool slotted = !m.materialNames().empty();
    std::map<int32_t, std::vector<uint32_t>> indices;
    for (Index t = 0; t < tris.triangleCount(); ++t)
    {
        const int32_t slot = faceMat.size() == m.faceCapacity() ? faceMat[tris.faces[t]] : 0;
        std::vector<uint32_t>& out = indices[slotted ? slot : 0];
        const Index* c = &tris.corners[size_t(t) * 3];
        if (mirrored)
            out.insert(out.end(), {vertexFor(c[0]), vertexFor(c[2]), vertexFor(c[1])});
        else
            out.insert(out.end(), {vertexFor(c[0]), vertexFor(c[1]), vertexFor(c[2])});
    }
    if (vertexPoint.empty())
        return -1;
    const size_t count = vertexPoint.size();

    std::vector<Stream> streams;
    streams.push_back({"POSITION", DracoAttribute::Position, 3, kFloat, std::move(pos)});
    if (opts.gather.normals)
        streams.push_back({"NORMAL", DracoAttribute::Normal, 3, kFloat, std::move(nrm)});
    if (!tan.empty())
        streams.push_back({"TANGENT", DracoAttribute::Generic, 4, kFloat, std::move(tan)});
    for (size_t s = 0; s < uvOut.size(); ++s)
        streams.push_back({"TEXCOORD_" + std::to_string(s), DracoAttribute::TexCoord, 2, kFloat, std::move(uvOut[s])});
    for (size_t s = 0; s < colOut.size(); ++s)
        streams.push_back({"COLOR_" + std::to_string(s), DracoAttribute::Color, 4, kFloat, std::move(colOut[s])});

    if (e.skin && !jointSlots.empty())
    {
        std::vector<std::vector<std::pair<int, float>>> per(static_cast<size_t>(points));
        for (size_t ci = 0; ci < e.skin->clusters.size(); ++ci)
        {
            const fbx::ExportSkin::Cluster& cl = e.skin->clusters[ci];
            for (size_t k = 0; k < cl.vertices.size(); ++k)
                if (cl.vertices[k] >= 0 && cl.vertices[k] < points && cl.weights[k] > 0.0)
                    per[size_t(cl.vertices[k])].push_back({jointSlots[ci], float(cl.weights[k])});
        }
        size_t most = 1;
        for (auto& list : per)
        {
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            most = std::max(most, list.size());
        }
        const size_t sets = (most + 3) / 4;
        const bool wide = jointSlots.size() > 256;
        for (size_t s = 0; s < sets; ++s)
        {
            Stream joints{"JOINTS_" + std::to_string(s), DracoAttribute::Generic, 4, wide ? kUShort : kUByte};
            Stream w{"WEIGHTS_" + std::to_string(s), DracoAttribute::Generic, 4, kFloat};
            w.f.assign(count * 4, 0.0f);
            (wide ? joints.u16.resize(count * 4, 0) : joints.u8.resize(count * 4, 0));
            for (size_t v = 0; v < count; ++v)
            {
                const auto& list = per[size_t(vertexPoint[v])];
                float total = 0.0f;
                for (const auto& [j, wt] : list)
                    total += wt;
                for (size_t k = 0; k < 4; ++k)
                {
                    const size_t at = s * 4 + k;
                    if (at >= list.size())
                        continue;
                    if (wide)
                        joints.u16[v * 4 + k] = uint16_t(list[at].first);
                    else
                        joints.u8[v * 4 + k] = uint8_t(list[at].first);
                    w.f[v * 4 + k] = total > 0.0f ? list[at].second / total : 0.0f;
                }
            }
            streams.push_back(std::move(joints));
            streams.push_back(std::move(w));
        }
    }

    // Morph target deltas per vertex: POSITION, and NORMAL when written.
    std::vector<std::pair<std::vector<float>, std::vector<float>>> deltas;
    json targetNames = json::array();
    json weights = json::array();
    const bool morphNormals = opts.morphNormals && opts.gather.normals;
    if (e.morphs && !e.morphs->targets.empty())
    {
        std::vector<glm::vec3> baseNormals;
        if (morphNormals)
            baseNormals = geo::cornerNormals(m);
        std::vector<Index> pointOfCompact(size_t(points), kInvalid);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (pointIndex[p] >= 0)
                pointOfCompact[size_t(pointIndex[p])] = p;
        for (const fbx::ExportMorphs::Target& t : e.morphs->targets)
        {
            std::vector<glm::dvec3> delta(size_t(points), glm::dvec3(0.0));
            for (size_t k = 0; k < t.vertices.size(); ++k)
                if (t.vertices[k] >= 0 && t.vertices[k] < points)
                    delta[size_t(t.vertices[k])] = t.deltas[k];
            std::vector<float> d(count * 3, 0.0f), dn;
            for (size_t v = 0; v < count; ++v)
            {
                const glm::dvec3 f = lin * delta[size_t(vertexPoint[v])];
                d[v * 3] = float(f.x);
                d[v * 3 + 1] = float(f.y);
                d[v * 3 + 2] = float(f.z);
            }
            if (morphNormals)
            {
                geo::Mesh shaped = m;
                for (size_t p = 0; p < delta.size(); ++p)
                    if (delta[p] != glm::dvec3(0.0))
                        shaped.setPosition(pointOfCompact[p], shaped.position(pointOfCompact[p]) + delta[p]);
                const std::vector<glm::vec3> after = geo::cornerNormals(shaped);
                dn.assign(count * 3, 0.0f);
                for (size_t v = 0; v < count; ++v)
                {
                    const Index c = vertexCorner[v];
                    glm::dvec3 a = nm * glm::dvec3(baseNormals[c]), b = nm * glm::dvec3(after[c]);
                    if (glm::length(a) == 0.0 || glm::length(b) == 0.0)
                        continue;
                    const glm::dvec3 diff = glm::normalize(b) - glm::normalize(a);
                    if (glm::length(diff) < 1e-6)
                        continue;
                    dn[v * 3] = float(diff.x);
                    dn[v * 3 + 1] = float(diff.y);
                    dn[v * 3 + 2] = float(diff.z);
                }
            }
            deltas.push_back({std::move(d), std::move(dn)});
            targetNames.push_back(t.name);
            weights.push_back(t.weight);
        }
    }
    /** The targets of the vertices in subset (in that order), or of every vertex. */
    auto targetsJson = [&](const std::vector<uint32_t>* subset) {
        json out = json::array();
        auto pick = [&](const std::vector<float>& all) {
            if (!subset)
                return all;
            std::vector<float> part;
            part.reserve(subset->size() * 3);
            for (uint32_t v : *subset)
                part.insert(part.end(), all.begin() + std::ptrdiff_t(v) * 3, all.begin() + std::ptrdiff_t(v) * 3 + 3);
            return part;
        };
        for (const auto& [d, dn] : deltas)
        {
            json target = {{"POSITION", bin.sparseFloats(pick(d), 3, true)}};
            if (!dn.empty())
                target["NORMAL"] = bin.sparseFloats(pick(dn), 3, false);
            out.push_back(std::move(target));
        }
        return out;
    };

    const TargetsFn targetsFn = targetsJson;
    json prims = json::array();
    json shared, sharedTargets;
    for (const auto& [slot, idx] : indices)
    {
        json prim = {{"mode", 4}};
        if (opts.draco)
            prim.update(dracoPrimitive(streams, idx, deltas.empty() ? nullptr : &targetsFn, e.name));
        if (!prim.contains("attributes"))
        {
            if (shared.is_null())
            {
                shared = json::object();
                for (const Stream& st : streams)
                    shared[st.semantic] = st.componentType == kFloat ? bin.floats(st.f, size_t(st.comps), kArrayBuffer, st.semantic == "POSITION")
                                          : st.componentType == kUShort ? bin.ints(st.u16, 4, kUShort, kArrayBuffer)
                                                                         : bin.ints(st.u8, 4, kUByte, kArrayBuffer);
                if (!deltas.empty())
                    sharedTargets = targetsJson(nullptr);
            }
            prim["attributes"] = shared;
            if (count <= 65535)
            {
                std::vector<uint16_t> i16(idx.begin(), idx.end());
                prim["indices"] = bin.ints(i16, 1, kUShort, kElementArrayBuffer);
            }
            else
                prim["indices"] = bin.ints(idx, 1, kUInt, kElementArrayBuffer);
            if (!deltas.empty())
                prim["targets"] = sharedTargets;
        }
        if (slotted)
            if (auto it = materialIndex.find(scene::materials::slotName(m, slot)); it != materialIndex.end())
                prim["material"] = it->second;
        prims.push_back(std::move(prim));
    }
    json mesh = {{"name", e.name}, {"primitives", prims}};
    if (!deltas.empty())
    {
        mesh["weights"] = weights;
        mesh["extras"] = {{"targetNames", targetNames}};
    }
    doc["meshes"].push_back(std::move(mesh));
    return int(doc["meshes"].size()) - 1;
}

/** The node's translate (group 0) or scale (group 2) curves as one exact glTF track, when their keys line up and map one to one. */
std::optional<Channel> exactTrack(const std::array<anim::Curve, 9>& curves, int group, const glm::dmat3& map)
{
    const anim::Curve* c[3] = {&curves[size_t(group * 3)], &curves[size_t(group * 3 + 1)], &curves[size_t(group * 3 + 2)]};
    std::vector<double> times;
    for (const anim::Curve* cv : c)
        for (const anim::Key& k : cv->keys)
            if (std::none_of(times.begin(), times.end(), [&](double t) { return std::abs(t - k.time) < 1e-9; }))
                times.push_back(k.time);
    std::sort(times.begin(), times.end());
    if (times.size() < 2)
        return std::nullopt;
    bool linear = true, step = true, anyStep = false;
    for (const anim::Curve* cv : c)
    {
        if (cv->keys.size() <= 1)
            continue;
        if (cv->weighted || cv->keys.size() != times.size())
            return std::nullopt;
        for (size_t k = 0; k < times.size(); ++k)
        {
            if (std::abs(cv->keys[k].time - times[k]) > 1e-9 || !cv->keys[k].interpolator.empty())
                return std::nullopt;
            if (k + 1 == times.size())
                continue;
            const bool st = cv->keys[k].out == anim::Tangent::Step;
            const bool li = cv->keys[k].out == anim::Tangent::Linear && cv->keys[k + 1].in == anim::Tangent::Linear;
            anyStep = anyStep || st;
            step = step && st;
            linear = linear && li;
        }
    }
    if (anyStep && !step)
        return std::nullopt;
    Channel ch;
    ch.interp = step ? "STEP" : linear ? "LINEAR" : "CUBICSPLINE";
    const bool cubic = !step && !linear;
    for (size_t k = 0; k < times.size(); ++k)
    {
        ch.times.push_back(float(times[k]));
        glm::dvec3 v, in(0.0), out(0.0);
        for (int a = 0; a < 3; ++a)
        {
            v[a] = c[a]->evaluate(times[k]);
            if (cubic && c[a]->keys.size() > 1)
            {
                in[a] = c[a]->inSlope(k);
                out[a] = c[a]->outSlope(k);
            }
        }
        auto push = [&](const glm::dvec3& x) {
            const glm::dvec3 f = map * x;
            ch.values.insert(ch.values.end(), {float(f.x), float(f.y), float(f.z)});
        };
        if (cubic)
            push(in);
        push(v);
        if (cubic)
            push(out);
    }
    return ch;
}

void Writer::writeTake(const ExportTake& take, json& animations)
{
    LILA_ZONE();

    std::vector<Channel> channels;
    const glm::dmat3 K3(K);
    glm::dmat3 absB(0.0);
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k)
            absB[c][k] = std::abs(B[c][k]);
    const size_t frames = take.times.size();
    bool cameraAnim = false;

    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const ExportNode& n = scene.nodes[i];
        cameraAnim = cameraAnim || (i < take.cameras.size() && !take.cameras[i].empty());
        // Lila-space locals per frame, sampled or rebuilt from the node's own curves.
        std::vector<glm::dmat4> locals;
        const auto& curves = i < take.curves.size() ? take.curves[i] : std::nullopt;
        if (i < take.locals.size() && !take.locals[i].empty())
            locals = take.locals[i];
        else if (curves)
            for (double t : take.times)
            {
                glm::dvec3 tr, rot, sc;
                for (int a = 0; a < 3; ++a)
                {
                    tr[a] = (*curves)[size_t(a)].evaluate(t);
                    rot[a] = (*curves)[size_t(3 + a)].evaluate(t);
                    sc[a] = (*curves)[size_t(6 + a)].evaluate(t);
                }
                locals.push_back(glm::translate(glm::dmat4(1.0), tr) * n.pre * scene::eulerToMatrix(rot, n.rotateOrder) * n.post *
                                 glm::scale(glm::dmat4(1.0), sc));
            }
        if (locals.size() == frames && frames > 0)
        {
            const FileTrs rest = fileTrs(fileLocal(i, n.local));
            std::vector<FileTrs> f;
            f.reserve(frames);
            bool sheared = false;
            for (const glm::dmat4& l : locals)
            {
                f.push_back(fileTrs(fileLocal(i, l)));
                sheared = sheared || f.back().sheared;
                if (f.size() > 1 && glm::dot(f[f.size() - 2].q, f.back().q) < 0.0)
                    f.back().q = -f.back().q;
            }
            if (sheared)
                warnings.push_back(std::format("{}: shear in its animation is not written (glTF animates translation, rotation and scale)", n.name));
            auto changes = [&](auto&& get, double tol) {
                return std::any_of(f.begin(), f.end(), [&](const FileTrs& x) { return get(x, rest) > tol; });
            };
            auto vectorChannel = [&](const char* path, auto&& get, double tol) {
                Channel ch;
                ch.node = int(i);
                ch.path = path;
                const std::vector<size_t> kept = reduceKeys(frames, [&](size_t a, size_t b, size_t j) {
                    const double s = (take.times[j] - take.times[a]) / (take.times[b] - take.times[a]);
                    const glm::dvec3 lerp = glm::mix(get(f[a]), get(f[b]), s);
                    return glm::length(lerp - get(f[j])) <= tol * std::max(1.0, glm::length(get(f[j])));
                });
                for (size_t k : kept)
                {
                    const glm::dvec3 v = get(f[k]);
                    ch.times.push_back(float(take.times[k]));
                    ch.values.insert(ch.values.end(), {float(v.x), float(v.y), float(v.z)});
                }
                channels.push_back(std::move(ch));
            };
            const double lenTol = 1e-6 / opts.gather.unitScale;
            std::optional<Channel> exactT = curves ? exactTrack(*curves, 0, K3) : std::nullopt;
            std::optional<Channel> exactS = curves ? exactTrack(*curves, 2, absB) : std::nullopt;
            if (changes([](const FileTrs& x, const FileTrs& r) { return glm::length(x.t - r.t); }, lenTol))
            {
                if (exactT)
                {
                    exactT->node = int(i);
                    exactT->path = "translation";
                    channels.push_back(std::move(*exactT));
                }
                else
                    vectorChannel("translation", [](const FileTrs& x) { return x.t; }, 1e-6);
            }
            if (changes([](const FileTrs& x, const FileTrs& r) { return 1.0 - std::abs(glm::dot(x.q, r.q)); }, 1e-10))
            {
                Channel ch;
                ch.node = int(i);
                ch.path = "rotation";
                ch.comps = 4;
                const std::vector<size_t> kept = reduceKeys(frames, [&](size_t a, size_t b, size_t j) {
                    const double s = (take.times[j] - take.times[a]) / (take.times[b] - take.times[a]);
                    const glm::dquat q = glm::slerp(f[a].q, f[b].q, s);
                    return 1.0 - std::abs(glm::dot(q, f[j].q)) <= 1e-10;
                });
                for (size_t k : kept)
                {
                    ch.times.push_back(float(take.times[k]));
                    ch.values.insert(ch.values.end(), {float(f[k].q.x), float(f[k].q.y), float(f[k].q.z), float(f[k].q.w)});
                }
                channels.push_back(std::move(ch));
            }
            if (changes([](const FileTrs& x, const FileTrs& r) { return glm::length(x.s - r.s); }, 1e-7))
            {
                if (exactS)
                {
                    exactS->node = int(i);
                    exactS->path = "scale";
                    channels.push_back(std::move(*exactS));
                }
                else
                    vectorChannel("scale", [](const FileTrs& x) { return x.s; }, 1e-7);
            }
        }

        if (i < take.morphWeights.size() && !take.morphWeights[i].empty() && frames > 0)
        {
            const auto& w = take.morphWeights[i];
            Channel ch;
            ch.node = int(i);
            ch.path = "weights";
            ch.comps = w.size();
            const std::vector<size_t> kept = reduceKeys(frames, [&](size_t a, size_t b, size_t j) {
                const double s = (take.times[j] - take.times[a]) / (take.times[b] - take.times[a]);
                for (const auto& target : w)
                    if (std::abs(target[a] + (target[b] - target[a]) * s - target[j]) > 1e-6)
                        return false;
                return true;
            });
            for (size_t k : kept)
            {
                ch.times.push_back(float(take.times[k]));
                for (const auto& target : w)
                    ch.values.push_back(float(target[k]));
            }
            channels.push_back(std::move(ch));
        }
    }
    if (cameraAnim)
        warnings.push_back(std::format("take '{}': camera lens and clipping animation is not written (glTF can't animate camera properties)", take.name));
    if (!take.namedCurves.empty())
        warnings.push_back(std::format("take '{}': {} named curve(s) not written (glTF has no named animation curves)", take.name, take.namedCurves.size()));
    if (channels.empty())
        return;

    json samplers = json::array(), out = json::array();
    std::map<std::vector<float>, int> inputs;
    for (const Channel& ch : channels)
    {
        auto it = inputs.find(ch.times);
        if (it == inputs.end())
            it = inputs.emplace(ch.times, bin.floats(ch.times, 1, 0, true)).first;
        const int output = bin.floats(ch.values, ch.path == std::string_view("weights") ? 1 : ch.comps, 0, false);
        samplers.push_back({{"input", it->second}, {"output", output}, {"interpolation", ch.interp}});
        out.push_back({{"sampler", int(samplers.size()) - 1}, {"target", {{"node", ch.node}, {"path", ch.path}}}});
    }
    animations.push_back({{"name", take.name}, {"samplers", samplers}, {"channels", out}});
}
} // namespace

ExportSnapshot gatherExport(scene::Graph& graph, const std::vector<scene::NodeId>& roots, const ExportOptions& opts)
{
    LILA_ZONE();

    ExportSnapshot out;
    out.scene = fbx::gatherExport(graph, roots, opts.gather);
    std::set<std::string> seen;
    for (const ExportNode& n : out.scene.nodes)
    {
        if (!n.mesh || n.mesh->materialNames().empty())
            continue;
        for (const auto& [slot, faces] : scene::materials::slotUsage(*n.mesh))
            if (faces && seen.insert(slot).second)
                out.materials.push_back(snapshotMaterial(graph, slot));
    }
    return out;
}

bool writeGltf(const ExportSnapshot& snap, const fs::path& path, const ExportOptions& opts, std::string* error, std::vector<std::string>* warningsOut)
{
    LILA_ZONE();

    std::vector<std::string> localWarnings;
    std::vector<std::string>& warnings = warningsOut ? *warningsOut : localWarnings;
    const ExportScene& scene = snap.scene;
    Writer w{snap, scene, opts, warnings, path.parent_path()};
    w.B = lilaToFile(scene.upAxis);
    w.K = glm::scale(glm::dmat4(1.0), glm::dvec3(1.0 / opts.gather.unitScale)) * glm::dmat4(w.B);
    w.Kinv = glm::inverse(w.K);
    std::error_code ec;
    if (!w.outDir.empty())
        fs::create_directories(w.outDir, ec);

    json& doc = w.doc;
    doc["asset"] = {{"version", "2.0"}, {"generator", "Lila"}};
    // Geometry is relative to the scene origin; extras say where that is on the Earth.
    if (json geo = scene::georef::exportJson(); !geo.is_null())
        doc["asset"]["extras"] = {{"lilaGeoreference", std::move(geo)}};
    doc["meshes"] = json::array();

    Textures textures(w.bin, opts, w.outDir, warnings);
    json materials = json::array();
    for (const MaterialSnapshot& m : snap.materials)
    {
        w.materialIndex[m.name] = int(materials.size());
        materials.push_back(materialJson(m, textures, opts, w.extensionsUsed, warnings));
    }

    // Nodes, with their meshes, cameras and skins.
    json nodes = json::array(), cameras = json::array(), skins = json::array(), lightList = json::array();
    std::vector<std::vector<int>> children(scene.nodes.size());
    std::vector<int> rootNodes;
    std::map<const geo::Mesh*, int> sharedMesh;
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const ExportNode& n = scene.nodes[i];
        json node = {{"name", n.name}};
        for (const auto& [prop, value] : n.properties)
            node["extras"][prop] = value;
        const glm::dmat4 local = w.fileLocal(i, n.local);
        const FileTrs trs = fileTrs(local);
        if (trs.sheared)
        {
            json m = json::array();
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    m.push_back(tidy(local[c][r]));
            node["matrix"] = m;
        }
        else
        {
            if (glm::length(trs.t) > 1e-12)
                node["translation"] = {tidy(trs.t.x), tidy(trs.t.y), tidy(trs.t.z)};
            if (std::abs(trs.q.w) < 1.0 - 1e-15)
                node["rotation"] = {tidy(trs.q.x), tidy(trs.q.y), tidy(trs.q.z), tidy(trs.q.w)};
            if (glm::length(trs.s - glm::dvec3(1.0)) > 1e-12)
                node["scale"] = vec(trs.s);
        }
        if (n.camera)
        {
            json cam = w.cameraJson(*n.camera);
            cam["name"] = n.name;
            cameras.push_back(std::move(cam));
            node["camera"] = int(cameras.size()) - 1;
        }
        if (n.light)
            if (std::optional<json> light = w.lightJson(n))
            {
                lightList.push_back(std::move(*light));
                node["extensions"]["KHR_lights_punctual"] = {{"light", int(lightList.size()) - 1}};
            }
        if (n.mesh && n.mesh->numFaces() > 0)
        {
            std::vector<int> jointSlots;
            if (n.skin && !n.skin->clusters.empty())
            {
                json joints = json::array(), ibm = json::array();
                std::vector<float> mats;
                std::set<int> jointSet;
                for (size_t c = 0; c < n.skin->clusters.size(); ++c)
                {
                    const fbx::ExportSkin::Cluster& cl = n.skin->clusters[c];
                    jointSlots.push_back(int(joints.size()));
                    joints.push_back(cl.joint);
                    jointSet.insert(cl.joint);
                    const glm::dmat4 inv = glm::inverse(w.K * cl.bindWorld * w.Kinv) * (w.K * n.skin->meshBindWorld * w.Kinv);
                    for (int col = 0; col < 4; ++col)
                        for (int r = 0; r < 4; ++r)
                            mats.push_back(float(col == 3 && r == 3 ? 1.0 : (r == 3 ? 0.0 : inv[col][r])));
                }
                json skin = {{"joints", joints}, {"inverseBindMatrices", w.bin.floats(mats, 16, 0, false)}};
                std::set<int> tops;
                for (int j : jointSet)
                {
                    int top = j;
                    while (scene.nodes[size_t(top)].parent >= 0 && jointSet.count(scene.nodes[size_t(top)].parent))
                        top = scene.nodes[size_t(top)].parent;
                    tops.insert(top);
                }
                if (tops.size() == 1)
                    skin["skeleton"] = *tops.begin();
                skin["name"] = n.name + "_skin";
                skins.push_back(std::move(skin));
                node["skin"] = int(skins.size()) - 1;
                if (n.skin->dualQuaternion)
                    warnings.push_back(std::format("{}: dual quaternion skinning is written as linear (glTF skins linearly)", n.name));
            }
            const bool shareable = !n.skin && !n.morphs && n.bake == glm::dmat4(1.0);
            int mesh = -1;
            if (shareable)
                if (auto it = sharedMesh.find(n.mesh.get()); it != sharedMesh.end())
                    mesh = it->second;
            if (mesh < 0)
                mesh = w.writeMesh(i, jointSlots);
            if (shareable && mesh >= 0)
                sharedMesh[n.mesh.get()] = mesh;
            if (mesh >= 0)
            {
                node["mesh"] = mesh;
                if (n.morphs)
                {
                    json weights = json::array();
                    for (const auto& t : n.morphs->targets)
                        weights.push_back(t.weight);
                    node["weights"] = weights;
                }
            }
        }
        nodes.push_back(std::move(node));
        if (n.parent >= 0)
            children[size_t(n.parent)].push_back(int(i));
        else
            rootNodes.push_back(int(i));
    }
    for (size_t i = 0; i < nodes.size(); ++i)
        if (!children[i].empty())
            nodes[i]["children"] = children[i];

    json animations = json::array();
    if (scene.take)
        w.writeTake(*scene.take, animations);
    for (const ExportTake& t : scene.extraTakes)
        w.writeTake(t, animations);

    doc["scene"] = 0;
    doc["scenes"] = json::array({{{"nodes", rootNodes}}});
    doc["nodes"] = nodes;
    if (doc["meshes"].empty())
        doc.erase("meshes");
    if (!materials.empty())
        doc["materials"] = materials;
    if (!textures.textures.empty())
    {
        doc["textures"] = textures.textures;
        doc["images"] = textures.images;
        doc["samplers"] = textures.samplers;
    }
    if (!cameras.empty())
        doc["cameras"] = cameras;
    if (!lightList.empty())
        doc["extensions"]["KHR_lights_punctual"] = {{"lights", lightList}};
    if (!skins.empty())
        doc["skins"] = skins;
    if (!animations.empty())
        doc["animations"] = animations;
    if (!w.extensionsUsed.empty())
        doc["extensionsUsed"] = std::vector<std::string>(w.extensionsUsed.begin(), w.extensionsUsed.end());
    if (!w.extensionsRequired.empty())
        doc["extensionsRequired"] = std::vector<std::string>(w.extensionsRequired.begin(), w.extensionsRequired.end());

    std::string& bin = w.bin.data;
    while (bin.size() % 4)
        bin.push_back('\0');
    if (!bin.empty())
    {
        doc["bufferViews"] = w.bin.views;
        doc["accessors"] = w.bin.accessors;
        json buffer = {{"byteLength", bin.size()}};
        if (!opts.binary)
        {
            if (opts.embedBuffers)
                buffer["uri"] = dataUri("application/octet-stream", bin);
            else
            {
                const std::string binName = utf8(path.stem()) + ".bin";
                if (!writeFile(w.outDir / utf8Path(binName), bin))
                {
                    if (error)
                        *error = "can't write " + utf8(w.outDir / utf8Path(binName));
                    return false;
                }
                buffer["uri"] = uriEncode(binName);
            }
        }
        doc["buffers"] = json::array({buffer});
    }
    else if (!w.bin.accessors.empty())
        doc["accessors"] = w.bin.accessors;

    std::string out;
    if (opts.binary)
    {
        std::string text = doc.dump();
        while (text.size() % 4)
            text.push_back(' ');
        auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
        const uint32_t total = uint32_t(12 + 8 + text.size() + (bin.empty() ? 0 : 8 + bin.size()));
        u32(0x46546C67); // "glTF"
        u32(2);
        u32(total);
        u32(uint32_t(text.size()));
        u32(0x4E4F534A); // "JSON"
        out += text;
        if (!bin.empty())
        {
            u32(uint32_t(bin.size()));
            u32(0x004E4942); // "BIN\0"
            out += bin;
        }
    }
    else
        out = doc.dump(2);
    if (!writeFile(path, out))
    {
        if (error)
            *error = "can't write " + utf8(path);
        return false;
    }
    return true;
}
} // namespace lila::gltf
