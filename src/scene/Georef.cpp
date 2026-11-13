#include "scene/Georef.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/SceneData.h"
#include "scene/Transform.h"

#include <proj.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace lila::scene::georef
{
namespace fs = std::filesystem;


namespace
{
constexpr const char* kKey = "georeference";

/** One PROJ context for the whole app, behind a lock, with transforms cached by their two CRSs. */
struct Proj
{
    std::mutex mutex;
    PJ_CONTEXT* ctx = nullptr;
    std::map<std::pair<std::string, std::string>, PJ*> transforms;

    Proj()
    {
        ctx = proj_context_create();
        // proj.db ships next to the binaries (share/proj); PROJ_DATA still wins when it is set.
        std::vector<std::string> dirs;
        for (const fs::path& d : {paths::executableDir() / "share" / "proj", paths::executableDir() / ".." / "share" / "proj", paths::resourcesDir() / "proj"})
            if (fs::exists(d / "proj.db"))
                dirs.push_back(paths::toUtf8(d));
        std::vector<const char*> c;
        for (const std::string& d : dirs)
            c.push_back(d.c_str());
        if (!c.empty() && !std::getenv("PROJ_DATA") && !std::getenv("PROJ_LIB"))
            proj_context_set_search_paths(ctx, int(c.size()), c.data());
        proj_context_set_enable_network(ctx, 0);
        proj_log_level(ctx, PJ_LOG_NONE);
    }

    /** from to to, axes in east/north or lon/lat order. Null when PROJ has no way between them. Locked by the caller. */
    PJ* transform(const std::string& from, const std::string& to)
    {
        auto key = std::make_pair(from, to);
        if (auto it = transforms.find(key); it != transforms.end())
            return it->second;
        PJ* raw = proj_create_crs_to_crs(ctx, from.c_str(), to.c_str(), nullptr);
        PJ* t = raw ? proj_normalize_for_visualization(ctx, raw) : nullptr;
        if (raw)
            proj_destroy(raw);
        transforms[key] = t;
        return t;
    }
};

Proj& proj()
{
    // Never destroyed: PROJ objects may still be in use while statics go away at exit.
    static Proj* p = new Proj();
    return *p;
}

/** The geographic CRS a CRS's datum uses (its lat/long), as a string PROJ takes; WGS 84 when it has none. */
std::string geographicOf(PJ_CONTEXT* ctx, const std::string& crs)
{
    PJ* c = proj_create(ctx, crs.c_str());
    std::string out = "EPSG:4979";
    if (c)
    {
        if (PJ* geo = proj_crs_get_geodetic_crs(ctx, c))
        {
            if (const char* wkt = proj_as_wkt(ctx, geo, PJ_WKT2_2019, nullptr))
                out = wkt;
            proj_destroy(geo);
        }
        proj_destroy(c);
    }
    return out;
}

/** The axis conversion factor to metres of a coordinate system's first axis, 1 when it has none. */
double axisToMetre(PJ_CONTEXT* ctx, PJ* crs, int axis, std::string* unit)
{
    double factor = 1.0;
    if (PJ* cs = proj_crs_get_coordinate_system(ctx, crs))
    {
        const char* name = nullptr;
        if (axis < proj_cs_get_axis_count(ctx, cs))
        {
            proj_cs_get_axis_info(ctx, cs, axis, nullptr, nullptr, nullptr, &factor, &name, nullptr, nullptr);
            if (unit && name)
                *unit = name;
        }
        proj_destroy(cs);
    }
    return factor > 0.0 ? factor : 1.0;
}

json toJson(const Georeference& g) { return {{"crs", g.crs}, {"origin", {g.origin.x, g.origin.y, g.origin.z}}}; }

glm::dvec3 vec3(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string(what) + ": expected [x, y, z], got " + j.dump());
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

/** Scene cm per CRS unit, horizontally and vertically. */
glm::dvec2 cmPerUnit(const Georeference& g)
{
    const CrsInfo info = crsInfo(g.crs);
    return {info.unitToMetre * 100.0, info.verticalToMetre * 100.0};
}
} // namespace

std::optional<Georeference> current()
{
    const json j = data::get(kKey);
    if (!j.is_object() || !j.contains("crs"))
        return std::nullopt;
    Georeference g;
    g.crs = j.value("crs", "");
    if (const json& o = j.value("origin", json()); o.is_array() && o.size() == 3)
        g.origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
    return g;
}

void setCurrent(const std::optional<Georeference>& g)
{
    if (g)
    {
        const CrsInfo info = crsInfo(g->crs);
        if (!info.valid)
            throw CommandError("georeference: PROJ doesn't know the CRS '" + g->crs + "'" + (info.error.empty() ? "" : ": " + info.error));
        if (!usable(info))
            throw CommandError("georeference: '" + info.name + "' is not a projected CRS; give a projected one (UTM, a national grid) so scene axes map onto it");
    }
    data::set(kKey, g ? toJson(*g) : json(nullptr));
}

CrsInfo crsInfo(const std::string& crs)
{
    CrsInfo info;
    if (crs.empty())
    {
        info.error = "no CRS given";
        return info;
    }
    Proj& p = proj();
    std::scoped_lock lock(p.mutex);
    PJ* c = proj_create(p.ctx, crs.c_str());
    if (!c)
    {
        info.error = proj_context_errno_string(p.ctx, proj_context_errno(p.ctx));
        return info;
    }
    info.valid = true;
    if (const char* n = proj_get_name(c))
        info.name = n;
    if (const char* auth = proj_get_id_auth_name(c, 0))
        if (const char* code = proj_get_id_code(c, 0))
            info.authority = std::string(auth) + ":" + code;
    PJ* horizontal = c;
    PJ* vertical = nullptr;
    PJ_TYPE type = proj_get_type(c);
    if (type == PJ_TYPE_COMPOUND_CRS)
    {
        horizontal = proj_crs_get_sub_crs(p.ctx, c, 0);
        vertical = proj_crs_get_sub_crs(p.ctx, c, 1);
        type = horizontal ? proj_get_type(horizontal) : PJ_TYPE_UNKNOWN;
    }
    info.projected = type == PJ_TYPE_PROJECTED_CRS;
    info.geographic = type == PJ_TYPE_GEOGRAPHIC_2D_CRS || type == PJ_TYPE_GEOGRAPHIC_3D_CRS;
    if (horizontal)
        info.unitToMetre = axisToMetre(p.ctx, horizontal, 0, &info.unitName);
    info.verticalToMetre = vertical ? axisToMetre(p.ctx, vertical, 0, nullptr) : info.unitToMetre;
    if (info.geographic)
        info.verticalToMetre = horizontal && proj_get_type(horizontal) == PJ_TYPE_GEOGRAPHIC_3D_CRS ? axisToMetre(p.ctx, horizontal, 2, nullptr) : 1.0;
    if (horizontal && horizontal != c)
        proj_destroy(horizontal);
    if (vertical)
        proj_destroy(vertical);
    proj_destroy(c);
    return info;
}

bool usable(const CrsInfo& info) { return info.valid && info.projected; }

glm::dvec3 toProjected(const Georeference& g, const glm::dvec3& p, bool zUp)
{
    const glm::dvec2 k = cmPerUnit(g);
    const double east = zUp ? p.x : p.z, north = zUp ? p.y : p.x, up = zUp ? p.z : p.y;
    return g.origin + glm::dvec3(east / k.x, north / k.x, up / k.y);
}

glm::dvec3 fromProjected(const Georeference& g, const glm::dvec3& q, bool zUp)
{
    const glm::dvec2 k = cmPerUnit(g);
    const glm::dvec3 d = q - g.origin;
    const double east = d.x * k.x, north = d.y * k.x, up = d.z * k.y;
    return zUp ? glm::dvec3(east, north, up) : glm::dvec3(north, up, east);
}

std::optional<glm::dvec3> toGeographic(const std::string& crs, const glm::dvec3& projected)
{
    Proj& p = proj();
    std::string geo;
    {
        std::scoped_lock lock(p.mutex);
        geo = geographicOf(p.ctx, crs);
    }
    const auto out = convert(crs, geo, {projected});
    if (!out)
        return std::nullopt;
    // Normalised for visualisation: longitude first.
    return glm::dvec3((*out)[0].y, (*out)[0].x, (*out)[0].z);
}

std::optional<glm::dvec3> toWgs84(const std::string& crs, const glm::dvec3& projected)
{
    const auto out = convert(crs, "EPSG:4979", {projected});
    if (!out)
        return std::nullopt;
    return glm::dvec3((*out)[0].y, (*out)[0].x, (*out)[0].z);
}

json exportJson()
{
    const std::optional<Georeference> g = current();
    if (!g)
        return nullptr;
    const CrsInfo info = crsInfo(g->crs);
    json j = toJson(*g);
    j["name"] = info.name;
    if (!info.authority.empty())
        j["authority"] = info.authority;
    j["unitToMetre"] = info.unitToMetre;
    j["zUp"] = sceneZUp();
    if (const auto w = toWgs84(g->crs, g->origin))
        j["wgs84"] = {{"latitude", w->x}, {"longitude", w->y}, {"height", w->z}};
    return j;
}

std::optional<glm::dvec3> fromGeographic(const std::string& crs, const glm::dvec3& latLonHeight)
{
    Proj& p = proj();
    std::string geo;
    {
        std::scoped_lock lock(p.mutex);
        geo = geographicOf(p.ctx, crs);
    }
    const auto out = convert(geo, crs, {glm::dvec3(latLonHeight.y, latLonHeight.x, latLonHeight.z)});
    if (!out)
        return std::nullopt;
    return (*out)[0];
}

std::optional<std::vector<glm::dvec3>> convert(const std::string& from, const std::string& to, const std::vector<glm::dvec3>& points)
{
    if (from == to)
        return points;
    Proj& p = proj();
    std::scoped_lock lock(p.mutex);
    PJ* t = p.transform(from, to);
    if (!t)
        return std::nullopt;
    std::vector<glm::dvec3> out;
    out.reserve(points.size());
    for (const glm::dvec3& q : points)
    {
        PJ_COORD c = proj_trans(t, PJ_FWD, proj_coord(q.x, q.y, q.z, 0.0));
        if (!std::isfinite(c.xyz.x) || !std::isfinite(c.xyz.y) || c.xyz.x == HUGE_VAL)
            return std::nullopt;
        out.push_back({c.xyz.x, c.xyz.y, c.xyz.z});
    }
    return out;
}

glm::dmat4 localFrame(const Georeference& file, bool zUp)
{
    const glm::dvec3 o = fromProjected(file, file.origin, zUp);
    glm::dmat4 m(1.0);
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::dvec3 step(0.0);
        step[axis] = 1.0;
        m[axis] = glm::dvec4(fromProjected(file, file.origin + step, zUp) - o, 0.0);
    }
    return m;
}

std::string wkt(const std::string& crs)
{
    Proj& p = proj();
    std::scoped_lock lock(p.mutex);
    PJ* c = proj_create(p.ctx, crs.c_str());
    if (!c)
        return {};
    const char* text = proj_as_wkt(p.ctx, c, PJ_WKT1_GDAL, nullptr);
    std::string out = text ? text : "";
    proj_destroy(c);
    return out;
}

bool sceneZUp() { return PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z"; }

Placement place(const Georeference& file)
{
    const CrsInfo info = crsInfo(file.crs);
    if (!usable(info))
        throw CommandError("georeference: the file's CRS '" + file.crs + "' is " + (info.valid ? "not projected" : "unknown to PROJ") +
                           (info.error.empty() ? "" : " (" + info.error + ")"));
    Placement out;
    const std::optional<Georeference> scene = current();
    const std::string label = info.authority.empty() ? info.name : info.authority + " (" + info.name + ")";
    if (!scene)
    {
        // By its EPSG code when PROJ recognises the file's WKT: what Unreal's georeferencing and Cesium take.
        Georeference adopted = file;
        if (!info.authority.empty())
            adopted.crs = info.authority;
        setCurrent(adopted);
        out.adopted = true;
        out.message = "Set the scene's georeference from the file: " + label;
        return out;
    }
    const bool zUp = sceneZUp();
    const CrsInfo sceneInfo = crsInfo(scene->crs);
    const std::string sceneLabel = sceneInfo.authority.empty() ? sceneInfo.name : sceneInfo.authority;
    // The file's origin and a step east and north of it, carried into the scene's CRS: the move, the turn between the
    // two grids and their scale, as one affine map for geometry near the file's origin.
    const double step = 100.0 / std::max(info.unitToMetre, 1e-12); // 100 m
    const std::vector<glm::dvec3> probe = {file.origin, file.origin + glm::dvec3(step, 0, 0), file.origin + glm::dvec3(0, step, 0),
                                           file.origin + glm::dvec3(0, 0, step)};
    const std::optional<std::vector<glm::dvec3>> moved = convert(file.crs, scene->crs, probe);
    if (!moved)
        throw CommandError("georeference: PROJ can't transform from " + label + " to the scene's " + sceneLabel);
    const glm::dvec3 o = fromProjected(*scene, (*moved)[0], zUp);
    const glm::dvec3 ex = (fromProjected(*scene, (*moved)[1], zUp) - o);
    const glm::dvec3 ny = (fromProjected(*scene, (*moved)[2], zUp) - o);
    const glm::dvec3 uz = (fromProjected(*scene, (*moved)[3], zUp) - o);
    // File-local scene axes (cm) to the step vectors, along the axes toProjected uses.
    const double fileCm = step * info.unitToMetre * 100.0, fileUpCm = step * info.verticalToMetre * 100.0;
    glm::dmat4 m(1.0);
    const glm::dvec3 east = ex / fileCm, north = ny / fileCm, up = uz / fileUpCm;
    if (zUp)
    {
        m[0] = glm::dvec4(east, 0.0);
        m[1] = glm::dvec4(north, 0.0);
        m[2] = glm::dvec4(up, 0.0);
    }
    else
    {
        m[0] = glm::dvec4(north, 0.0);
        m[1] = glm::dvec4(up, 0.0);
        m[2] = glm::dvec4(east, 0.0);
    }
    m[3] = glm::dvec4(o, 1.0);
    out.matrix = m;
    out.message = file.crs == scene->crs ? "Moved into the scene's georeference (" + sceneLabel + ")"
                                         : "Transformed from " + label + " into the scene's georeference (" + sceneLabel + ")";
    return out;
}

void placeRoots(Graph& g, const std::vector<uint64_t>& roots, const glm::dmat4& m)
{
    if (m == glm::dmat4(1.0))
        return;
    g.evaluate();
    for (uint64_t id : roots)
    {
        Node* n = g.node(NodeId(id));
        if (!n || !n->type().isA("transform"))
            continue;
        const glm::dmat4 local = m * n->as<glm::dmat4>(Transform::kMatrix);
        const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
        glm::dmat3 r(local);
        for (int c = 0; c < 3; ++c)
            r[c] = glm::normalize(r[c]);
        g.setValue({n->id(), Transform::kTranslate, -1}, glm::dvec3(local[3]));
        g.setValue({n->id(), Transform::kRotate, -1}, matrixToEuler(r, order));
    }
}

namespace
{
/** WGS 84 latitude, longitude (degrees) and ellipsoidal height (m) to Earth-centred coordinates (m). */
glm::dvec3 wgs84ToEcef(const glm::dvec3& llh)
{
    constexpr double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2.0 - f);
    const double lat = glm::radians(llh.x), lon = glm::radians(llh.y);
    const double n = a / std::sqrt(1.0 - e2 * std::sin(lat) * std::sin(lat));
    return {(n + llh.z) * std::cos(lat) * std::cos(lon), (n + llh.z) * std::cos(lat) * std::sin(lon), (n * (1.0 - e2) + llh.z) * std::sin(lat)};
}
} // namespace

std::optional<GlobeFrame> globeFrame(const Georeference& g)
{
    const auto llh = toWgs84(g.crs, g.origin);
    if (!llh)
        return std::nullopt;
    GlobeFrame f;
    f.georeference = g;
    f.originEcef = wgs84ToEcef(*llh);
    const double lat = glm::radians(llh->x), lon = glm::radians(llh->y);
    const glm::dvec3 east(-std::sin(lon), std::cos(lon), 0.0);
    const glm::dvec3 north(-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat));
    const glm::dvec3 up(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
    f.ecefToEnu = glm::transpose(glm::dmat3(east, north, up));
    return f;
}

glm::dvec3 GlobeFrame::toGlobe(const glm::dvec3& gridCm) const
{
    const double perUnit = 100.0 * crsInfo(georeference.crs).unitToMetre;
    const glm::dvec3 projected = georeference.origin + glm::dvec3(gridCm.x, -gridCm.y, gridCm.z) / perUnit;
    const auto llh = toWgs84(georeference.crs, projected);
    if (!llh)
        return gridCm;
    const glm::dvec3 enu = ecefToEnu * (wgs84ToEcef(*llh) - originEcef);
    return glm::dvec3(enu.x, -enu.y, enu.z) * 100.0;
}

glm::dmat3 GlobeFrame::rotationAt(const glm::dvec3& gridCm) const
{
    // The globe image of a metre step along each grid axis, made orthonormal: what a rigid object turns by.
    constexpr double step = 100.0;
    const glm::dvec3 at = toGlobe(gridCm);
    glm::dvec3 x = glm::normalize(toGlobe(gridCm + glm::dvec3(step, 0, 0)) - at);
    glm::dvec3 z = toGlobe(gridCm + glm::dvec3(0, 0, step)) - at;
    z = glm::normalize(z - glm::dot(z, x) * x);
    // A proper rotation of these numbers keeps Y = Z cross X.
    return glm::dmat3(x, glm::cross(z, x), z);
}

std::optional<std::string> placeImported(Graph& g, const json& j, const std::vector<uint64_t>& roots)
{
    if (!j.is_object() || !j.contains("crs") || !j.contains("origin"))
        return std::nullopt;
    Georeference file;
    file.crs = j.value("authority", j["crs"].get<std::string>());
    file.origin = vec3(j["origin"], "georeference");
    const Placement p = place(file);
    placeRoots(g, roots, p.matrix);
    log::info("{}", p.message);
    return p.message;
}

std::vector<std::pair<std::string, std::string>> search(const std::string& query, size_t limit)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::string q = query;
    std::ranges::transform(q, q.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Proj& p = proj();
    std::scoped_lock lock(p.mutex);
    PROJ_CRS_LIST_PARAMETERS* params = proj_get_crs_list_parameters_create();
    const PJ_TYPE types[] = {PJ_TYPE_PROJECTED_CRS, PJ_TYPE_COMPOUND_CRS};
    params->typesCount = 2;
    params->types = types;
    int count = 0;
    PROJ_CRS_INFO** list = proj_get_crs_info_list_from_database(p.ctx, "EPSG", params, &count);
    for (int i = 0; i < count && out.size() < limit; ++i)
    {
        const PROJ_CRS_INFO* c = list[i];
        if (c->deprecated)
            continue;
        const std::string code = std::string(c->auth_name) + ":" + c->code;
        std::string name = c->name ? c->name : "";
        std::string hay = code + " " + name;
        std::ranges::transform(hay, hay.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (q.empty() || hay.find(q) != std::string::npos)
            out.emplace_back(code, name);
    }
    proj_crs_info_list_destroy(list);
    proj_get_crs_list_parameters_destroy(params);
    return out;
}

namespace
{
json describe()
{
    const std::optional<Georeference> g = current();
    if (!g)
        return nullptr;
    const CrsInfo info = crsInfo(g->crs);
    json j = exportJson();
    j["unit"] = info.unitName;
    if (const auto ll = toGeographic(g->crs, g->origin))
        j["originLatLonHeight"] = {ll->x, ll->y, ll->z};
    return j;
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "georeference",
        .doc = "The scene's georeference: its CRS, the projected coordinates of the scene origin (origin), that origin's latitude, "
               "longitude and height in the CRS's datum (originLatLonHeight) and in WGS 84 with ellipsoidal height (wgs84). None when "
               "the scene has none.",
        .undoable = false,
        .run = [](const Args&) -> json { return describe(); },
    });
    ops::registerCommand({
        .name = "setGeoreference",
        .doc = "Place the scene on the Earth: a projected CRS and the projected coordinates of the scene origin. Nothing in the scene "
               "moves; imports, exports and readouts use it. Saved with the scene.\n\n"
               "Axes match Unreal's georeferencing: a Z-up scene has east +X and north +Y, a Y-up one north +X and east +Z; the up axis "
               "is height. Scene cm become the CRS's units.",
        .flags = {
            {"crs", "", T::String, nullptr, "EPSG code (\"EPSG:32633\", or with a vertical CRS \"EPSG:32633+5773\"), WKT or a PROJ string.", {},
             "setGeoreference(crs=\"EPSG:32633\", origin=(500000, 4649776, 0))"},
            {"origin", "", T::Vec3, nullptr, "The scene origin's easting, northing and height, in the CRS's units."},
            {"latLonHeight", "llh", T::Vec3, nullptr, "The scene origin as latitude, longitude (degrees) and height instead of origin="},
            {"clear", "", T::Bool, false, "Remove the scene's georeference."},
        },
        .run = [](const Args& a) -> json {
            if (a.get<bool>("clear"))
            {
                setCurrent(std::nullopt);
                return nullptr;
            }
            Georeference g = current().value_or(Georeference{});
            if (a.has("crs"))
                g.crs = a.get<std::string>("crs");
            if (g.crs.empty())
                throw CommandError("setGeoreference: give crs=");
            if (a.has("origin"))
                g.origin = vec3(a.flags.at("origin"), "setGeoreference");
            else if (a.has("latLonHeight"))
            {
                const auto p = fromGeographic(g.crs, vec3(a.flags.at("latLonHeight"), "setGeoreference"));
                if (!p)
                    throw CommandError("setGeoreference: PROJ can't project that latitude and longitude into " + g.crs);
                g.origin = *p;
            }
            setCurrent(g);
            return describe();
        },
        .examples = {"setGeoreference(crs=\"EPSG:32633\", origin=(500000, 4649776, 0))", "setGeoreference(crs=\"EPSG:27700\", llh=(51.5007, -0.1246, 0))"},
    });
    ops::registerCommand({
        .name = "georeferencePlace",
        .doc = "Place nodes from a georeferenced file into the scene: the file's CRS and origin set the scene's georeference when "
               "it has none, otherwise the nodes are moved and turned into the scene's. Importers call it with what the file "
               "carries; returns the message saying which happened.",
        .positionalDoc = "nodes: the imported root transforms",
        .flags = {
            {"crs", "", T::String, nullptr, "The file's CRS: an authority code (EPSG:32632) or WKT."},
            {"origin", "", T::List, nullptr, "The file's origin in that CRS: [easting, northing, height]."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<uint64_t> roots;
            for (const json& name : a.positional)
            {
                const Node* n = g.find(name.get<std::string>());
                if (!n)
                    throw CommandError("georeferencePlace: no node named '" + name.get<std::string>() + "'");
                roots.push_back(uint64_t(n->id()));
            }
            const auto placed = placeImported(g, json{{"crs", a.get<std::string>("crs")}, {"origin", a.flags.at("origin")}}, roots);
            return placed ? json(*placed) : json();
        },
    });
    ops::registerCommand({
        .name = "georeferenceConvert",
        .doc = "Convert points between scene coordinates (cm), the scene CRS's projected coordinates, latitude/longitude/height, or "
               "another CRS.",
        .flags = {
            {"points", "p", T::List, nullptr, "[[x, y, z], ...] in the from= space."},
            {"from", "", T::String, "scene", "scene, projected, geographic (latitude, longitude, height) or a CRS."},
            {"to", "", T::String, "geographic", "scene, projected, geographic or a CRS."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::optional<Georeference> g = current();
            if (!g)
                throw CommandError("georeferenceConvert: the scene has no georeference (setGeoreference)");
            const bool zUp = sceneZUp();
            const std::string from = a.get<std::string>("from"), to = a.get<std::string>("to");
            json out = json::array();
            for (const json& p : a.flags.value("points", json::array()))
            {
                glm::dvec3 v = vec3(p, "georeferenceConvert");
                // Into the scene CRS's projected coordinates first.
                if (from == "scene")
                    v = toProjected(*g, v, zUp);
                else if (from == "geographic")
                {
                    const auto q = fromGeographic(g->crs, v);
                    if (!q)
                        throw CommandError("georeferenceConvert: can't project " + p.dump());
                    v = *q;
                }
                else if (from != "projected")
                {
                    const auto q = convert(from, g->crs, {v});
                    if (!q)
                        throw CommandError("georeferenceConvert: PROJ can't transform from " + from);
                    v = (*q)[0];
                }
                if (to == "scene")
                    v = fromProjected(*g, v, zUp);
                else if (to == "geographic")
                {
                    const auto q = toGeographic(g->crs, v);
                    if (!q)
                        throw CommandError("georeferenceConvert: can't find the latitude of " + p.dump());
                    v = *q;
                }
                else if (to != "projected")
                {
                    const auto q = convert(g->crs, to, {v});
                    if (!q)
                        throw CommandError("georeferenceConvert: PROJ can't transform to " + to);
                    v = (*q)[0];
                }
                out.push_back({v.x, v.y, v.z});
            }
            return out;
        },
    });
    ops::registerCommand({
        .name = "georeferenceSearch",
        .doc = "Projected CRSs in PROJ's EPSG database whose code or name contains the query: [[code, name], ...].",
        .positionalDoc = "query",
        .flags = {{"limit", "", T::Int, 50, "Most results.", {}, "", 1, 5000}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            const std::string q = a.positional.empty() ? "" : a.positional[0].get<std::string>();
            for (const auto& [code, name] : search(q, size_t(a.get<int64_t>("limit"))))
                out.push_back({code, name});
            return out;
        },
    });
}
} // namespace lila::scene::georef
