#include "tiles/Tiles3d.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "gltf/Gltf.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "pointcloud/PointSource.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <span>

namespace lila::tiles
{
namespace fs = std::filesystem;
using json = nlohmann::json;
using scene::Graph;
using scene::NodeId;

namespace
{
/** glTF's Y up turned to 3D Tiles' Z up, as the spec places glTF content. */
const glm::dmat4 kYUpToZUp(glm::dvec4(1, 0, 0, 0), glm::dvec4(0, 0, 1, 0), glm::dvec4(0, -1, 0, 0), glm::dvec4(0, 0, 0, 1));

std::vector<uint8_t> readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw CommandError("import3dTiles: can't open " + paths::toUtf8(p));
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

uint32_t u32(std::span<const uint8_t> b, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= b.size())
        std::memcpy(&v, b.data() + at, 4);
    return v;
}

glm::dmat4 matrixOf(const json& j)
{
    glm::dmat4 m(1.0);
    if (j.is_array() && j.size() == 16)
        for (int i = 0; i < 16; ++i)
            m[i / 4][i % 4] = j[size_t(i)].get<double>();
    return m;
}

/** A feature table: its JSON and binary body, with typed reads of properties stored in either. */
struct FeatureTable
{
    json meta = json::object();
    std::span<const uint8_t> body;

    /** n vec3s of a float property: from the binary body at its byteOffset, or the JSON array. */
    std::vector<glm::dvec3> vec3f(const char* name, size_t n) const
    {
        std::vector<glm::dvec3> out;
        if (!meta.contains(name))
            return out;
        const json& v = meta[name];
        if (v.is_array() && v.size() >= 3 * n)
            for (size_t i = 0; i < n; ++i)
                out.emplace_back(v[3 * i].get<double>(), v[3 * i + 1].get<double>(), v[3 * i + 2].get<double>());
        else if (v.is_object())
        {
            const size_t off = v.value("byteOffset", size_t(0));
            if (off + n * 12 > body.size())
                return out;
            for (size_t i = 0; i < n; ++i)
            {
                float f[3];
                std::memcpy(f, body.data() + off + i * 12, 12);
                out.emplace_back(f[0], f[1], f[2]);
            }
        }
        return out;
    }
    /** Unit vectors stored as two uint16 octahedral coordinates each. */
    std::vector<glm::dvec3> oct32p(const char* name, size_t n) const
    {
        std::vector<glm::dvec3> out;
        const size_t off = meta[name].value("byteOffset", size_t(0));
        if (off + n * 4 > body.size())
            return out;
        for (size_t i = 0; i < n; ++i)
        {
            uint16_t q[2];
            std::memcpy(q, body.data() + off + i * 4, 4);
            glm::dvec3 v(q[0] / 65535.0 * 2.0 - 1.0, q[1] / 65535.0 * 2.0 - 1.0, 0.0);
            v.z = 1.0 - std::abs(v.x) - std::abs(v.y);
            if (v.z < 0.0)
                v = glm::dvec3((1.0 - std::abs(v.y)) * (v.x >= 0.0 ? 1.0 : -1.0), (1.0 - std::abs(v.x)) * (v.y >= 0.0 ? 1.0 : -1.0), v.z);
            out.push_back(glm::normalize(v));
        }
        return out;
    }
    /** POSITION_QUANTIZED (uint16 x3) scaled into QUANTIZED_VOLUME_OFFSET + SCALE. */
    std::vector<glm::dvec3> quantized(size_t n) const
    {
        std::vector<glm::dvec3> out;
        if (!meta.contains("POSITION_QUANTIZED") || !meta.contains("QUANTIZED_VOLUME_OFFSET") || !meta.contains("QUANTIZED_VOLUME_SCALE"))
            return out;
        const size_t off = meta["POSITION_QUANTIZED"].value("byteOffset", size_t(0));
        const json& o = meta["QUANTIZED_VOLUME_OFFSET"];
        const json& s = meta["QUANTIZED_VOLUME_SCALE"];
        const glm::dvec3 offset(o[0].get<double>(), o[1].get<double>(), o[2].get<double>());
        const glm::dvec3 scale(s[0].get<double>(), s[1].get<double>(), s[2].get<double>());
        if (off + n * 6 > body.size())
            return out;
        for (size_t i = 0; i < n; ++i)
        {
            uint16_t q[3];
            std::memcpy(q, body.data() + off + i * 6, 6);
            out.push_back(offset + glm::dvec3(q[0], q[1], q[2]) / 65535.0 * scale);
        }
        return out;
    }
    std::optional<glm::dvec3> rtcCenter() const
    {
        if (!meta.contains("RTC_CENTER"))
            return std::nullopt;
        const std::vector<glm::dvec3> c = vec3f("RTC_CENTER", 1);
        return c.empty() ? std::nullopt : std::optional(c.front());
    }
};

FeatureTable featureTable(std::span<const uint8_t> b, size_t at, uint32_t jsonBytes, uint32_t binBytes)
{
    FeatureTable t;
    if (jsonBytes && at + jsonBytes <= b.size())
        t.meta = json::parse(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + jsonBytes), nullptr, false);
    if (!t.meta.is_object())
        t.meta = json::object();
    if (at + jsonBytes + binBytes <= b.size())
        t.body = b.subspan(at + jsonBytes, binBytes);
    return t;
}

/** One import: the scene's frame, the options, and what came in. */
struct Importer
{
    Graph& g;
    scene::georef::Georeference geo;
    bool zUp = false;
    double maxError = 0.0;
    std::optional<glm::dvec3> centreEcef;
    double radius = 0.0;
    size_t maxTiles = 1000;
    NodeId root = scene::kNoNode;
    fs::path work;

    size_t tiles = 0, meshes = 0, points = 0, culled = 0;
    /** The tile being read's bounding volume centre, Earth-centred: where its content is, whatever its transform says. */
    std::optional<glm::dvec3> where;
    std::vector<std::string> skipped;
    bool truncated = false;

    /** Earth-centred metres to scene cm, exactly at e. */
    glm::dvec3 toScene(const glm::dvec3& e) const
    {
        const auto llh = scene::georef::convert("EPSG:4978", "EPSG:4979", {e});
        if (!llh)
            throw CommandError("import3dTiles: PROJ can't convert Earth-centred coordinates");
        const glm::dvec3 lonLatH = llh->front();
        const auto q = scene::georef::fromGeographic(geo.crs, {lonLatH.y, lonLatH.x, lonLatH.z});
        if (!q)
            throw CommandError("import3dTiles: can't project a tile into " + geo.crs);
        return scene::georef::fromProjected(geo, *q, zUp);
    }

    /** Earth-centred metres to scene cm as one linear map about e0: exact at e0, the grid's scale and turn across a tile. */
    glm::dmat4 affineAt(const glm::dvec3& e0) const
    {
        const glm::dvec3 s0 = toScene(e0);
        glm::dmat4 a(1.0);
        for (int axis = 0; axis < 3; ++axis)
        {
            glm::dvec3 step(0.0);
            step[axis] = 1.0;
            a[axis] = glm::dvec4(toScene(e0 + step) - s0, 0.0);
        }
        a[3] = glm::dvec4(s0 - glm::dvec3(a * glm::dvec4(e0, 0.0)), 1.0);
        return a;
    }

    /** A glb (bytes) or glTF file through the glTF importer, under a group placed by tile (glTF space to Earth-centred). */
    void importGltfContent(const fs::path& file, const glm::dmat4& tile, const std::string& name, bool place = true)
    {
        const glm::dvec3 centre = where.value_or(glm::dvec3(tile[3]));
        // The importer turns glTF metres to scene cm in the scene's axes (F); the group undoes that and places the tile.
        glm::dmat4 importer(100.0);
        importer[3][3] = 1.0;
        if (zUp)
            importer = importer * kYUpToZUp;
        const glm::dmat4 m = affineAt(centre) * tile * kYUpToZUp * glm::inverse(importer);
        scene::Node* group = g.createNode(*scene::types::find("transform"), name, root, true);
        gltf::ImportOptions o;
        o.parent = group->id();
        o.animation = false;
        o.setPlayback = false;
        o.textureDir = work / "textures";
        const gltf::ImportResult r = gltf::importGltf(g, file, o);
        if (!r.ok)
        {
            skipped.push_back(name + ": " + r.error);
            g.deleteNode(group->id());
            return;
        }
        meshes += r.roots.size();
        if (!place)
            return;
        // Each root composed in double and set whole: a glb's root can sit at its Earth-centred place (about 6e8 cm), where
        // a transform's small shear dropped by translate/rotate/scale would move it by kilometres if m went on the group.
        g.evaluate();
        for (NodeId id : r.roots)
        {
            const glm::dmat4 w = m * g.node(id)->as<glm::dmat4>(scene::Transform::kMatrix);
            json flat = json::array();
            for (int i = 0; i < 16; ++i)
                flat.push_back(w[i / 4][i % 4]);
            ops::execute("xform", {{"args", {g.node(id)->path()}}, {"matrix", flat}});
        }
    }

    void glbBytes(std::span<const uint8_t> glb, const glm::dmat4& tile, const std::string& name)
    {
        const fs::path file = work / (name + ".glb");
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
        importGltfContent(file, tile, name);
    }

    void b3dm(std::span<const uint8_t> b, const glm::dmat4& tile, const std::string& name)
    {
        const uint32_t ftJ = u32(b, 12), ftB = u32(b, 16), btJ = u32(b, 20), btB = u32(b, 24);
        const FeatureTable ft = featureTable(b, 28, ftJ, ftB);
        const size_t glbAt = 28 + size_t(ftJ) + ftB + btJ + btB;
        if (glbAt >= b.size())
            return skipped.push_back(name + ": b3dm with no glTF");
        glm::dmat4 m = tile;
        if (const auto rtc = ft.rtcCenter())
            m = m * glm::translate(glm::dmat4(1.0), *rtc);
        glbBytes(b.subspan(glbAt, std::min<size_t>(u32(b, 8), b.size()) - glbAt), m, name);
    }

    /** Instances: the glTF once, hidden, then an instance of it per instance, placed by position, up and right, and scale. */
    void i3dm(std::span<const uint8_t> b, const glm::dmat4& tile, const std::string& name, const fs::path& dir)
    {
        const uint32_t ftJ = u32(b, 12), ftB = u32(b, 16), btJ = u32(b, 20), btB = u32(b, 24), format = u32(b, 28);
        const FeatureTable ft = featureTable(b, 32, ftJ, ftB);
        const size_t n = ft.meta.value("INSTANCES_LENGTH", size_t(0));
        std::vector<glm::dvec3> pos = ft.vec3f("POSITION", n);
        if (pos.empty())
            pos = ft.quantized(n);
        if (pos.size() != n || n == 0)
            return skipped.push_back(name + ": i3dm with no readable positions");
        std::vector<glm::dvec3> up = ft.vec3f("NORMAL_UP", n), right = ft.vec3f("NORMAL_RIGHT", n);
        if (up.empty() && ft.meta.contains("NORMAL_UP_OCT32P") && ft.meta.contains("NORMAL_RIGHT_OCT32P"))
        {
            up = ft.oct32p("NORMAL_UP_OCT32P", n);
            right = ft.oct32p("NORMAL_RIGHT_OCT32P", n);
        }
        const bool eastNorthUp = up.empty() && ft.meta.value("EAST_NORTH_UP", false);
        std::vector<double> uniform(n, 1.0);
        if (ft.meta.contains("SCALE") && ft.meta["SCALE"].is_object())
        {
            const size_t off = ft.meta["SCALE"].value("byteOffset", size_t(0));
            for (size_t i = 0; i < n && off + i * 4 + 4 <= ft.body.size(); ++i)
            {
                float s;
                std::memcpy(&s, ft.body.data() + off + i * 4, 4);
                uniform[i] = s;
            }
        }
        const std::vector<glm::dvec3> nonUniform = ft.vec3f("SCALE_NON_UNIFORM", n);
        const glm::dvec3 rtc = ft.rtcCenter().value_or(glm::dvec3(0.0));
        const size_t at = 32 + size_t(ftJ) + ftB + btJ + btB, end = std::min<size_t>(u32(b, 8), b.size());
        if (at >= end)
            return skipped.push_back(name + ": i3dm with no glTF");
        fs::path file;
        if (format == 1)
        {
            file = work / (name + ".glb");
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(b.data() + at), std::streamsize(end - at));
        }
        else
            file = dir / std::string(reinterpret_cast<const char*>(b.data() + at), end - at);
        // Each instance: tile * translate(rtc + position) * [right, up x right, up] * scale, then glTF's Y up to Z up.
        const size_t before = g.nodes().size();
        // The source stays as imported, hidden; each instance carries its whole placement.
        importGltfContent(file, tile, name + "_source", false);
        scene::Node* source = g.find(name + "_source");
        if (!source || g.nodes().size() == before)
            return;
        g.setValue({source->id(), scene::Transform::kVisibility, -1}, false);
        for (size_t i = 0; i < n; ++i)
        {
            glm::dmat4 local(1.0);
            if (eastNorthUp)
            {
                // Turned to east, north and up where the instance stands on the ellipsoid, in the tile's frame.
                const glm::dvec3 e = glm::dvec3(tile * glm::dvec4(rtc + pos[i], 1.0));
                const auto llh = scene::georef::convert("EPSG:4978", "EPSG:4979", {e});
                if (llh)
                {
                    const double lon = glm::radians(llh->front().x), lat = glm::radians(llh->front().y);
                    const glm::dmat3 toTile = glm::inverse(glm::dmat3(tile));
                    const glm::dvec3 east = glm::normalize(toTile * glm::dvec3(-std::sin(lon), std::cos(lon), 0.0));
                    const glm::dvec3 upE = glm::normalize(toTile * glm::dvec3(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)));
                    local[0] = glm::dvec4(east, 0.0);
                    local[1] = glm::dvec4(glm::cross(upE, east), 0.0);
                    local[2] = glm::dvec4(upE, 0.0);
                }
            }
            else if (up.size() == n && right.size() == n)
            {
                const glm::dvec3 u = glm::normalize(up[i]), r = glm::normalize(right[i]);
                local[0] = glm::dvec4(r, 0.0);
                local[1] = glm::dvec4(glm::cross(u, r), 0.0);
                local[2] = glm::dvec4(u, 0.0);
            }
            const glm::dvec3 s = nonUniform.size() == n ? nonUniform[i] * uniform[i] : glm::dvec3(uniform[i]);
            local = glm::translate(glm::dmat4(1.0), rtc + pos[i]) * local * glm::scale(glm::dmat4(1.0), s);
            // Instances sit beside the source at the tile's root: Earth-centred placement, then glTF's Y up, then the importer.
            glm::dmat4 importer(100.0);
            importer[3][3] = 1.0;
            if (zUp)
                importer = importer * kYUpToZUp;
            const glm::dmat4 m = affineAt(glm::dvec3(tile * glm::dvec4(rtc + pos[i], 1.0))) * tile * local * kYUpToZUp * glm::inverse(importer);
            const json made = ops::execute("instance", {{"args", {source->path()}}});
            const std::string inst = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
            json flat = json::array();
            for (int k = 0; k < 16; ++k)
                flat.push_back(m[k / 4][k % 4]);
            ops::execute("xform", {{"args", {inst}}, {"matrix", flat}});
            ops::execute("setAttr", {{"args", {inst + ".visibility", true}}});
        }
    }

    /** Points into a LAS at the scene CRS's coordinates, then a point cloud placed by that CRS like any scan. */
    void pnts(std::span<const uint8_t> b, const glm::dmat4& tile, const std::string& name)
    {
        const uint32_t ftJ = u32(b, 12), ftB = u32(b, 16);
        const FeatureTable ft = featureTable(b, 28, ftJ, ftB);
        const size_t n = ft.meta.value("POINTS_LENGTH", size_t(0));
        if (ft.meta.contains("extensions") && ft.meta["extensions"].contains("3DTILES_draco_point_compression"))
            return skipped.push_back(name + ": Draco-compressed point tile");
        std::vector<glm::dvec3> pos = ft.vec3f("POSITION", n);
        if (pos.empty())
            pos = ft.quantized(n);
        if (pos.size() != n || n == 0)
            return skipped.push_back(name + ": pnts with no readable positions");
        const glm::dvec3 rtc = ft.rtcCenter().value_or(glm::dvec3(0.0));
        std::vector<glm::dvec3> ecef(n);
        for (size_t i = 0; i < n; ++i)
            ecef[i] = glm::dvec3(tile * glm::dvec4(rtc + pos[i], 1.0));
        const auto llh = scene::georef::convert("EPSG:4978", "EPSG:4979", ecef);
        if (!llh)
            return skipped.push_back(name + ": PROJ can't convert its points");
        std::vector<pointcloud::RawPoint> out(n);
        auto colour = [&](const char* key, size_t stride) -> const uint8_t* {
            if (!ft.meta.contains(key) || !ft.meta[key].is_object())
                return nullptr;
            const size_t off = ft.meta[key].value("byteOffset", size_t(0));
            return off + n * stride <= ft.body.size() ? ft.body.data() + off : nullptr;
        };
        const uint8_t* rgb = colour("RGB", 3);
        const uint8_t* rgba = rgb ? nullptr : colour("RGBA", 4);
        for (size_t i = 0; i < n; ++i)
        {
            const glm::dvec3 g3 = (*llh)[i];
            const auto q = scene::georef::fromGeographic(geo.crs, {g3.y, g3.x, g3.z});
            if (!q)
                return skipped.push_back(name + ": can't project its points into " + geo.crs);
            out[i].position = *q;
            if (rgb)
                std::memcpy(out[i].rgb, rgb + i * 3, 3);
            else if (rgba)
                std::memcpy(out[i].rgb, rgba + i * 4, 3);
        }
        const fs::path las = work / (name + ".las");
        pointcloud::writeLas(las, out, scene::georef::wkt(geo.crs));
        const json r = ops::execute("importPointCloud", {{"args", {paths::toUtf8(las)}}, {"name", name}});
        if (NodeId id = g.find(r["node"].get<std::string>()) ? g.find(r["node"].get<std::string>())->id() : scene::kNoNode; id != scene::kNoNode)
            g.setParent(id, root, false);
        points += n;
    }

    void content(std::span<const uint8_t> b, const glm::dmat4& tile, const std::string& name, const fs::path& dir)
    {
        const std::string magic = b.size() >= 4 ? std::string(reinterpret_cast<const char*>(b.data()), 4) : std::string();
        if (magic == "b3dm")
            b3dm(b, tile, name);
        else if (magic == "i3dm")
            i3dm(b, tile, name, dir);
        else if (magic == "pnts")
            pnts(b, tile, name);
        else if (magic == "glTF")
            glbBytes(b, tile, name);
        else if (magic == "cmpt")
        {
            const uint32_t count = u32(b, 12);
            size_t at = 16;
            for (uint32_t i = 0; i < count && at + 12 <= b.size(); ++i)
            {
                const uint32_t len = u32(b, at + 8);
                if (len < 12 || at + len > b.size())
                    break;
                content(b.subspan(at, len), tile, std::format("{}_{}", name, i), dir);
                at += len;
            }
        }
        else
            skipped.push_back(name + ": unknown tile content '" + magic + "'");
    }

    /** The tile's bounding sphere, Earth-centred: from its box, sphere or region. */
    std::optional<std::pair<glm::dvec3, double>> sphereOf(const json& bv, const glm::dmat4& tile) const
    {
        if (bv.contains("sphere"))
        {
            const json& s = bv["sphere"];
            const glm::dvec3 c(tile * glm::dvec4(s[0].get<double>(), s[1].get<double>(), s[2].get<double>(), 1.0));
            const double scale = std::max({glm::length(glm::dvec3(tile[0])), glm::length(glm::dvec3(tile[1])), glm::length(glm::dvec3(tile[2]))});
            return std::pair{c, s[3].get<double>() * scale};
        }
        if (bv.contains("box"))
        {
            const json& b = bv["box"];
            const glm::dvec3 c(b[0].get<double>(), b[1].get<double>(), b[2].get<double>());
            glm::dvec3 half(0.0);
            for (int k = 0; k < 3; ++k)
                half += glm::abs(glm::dvec3(tile * glm::dvec4(b[3 + 3 * k].get<double>(), b[4 + 3 * k].get<double>(), b[5 + 3 * k].get<double>(), 0.0)));
            return std::pair{glm::dvec3(tile * glm::dvec4(c, 1.0)), glm::length(half)};
        }
        if (bv.contains("region"))
        {
            const json& r = bv["region"];
            std::vector<glm::dvec3> corners;
            for (int k = 0; k < 8; ++k)
                corners.push_back(glm::dvec3(glm::degrees(r[(k & 1) ? 2 : 0].get<double>()), glm::degrees(r[(k & 2) ? 3 : 1].get<double>()),
                                             r[(k & 4) ? 5 : 4].get<double>()));
            const auto e = scene::georef::convert("EPSG:4979", "EPSG:4978", corners);
            if (!e)
                return std::nullopt;
            glm::dvec3 c(0.0);
            for (const glm::dvec3& p : *e)
                c += p / 8.0;
            double rad = 0.0;
            for (const glm::dvec3& p : *e)
                rad = std::max(rad, glm::length(p - c));
            return std::pair{c, rad};
        }
        return std::nullopt;
    }

    /**
     * Picks the tiles to bring in: the first along each branch whose geometric error is within maxError (or a leaf), with
     * their parents' content too where refinement adds, and tiles outside the radius left out.
     */
    void visit(const json& tile, const glm::dmat4& parent, std::string refine, const fs::path& dir, int depth)
    {
        if (truncated)
            return;
        const glm::dmat4 t = parent * (tile.contains("transform") ? matrixOf(tile["transform"]) : glm::dmat4(1.0));
        if (centreEcef && radius > 0.0 && tile.contains("boundingVolume"))
            if (const auto s = sphereOf(tile["boundingVolume"], t); s && glm::length(s->first - *centreEcef) > s->second + radius)
            {
                ++culled;
                return;
            }
        refine = tile.value("refine", refine);
        const double error = tile.value("geometricError", 0.0);
        const json children = tile.value("children", json::array());
        const bool enough = children.empty() || error <= maxError;
        std::vector<std::string> uris;
        if (tile.contains("content"))
            uris.push_back(tile["content"].value("uri", tile["content"].value("url", std::string())));
        for (const json& c : tile.value("contents", json::array()))
            uris.push_back(c.value("uri", std::string()));
        for (const std::string& uri : uris)
        {
            if (uri.empty() || uri.find("://") != std::string::npos)
            {
                if (!uri.empty())
                    skipped.push_back(uri + ": remote content (only local tilesets)");
                continue;
            }
            const fs::path file = dir / paths::fromUtf8(uri.substr(0, uri.find('?')));
            if (file.extension() == ".json")
            {
                const json sub = json::parse(readAll(file), nullptr, false);
                if (sub.is_object() && sub.contains("root"))
                    visit(sub["root"], t, refine, file.parent_path(), depth + 1);
                continue;
            }
            if (!enough && refine != "ADD")
                continue;
            if (tiles >= maxTiles)
            {
                truncated = true;
                return;
            }
            ++tiles;
            const std::string name = std::format("Tile_{}", tiles);
            where.reset();
            if (tile.contains("boundingVolume"))
                if (const auto s = sphereOf(tile["boundingVolume"], t))
                    where = s->first;
            if (file.extension() == ".gltf")
                importGltfContent(file, t, name);
            else
            {
                const std::vector<uint8_t> bytes = readAll(file);
                content(bytes, t, name, file.parent_path());
            }
        }
        if (!enough)
            for (const json& c : children)
                visit(c, t, refine, dir, depth + 1);
    }
};

json import3dTiles(const Args& a)
{
    LILA_ZONE();
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("import3dTiles: path required");
    const fs::path path = paths::fromUtf8(a.positional[0].get<std::string>());
    Graph& g = Graph::current();
    const bool tileset = path.extension() == ".json";
    json root;
    if (tileset)
    {
        root = json::parse(readAll(path), nullptr, false);
        if (!root.is_object() || !root.contains("root"))
            throw CommandError("import3dTiles: " + paths::toUtf8(path) + " is not a tileset (no root tile)");
    }
    else
        root = {{"root", {{"geometricError", 0.0}, {"content", {{"uri", paths::toUtf8(path.filename())}}}, {"refine", "REPLACE"}}}};

    Importer im{.g = g};
    im.zUp = scene::georef::sceneZUp();
    im.maxError = a.get<double>("maxError", 0.0);
    im.maxTiles = size_t(a.get<int64_t>("maxTiles", 1000));
    im.radius = a.get<double>("radius", 0.0);
    if (a.has("centre"))
    {
        const json c = a.flags.at("centre");
        const auto e = scene::georef::convert("EPSG:4979", "EPSG:4978", {glm::dvec3(c[1].get<double>(), c[0].get<double>(), c.size() > 2 ? c[2].get<double>() : 0.0)});
        if (!e)
            throw CommandError("import3dTiles: can't convert centre=");
        im.centreEcef = e->front();
    }

    // Where the tileset is: its root's bounding volume centre, for the scene's georeference when it has none.
    const glm::dmat4 rootTransform = root["root"].contains("transform") ? matrixOf(root["root"]["transform"]) : glm::dmat4(1.0);
    std::optional<glm::dvec3> where = im.centreEcef;
    if (!where && root["root"].contains("boundingVolume"))
        if (const auto s = im.sphereOf(root["root"]["boundingVolume"], rootTransform))
            where = s->first;
    std::string geoMessage;
    std::optional<scene::georef::Georeference> geo = scene::georef::current();
    if (!geo)
    {
        if (!where || glm::length(*where) < 1e6 || !a.get<bool>("georeference", true))
            throw CommandError("import3dTiles: the scene has no georeference and the tileset's place on the Earth is unknown; set one "
                               "with setGeoreference first");
        const auto llh = scene::georef::convert("EPSG:4978", "EPSG:4979", {*where});
        if (!llh)
            throw CommandError("import3dTiles: PROJ can't convert the tileset's centre");
        const glm::dvec3 lonLatH = llh->front();
        const int zone = std::clamp(int(std::floor((lonLatH.x + 180.0) / 6.0)) + 1, 1, 60);
        const std::string crs = std::format("EPSG:{}", (lonLatH.y >= 0.0 ? 32600 : 32700) + zone);
        ops::execute("setGeoreference", {{"crs", crs}, {"latLonHeight", {lonLatH.y, lonLatH.x, std::round(lonLatH.z)}}});
        geo = scene::georef::current();
        geoMessage = std::format("Set the scene's georeference from the tileset: {} at {:.6f}, {:.6f}", crs, lonLatH.y, lonLatH.x);
    }
    else
        geoMessage = "Placed into the scene's georeference (" + geo->crs + ")";
    im.geo = *geo;

    im.work = paths::userConfigDir() / "cache" / "3dtiles" / paths::fromUtf8(path.parent_path().filename().string() + "_" + path.stem().string());
    std::error_code ec;
    fs::create_directories(im.work / "textures", ec);
    const std::string name = a.get<std::string>("name", tileset ? path.parent_path().filename().string() : path.stem().string());
    im.root = g.createNode(*scene::types::find("transform"), name.empty() ? std::string("Tileset") : name, scene::kNoNode, true)->id();
    im.visit(root["root"], glm::dmat4(1.0), root["root"].value("refine", std::string("REPLACE")), path.parent_path(), 0);

    log::info("import3dTiles: {}; {} tile(s), {} mesh root(s), {} point(s){}", geoMessage, im.tiles, im.meshes, im.points,
              im.culled ? std::format(", {} tile(s) outside the radius", im.culled) : std::string());
    if (im.truncated)
        log::warn("import3dTiles: stopped at maxTiles={}; raise it or give a larger maxError or a smaller radius", im.maxTiles);
    for (const std::string& s : im.skipped)
        log::warn("import3dTiles: skipped {}", s);
    g.select({im.root});
    return {{"node", g.node(im.root)->path()}, {"tiles", im.tiles}, {"meshes", im.meshes}, {"points", im.points}, {"culled", im.culled},
            {"truncated", im.truncated}, {"skipped", im.skipped}, {"georeference", geoMessage}};
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "import3dTiles",
        .doc = "Import a 3D Tiles tileset (tileset.json, 1.0 or 1.1) from a local folder, or a single b3dm, i3dm, pnts, cmpt or glb "
               "tile. Tiles are picked by geometric error: along each branch the first tile within maxError, or the finest. "
               "Meshes come in through the glTF importer (Draco and meshopt included) and point tiles as point clouds, all "
               "placed from the tileset's Earth-centred frame into the scene's georeference; a scene without one takes the "
               "UTM zone around the tileset. Returns the group and counts; skipped content is listed and logged.",
        .positionalDoc = "path",
        .flags = {{"maxError", "", T::Float, 0.0, "Geometric error to stop refining at, metres; 0 takes the finest tiles.", {}, {}, 0.0},
                  {"centre", "", T::List, nullptr, "With radius: only tiles reaching within radius of this [latitude, longitude, height]."},
                  {"radius", "", T::Float, 0.0, "Metres around centre (or the tileset's centre) to bring in; 0 takes everything.", {}, {}, 0.0},
                  {"maxTiles", "", T::Int, 1000, "Stop after this many tiles, and say so.", {}, {}, 1.0},
                  {"georeference", "", T::Bool, true, "Set the scene's georeference from the tileset when it has none."},
                  {"name", "n", T::String, nullptr, "Name of the group the tiles go under."}},
        .run = import3dTiles,
        .examples = {"import3dTiles(\"D:/city/tileset.json\", maxError=2, centre=[45.8257, 11.5324], radius=500)"},
    });
    if (!io::translators::find("3dTiles"))
        io::translators::add({.id = "3dTiles",
                              .label = "3D Tiles",
                              .doc = "3D Tiles content (b3dm, i3dm, pnts, cmpt), placed on the Earth; import a whole tileset's tileset.json with import3dTiles.",
                              .extensions = {".b3dm", ".i3dm", ".pnts", ".cmpt"},
                              .import = {.command = "import3dTiles"}});
}
} // namespace lila::tiles
