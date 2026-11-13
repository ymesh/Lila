#include "ifc/IfcImport.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <web-ifc/geometry/IfcGeometryProcessor.h>
#include <web-ifc/parsing/IfcLoader.h>
#include <web-ifc/schema/IfcSchemaManager.h>
#include <web-ifc/schema/ifc-schema.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <span>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace lila::ifc
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;
using scene::NodeId;
using webifc::parsing::IfcLoader;
using webifc::parsing::IfcTokenType;
namespace ws = webifc::schema;

namespace
{
/** One attribute of an IFC line as a JSON value: text, number, bool, reference id, list, or null when unset. */
json readValue(const IfcLoader& l)
{
    const IfcTokenType t = l.GetTokenType();
    l.StepBack();
    switch (t)
    {
    case IfcTokenType::STRING: return l.GetDecodedStringArgument();
    case IfcTokenType::REAL: return l.GetDoubleArgument();
    case IfcTokenType::INTEGER: return l.GetIntArgument();
    case IfcTokenType::ENUM:
    {
        const std::string e(l.GetStringArgument());
        if (e == "T")
            return true;
        if (e == "F")
            return false;
        return e;
    }
    case IfcTokenType::REF: return l.GetRefArgument();
    case IfcTokenType::SET_BEGIN:
    {
        l.GetTokenType();
        json list = json::array();
        while (l.GetTokenType() != IfcTokenType::SET_END)
        {
            l.StepBack();
            list.push_back(readValue(l));
        }
        return list;
    }
    case IfcTokenType::LABEL:
    {
        // A typed value, IFCLABEL('x'): the label, then the value in parentheses.
        l.GetStringArgument();
        l.GetTokenType(); // SET_BEGIN
        json v = readValue(l);
        l.GetTokenType(); // SET_END
        return v;
    }
    default: l.GetTokenType(); return nullptr;
    }
}

json argument(const IfcLoader& l, uint32_t id, uint32_t index)
{
    l.MoveToArgumentOffset(id, index);
    return readValue(l);
}

std::string text(const IfcLoader& l, uint32_t id, uint32_t index)
{
    const json v = argument(l, id, index);
    return v.is_string() ? v.get<std::string>() : std::string();
}

std::vector<uint32_t> refs(const IfcLoader& l, uint32_t id, uint32_t index)
{
    l.MoveToArgumentOffset(id, index);
    std::vector<uint32_t> out;
    if (l.GetTokenType() != IfcTokenType::SET_BEGIN)
        return out;
    l.StepBack();
    for (uint32_t offset : l.GetSetArgument())
        out.push_back(l.GetRefArgument(offset));
    return out;
}

/** Lila's attr name for an IFC property: letters, digits and underscores, starting with a letter. */
std::string attrName(const std::string& set, const std::string& prop)
{
    std::string s = set + "_" + prop;
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0])))
        s = "ifc_" + s;
    return s;
}

/** web-ifc hands geometry over Y up, as WebGL wants it: (x, z, -y) of IFC's own. This turns it back. */
const glm::dmat4 kFromWebIfc(glm::dvec4(1, 0, 0, 0), glm::dvec4(0, 0, 1, 0), glm::dvec4(0, -1, 0, 0), glm::dvec4(0, 0, 0, 1));

/** IFC's own coordinates (metres, Z up) to the scene: through the map conversion into the scene's georeference, or as metres. */
struct Placing
{
    glm::dmat4 toScene{1.0};
    std::string message;
};

/** IfcCompoundPlaneAngleMeasure (degrees, minutes, seconds, millionths of a second, all with the same sign) in degrees. */
std::optional<double> compoundAngle(const json& v)
{
    if (!v.is_array() || v.size() < 3)
        return std::nullopt;
    const double scale[] = {1.0, 1.0 / 60.0, 1.0 / 3600.0, 1.0 / 3.6e9};
    double deg = 0.0;
    for (size_t i = 0; i < std::min<size_t>(v.size(), 4); ++i)
        if (v[i].is_number())
            deg += v[i].get<double>() * scale[i];
    return deg;
}

/**
 * Without an IfcMapConversion (IFC2x3, and IFC4 files that never got one): IfcSite's RefLatitude, RefLongitude and
 * RefElevation place the site origin, in the UTM zone around it, with the model's Y axis turned to its context's
 * TrueNorth. Local metres become grid metres through the grid's convergence and scale at the site.
 */
std::optional<Placing> sitePlacing(const IfcLoader& l, bool zUp)
{
    for (uint32_t site : l.GetExpressIDsWithType(ws::IFCSITE))
    {
        const auto lat = compoundAngle(argument(l, site, 9));
        const auto lon = compoundAngle(argument(l, site, 10));
        if (!lat || !lon)
            continue;
        const json elevation = argument(l, site, 11);
        const double h = elevation.is_number() ? elevation.get<double>() : 0.0;
        const int zone = std::clamp(int(std::floor((*lon + 180.0) / 6.0)) + 1, 1, 60);
        const std::string crs = std::format("EPSG:{}", (*lat >= 0.0 ? 32600 : 32700) + zone);
        const auto origin = scene::georef::fromGeographic(crs, {*lat, *lon, h});
        if (!origin)
            return std::nullopt;
        // True north in the model: the first 3D context's TrueNorth direction, else +Y.
        glm::dvec2 north(0.0, 1.0);
        for (uint32_t ctx : l.GetExpressIDsWithType(ws::IFCGEOMETRICREPRESENTATIONCONTEXT))
        {
            const json dir = argument(l, ctx, 5);
            if (dir.is_number_integer() || dir.is_number_unsigned())
            {
                const json ratios = argument(l, uint32_t(dir.get<uint64_t>()), 0);
                if (ratios.is_array() && ratios.size() >= 2 && ratios[0].is_number() && ratios[1].is_number())
                {
                    const glm::dvec2 d(ratios[0].get<double>(), ratios[1].get<double>());
                    if (glm::length(d) > 1e-12)
                        north = glm::normalize(d);
                }
            }
            break;
        }
        // A metre east and north of the site on the ellipsoid, in grid metres: the grid's convergence and scale there.
        constexpr double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2.0 - f);
        const double phi = glm::radians(*lat), w = std::sqrt(1.0 - e2 * std::sin(phi) * std::sin(phi));
        const double metresPerDegLat = glm::radians(a * (1.0 - e2) / (w * w * w)), metresPerDegLon = glm::radians(a / w * std::cos(phi));
        const auto eastOf = scene::georef::fromGeographic(crs, {*lat, *lon + 1.0 / metresPerDegLon, h});
        const auto northOf = scene::georef::fromGeographic(crs, {*lat + 1.0 / metresPerDegLat, *lon, h});
        if (!eastOf || !northOf)
            return std::nullopt;
        const glm::dvec2 gridEast = glm::dvec2(*eastOf - *origin), gridNorth = glm::dvec2(*northOf - *origin);
        // Model x, y to true east, north (TrueNorth gives where north points in the model), then to grid metres.
        const glm::dvec2 modelEast(north.y, -north.x);
        const glm::dmat2 toTrue = glm::transpose(glm::dmat2(modelEast, north));
        const glm::dmat2 toGrid = glm::dmat2(gridEast, gridNorth) * toTrue;
        glm::dmat4 toMap(1.0);
        toMap[0] = glm::dvec4(toGrid[0], 0.0, 0.0);
        toMap[1] = glm::dvec4(toGrid[1], 0.0, 0.0);
        const scene::georef::Georeference file{crs, *origin};
        const scene::georef::Placement p = scene::georef::place(file);
        Placing out;
        out.toScene = p.matrix * scene::georef::localFrame(file, zUp) * toMap;
        out.message = std::format("{} (IfcSite {:.6f}, {:.6f}: no IfcMapConversion)", p.message, *lat, *lon);
        return out;
    }
    return std::nullopt;
}

Placing placing(const IfcLoader& l, bool useGeo)
{
    Placing out;
    const bool zUp = scene::georef::sceneZUp();
    glm::dmat4 metres(100.0);
    metres[3][3] = 1.0;
    if (!zUp)
        metres = glm::dmat4(glm::dvec4(100, 0, 0, 0), glm::dvec4(0, 0, -100, 0), glm::dvec4(0, 100, 0, 0), glm::dvec4(0, 0, 0, 1));
    out.toScene = metres;
    out.message = "No IfcMapConversion: not georeferenced, read as metres";
    if (!useGeo)
        return out;
    const std::vector<uint32_t> conversions = l.GetExpressIDsWithType(ws::IFCMAPCONVERSION);
    if (conversions.empty())
    {
        if (auto site = sitePlacing(l, zUp))
            return *site;
        return out;
    }
    const uint32_t mc = conversions.front();
    const json target = argument(l, mc, 1);
    if (!target.is_number_unsigned() && !target.is_number_integer())
        return out;
    std::string crs = text(l, uint32_t(target.get<uint64_t>()), 0);
    if (!crs.empty() && std::all_of(crs.begin(), crs.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        crs = "EPSG:" + crs;
    if (!scene::georef::usable(scene::georef::crsInfo(crs)))
    {
        out.message = "IfcMapConversion names the CRS '" + crs + "', which PROJ doesn't know as a projected CRS: not georeferenced";
        return out;
    }
    auto number = [&](uint32_t i, double fallback) {
        const json v = argument(l, mc, i);
        return v.is_number() ? v.get<double>() : fallback;
    };
    const glm::dvec3 origin(number(2, 0.0), number(3, 0.0), number(4, 0.0));
    glm::dvec2 axis(number(5, 1.0), number(6, 0.0));
    axis = glm::length(axis) > 1e-12 ? glm::normalize(axis) : glm::dvec2(1.0, 0.0);
    const double s = number(7, 1.0);
    // Engineering x, y, z (metres) to easting, northing, height offsets: turned so x runs along the map's x axis, scaled.
    glm::dmat4 toMap(1.0);
    toMap[0] = glm::dvec4(s * axis.x, s * axis.y, 0.0, 0.0);
    toMap[1] = glm::dvec4(-s * axis.y, s * axis.x, 0.0, 0.0);
    const scene::georef::Georeference file{crs, origin};
    const scene::georef::Placement p = scene::georef::place(file);
    out.toScene = p.matrix * scene::georef::localFrame(file, zUp) * toMap;
    out.message = p.message + " (IfcMapConversion)";
    return out;
}

/** "IFC_RRGGBB": IFC's colour (display values) as bytes, one material per distinct colour, shared like CadImport's. */
std::string materialName(const glm::vec4& c)
{
    auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return std::format("IFC_{:02X}{:02X}{:02X}", byte(c.r), byte(c.g), byte(c.b));
}

/** Each face's material slot from its colour, the materials made on first use with that colour and its opacity. */
void assignMaterials(Graph& g, geo::Mesh& mesh, const std::vector<glm::vec4>& faceColors)
{
    std::map<std::string, int32_t> slots;
    std::span<int32_t> m = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (geo::Index f = 0; f < geo::Index(faceColors.size()); ++f)
    {
        const std::string name = materialName(faceColors[size_t(f)]);
        auto [it, made] = slots.try_emplace(name, int32_t(mesh.materialNames().size()));
        if (made)
        {
            mesh.materialNames().push_back(name);
            if (!scene::materials::find(g, name))
            {
                Node* mat = scene::materials::create(g, name);
                const glm::vec4& c = faceColors[size_t(f)];
                auto linear = [](double s) { return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4); };
                g.setValue({mat->id(), scene::material::kBaseColor, -1}, glm::dvec3(linear(c.r), linear(c.g), linear(c.b)));
                if (c.a < 0.999f)
                {
                    // Glazing and other see-through elements.
                    g.setValue({mat->id(), scene::material::kOpacity, -1}, double(c.a));
                    g.setValue({mat->id(), scene::material::kBlendMode, -1}, int64_t(2));
                }
            }
        }
        m[size_t(f)] = it->second;
    }
}

json importIfc(const Args& a)
{
    LILA_ZONE();
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("importIfc: path required");
    const fs::path path = paths::fromUtf8(a.positional[0].get<std::string>());
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw CommandError("importIfc: can't open " + path.string());
    progress::Scope scope("Importing " + path.filename().string(), 3.0);

    const ws::IfcSchemaManager schema;
    // web-ifc's own defaults (its LoaderSettings), with the plane refits of its native tools.
    constexpr uint32_t kTape = 67108864, kLineWriter = 10000;
    constexpr uint64_t kMemory = 2147483648ull;
    IfcLoader loader(kTape, kMemory, kLineWriter, schema);
    {
        progress::Scope read("Reading", 0.0, progress::None, 1.0);
        if (!loader.LoadFile(in))
            throw CommandError("importIfc: " + path.filename().string() + " is not a readable IFC file");
    }
    webifc::geometry::IfcGeometryProcessor geometry(loader, schema, uint16_t(a.get<int64_t>("circleSegments")), false, 1.0e-4, 1.0e-4, 3.0e-4, 1.0e-10, 1.0e-4,
                                                   10, 150);
    const Placing place = placing(loader, a.get<bool>("georeference"));

    // The spatial tree: what aggregates what, and which elements each structure contains.
    std::unordered_map<uint32_t, uint32_t> parentOf;
    std::vector<uint32_t> spatial;
    for (uint32_t rel : loader.GetExpressIDsWithType(ws::IFCRELAGGREGATES))
    {
        const json parent = argument(loader, rel, 4);
        if (!parent.is_number())
            continue;
        for (uint32_t child : refs(loader, rel, 5))
            parentOf[child] = uint32_t(parent.get<uint64_t>());
    }
    for (uint32_t rel : loader.GetExpressIDsWithType(ws::IFCRELCONTAINEDINSPATIALSTRUCTURE))
    {
        const json parent = argument(loader, rel, 5);
        if (!parent.is_number())
            continue;
        for (uint32_t child : refs(loader, rel, 4))
            parentOf[child] = uint32_t(parent.get<uint64_t>());
    }
    for (uint32_t type : {ws::IFCPROJECT, ws::IFCSITE, ws::IFCBUILDING, ws::IFCBUILDINGSTOREY, ws::IFCSPACE})
        for (uint32_t id : loader.GetExpressIDsWithType(type))
            spatial.push_back(id);

    // Property sets by the objects they describe.
    std::unordered_map<uint32_t, std::vector<std::pair<std::string, json>>> props;
    if (a.get<bool>("properties"))
        for (uint32_t rel : loader.GetExpressIDsWithType(ws::IFCRELDEFINESBYPROPERTIES))
        {
            const json def = argument(loader, rel, 5);
            if (!def.is_number() || loader.GetLineType(uint32_t(def.get<uint64_t>())) != ws::IFCPROPERTYSET)
                continue;
            const uint32_t pset = uint32_t(def.get<uint64_t>());
            const std::string setName = text(loader, pset, 2);
            std::vector<std::pair<std::string, json>> values;
            for (uint32_t p : refs(loader, pset, 4))
                if (loader.GetLineType(p) == ws::IFCPROPERTYSINGLEVALUE)
                {
                    const json v = argument(loader, p, 2);
                    if (!v.is_null())
                        values.emplace_back(attrName(setName, text(loader, p, 0)), v);
                }
            for (uint32_t obj : refs(loader, rel, 4))
                props[obj].insert(props[obj].end(), values.begin(), values.end());
        }

    Graph& g = Graph::current();
    Graph::Batch batch(g);
    std::unordered_map<uint32_t, NodeId> made;
    auto label = [&](uint32_t id) {
        std::string name = text(loader, id, 2);
        if (name.empty())
            name = schema.IfcTypeCodeToType(loader.GetLineType(id));
        // Node names take letters, digits and underscores: "Wall A" becomes Wall_A.
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                c = '_';
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
            name = "Ifc_" + name;
        return Graph::validName(name) ? name : std::string("Ifc");
    };
    auto tag = [&](Node& n, uint32_t id) {
        auto addString = [&](const std::string& attr, const std::string& value) {
            try
            {
                const int k = g.addAttr(n.id(), {.name = attr, .type = scene::PlugType::String, .defaultValue = std::string()});
                g.setValue({n.id(), k, -1}, value);
            }
            catch (const std::invalid_argument&)
            {
            }
        };
        addString("ifcType", schema.IfcTypeCodeToType(loader.GetLineType(id)));
        addString("ifcGlobalId", text(loader, id, 0));
        if (auto it = props.find(id); it != props.end())
            for (const auto& [attr, v] : it->second)
                try
                {
                    if (v.is_boolean())
                        g.setValue({n.id(), g.addAttr(n.id(), {.name = attr, .type = scene::PlugType::Bool, .defaultValue = false}), -1}, v.get<bool>());
                    else if (v.is_number())
                        g.setValue({n.id(), g.addAttr(n.id(), {.name = attr, .type = scene::PlugType::Float, .defaultValue = 0.0}), -1}, v.get<double>());
                    else
                        g.setValue({n.id(), g.addAttr(n.id(), {.name = attr, .type = scene::PlugType::String, .defaultValue = std::string()}), -1},
                                   v.is_string() ? v.get<std::string>() : v.dump());
                }
                catch (const std::invalid_argument&)
                {
                }
    };
    // Groups for the spatial structure, parents first.
    std::function<NodeId(uint32_t)> groupFor = [&](uint32_t id) -> NodeId {
        if (auto it = made.find(id); it != made.end())
            return it->second;
        NodeId parent = scene::kNoNode;
        if (auto p = parentOf.find(id); p != parentOf.end() && p->second != id)
            parent = groupFor(p->second);
        Node* n = g.createNode(*scene::types::find("transform"), label(id), parent, true);
        tag(*n, id);
        made[id] = n->id();
        return n->id();
    };
    for (uint32_t id : spatial)
        groupFor(id);

    size_t meshes = 0, triangles = 0;
    std::vector<std::string> failed;
    const std::vector<uint32_t> elementTypes(schema.GetIfcElementList().begin(), schema.GetIfcElementList().end());
    progress::Scope build("Building elements", double(elementTypes.size()), progress::None, 2.0);
    for (uint32_t type : elementTypes)
    {
        build.step();
        if (type == ws::IFCOPENINGELEMENT || type == ws::IFCSPACE)
            continue;
        for (uint32_t id : loader.GetExpressIDsWithType(type))
        {
            geo::Mesh mesh;
            std::vector<glm::vec4> faceColors;
            try
            {
                const webifc::geometry::IfcFlatMesh flat = geometry.GetFlatMesh(id);
                for (const webifc::geometry::IfcPlacedGeometry& placed : flat.geometries)
                {
                    webifc::geometry::IfcGeometry& geom = geometry.GetGeometry(placed.geometryExpressID);
                    const glm::dmat4 m = place.toScene * kFromWebIfc * placed.transformation;
                    const size_t verts = geom.vertexData.size() / 6;
                    const geo::Index base = geo::Index(mesh.pointCapacity());
                    for (size_t v = 0; v < verts; ++v)
                        mesh.addPoint(glm::dvec3(m * glm::dvec4(geom.vertexData[v * 6], geom.vertexData[v * 6 + 1], geom.vertexData[v * 6 + 2], 1.0)));
                    const bool flip = glm::determinant(glm::dmat3(m)) < 0.0;
                    for (size_t t = 0; t + 2 < geom.indexData.size(); t += 3)
                    {
                        const geo::Index i0 = base + geom.indexData[t], i1 = base + geom.indexData[t + 1], i2 = base + geom.indexData[t + 2];
                        if (i0 == i1 || i1 == i2 || i0 == i2)
                            continue;
                        if (flip)
                            mesh.addFace({i0, i2, i1});
                        else
                            mesh.addFace({i0, i1, i2});
                        faceColors.emplace_back(placed.color);
                    }
                }
            }
            catch (const std::exception& e)
            {
                failed.push_back(std::to_string(id) + ": " + e.what());
                continue;
            }
            if (mesh.numFaces() == 0)
                continue;
            // Each face's colour on its corners, then the triangles welded where web-ifc split them.
            std::span<glm::vec4> colors = geo::colors::add(mesh, "ifcColor");
            for (geo::Index face = 0; face < geo::Index(faceColors.size()); ++face)
            {
                geo::Index c = mesh.faceCorner(face);
                for (int k = 0; k < 3; ++k, c = mesh.cornerNext(c))
                    colors[size_t(c)] = faceColors[size_t(face)];
            }
            geo::colors::setCurrent(mesh, "ifcColor");
            assignMaterials(g, mesh, faceColors);
            geo::mergeByDistance(mesh, {}, {.distance = 0.01});
            NodeId parent = scene::kNoNode;
            if (auto p = parentOf.find(id); p != parentOf.end())
                parent = groupFor(p->second);
            Node* n = g.createNode(*scene::types::find("mesh"), label(id), parent, true);
            triangles += size_t(mesh.numFaces());
            g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
            tag(*n, id);
            ++meshes;
        }
    }
    log::info("importIfc: {}", place.message);
    if (!failed.empty())
        log::warn("importIfc: {} element(s) had no geometry web-ifc could build, first {}", failed.size(), failed.front());
    std::vector<NodeId> roots;
    for (const auto& [ifcId, node] : made)
        if (g.parent(node) == scene::kNoNode)
            roots.push_back(node);
    g.select(roots);
    return {{"meshes", meshes}, {"groups", made.size()}, {"triangles", triangles}, {"failed", failed.size()}, {"georeference", place.message},
            {"schema", std::string(schema.GetSchemaName(loader.GetSchema()))}};
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "importIfc",
        .doc = "Import an IFC file (IFC2x3, IFC4, IFC4x3): each building element a mesh with its colours in the ifcColor set, the spatial "
               "structure (project, site, building, storeys, spaces) as groups, and every property set value as an attribute named "
               "<set>_<property>, with ifcType and ifcGlobalId.\n\n"
               "IfcMapConversion places it: it sets the scene's georeference or moves the model into it; without one the model is read "
               "as metres. The log says which. Returns the counts.",
        .positionalDoc = "path",
        .flags = {{"georeference", "", T::Bool, true, "Place it by its IfcMapConversion."},
                  {"properties", "", T::Bool, true, "Bring the property sets in as attributes."},
                  {"circleSegments", "", T::Int, 12, "Segments a full circle is tessellated with.", {}, "", 3, 256}},
        .run = importIfc,
        .examples = {"importIfc(\"D:/bim/school.ifc\")"},
    });
}
} // namespace lila::ifc
