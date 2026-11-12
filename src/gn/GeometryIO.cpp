#include "gn/GeometryIO.h"

#include "gn/Volume.h"
#include "scene/MeshData.h"

#include <fstream>

namespace lila::gn
{
namespace
{
json blob(const void* data, size_t size)
{
    json::binary_t::container_type bytes(size);
    if (size)
        std::memcpy(bytes.data(), data, size);
    return json::binary(std::move(bytes));
}

template <class T> json blobOf(const std::vector<T>& v) { return blob(v.data(), v.size() * sizeof(T)); }

template <class T> std::vector<T> unblob(const json& j)
{
    if (!j.is_binary())
        throw std::invalid_argument("expected binary data");
    const auto& bytes = j.get_binary();
    if (bytes.size() % sizeof(T))
        throw std::invalid_argument("binary data of the wrong size");
    std::vector<T> out(bytes.size() / sizeof(T));
    if (!out.empty())
        std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

json attributesToJson(const geo::AttributeSet& set)
{
    json out = json::array();
    for (size_t i = 0; i < set.layerCount(); ++i)
    {
        const geo::Attribute& a = set.layer(i);
        out.push_back({{"name", a.name()}, {"type", int(a.type())}, {"data", blob(a.size() ? a.raw(0) : nullptr, a.size() * a.stride())}});
    }
    return out;
}

void attributesFromJson(const json& j, geo::AttributeSet& set, size_t size)
{
    set.resize(size);
    for (const json& e : j)
    {
        geo::Attribute& a = set.add(e.at("name").get<std::string>(), geo::AttrType(e.at("type").get<int>()));
        const auto& bytes = e.at("data").get_binary();
        if (bytes.size() != a.size() * a.stride())
            throw std::invalid_argument("attribute " + a.name() + " has the wrong size");
        if (!bytes.empty())
            std::memcpy(a.raw(0), bytes.data(), bytes.size());
    }
}

json curvesToJson(const CurveSet& c)
{
    json knots = json::array();
    for (const std::vector<double>& k : c.knots)
        knots.push_back(blobOf(k));
    return {{"positions", blobOf(c.positions)}, {"offsets", blobOf(c.offsets)}, {"cyclic", blobOf(c.cyclic)},
            {"pointAttrs", attributesToJson(c.pointAttrs)}, {"curveAttrs", attributesToJson(c.curveAttrs)}, {"knots", knots}};
}

CurveSet curvesFromJson(const json& j)
{
    CurveSet c;
    c.positions = unblob<glm::dvec3>(j.at("positions"));
    c.offsets = unblob<geo::Index>(j.at("offsets"));
    c.cyclic = unblob<uint8_t>(j.at("cyclic"));
    if (c.offsets.size() != c.cyclic.size() + 1 || c.offsets.back() != c.positions.size())
        throw std::invalid_argument("curves don't add up");
    attributesFromJson(j.at("pointAttrs"), c.pointAttrs, c.positions.size());
    attributesFromJson(j.at("curveAttrs"), c.curveAttrs, c.cyclic.size());
    for (const json& k : j.value("knots", json::array()))
        c.knots.push_back(unblob<double>(k));
    return c;
}

json matrixJson(const glm::dmat4& m)
{
    json out = json::array();
    for (int k = 0; k < 16; ++k)
        out.push_back(m[k / 4][k % 4]);
    return out;
}

glm::dmat4 matrixOf(const json& j)
{
    glm::dmat4 m(1.0);
    for (int k = 0; k < 16 && k < int(j.size()); ++k)
        m[k / 4][k % 4] = j[size_t(k)].get<double>();
    return m;
}

json volumeToJson(const Volume& v)
{
    json j = {{"name", v.name}, {"kind", int(v.kind)}, {"dims", {v.dims.x, v.dims.y, v.dims.z}}, {"indexToWorld", matrixJson(v.indexToWorld)},
              {"values", blobOf(v.values)}, {"background", v.background}};
    if (v.isVector())
        j["vectors"] = blobOf(v.vectors);
    return j;
}

Volume volumeFromJson(const json& j)
{
    Volume v;
    v.name = j.at("name").get<std::string>();
    v.kind = Volume::Kind(j.at("kind").get<int>());
    v.dims = {j.at("dims")[0].get<int>(), j.at("dims")[1].get<int>(), j.at("dims")[2].get<int>()};
    v.indexToWorld = matrixOf(j.at("indexToWorld"));
    v.values = unblob<float>(j.at("values"));
    v.background = j.at("background").get<float>();
    if (v.values.size() != size_t(v.dims.x) * size_t(v.dims.y) * size_t(v.dims.z))
        throw std::invalid_argument("volume values don't match its size");
    if (v.isVector())
    {
        v.vectors = unblob<glm::vec3>(j.at("vectors"));
        if (v.vectors.size() != v.values.size())
            throw std::invalid_argument("volume vectors don't match its size");
    }
    return v;
}

json heightfieldToJson(const geo::Heightfield& h)
{
    json masks = json::object();
    for (const auto& [name, values] : h.masks)
        masks[name] = blobOf(values);
    return {{"nx", h.nx}, {"nz", h.nz}, {"cell", h.cell}, {"origin", {h.origin.x, h.origin.y}}, {"heights", blobOf(h.heights)}, {"masks", masks}};
}

geo::Heightfield heightfieldFromJson(const json& j)
{
    geo::Heightfield h;
    h.nx = j.at("nx").get<int>();
    h.nz = j.at("nz").get<int>();
    h.cell = j.at("cell").get<double>();
    h.origin = {j.at("origin")[0].get<double>(), j.at("origin")[1].get<double>()};
    h.heights = unblob<float>(j.at("heights"));
    for (auto it = j.at("masks").begin(); it != j.at("masks").end(); ++it)
        h.masks[it.key()] = unblob<float>(it.value());
    if (h.heights.size() != h.size())
        throw std::invalid_argument("heightfield heights don't match its size");
    return h;
}

/** The value a data field holds at its first element (the whole of it when constant). */
json dataJson(SocketType t, const FieldPtr& f)
{
    if (!f)
        return nullptr;
    switch (t)
    {
    case SocketType::Float: return evaluateSingle<double>(f);
    case SocketType::Int: return evaluateSingle<int64_t>(f);
    case SocketType::Bool: return evaluateSingle<uint8_t>(f) != 0;
    case SocketType::Color:
    {
        glm::dvec4 c = evaluateSingle<glm::dvec4>(f);
        return {c.x, c.y, c.z, c.w};
    }
    case SocketType::Matrix: return matrixJson(evaluateSingle<glm::dmat4>(f));
    default:
    {
        glm::dvec3 v = evaluateSingle<glm::dvec3>(f);
        return {v.x, v.y, v.z};
    }
    }
}
} // namespace

json geometryToJson(const Geometry& g)
{
    json j = json::object();
    if (g.mesh)
        j["mesh"] = scene::meshToJsonPacked(*g.mesh);
    if (g.points)
        j["points"] = scene::meshToJsonPacked(*g.points);
    if (g.curves)
        j["curves"] = curvesToJson(*g.curves);
    if (g.instances)
    {
        const Instances& in = *g.instances;
        json refs = json::array();
        for (const GeometryPtr& r : in.references)
            refs.push_back(r ? geometryToJson(*r) : json(nullptr));
        j["instances"] = {{"transforms", blobOf(in.transforms)}, {"reference", blobOf(in.reference)}, {"references", refs},
                          {"attrs", attributesToJson(in.attrs)}};
    }
    if (g.heightfield)
        j["heightfield"] = heightfieldToJson(*g.heightfield);
    if (g.volume)
        j["volume"] = volumeToJson(*g.volume);
    if (!g.name.empty())
        j["name"] = g.name;
    return j;
}

GeometryPtr geometryFromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("geometry isn't an object");
    try
    {
        Geometry g;
        if (j.contains("mesh"))
            g.mesh = scene::meshFromJson(j["mesh"]);
        if (j.contains("points"))
            g.points = scene::meshFromJson(j["points"]);
        if (j.contains("curves"))
            g.curves = std::make_shared<const CurveSet>(curvesFromJson(j["curves"]));
        if (j.contains("instances"))
        {
            const json& ij = j["instances"];
            auto in = std::make_shared<Instances>();
            in->transforms = unblob<glm::dmat4>(ij.at("transforms"));
            in->reference = unblob<int>(ij.at("reference"));
            for (const json& r : ij.at("references"))
                in->references.push_back(r.is_null() ? nullptr : geometryFromJson(r));
            if (in->reference.size() != in->transforms.size())
                throw std::invalid_argument("instances don't add up");
            attributesFromJson(ij.at("attrs"), in->attrs, in->transforms.size());
            g.instances = std::move(in);
        }
        if (j.contains("heightfield"))
            g.heightfield = std::make_shared<const geo::Heightfield>(heightfieldFromJson(j["heightfield"]));
        if (j.contains("volume"))
            g.volume = std::make_shared<const Volume>(volumeFromJson(j["volume"]));
        g.name = j.value("name", std::string());
        return makeGeometry(std::move(g));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("unreadable geometry: ") + e.what());
    }
}

json valueToJson(SocketType t, const Value& v)
{
    switch (t)
    {
    case SocketType::Geometry: return v.geometry ? geometryToJson(*v.geometry) : json::object();
    case SocketType::String: return v.string;
    case SocketType::Closure: return nullptr;
    case SocketType::Bundle:
    {
        json items = json::array();
        if (v.bundle)
            for (const Bundle::Item& i : v.bundle->items)
                items.push_back({{"name", i.name}, {"type", socketTypeName(i.type)}, {"value", valueToJson(i.type, i.value)}});
        return {{"items", items}};
    }
    default: return dataJson(t, v.field);
    }
}

Value valueFromStored(SocketType t, const json& j)
{
    switch (t)
    {
    case SocketType::Geometry: return Value{.geometry = j.is_object() ? geometryFromJson(j) : makeGeometry({})};
    case SocketType::Bundle:
    {
        auto b = std::make_shared<Bundle>();
        for (const json& e : j.value("items", json::array()))
        {
            SocketType it{};
            if (e.is_object() && parseSocketType(e.value("type", ""), &it))
                b->items.push_back({e.value("name", ""), it, valueFromStored(it, e.value("value", json()))});
        }
        return Value{.bundle = std::move(b)};
    }
    case SocketType::Rotation:
        // Stored in radians, where valueFromJson reads degrees.
        if (j.is_array() && j.size() == 3)
            return Value{.field = constantVector({j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}, SocketType::Rotation)};
        return defaultValue(t);
    default: return valueFromJson(t, j);
    }
}

std::vector<uint8_t> toBytes(const json& j) { return json::to_cbor(j); }

json fromBytes(std::span<const uint8_t> bytes)
{
    try
    {
        return json::from_cbor(bytes.begin(), bytes.end(), true, true, json::cbor_tag_handler_t::store);
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("unreadable data: ") + e.what());
    }
}

void writeFile(const std::filesystem::path& path, const json& j)
{
    std::filesystem::create_directories(path.parent_path());
    std::vector<uint8_t> bytes = toBytes(j);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f)
            throw std::runtime_error("can't write " + path.string());
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f)
            throw std::runtime_error("can't write " + path.string());
    }
    std::filesystem::rename(temp, path);
}

json readFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("can't read " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try
    {
        return fromBytes(bytes);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}
} // namespace lila::gn
