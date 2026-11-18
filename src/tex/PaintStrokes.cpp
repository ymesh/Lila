#include "tex/PaintStrokes.h"

#include "core/PackedArray.h"
#include "core/Paths.h"
#include "scene/Node.h"
#include "tex/PageStore.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace lila::tex::paint
{
namespace
{
/** Floats per dab row in a stroke record: u, v, radius, pressure, opacity, angle, tilt. */
constexpr size_t kDabFloats = 7;
constexpr const char* kBlendNames[] = {"normal", "erase", "add", "subtract", "multiply"};

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(const std::vector<uint8_t>& in)
{
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3)
    {
        const uint32_t n = uint32_t(in[i]) << 16 | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0u) | (i + 2 < in.size() ? uint32_t(in[i + 2]) : 0u);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[n & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> fromBase64(const std::string& in)
{
    int table[256];
    std::fill(std::begin(table), std::end(table), -1);
    for (int i = 0; i < 64; ++i)
        table[uint8_t(kB64[i])] = i;
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in)
    {
        const int v = table[uint8_t(c)];
        if (v < 0)
            continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

json vec3Json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 vec3From(const json& j, const char* what)
{
    if (j.is_number())
        return glm::vec3(j.get<float>());
    if (!j.is_array() || j.size() < 3)
        throw std::invalid_argument(std::string(what) + ": expected a number or [x, y, z]");
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

/** Rows of a packed array or of nested arrays, as doubles; width per row. */
std::vector<double> rows(const json& j, int width, const char* what)
{
    std::vector<double> out;
    if (packed::is(j))
    {
        const auto& bin = j.get_binary();
        const size_t n = packed::count(j);
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.push_back(packed::element(packed::dtype(j), bin.data(), i).get<double>());
    }
    else if (j.is_array())
        for (const json& r : j)
        {
            if (r.is_array())
                for (const json& x : r)
                    out.push_back(x.get<double>());
            else
                out.push_back(r.get<double>());
        }
    else
        throw std::invalid_argument(std::string(what) + ": expected an array");
    if (out.size() % size_t(width) != 0)
        throw std::invalid_argument(std::string(what) + ": rows of " + std::to_string(width) + " expected");
    return out;
}

/** FNV-1a over bytes. */
uint64_t hashBytes(const std::vector<uint8_t>& data, uint64_t seed)
{
    uint64_t h = 1469598103934665603ull ^ seed;
    for (uint8_t b : data)
    {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

/** The exact float a stored double stands for (values written from floats round trip exactly). */
float f32(double d) { return float(d); }
} // namespace

const char* blendName(Blend b) { return kBlendNames[size_t(b)]; }

std::optional<Blend> blendFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kBlendNames); ++i)
        if (name == kBlendNames[i])
            return Blend(i);
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------- Payload

const std::vector<std::string>& payloadKinds()
{
    static const std::vector<std::string> kinds = {"paint", "direction", "smudge", "blur", "sharpen", "clone", "heal", "fill", "project", "normal"};
    return kinds;
}

std::vector<std::string> Payload::written() const
{
    std::vector<std::string> out;
    if (kind == "direction" || kind == "normal")
        out.push_back(directionChannel);
    else
        for (const auto& [name, v] : channels)
            if (name != kMaskChannel)
                out.push_back(name);
    // A projection with no colour channel paints only the mask (a selection made on screen).
    if (kind == "project" && !colorChannel.empty() && std::find(out.begin(), out.end(), colorChannel) == out.end())
        out.insert(out.begin(), colorChannel);
    if (mask || channels.contains(kMaskChannel))
        out.push_back(kMaskChannel);
    return out;
}

bool Payload::filters() const { return kind == "smudge" || kind == "blur" || kind == "sharpen" || kind == "clone" || kind == "heal"; }

json Payload::toJson() const
{
    json ch = json::object();
    for (const auto& [name, v] : channels)
        ch[name] = vec3Json(v);
    json j = {{"kind", kind}, {"channels", ch}, {"blend", blendName(blend)}, {"mask", mask}};
    if (kind == "direction" || kind == "normal")
        j["channel"] = directionChannel;
    if (kind == "direction")
        j["tilt"] = tilt;
    if (kind == "blur" || kind == "heal" || kind == "sharpen")
        j["filterRadius"] = filterRadius;
    if (kind == "smudge" || kind == "sharpen")
        j["amount"] = amount;
    if (kind == "clone" || kind == "heal")
    {
        j["sourceOffset"] = {sourceOffset.x, sourceOffset.y};
        if (!sourceOffsets.empty())
        {
            json list = json::array();
            for (const glm::vec2& o : sourceOffsets)
                list.push_back({o.x, o.y});
            j["sourceOffsets"] = list;
        }
    }
    if (kind == "fill")
    {
        j["triangles"] = packed::make<uint32_t>(triangles, 1);
        if (!fillBy.empty())
            j["fillBy"] = fillBy;
    }
    if (kind == "project")
    {
        j["image"] = image;
        j["colorChannel"] = colorChannel;
        j["facingFade"] = facingFade;
    }
    if (kind == "normal")
    {
        j["normalMode"] = normalMode;
        j["intensity"] = intensity;
        if (!stampImage.empty())
            j["stampImage"] = stampImage;
    }
    if (!source.is_null())
        j["source"] = source;
    if (!tip.is_null())
        j["tip"] = tip;
    if (!locked.empty())
        j["locked"] = packed::make<uint32_t>(locked, 1);
    if (!limit.empty())
        j["limit"] = limit;
    if (!gradient.is_null())
        j["gradient"] = gradient;
    if (!limits.is_null())
        j["limits"] = limits;
    return j;
}

Payload Payload::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("payload must be an object");
    Payload p;
    p.kind = j.value("kind", std::string("paint"));
    const auto& kinds = payloadKinds();
    if (std::find(kinds.begin(), kinds.end(), p.kind) == kinds.end())
        throw std::invalid_argument("payload kind " + p.kind + " is not one of paint, direction, smudge, blur, sharpen, clone, heal, fill, project, normal");
    if (j.contains("channels"))
    {
        if (!j["channels"].is_object())
            throw std::invalid_argument("payload channels must be an object of channel: value");
        for (auto it = j["channels"].begin(); it != j["channels"].end(); ++it)
            p.channels[it.key()] = vec3From(it.value(), it.key().c_str());
    }
    if (j.contains("blend"))
    {
        auto b = blendFromName(j["blend"].get<std::string>());
        if (!b)
            throw std::invalid_argument("unknown blend " + j["blend"].get<std::string>() + " (normal, erase, add, subtract, multiply)");
        p.blend = *b;
    }
    p.mask = j.value("mask", p.mask);
    p.tilt = j.value("tilt", p.tilt);
    p.directionChannel = j.value("channel", p.directionChannel);
    p.filterRadius = std::clamp(j.value("filterRadius", p.filterRadius), 0.5f, 64.0f);
    p.amount = j.value("amount", p.amount);
    if (j.contains("sourceOffset"))
    {
        const json& o = j["sourceOffset"];
        if (!o.is_array() || o.size() != 2)
            throw std::invalid_argument("payload sourceOffset must be [du, dv]");
        p.sourceOffset = {o[0].get<float>(), o[1].get<float>()};
    }
    if (j.contains("sourceOffsets") && j["sourceOffsets"].is_array())
        for (const json& o : j["sourceOffsets"])
        {
            if (!o.is_array() || o.size() != 2)
                throw std::invalid_argument("payload sourceOffsets must be [[du, dv], ...]");
            p.sourceOffsets.push_back({o[0].get<float>(), o[1].get<float>()});
        }
    if (j.contains("triangles"))
        for (double t : rows(j["triangles"], 1, "payload triangles"))
            p.triangles.push_back(uint32_t(t));
    std::sort(p.triangles.begin(), p.triangles.end());
    p.triangles.erase(std::unique(p.triangles.begin(), p.triangles.end()), p.triangles.end());
    p.fillBy = j.value("fillBy", std::string());
    if (j.contains("locked"))
        for (double t : rows(j["locked"], 1, "payload locked"))
            p.locked.push_back(uint32_t(t));
    std::sort(p.locked.begin(), p.locked.end());
    p.locked.erase(std::unique(p.locked.begin(), p.locked.end()), p.locked.end());
    p.limit = j.value("limit", std::string());
    if (j.contains("limits") && j["limits"].is_object())
        p.limits = j["limits"];
    if (j.contains("gradient") && j["gradient"].is_object())
    {
        p.gradient = j["gradient"];
        const std::string type = p.gradient.value("type", std::string("linear")), space = p.gradient.value("space", std::string("uv"));
        if (type != "linear" && type != "radial" && type != "reflected" && type != "angle")
            throw std::invalid_argument("gradient type is linear, radial, reflected or angle");
        if (space != "screen" && space != "uv")
            throw std::invalid_argument("gradient space is screen or uv");
    }
    p.image = j.value("image", std::string());
    p.colorChannel = j.value("colorChannel", p.colorChannel);
    p.facingFade = std::clamp(j.value("facingFade", p.facingFade), 0.0f, 1.0f);
    p.normalMode = j.value("normalMode", p.normalMode);
    p.stampImage = j.value("stampImage", std::string());
    p.intensity = j.value("intensity", p.intensity);
    if (j.contains("source") && !j["source"].is_null())
    {
        if (!j["source"].is_object())
            throw std::invalid_argument("payload source must be an object: {library | graph | node, resolution, scale}");
        p.source = j["source"];
    }
    if (j.contains("tip") && !j["tip"].is_null())
    {
        if (!j["tip"].is_object())
            throw std::invalid_argument("payload tip must be an object: {node | graph, values, resolution}");
        p.tip = j["tip"];
    }
    if (p.normalMode != "bump" && p.normalMode != "stamp")
        throw std::invalid_argument("payload normalMode must be bump or stamp");
    if (p.kind == "normal" && p.normalMode == "stamp" && p.stampImage.empty())
        throw std::invalid_argument("a normal stamp needs stampImage");
    if (p.kind == "project" && p.image.empty())
        throw std::invalid_argument("a project payload needs image");
    if ((p.kind == "paint" || p.kind == "fill") && p.channels.empty() && !p.mask && p.source.is_null())
        throw std::invalid_argument("payload paints no channels");
    if (p.filters() && p.channels.empty())
        throw std::invalid_argument("a " + p.kind + " payload needs the channels it filters (any values)");
    return p;
}

// ---------------------------------------------------------------------------------------------------- Stroke

json Stroke::toJson(bool packedDabs) const
{
    json j = {{"id", id}, {"mesh", mesh}, {"brush", brush}, {"payload", payload.toJson()}, {"seed", seed}, {"occlusion", occlusion}};
    if (!particles.is_null())
        j["particles"] = particles;
    if (!symmetry.is_null())
        j["symmetry"] = symmetry;
    if (stencil)
        j["stencil"] = stencil->toJson();
    if (!path.is_null())
        j["path"] = path;
    if (!anchors.empty())
    {
        std::vector<float> a(anchors.size() * 3);
        for (size_t i = 0; i < anchors.size(); ++i)
            a[i * 3] = anchors[i].x, a[i * 3 + 1] = anchors[i].y, a[i * 3 + 2] = anchors[i].z;
        j["anchors"] = packedDabs ? json(packed::make<float>(a, 3)) : json(a);
        // A string: JSON numbers can't hold every 64-bit value exactly.
        j["meshSignature"] = std::to_string(meshSignature);
    }
    if (view)
    {
        const float* m = &view->objectToClip[0][0];
        j["view"] = {{"objectToClip", std::vector<float>(m, m + 16)},
                     {"eye", vec3Json(view->eye)},
                     {"direction", vec3Json(view->direction)},
                     {"width", view->width},
                     {"height", view->height}};
    }
    std::vector<uint32_t> tri(dabs.size() * 2);
    std::vector<float> d(dabs.size() * kDabFloats);
    for (size_t i = 0; i < dabs.size(); ++i)
    {
        const Dab& a = dabs[i];
        tri[i * 2] = a.triangle;
        tri[i * 2 + 1] = a.copy;
        const float row[kDabFloats] = {a.u, a.v, a.radius, a.pressure, a.opacity, a.angle, a.tilt};
        std::copy(row, row + kDabFloats, &d[i * kDabFloats]);
    }
    if (packedDabs)
    {
        j["triangles"] = packed::make<uint32_t>(tri, 2);
        j["dabs"] = packed::make<float>(d, kDabFloats);
    }
    else
    {
        json t = json::array(), r = json::array();
        for (size_t i = 0; i < dabs.size(); ++i)
        {
            t.push_back({tri[i * 2], tri[i * 2 + 1]});
            r.push_back(json(std::vector<double>(&d[i * kDabFloats], &d[i * kDabFloats] + kDabFloats)));
        }
        j["triangles"] = t;
        j["dabs"] = r;
    }
    return j;
}

Stroke Stroke::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a stroke must be an object");
    Stroke s;
    try
    {
        s.id = j.value("id", uint64_t(0));
        s.mesh = j.value("mesh", std::string());
        s.brush = j.value("brush", json::object());
        if (!s.brush.is_object())
            throw std::invalid_argument("brush must be an object");
        s.payload = Payload::fromJson(j.value("payload", json::object()));
        s.seed = j.value("seed", uint32_t(0));
        s.occlusion = j.value("occlusion", true);
        if (j.contains("particles") && j["particles"].is_object())
            s.particles = j["particles"];
        if (j.contains("symmetry") && j["symmetry"].is_object())
            s.symmetry = j["symmetry"];
        if (j.contains("stencil") && j["stencil"].is_object())
            s.stencil = StrokeStencil::fromJson(j["stencil"]);
        if (j.contains("path") && j["path"].is_object())
            s.path = j["path"];
        if (j.contains("anchors"))
        {
            const std::vector<double> a = rows(j["anchors"], 3, "anchors");
            for (size_t i = 0; i + 2 < a.size(); i += 3)
                s.anchors.push_back({f32(a[i]), f32(a[i + 1]), f32(a[i + 2])});
            s.meshSignature = std::stoull(j.value("meshSignature", std::string("0")));
        }
        if (j.contains("view") && j["view"].is_object())
        {
            const json& v = j["view"];
            StrokeView view;
            const std::vector<double> m = rows(v.at("objectToClip"), 16, "view.objectToClip");
            if (m.size() != 16)
                throw std::invalid_argument("view.objectToClip needs 16 values");
            for (int i = 0; i < 16; ++i)
                (&view.objectToClip[0][0])[i] = f32(m[size_t(i)]);
            view.eye = vec3From(v.value("eye", json::array({0, 0, 0})), "view.eye");
            view.direction = vec3From(v.value("direction", json::array({0, 0, -1})), "view.direction");
            view.width = std::max(1, v.value("width", 1));
            view.height = std::max(1, v.value("height", 1));
            s.view = view;
        }
        const json dabs = j.value("dabs", json::array());
        if (dabs.is_array() && !dabs.empty() && dabs[0].is_object())
        {
            for (const json& o : dabs)
            {
                Dab d;
                d.triangle = o.at("triangle").get<uint32_t>();
                const json& b = o.at("bary");
                if (!b.is_array() || b.size() < 2)
                    throw std::invalid_argument("dab bary must be [u, v]");
                d.u = b[0].get<float>();
                d.v = b[1].get<float>();
                d.radius = o.at("radius").get<float>();
                d.pressure = o.value("pressure", 1.0f);
                d.opacity = o.value("opacity", 1.0f);
                d.angle = o.value("angle", 0.0f);
                d.copy = o.value("copy", 0u);
                d.tilt = o.value("tilt", 0.0f);
                s.dabs.push_back(d);
            }
        }
        else
        {
            const std::vector<double> t = rows(j.value("triangles", json::array()), 2, "triangles");
            const size_t count = t.size() / 2;
            // Rows of 6 are dabs recorded without tilt.
            const std::vector<double> d = rows(dabs, 1, "dabs");
            const size_t w = count ? d.size() / count : kDabFloats;
            if ((w != 6 && w != kDabFloats) || w * count != d.size())
                throw std::invalid_argument("triangles and dabs differ in length");
            for (size_t i = 0; i < count; ++i)
            {
                const double* r = &d[i * w];
                s.dabs.push_back({uint32_t(t[i * 2]), f32(r[0]), f32(r[1]), f32(r[2]), f32(r[3]), f32(r[4]), f32(r[5]), uint32_t(t[i * 2 + 1]),
                                  w > 6 ? f32(r[6]) : 0.0f});
            }
        }
        for (const Dab& d : s.dabs)
            if (!(d.radius > 0.0f) || !std::isfinite(d.u) || !std::isfinite(d.v))
                throw std::invalid_argument("dab radius must be positive and barycentrics finite");
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("stroke: ") + e.what());
    }
    return s;
}

bool Stroke::operator==(const Stroke& o) const
{
    return id == o.id && mesh == o.mesh && brush == o.brush && payload == o.payload && seed == o.seed && occlusion == o.occlusion && view == o.view &&
           dabs == o.dabs && particles == o.particles && symmetry == o.symmetry && stencil == o.stencil && path == o.path &&
           anchors == o.anchors && meshSignature == o.meshSignature;
}

std::vector<std::string> Stroke::images() const
{
    std::vector<std::string> out;
    if (!payload.image.empty())
        out.push_back(payload.image);
    if (stencil && !stencil->image.empty())
        out.push_back(stencil->image);
    if (!payload.limit.empty())
        out.push_back(payload.limit);
    return out;
}

json StrokeStencil::toJson() const
{
    return {{"image", image}, {"offset", {offset.x, offset.y}}, {"scale", scale}, {"rotation", rotation}, {"invert", invert},
            {"tile", tile},   {"mode", mode},                    {"colorChannel", colorChannel}};
}

StrokeStencil StrokeStencil::fromJson(const json& j)
{
    StrokeStencil s;
    try
    {
        s.image = j.value("image", std::string());
        if (j.contains("offset"))
        {
            const json& o = j["offset"];
            if (!o.is_array() || o.size() != 2)
                throw std::invalid_argument("stencil offset must be [x, y]");
            s.offset = {o[0].get<float>(), o[1].get<float>()};
        }
        s.scale = j.value("scale", 1.0f);
        s.rotation = j.value("rotation", 0.0f);
        s.invert = j.value("invert", false);
        s.tile = j.value("tile", false);
        s.mode = j.value("mode", std::string("mask"));
        s.colorChannel = j.value("colorChannel", std::string("baseColor"));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("stencil: ") + e.what());
    }
    if (s.mode != "mask" && s.mode != "color")
        throw std::invalid_argument("stencil mode is mask or color");
    if (!(s.scale > 0.0f) || !std::isfinite(s.rotation))
        throw std::invalid_argument("stencil scale must be positive");
    return s;
}

namespace
{
std::mutex gHeldMutex;
std::vector<std::shared_ptr<const PaintImage>> gHeld;
} // namespace

void holdImage(std::shared_ptr<const PaintImage> image)
{
    if (!image)
        return;
    const std::string key = image->key();
    std::scoped_lock lock(gHeldMutex);
    std::erase_if(gHeld, [&](const auto& h) { return h->key() == key; });
    gHeld.push_back(std::move(image));
    if (gHeld.size() > 8)
        gHeld.erase(gHeld.begin());
}

std::shared_ptr<const PaintImage> heldImage(const std::string& key)
{
    std::scoped_lock lock(gHeldMutex);
    for (const auto& h : gHeld)
        if (h->key() == key)
            return h;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------- Baseline

std::vector<uint8_t> compressPages(const std::vector<uint8_t>& raw)
{
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(n))
        throw std::runtime_error(std::string("paint pages: ") + ZSTD_getErrorName(n));
    out.resize(n);
    return out;
}

std::vector<uint8_t> decompressPages(const std::vector<uint8_t>& packedBytes, size_t rawBytes)
{
    std::vector<uint8_t> out(rawBytes);
    const size_t n = ZSTD_decompress(out.data(), out.size(), packedBytes.data(), packedBytes.size());
    if (ZSTD_isError(n) || n != rawBytes)
        throw std::runtime_error("paint pages: corrupt baseline data");
    return out;
}

size_t Baseline::bytes() const
{
    size_t n = 0;
    for (const Tile& t : tiles)
        n += t.data.size() + t.pages.size() * 4;
    return n;
}

std::vector<uint8_t> Baseline::Tile::raw() const
{
    const size_t pageBytes = size_t(kPageSize) * size_t(kPageSize) * 8;
    if (chunks.empty())
        return decompressPages(data, pageBytes * pages.size());
    // Chunks group the pages their own way: put each back at its place in pages.
    std::map<uint32_t, size_t> at;
    for (size_t i = 0; i < pages.size(); ++i)
        at[pages[i]] = i;
    std::vector<uint8_t> out(pageBytes * pages.size());
    size_t found = 0;
    for (const Chunk& c : chunks)
    {
        const store::fs::path file = store::resolve(store, paths::fromUtf8(storeAbs), c.file);
        if (file.empty())
            throw std::runtime_error("paint pages: the scene's page store has no " + c.file);
        const std::vector<uint8_t> texels = store::readChunk(file, pageBytes);
        if (texels.size() != c.pages.size() * pageBytes)
            throw std::runtime_error("paint pages: " + c.file + " doesn't hold the pages it should");
        for (size_t k = 0; k < c.pages.size(); ++k)
            if (auto it = at.find(c.pages[k]); it != at.end())
            {
                std::memcpy(out.data() + it->second * pageBytes, texels.data() + k * pageBytes, pageBytes);
                ++found;
            }
    }
    if (found != pages.size())
        throw std::runtime_error("paint pages: the page store's chunks don't add up to the tile");
    return out;
}

json Baseline::toJson(const std::string& layer) const
{
    const store::fs::path active = store::activeStore();
    const size_t pageBytes = size_t(kPageSize) * size_t(kPageSize) * 8;
    json list = json::array();
    for (const Tile& t : tiles)
    {
        json j = {{"channel", t.channel}, {"udim", t.udim}, {"width", t.width}, {"height", t.height}};
        if (active.empty())
        {
            j["pages"] = t.pages;
            j["data"] = toBase64(t.chunks.empty() ? t.data : compressPages(t.raw()));
            list.push_back(std::move(j));
            continue;
        }
        const std::string activeAbs = paths::toUtf8(store::fs::absolute(active));
        if (t.chunks.empty() || t.storeAbs != activeAbs)
        {
            // Into this store, chunk by chunk: unchanged chunks hash to files that are already there.
            const std::vector<uint8_t> raw = t.raw();
            const uint32_t pagesX = uint32_t(std::max((t.width + kPageSize - 1) / kPageSize, 1));
            std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> groups;
            for (size_t i = 0; i < t.pages.size(); ++i)
                groups[{t.pages[i] / pagesX / uint32_t(store::kChunkPages), t.pages[i] % pagesX / uint32_t(store::kChunkPages)}].push_back(i);
            std::vector<Tile::Chunk> chunks;
            for (const auto& [at, members] : groups)
            {
                Tile::Chunk c;
                std::vector<uint8_t> texels;
                texels.reserve(members.size() * pageBytes);
                for (size_t i : members)
                {
                    c.pages.push_back(t.pages[i]);
                    texels.insert(texels.end(), raw.begin() + std::ptrdiff_t(i * pageBytes), raw.begin() + std::ptrdiff_t((i + 1) * pageBytes));
                }
                const std::string key = "paint/" + (layer.empty() ? std::string("layer") : layer) + "/" + t.channel + "/" + std::to_string(t.udim) +
                                        "/m0/c" + std::to_string(at.second) + "_" + std::to_string(at.first);
                c.file = store::putChunk(key, c.pages, texels, pageBytes);
                chunks.push_back(std::move(c));
            }
            t.chunks = std::move(chunks);
            t.store = paths::toUtf8(active.filename());
            t.storeAbs = activeAbs;
        }
        else
            for (const Tile::Chunk& c : t.chunks)
                store::keepChunk(c.file);
        // The chunks group the pages, so the tile's pages are listed in chunk order.
        json refs = json::array();
        std::vector<uint32_t> order;
        for (const Tile::Chunk& c : t.chunks)
        {
            refs.push_back({{"file", c.file}, {"pages", c.pages}});
            order.insert(order.end(), c.pages.begin(), c.pages.end());
        }
        j["pages"] = order;
        j["store"] = t.store;
        j["storeAbs"] = t.storeAbs;
        j["chunks"] = refs;
        list.push_back(std::move(j));
    }
    return {{"tiles", list}};
}

std::shared_ptr<const Baseline> Baseline::fromJson(const json& j)
{
    if (!j.is_object())
        return nullptr;
    auto b = std::make_shared<Baseline>();
    for (const json& t : j.value("tiles", json::array()))
    {
        Tile tile;
        tile.channel = t.value("channel", std::string());
        tile.udim = t.value("udim", 1001);
        tile.width = t.value("width", 0);
        tile.height = t.value("height", 0);
        for (double p : rows(t.value("pages", json::array()), 1, "baseline pages"))
            tile.pages.push_back(uint32_t(p));
        if (t.contains("chunks"))
        {
            tile.store = t.value("store", std::string());
            tile.storeAbs = t.value("storeAbs", std::string());
            for (const json& c : t["chunks"])
            {
                Tile::Chunk chunk;
                chunk.file = c.value("file", std::string());
                for (double p : rows(c.value("pages", json::array()), 1, "baseline chunk pages"))
                    chunk.pages.push_back(uint32_t(p));
                tile.chunks.push_back(std::move(chunk));
            }
        }
        else
            tile.data = fromBase64(t.value("data", std::string()));
        b->tiles.push_back(std::move(tile));
    }
    return b;
}

// ---------------------------------------------------------------------------------------------------- PaintImage

float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
    uint32_t bits;
    if (e == 0)
    {
        if (m == 0)
            bits = sign;
        else
        {
            float f = std::ldexp(float(m), -24);
            std::memcpy(&bits, &f, 4);
            bits |= sign;
        }
    }
    else if (e == 31)
        bits = sign | 0x7F800000u | (m << 13);
    else
        bits = sign | ((e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t floatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int e = int((x >> 23) & 255) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 255) == 255)
        return uint16_t(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31)
        return uint16_t(sign | 0x7C00);
    if (e <= 0)
    {
        if (e < -10)
            return uint16_t(sign);
        m |= 0x800000;
        const int shift = 14 - e;
        uint32_t h = m >> shift;
        if ((m >> (shift - 1)) & 1)
            h += 1;
        return uint16_t(sign | h);
    }
    uint32_t h = sign | uint32_t(e << 10) | (m >> 13);
    if (m & 0x1000)
        h += 1;
    return uint16_t(h);
}

std::shared_ptr<const PaintImage> PaintImage::fromRgba(int width, int height, std::span<const float> rgba)
{
    if (width < 1 || height < 1 || rgba.size() != size_t(width) * size_t(height) * 4)
        throw std::invalid_argument("paint image: size and texels disagree");
    std::vector<uint8_t> raw(rgba.size() * 2);
    auto* h = reinterpret_cast<uint16_t*>(raw.data());
    for (size_t i = 0; i < rgba.size(); ++i)
        h[i] = floatToHalf(rgba[i]);
    auto img = std::make_shared<PaintImage>();
    img->width = width;
    img->height = height;
    img->data = compressPages(raw);
    return img;
}

std::vector<float> PaintImage::rgba() const
{
    const size_t n = size_t(width) * size_t(height) * 4;
    const std::vector<uint8_t> raw = decompressPages(data, n * 2);
    std::vector<float> out(n);
    const auto* h = reinterpret_cast<const uint16_t*>(raw.data());
    for (size_t i = 0; i < n; ++i)
        out[i] = halfToFloat(h[i]);
    return out;
}

std::string PaintImage::key() const
{
    const uint64_t h = hashBytes(data, uint64_t(width) * 73856093ull ^ uint64_t(height) * 19349663ull);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "img%016llx", static_cast<unsigned long long>(h));
    return buf;
}

json PaintImage::toJson() const { return {{"width", width}, {"height", height}, {"data", toBase64(data)}}; }

std::shared_ptr<const PaintImage> PaintImage::fromJson(const json& j)
{
    if (!j.is_object())
        return nullptr;
    auto img = std::make_shared<PaintImage>();
    img->width = j.value("width", 0);
    img->height = j.value("height", 0);
    img->data = fromBase64(j.value("data", std::string()));
    if (img->width < 1 || img->height < 1)
        return nullptr;
    return img;
}

// ---------------------------------------------------------------------------------------------------- PaintData

json PaintData::toJson() const
{
    json list = json::object();
    for (const auto& [id, buf] : buffers)
    {
        json strokes = json::array();
        for (const StrokePtr& s : buf.strokes)
            strokes.push_back(s->toJson(true));
        json b = {{"strokes", strokes}, {"width", buf.width}, {"height", buf.height}, {"clears", buf.clears}};
        if (buf.baseline)
            b["baseline"] = buf.baseline->toJson(id);
        list[id] = b;
    }
    json imgs = json::object();
    for (const auto& [key, img] : images)
        imgs[key] = img->toJson();
    return {{"kind", kDataKind}, {"active", active}, {"buffers", list}, {"images", imgs}};
}

size_t PaintData::bytes() const
{
    size_t n = 256;
    for (const auto& [id, buf] : buffers)
        n += buf.strokes.size() * sizeof(StrokePtr) + (buf.baseline ? buf.baseline->bytes() : 0);
    for (const auto& [key, img] : images)
        n += img->data.size();
    return n;
}

scene::DataPtr PaintData::fromJson(const json& j)
{
    auto d = std::make_shared<PaintData>();
    d->active = j.value("active", std::string());
    if (j.contains("buffers") && j["buffers"].is_object())
        for (auto it = j["buffers"].begin(); it != j["buffers"].end(); ++it)
        {
            Buffer buf;
            const json& b = it.value();
            buf.width = b.value("width", 0);
            buf.height = b.value("height", 0);
            buf.clears = b.value("clears", uint64_t(0));
            for (const json& s : b.value("strokes", json::array()))
                buf.strokes.push_back(std::make_shared<const Stroke>(Stroke::fromJson(s)));
            if (b.contains("baseline"))
                buf.baseline = Baseline::fromJson(b["baseline"]);
            d->buffers[it.key()] = std::move(buf);
        }
    if (j.contains("images") && j["images"].is_object())
        for (auto it = j["images"].begin(); it != j["images"].end(); ++it)
            if (auto img = PaintImage::fromJson(it.value()))
                d->images[it.key()] = img;
    return d;
}

void PaintData::pruneImages()
{
    std::set<std::string> used;
    for (const auto& [id, buf] : buffers)
        for (const StrokePtr& st : buf.strokes)
            for (const std::string& k : st->images())
                used.insert(k);
    std::erase_if(images, [&](const auto& kv) { return !used.contains(kv.first); });
}

const Buffer* PaintData::buffer(std::string_view id) const
{
    auto it = buffers.find(std::string(id));
    return it == buffers.end() ? nullptr : &it->second;
}

std::string PaintData::uniqueId() const
{
    for (int i = 1;; ++i)
        if (const std::string id = "paint" + std::to_string(i); !buffers.contains(id))
            return id;
}

void registerDataKind() { scene::data::registerKind(kDataKind, &PaintData::fromJson); }

PaintPtr readPaint(const scene::Node& set)
{
    const int a = set.type().attrIndex(kPaintAttr);
    if (a >= 0)
        if (const auto* v = std::get_if<scene::DataPtr>(&set.value(a)); v && *v)
            if (auto p = std::dynamic_pointer_cast<const PaintData>(*v))
                return p;
    return std::make_shared<const PaintData>();
}

void writePaint(scene::Graph& g, scene::Node& set, PaintPtr data)
{
    const int a = set.type().attrIndex(kPaintAttr);
    if (a < 0)
        throw std::invalid_argument(set.path() + " has no paint attr");
    g.setValue({set.id(), a, -1}, scene::DataPtr(std::move(data)));
}

std::string externalName(std::string_view buffer, std::string_view channel)
{
    return "paint/" + std::string(buffer) + "/" + std::string(channel);
}

std::string imageRef(std::string_view buffer, std::string_view channel) { return "external:" + externalName(buffer, channel); }

bool parseExternalName(std::string_view name, std::string* buffer, std::string* channel)
{
    if (!name.starts_with("paint/"))
        return false;
    name.remove_prefix(6);
    const size_t slash = name.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= name.size())
        return false;
    if (buffer)
        *buffer = std::string(name.substr(0, slash));
    if (channel)
        *channel = std::string(name.substr(slash + 1));
    return true;
}

glm::vec3 tipFrame(const glm::vec3& normal)
{
    const glm::vec3 n = glm::length(normal) > 0.0f ? glm::normalize(normal) : glm::vec3(0, 1, 0);
    // The axis least aligned with the normal, so the frame never degenerates.
    const glm::vec3 a = std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    return glm::normalize(glm::cross(a, n));
}
} // namespace lila::tex::paint
