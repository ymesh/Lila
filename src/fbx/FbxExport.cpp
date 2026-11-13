#include "fbx/Fbx.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "fbx/FbxAxes.h"
#include "fbx/FbxBinary.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"
#include "scene/Anim.h"
#include "scene/Georef.h"
#include "scene/AttrComponents.h"
#include "scene/ArrayGroup.h"
#include "scene/Camera.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/LodGroup.h"
#include "scene/Instancing.h"
#include "scene/Materials.h"
#include "scene/MeshCache.h"
#include "scene/MeshNodes.h"
#include "scene/Pin.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/ProxySet.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/RotationCurve.h"
#include "scene/Skin.h"
#include "scene/SkinBake.h"
#include "scene/Spaces.h"
#include "scene/Take.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace lila::fbx
{
namespace anim = scene::anim;

namespace
{
using geo::Index;
using geo::kInvalid;
using bin::Record;

/** GlobalSettings axis declaration: axis index 0/1/2 and sign for up, front and coord (right). */
struct AxisDecl
{
    int up, upSign, front, frontSign, coord, coordSign;
};

constexpr AxisDecl kZUp{2, 1, 1, -1, 0, 1};
constexpr AxisDecl kYUp{1, 1, 2, 1, 0, 1};

bool fileIsZUp(AxisProfile p, math::UpAxis up)
{
    return p == AxisProfile::Ue5 || (p == AxisProfile::Generic && up == math::UpAxis::Z);
}

struct Trs
{
    glm::dvec3 t{0.0}, r{0.0}, s{1.0};
};

Trs decompose(const glm::dmat4& m)
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, q, out.t, skew, persp))
        return out;
    out.r = scene::matrixToEuler(glm::mat3_cast(q), scene::RotateOrder::XYZ);
    return out;
}

/** TRS of a file-space local matrix with its pre-rotation taken out, Euler angles near ref. */
Trs fileTrs(const glm::dmat4& local, const glm::dmat3& pre, scene::RotateOrder order, const glm::dvec3& ref,
            const glm::dmat3& post = glm::dmat3(1.0))
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(local, out.s, q, out.t, skew, persp))
        return out;
    // FBX composes Pre * R * Post^-1, so R = Pre^-1 * rotation * Post.
    out.r = eulerNear(glm::transpose(pre) * glm::dmat3(glm::mat3_cast(q)) * post, order, ref);
    return out;
}

glm::dvec3 cleanVec(glm::dvec3 v)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(v[i]) < 1e-9)
            v[i] = 0.0;
    return v;
}

bool nearMat(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-7)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0, std::abs(b[c][r])))
                return false;
    return true;
}

/** True when the node's local matrix is T * pre * R * S of its channels: no pivots, no segment scale compensation. */
bool plainChannels(const scene::Node& n)
{
    using scene::Transform;
    for (int a : {Transform::kRotatePivot, Transform::kScalePivot, Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate})
        if (glm::length(n.as<glm::dvec3>(a)) > 1e-9)
            return false;
    if (scene::rig::isJoint(n) && (n.as<bool>(scene::Joint::kSegmentScaleCompensate) || n.as<double>(scene::Joint::kIkBlend) != 0.0 ||
                                   n.as<double>(scene::Joint::kIkStretch) != 1.0))
        return false;
    return true;
}

std::vector<double> flat(const glm::dmat4& m)
{
    std::vector<double> out(16);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out[size_t(c * 4 + r)] = m[c][r];
    return out;
}

constexpr int64_t kKTimeSecond = 46186158000;

int64_t ktime(double seconds) { return int64_t(std::llround(seconds * double(kKTimeSecond))); }

/** FBX TimeMode for a frame rate, or 14 (custom). */
int timeMode(double fps)
{
    struct Mode
    {
        double fps;
        int mode;
    };
    static constexpr Mode kModes[] = {{120, 1}, {100, 2},  {60, 3},    {50, 4},  {48, 5},     {30, 6},     {29.97, 9},
                                      {25, 10}, {24, 11},  {1000, 12}, {23.976, 13}, {96, 15}, {72, 16}, {59.94, 17}, {119.88, 18}};
    for (const Mode& m : kModes)
        if (std::abs(fps - m.fps) < 1e-3)
            return m.mode;
    return 14;
}

void p3(Record& props, std::string_view name, std::string_view type, std::string_view label, std::string_view flags,
        const glm::dvec3& v)
{
    props.addP(name, type, label, flags).f64(v.x).f64(v.y).f64(v.z);
}

void pInt(Record& props, std::string_view name, int32_t v) { props.addP(name, "int", "Integer", "").i32(v); }
void pEnum(Record& props, std::string_view name, int32_t v) { props.addP(name, "enum", "", "").i32(v); }
void pBool(Record& props, std::string_view name, bool v) { props.addP(name, "bool", "", "").i32(v ? 1 : 0); }
void pDouble(Record& props, std::string_view name, double v) { props.addP(name, "double", "Number", "").f64(v); }
void pString(Record& props, std::string_view name, std::string_view v) { props.addP(name, "KString", "", "").str(v); }

/** Geometry arrays for one exported mesh, already in file space. */
struct Geometry
{
    std::vector<double> vertices;
    std::vector<int32_t> polygonVertexIndex;
    std::vector<int32_t> edges;
    std::vector<int32_t> smoothing;
    std::vector<double> normals;
    std::vector<double> tangents, binormals;
    struct Uv
    {
        std::string name;
        std::vector<double> values;
        std::vector<int32_t> index;
    };
    std::vector<Uv> uvs;
    /** Color sets in order; the first is layer 0, the vertex color Unreal reads. */
    std::vector<Uv> colors;
    std::vector<int32_t> materials;   // per polygon, index into slots
    std::vector<std::string> slots;   // material names in connection order
};

/** Corners of a face edge between consecutive polygon corners a then b, or kInvalid for a triangulation diagonal. */
Index polygonEdge(const geo::Mesh& m, Index a, Index b)
{
    if (m.cornerNext(a) == b)
        return m.cornerEdge(a);
    if (m.cornerNext(b) == a)
        return m.cornerEdge(b);
    return kInvalid;
}

/** Hard where the corner normals of the edge's two faces disagree; for meshes without a "hard" layer. */
bool normalsSplit(const geo::Mesh& m, Index e, std::span<const glm::vec3> normals)
{
    Index c0 = m.edgeCorner(e);
    if (c0 == kInvalid)
        return false;
    Index c1 = m.cornerRadial(c0);
    if (c1 == c0)
        return false;
    if (m.cornerRadial(c1) != c0)
        return true;
    auto at = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    for (int i = 0; i < 2; ++i)
    {
        Index p = m.edgePoint(e, i);
        if (glm::dot(normals[at(c0, p)], normals[at(c1, p)]) < 0.9999f)
            return true;
    }
    return false;
}

template <class T> struct BitsHash
{
    size_t operator()(const T& v) const
    {
        uint64_t h = 1469598103934665603ull;
        const auto* b = reinterpret_cast<const unsigned char*>(&v);
        for (size_t i = 0; i < sizeof(T); ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return size_t(h);
    }
};

template <class T> struct BitsEq
{
    bool operator()(const T& a, const T& b) const { return std::memcmp(&a, &b, sizeof(T)) == 0; }
};

std::string slotName(const std::vector<std::string>& names, int32_t id)
{
    if (id >= 0 && size_t(id) < names.size() && !names[size_t(id)].empty())
        return names[size_t(id)];
    return names.empty() && id == 0 ? std::string("Default") : "Material" + std::to_string(id);
}

/**
 * Ids for keys in the order each distinct key first appears, as a map handing out ids on insertion would give them,
 * built with a parallel sort. firsts gets the position of each key's first appearance, in id order.
 */
std::vector<int32_t> firstSeenIds(const std::vector<uint64_t>& keys, std::vector<uint32_t>& firsts)
{
    const size_t n = keys.size();
    std::vector<uint32_t> order(n);
    tbb::parallel_for(size_t(0), n, [&](size_t i) { order[i] = uint32_t(i); });
    tbb::parallel_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return keys[a] != keys[b] ? keys[a] < keys[b] : a < b; });
    std::vector<uint32_t> head(n);
    for (size_t i = 0; i < n;)
    {
        const uint32_t first = order[i];
        for (; i < n && keys[order[i]] == keys[first]; ++i)
            head[order[i]] = first;
    }
    std::vector<int32_t> rank(n, -1);
    firsts.clear();
    for (size_t k = 0; k < n; ++k)
        if (head[k] == k)
        {
            rank[k] = int32_t(firsts.size());
            firsts.push_back(uint32_t(k));
        }
    std::vector<int32_t> ids(n);
    tbb::parallel_for(size_t(0), n, [&](size_t k) { ids[k] = rank[head[k]]; });
    return ids;
}

Geometry buildGeometry(const geo::Mesh& m, const glm::dmat4& toFile, const ExportOptions& opts)
{
    LILA_ZONE();

    Geometry g;
    const glm::dmat3 lin(toFile);
    const glm::dmat3 normalMat = glm::inverse(glm::transpose(lin));
    const bool mirrored = glm::determinant(lin) < 0.0;

    std::vector<int32_t> pointIndex(m.pointCapacity(), -1);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        pointIndex[p] = int32_t(g.vertices.size() / 3);
        glm::dvec3 v = glm::dvec3(toFile * glm::dvec4(m.position(p), 1.0));
        g.vertices.insert(g.vertices.end(), {v.x, v.y, v.z});
    }

    std::vector<glm::vec3> computedNormals;
    std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (normals.size() != m.cornerCapacity())
    {
        computedNormals = geo::cornerNormals(m);
        normals = computedNormals;
    }

    geo::Triangulation tris;
    const bool wantTangents = opts.tangents && m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).size() == m.cornerCapacity();
    if (opts.triangulate || wantTangents)
        tris = geo::triangulate(m);

    // Polygons as runs of source corners.
    std::vector<Index> corners;
    std::vector<Index> polyFace;
    std::vector<uint32_t> polyStart;
    if (opts.triangulate)
    {
        corners = tris.corners;
        polyFace = tris.faces;
        for (Index t = 0; t < tris.triangleCount(); ++t)
            polyStart.push_back(t * 3);
    }
    else
    {
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            polyStart.push_back(uint32_t(corners.size()));
            polyFace.push_back(f);
            m.forEachFaceCorner(f, [&](Index c) { corners.push_back(c); });
        }
    }
    polyStart.push_back(uint32_t(corners.size()));
    if (mirrored)
        for (size_t i = 0; i + 1 < polyStart.size(); ++i)
            std::reverse(corners.begin() + polyStart[i], corners.begin() + polyStart[i + 1]);

    const size_t polyCount = polyStart.size() - 1;
    g.polygonVertexIndex.resize(corners.size());
    // Each corner's next around its polygon; the last wraps to the polygon's start.
    std::vector<uint32_t> nextCorner(corners.size());
    tbb::parallel_for(size_t(0), polyCount, [&](size_t i) {
        for (uint32_t k = polyStart[i]; k < polyStart[i + 1]; ++k)
        {
            const int32_t idx = pointIndex[m.cornerPoint(corners[k])];
            const bool last = k + 1 == polyStart[i + 1];
            g.polygonVertexIndex[k] = last ? ~idx : idx;
            nextCorner[k] = last ? polyStart[i] : k + 1;
        }
    });

    // One Edges entry per unique point pair, pointing at the polygon vertex that starts it.
    std::span<const bool> hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
    {
        std::vector<uint64_t> pairs(corners.size());
        tbb::parallel_for(size_t(0), corners.size(), [&](size_t k) {
            const uint32_t pa = uint32_t(pointIndex[m.cornerPoint(corners[k])]);
            const uint32_t pb = uint32_t(pointIndex[m.cornerPoint(corners[nextCorner[k]])]);
            pairs[k] = (uint64_t(std::min(pa, pb)) << 32) | std::max(pa, pb);
        });
        std::vector<uint32_t> firsts;
        firstSeenIds(pairs, firsts);
        g.edges.resize(firsts.size());
        g.smoothing.resize(firsts.size());
        tbb::parallel_for(size_t(0), firsts.size(), [&](size_t i) {
            const uint32_t k = firsts[i];
            g.edges[i] = int32_t(k);
            const Index e = polygonEdge(m, corners[k], corners[nextCorner[k]]);
            const bool isHard = e != kInvalid && (hard.empty() ? normalsSplit(m, e, normals) : hard[e]);
            g.smoothing[i] = isHard ? 0 : 1;
        });
    }
    if (!opts.smoothingGroups)
        g.smoothing.clear();

    g.normals.resize(corners.size() * 3);
    tbb::parallel_for(size_t(0), corners.size(), [&](size_t k) {
        const glm::dvec3 n = glm::normalize(normalMat * glm::dvec3(normals[corners[k]]));
        g.normals[k * 3] = n.x;
        g.normals[k * 3 + 1] = n.y;
        g.normals[k * 3 + 2] = n.z;
    });

    if (wantTangents)
    {
        std::vector<glm::vec4> tan = geo::cornerTangents(m, tris, normals);
        if (tan.size() == m.cornerCapacity())
        {
            g.tangents.resize(corners.size() * 3);
            g.binormals.resize(corners.size() * 3);
            tbb::parallel_for(size_t(0), corners.size(), [&](size_t k) {
                const glm::vec4& t4 = tan[corners[k]];
                glm::dvec3 n(g.normals[k * 3], g.normals[k * 3 + 1], g.normals[k * 3 + 2]);
                glm::dvec3 t = lin * glm::dvec3(t4);
                t = glm::length(t) > 0.0 ? glm::normalize(t) : t;
                double w = (t4.w < 0.0f ? -1.0 : 1.0) * (mirrored ? -1.0 : 1.0);
                glm::dvec3 b = w * glm::cross(n, t);
                for (int i = 0; i < 3; ++i)
                {
                    g.tangents[k * 3 + i] = t[i];
                    g.binormals[k * 3 + i] = b[i];
                }
            });
        }
    }
    if (!opts.normals)
        g.normals.clear();

    for (int set = 0; set < 8; ++set)
    {
        std::string name = geo::attr::uvName(set);
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(name);
        if (uv.size() != m.cornerCapacity())
            continue;
        Geometry::Uv layer;
        layer.name = name;
        // Equal bits share an entry, as a hash on the bits would.
        std::vector<uint64_t> bits(corners.size());
        tbb::parallel_for(size_t(0), corners.size(), [&](size_t k) { std::memcpy(&bits[k], &uv[corners[k]], sizeof(uint64_t)); });
        std::vector<uint32_t> firsts;
        const std::vector<int32_t> ids = firstSeenIds(bits, firsts);
        layer.index.assign(ids.begin(), ids.end());
        layer.values.resize(firsts.size() * 2);
        tbb::parallel_for(size_t(0), firsts.size(), [&](size_t i) {
            const glm::vec2 v = uv[corners[firsts[i]]];
            layer.values[i * 2] = double(v.x);
            layer.values[i * 2 + 1] = double(v.y);
        });
        g.uvs.push_back(std::move(layer));
    }

    for (const std::string& set : geo::colors::sets(m))
    {
        std::span<const glm::vec4> col = geo::colors::values(m, set);
        if (col.size() != m.cornerCapacity())
            continue;
        Geometry::Uv layer{set, {}, {}};
        std::unordered_map<glm::vec4, int32_t, BitsHash<glm::vec4>, BitsEq<glm::vec4>> ids;
        for (Index c : corners)
        {
            auto [it, added] = ids.emplace(col[c], int32_t(ids.size()));
            if (added)
                layer.values.insert(layer.values.end(), {double(col[c].r), double(col[c].g), double(col[c].b), double(col[c].a)});
            layer.index.push_back(it->second);
        }
        g.colors.push_back(std::move(layer));
    }

    std::span<const int32_t> faceMat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::vector<int32_t> ids(polyCount);
    std::set<int32_t> used;
    for (size_t i = 0; i < polyCount; ++i)
    {
        ids[i] = faceMat.size() == m.faceCapacity() ? faceMat[polyFace[i]] : 0;
        used.insert(ids[i]);
    }
    if (used.empty())
        used.insert(0);
    std::map<int32_t, int32_t> slotOf;
    for (int32_t id : used)
    {
        slotOf[id] = int32_t(g.slots.size());
        g.slots.push_back(slotName(m.materialNames(), id));
    }
    g.materials.reserve(polyCount);
    for (int32_t id : ids)
        g.materials.push_back(slotOf[id]);
    return g;
}

void layerElement(Record& layer, std::string_view type, int index)
{
    Record& e = layer.add("LayerElement");
    e.add("Type").str(type);
    e.add("TypedIndex").i32(index);
}

void writeGeometry(Record& objects, int64_t id, const std::string& name, const Geometry& g)
{
    Record& geom = objects.add("Geometry");
    geom.i64(id).str(bin::objectName(name, "Geometry")).str("Mesh");
    geom.add("Properties70");
    geom.add("GeometryVersion").i32(124);
    geom.add("Vertices").f64s(g.vertices);
    geom.add("PolygonVertexIndex").i32s(g.polygonVertexIndex);
    geom.add("Edges").i32s(g.edges);

    auto header = [](Record& e, int version, std::string_view name, std::string_view mapping, std::string_view ref) {
        e.add("Version").i32(version);
        e.add("Name").str(name);
        e.add("MappingInformationType").str(mapping);
        e.add("ReferenceInformationType").str(ref);
    };

    if (!g.normals.empty())
    {
        Record& e = geom.add("LayerElementNormal");
        e.i32(0);
        header(e, 101, "", "ByPolygonVertex", "Direct");
        e.add("Normals").f64s(g.normals);
    }
    if (!g.tangents.empty())
    {
        Record& b = geom.add("LayerElementBinormal");
        b.i32(0);
        header(b, 101, "uv0", "ByPolygonVertex", "Direct");
        b.add("Binormals").f64s(g.binormals);
        Record& t = geom.add("LayerElementTangent");
        t.i32(0);
        header(t, 101, "uv0", "ByPolygonVertex", "Direct");
        t.add("Tangents").f64s(g.tangents);
    }
    if (!g.smoothing.empty())
    {
        Record& e = geom.add("LayerElementSmoothing");
        e.i32(0);
        header(e, 102, "", "ByEdge", "Direct");
        e.add("Smoothing").i32s(g.smoothing);
    }
    for (size_t i = 0; i < g.colors.size(); ++i)
    {
        Record& e = geom.add("LayerElementColor");
        e.i32(int32_t(i));
        header(e, 101, g.colors[i].name, "ByPolygonVertex", "IndexToDirect");
        e.add("Colors").f64s(g.colors[i].values);
        e.add("ColorIndex").i32s(g.colors[i].index);
    }
    for (size_t i = 0; i < g.uvs.size(); ++i)
    {
        Record& e = geom.add("LayerElementUV");
        e.i32(int32_t(i));
        header(e, 101, g.uvs[i].name, "ByPolygonVertex", "IndexToDirect");
        e.add("UV").f64s(g.uvs[i].values);
        e.add("UVIndex").i32s(g.uvs[i].index);
    }
    {
        Record& e = geom.add("LayerElementMaterial");
        e.i32(0);
        header(e, 101, "", "ByPolygon", "IndexToDirect");
        e.add("Materials").i32s(g.materials);
    }

    Record& layer0 = geom.add("Layer");
    layer0.i32(0);
    layer0.add("Version").i32(100);
    if (!g.normals.empty())
        layerElement(layer0, "LayerElementNormal", 0);
    if (!g.tangents.empty())
    {
        layerElement(layer0, "LayerElementBinormal", 0);
        layerElement(layer0, "LayerElementTangent", 0);
    }
    layerElement(layer0, "LayerElementMaterial", 0);
    if (!g.smoothing.empty())
        layerElement(layer0, "LayerElementSmoothing", 0);
    if (!g.colors.empty())
        layerElement(layer0, "LayerElementColor", 0);
    if (!g.uvs.empty())
        layerElement(layer0, "LayerElementUV", 0);
    for (size_t i = 1; i < std::max(g.uvs.size(), g.colors.size()); ++i)
    {
        Record& layer = geom.add("Layer");
        layer.i32(int32_t(i));
        layer.add("Version").i32(100);
        if (i < g.colors.size())
            layerElement(layer, "LayerElementColor", int32_t(i));
        if (i < g.uvs.size())
            layerElement(layer, "LayerElementUV", int32_t(i));
    }
}

struct ModelInfo
{
    std::string_view type = "Null"; // Mesh, Null, LimbNode or LodGroup
    Trs trs;
    glm::dvec3 pre{0.0};
    glm::dvec3 post{0.0};
    scene::RotateOrder order = scene::RotateOrder::XYZ;
};

/**
 * An added attr as an FBX user property: numbers, bools and strings as themselves, a 3-number list as a vector, any
 * other list as comma-separated text.
 */
void writeAttrProperty(Record& props, const std::string& name, const nlohmann::json& v)
{
    if (v.is_boolean())
        props.addP(name, "bool", "", "A+U").i32(v.get<bool>() ? 1 : 0);
    else if (v.is_number_integer())
        props.addP(name, "int", "Integer", "A+U").i32(int32_t(std::clamp<int64_t>(v.get<int64_t>(), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())));
    else if (v.is_number())
        props.addP(name, "double", "Number", "A+U").f64(v.get<double>());
    else if (v.is_string())
        props.addP(name, "KString", "", "U").str(v.get<std::string>());
    else if (v.is_array() && v.size() == 3 && std::ranges::all_of(v, [](const nlohmann::json& x) { return x.is_number(); }))
        p3(props, name, "Vector3D", "Vector", "A+U", {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
    else if (v.is_array())
    {
        std::string text;
        for (const nlohmann::json& x : v)
            text += (text.empty() ? "" : ",") + (x.is_string() ? x.get<std::string>() : x.dump());
        props.addP(name, "KString", "", "U").str(text);
    }
}

void writeModel(Record& objects, int64_t id, const std::string& name, const ModelInfo& info,
                const std::vector<std::pair<std::string, double>>& userProps = {}, const std::string& socket = {},
                const std::vector<std::pair<std::string, nlohmann::json>>& attrProps = {})
{
    Record& model = objects.add("Model");
    model.i64(id).str(bin::objectName(name, "Model")).str(info.type);
    model.add("Version").i32(232);
    Record& props = model.add("Properties70");
    if (info.order != scene::RotateOrder::XYZ)
        pEnum(props, "RotationOrder", fbxOrder(info.order));
    if (info.pre != glm::dvec3(0.0))
        p3(props, "PreRotation", "Vector3D", "Vector", "", info.pre);
    if (info.post != glm::dvec3(0.0))
        p3(props, "PostRotation", "Vector3D", "Vector", "", info.post);
    pBool(props, "RotationActive", true);
    pEnum(props, "InheritType", 1);
    pInt(props, "DefaultAttributeIndex", 0);
    p3(props, "Lcl Translation", "Lcl Translation", "", "A", info.trs.t);
    p3(props, "Lcl Rotation", "Lcl Rotation", "", "A", info.trs.r);
    p3(props, "Lcl Scaling", "Lcl Scaling", "", "A", info.trs.s);
    for (const auto& [prop, value] : userProps)
        props.addP(prop, "double", "Number", "A+U").f64(value);
    if (!socket.empty())
        props.addP("UnrealSocket", "KString", "", "U").str(socket);
    for (const auto& [prop, value] : attrProps)
        writeAttrProperty(props, prop, value);
    model.add("Shading").boolean(true);
    model.add("Culling").str("CullingOff");
}

void writeNullAttribute(Record& objects, int64_t id, const std::string& name)
{
    Record& attr = objects.add("NodeAttribute");
    attr.i64(id).str(bin::objectName(name, "NodeAttribute")).str("Null");
    attr.add("Properties70");
    attr.add("TypeFlags").str("Null");
}

void writeLimbAttribute(Record& objects, int64_t id, const std::string& name)
{
    Record& attr = objects.add("NodeAttribute");
    attr.i64(id).str(bin::objectName(name, "NodeAttribute")).str("LimbNode");
    Record& props = attr.add("Properties70");
    pDouble(props, "Size", 33.333333);
    attr.add("TypeFlags").str("Skeleton");
}

/**
 * An FbxLODGroup, laid out as Maya writes it. ThresholdsUsedAsPercentage makes LevelN the screen percentage at which
 * level N+1 takes over, which is that level's Unreal screen size times 100.
 */
void writeLodGroupAttribute(Record& objects, int64_t id, const std::string& name, const std::vector<double>& screenSizes, size_t levels)
{
    Record& attr = objects.add("NodeAttribute");
    attr.i64(id).str(bin::objectName(name, "NodeAttribute")).str("LodGroup");
    Record& props = attr.add("Properties70");
    pBool(props, "ThresholdsUsedAsPercentage", true);
    for (size_t i = 1; i < screenSizes.size(); ++i)
        props.addP(std::format("Thresholds|Level{}", i - 1), "Distance", "", "").f64(screenSizes[i] * 100.0).str("cm");
    for (size_t i = 0; i < levels; ++i)
        pEnum(props, std::format("DisplayLevels|Level{}", i), 0);
    attr.add("TypeFlags").str("LodGroup");
}

/** FBX GateFit for scene::FilmFit fill, horizontal, vertical, overscan. */
constexpr int32_t kGateFit[4] = {3, 2, 1, 4};

/** Camera settings as animatable FBX properties, in the order ExportCamera holds them. */
struct CameraChannel
{
    const char* prop;
    double ExportCamera::*value;
};
constexpr CameraChannel kCameraChannels[] = {
    {"FocalLength", &ExportCamera::focalLength}, {"FilmWidth", &ExportCamera::filmWidth}, {"FilmHeight", &ExportCamera::filmHeight},
    {"NearPlane", &ExportCamera::nearClip},      {"FarPlane", &ExportCamera::farClip},    {"OrthoZoom", &ExportCamera::orthoWidth},
};

/**
 * A Camera attribute with its lens in focal length mode, the way Unreal's own exporter writes one, so Sequencer's
 * FBX import makes a CineCamera and keys its CurrentFocalLength from the FocalLength curve.
 */
void writeCameraAttribute(Record& objects, int64_t id, const std::string& name, const ExportCamera& c)
{
    Record& attr = objects.add("NodeAttribute");
    attr.i64(id).str(bin::objectName(name, "NodeAttribute")).str("Camera");
    Record& props = attr.add("Properties70");
    const double aspect = c.filmWidth / std::max(c.filmHeight, 1e-9);
    for (const CameraChannel& ch : kCameraChannels)
        props.addP(ch.prop, "Number", "", "A").f64(c.*ch.value);
    pDouble(props, "FilmAspectRatio", aspect);
    pEnum(props, "ApertureMode", 3);
    pEnum(props, "GateFit", kGateFit[std::clamp(c.filmFit, 0, 3)]);
    const double fov = glm::degrees(2.0 * std::atan(c.filmWidth * 25.4 * 0.5 / std::max(c.focalLength, 1e-6)));
    props.addP("FieldOfView", "FieldOfView", "", "A").f64(fov);
    pDouble(props, "AspectWidth", aspect);
    pDouble(props, "AspectHeight", 1.0);
    pEnum(props, "CameraProjectionType", c.ortho ? 1 : 0);
    attr.add("TypeFlags").str("Camera");
    attr.add("GeometryVersion").i32(124);
}

/**
 * A Light attribute as Maya writes one (FBX intensity is percent, cone angles are whole), plus Lila's own settings as
 * user properties so a Lila round trip keeps what FBX has no word for.
 */
void writeLightAttribute(Record& objects, int64_t id, const std::string& name, const scene::lights::Settings& l)
{
    using scene::lights::Kind;
    Record& attr = objects.add("NodeAttribute");
    attr.i64(id).str(bin::objectName(name, "NodeAttribute")).str("Light");
    Record& props = attr.add("Properties70");
    const int type = l.kind == Kind::Directional ? 1 : l.kind == Kind::Spot ? 2 : l.kind == Kind::Area || l.kind == Kind::Disc ? 3 : 0;
    pEnum(props, "LightType", type);
    p3(props, "Color", "Color", "", "A", scene::lights::effectiveColor(l));
    props.addP("Intensity", "Number", "", "A").f64(l.intensity * 100.0);
    props.addP("InnerAngle", "Number", "", "A").f64(l.innerConeAngle * 2.0);
    props.addP("OuterAngle", "Number", "", "A").f64(l.outerConeAngle * 2.0);
    if (type == 3)
        pEnum(props, "AreaLightShape", 0);
    int decay = l.kind == Kind::Directional || l.kind == Kind::Dome ? 0 : 2;
    if (const nlohmann::json extra = nlohmann::json::parse(l.extra.empty() ? "{}" : l.extra, nullptr, false);
        extra.is_object() && extra.contains("decayRate") && extra["decayRate"].is_number())
        decay = std::clamp(extra["decayRate"].get<int>(), 0, 3);
    pEnum(props, "DecayType", decay);
    pBool(props, "CastShadows", l.castShadows);
    auto user = [&](std::string_view prop, double v) { props.addP(prop, "double", "Number", "A+U").f64(v); };
    user("LilaLightType", double(l.kind));
    user("LilaIntensityUnits", double(l.units));
    user("LilaUseTemperature", l.useTemperature ? 1.0 : 0.0);
    user("LilaTemperature", l.temperature);
    user("LilaRange", l.range);
    user("LilaWidth", l.width);
    user("LilaHeight", l.height);
    user("LilaSourceRadius", l.sourceRadius);
    props.addP("LilaColor", "Vector3D", "Vector", "U").f64(l.color.x).f64(l.color.y).f64(l.color.z);
    if (!l.texture.empty())
        props.addP("LilaTexture", "KString", "", "U").str(l.texture);
    if (!l.extra.empty())
        props.addP("LilaExtra", "KString", "", "U").str(l.extra);
    attr.add("TypeFlags").str("Light");
    attr.add("GeometryVersion").i32(124);
}

void writeMaterial(Record& objects, int64_t id, const std::string& name, const ExportMaterial* m)
{
    Record& mat = objects.add("Material");
    mat.i64(id).str(bin::objectName(name, "Material")).str("");
    mat.add("Version").i32(102);
    mat.add("ShadingModel").str("lambert");
    mat.add("MultiLayer").i32(0);
    Record& props = mat.add("Properties70");
    p3(props, "DiffuseColor", "Color", "", "A", m ? m->baseColor : glm::dvec3(0.8));
    pDouble(props, "DiffuseFactor", 1.0);
    if (m && m->emissive != glm::dvec3(0.0))
    {
        p3(props, "EmissiveColor", "Color", "", "A", m->emissive);
        pDouble(props, "EmissiveFactor", 1.0);
    }
}

/** A file texture and the video clip it shows, as Maya writes them. */
/** A texture's object name: its file's stem. */
std::string textureName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
        name.resize(dot);
    return name;
}

/** The file is written absolute in both its file names; readers fall back to the relative one. */
void writeTexture(Record& objects, int64_t textureId, int64_t videoId, const std::string& name, const ExportTexture& t)
{
    Record& tex = objects.add("Texture");
    tex.i64(textureId).str(bin::objectName(name, "Texture")).str("");
    tex.add("Type").str("TextureVideoClip");
    tex.add("Version").i32(202);
    tex.add("TextureName").str(bin::objectName(name, "Texture"));
    Record& props = tex.add("Properties70");
    pBool(props, "UseMaterial", true);
    // FBX wrap modes: 0 repeat, 1 clamp.
    pEnum(props, "WrapModeU", t.clampU ? 1 : 0);
    pEnum(props, "WrapModeV", t.clampV ? 1 : 0);
    tex.add("Media").str(bin::objectName(name, "Video"));
    tex.add("FileName").str(t.path);
    tex.add("RelativeFilename").str(t.path);
    tex.add("ModelUVTranslation").f64(0.0).f64(0.0);
    tex.add("ModelUVScaling").f64(1.0).f64(1.0);
    tex.add("Texture_Alpha_Source").str("None");
    tex.add("Cropping").i32(0).i32(0).i32(0).i32(0);

    Record& video = objects.add("Video");
    video.i64(videoId).str(bin::objectName(name, "Video")).str("Clip");
    video.add("Type").str("Clip");
    Record& vp = video.add("Properties70");
    vp.addP("Path", "KString", "XRefUrl", "").str(t.path);
    video.add("UseMipMap").i32(0);
    video.add("Filename").str(t.path);
    video.add("RelativeFilename").str(t.path);
}

void writeHeader(Record& root, const std::string& creator)
{
    Record& ext = root.add("FBXHeaderExtension");
    ext.add("FBXHeaderVersion").i32(1003);
    ext.add("FBXVersion").i32(7400);
    ext.add("EncryptionType").i32(0);
    // A fixed stamp keeps the output byte-identical for the same scene.
    Record& stamp = ext.add("CreationTimeStamp");
    stamp.add("Version").i32(1000);
    stamp.add("Year").i32(1970);
    stamp.add("Month").i32(1);
    stamp.add("Day").i32(1);
    stamp.add("Hour").i32(10);
    stamp.add("Minute").i32(0);
    stamp.add("Second").i32(0);
    stamp.add("Millisecond").i32(0);
    ext.add("Creator").str(creator);
    Record& info = ext.add("SceneInfo");
    info.str(bin::objectName("GlobalInfo", "SceneInfo")).str("UserData");
    info.add("Type").str("UserData");
    info.add("Version").i32(100);
    Record& meta = info.add("MetaData");
    meta.add("Version").i32(100);
    for (const char* k : {"Title", "Subject", "Author", "Keywords", "Revision", "Comment"})
        meta.add(k).str("");
    Record& props = info.add("Properties70");
    props.addP("Original", "Compound", "", "");
    pString(props, "Original|ApplicationVendor", "Lila");
    pString(props, "Original|ApplicationName", "Lila");
    pString(props, "Original|ApplicationVersion", "0.1");
    props.addP("LastSaved", "Compound", "", "");
    pString(props, "LastSaved|ApplicationVendor", "Lila");
    pString(props, "LastSaved|ApplicationName", "Lila");
    pString(props, "LastSaved|ApplicationVersion", "0.1");
    // The scene's place on the Earth, as scene info a pipeline can read back (LilaGeoreference*).
    if (const json geo = scene::georef::exportJson(); !geo.is_null())
    {
        pString(props, "LilaGeoreferenceCRS", geo.value("authority", geo["crs"].get<std::string>()));
        pDouble(props, "LilaGeoreferenceEasting", geo["origin"][0].get<double>());
        pDouble(props, "LilaGeoreferenceNorthing", geo["origin"][1].get<double>());
        pDouble(props, "LilaGeoreferenceHeight", geo["origin"][2].get<double>());
        if (geo.contains("wgs84"))
        {
            pDouble(props, "LilaGeoreferenceLatitude", geo["wgs84"]["latitude"].get<double>());
            pDouble(props, "LilaGeoreferenceLongitude", geo["wgs84"]["longitude"].get<double>());
            pDouble(props, "LilaGeoreferenceEllipsoidHeight", geo["wgs84"]["height"].get<double>());
        }
    }

    static constexpr uint8_t kFileId[16] = {0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2,
                                            0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1};
    root.add("FileId").raw(kFileId);
    root.add("CreationTime").str("1970-01-01 10:00:00:000");
    root.add("Creator").str(creator);
}

/** One AnimationCurve's arrays. Key attributes are run-length shared through refs, as FBX stores them. */
struct CurveKeys
{
    std::vector<int64_t> times;
    std::vector<float> values;
    std::vector<int32_t> flags;
    std::vector<float> data;
    std::vector<int32_t> refs;
};

constexpr int32_t kKeyConstant = 0x2;
constexpr int32_t kKeyLinear = 0x4;
constexpr int32_t kKeyCubicUser = 0x8 | 0x400 | 0x800; // cubic, user slopes, broken (each side as given)
constexpr int32_t kKeyWeightedRight = 0x1000000;
constexpr int32_t kKeyWeightedNextLeft = 0x2000000;

float packedWeights(double right, double nextLeft)
{
    auto fixed = [](double w) { return uint32_t(std::clamp(std::lround(w * 10000.0), 0L, 10000L)); };
    uint32_t bits = fixed(right) | (fixed(nextLeft) << 16);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

/** Baked samples, linear between frames. */
CurveKeys sampledKeys(const std::vector<double>& times, const std::vector<double>& values)
{
    CurveKeys k;
    for (size_t i = 0; i < times.size(); ++i)
    {
        k.times.push_back(ktime(times[i]));
        k.values.push_back(float(values[i]));
    }
    k.flags = {kKeyLinear};
    k.data = {0.0f, 0.0f, packedWeights(1.0 / 3.0, 1.0 / 3.0), 0.0f};
    k.refs = {int32_t(times.size())};
    return k;
}

/** A Lila curve's own keys, scaled by factor (an axis flip). Slopes are value per second, as in FBX. */
CurveKeys curveKeys(const anim::Curve& c, double factor)
{
    CurveKeys k;
    for (size_t i = 0; i < c.keys.size(); ++i)
    {
        const anim::Key& key = c.keys[i];
        k.times.push_back(ktime(key.time));
        k.values.push_back(float(key.value * factor));
        bool last = i + 1 == c.keys.size();
        if (!last && key.out == anim::Tangent::Step)
        {
            k.flags.push_back(kKeyConstant);
            k.data.insert(k.data.end(), {0.0f, 0.0f, packedWeights(1.0 / 3.0, 1.0 / 3.0), 0.0f});
        }
        else
        {
            int32_t flags = kKeyCubicUser;
            double right = last ? 0.0 : c.outSlope(i) * factor;
            double nextLeft = last ? 0.0 : c.inSlope(i + 1) * factor;
            double wr = 1.0 / 3.0, wl = 1.0 / 3.0;
            // Lila weights are relative to a third of the segment, FBX's to the whole segment.
            if (c.weighted && !last)
            {
                wr = std::clamp(key.outWeight, 0.0, 3.0) / 3.0;
                wl = std::clamp(c.keys[i + 1].inWeight, 0.0, 3.0) / 3.0;
                flags |= kKeyWeightedRight | kKeyWeightedNextLeft;
            }
            k.flags.push_back(flags);
            k.data.insert(k.data.end(), {float(right), float(nextLeft), packedWeights(wr, wl), 0.0f});
        }
        k.refs.push_back(1);
    }
    return k;
}

void writeCurve(Record& objects, int64_t id, const CurveKeys& k, double defaultValue)
{
    Record& c = objects.add("AnimationCurve");
    c.i64(id).str(bin::objectName("", "AnimCurve")).str("");
    c.add("Default").f64(defaultValue);
    c.add("KeyVer").i32(4009);
    c.add("KeyTime").i64s(k.times);
    c.add("KeyValueFloat").f32s(k.values);
    c.add("KeyAttrFlags").i32s(k.flags);
    c.add("KeyAttrDataFloat").f32s(k.data);
    c.add("KeyAttrRefCount").i32s(k.refs);
}

void writeCurveNode(Record& objects, int64_t id, std::string_view name, const glm::dvec3& defaults)
{
    Record& cn = objects.add("AnimationCurveNode");
    cn.i64(id).str(bin::objectName(name, "AnimCurveNode")).str("");
    Record& props = cn.add("Properties70");
    props.addP("d|X", "Number", "", "A").f64(defaults.x);
    props.addP("d|Y", "Number", "", "A").f64(defaults.y);
    props.addP("d|Z", "Number", "", "A").f64(defaults.z);
}

/** A curve node animating one Number property (a morph channel's DeformPercent, a named curve). */
void writeCurveNode1(Record& objects, int64_t id, std::string_view name, std::string_view prop, double defaultValue)
{
    Record& cn = objects.add("AnimationCurveNode");
    cn.i64(id).str(bin::objectName(name, "AnimCurveNode")).str("");
    Record& props = cn.add("Properties70");
    props.addP(std::string("d|") + std::string(prop), "Number", "", "A").f64(defaultValue);
}

void writeGlobalSettings(Record& root, const AxisDecl& a, double fps, int64_t spanStart, int64_t spanStop, double unitScale)
{
    Record& gs = root.add("GlobalSettings");
    gs.add("Version").i32(1000);
    Record& props = gs.add("Properties70");
    pInt(props, "UpAxis", a.up);
    pInt(props, "UpAxisSign", a.upSign);
    pInt(props, "FrontAxis", a.front);
    pInt(props, "FrontAxisSign", a.frontSign);
    pInt(props, "CoordAxis", a.coord);
    pInt(props, "CoordAxisSign", a.coordSign);
    pInt(props, "OriginalUpAxis", a.up);
    pInt(props, "OriginalUpAxisSign", a.upSign);
    pDouble(props, "UnitScaleFactor", unitScale);
    pDouble(props, "OriginalUnitScaleFactor", unitScale);
    props.addP("AmbientColor", "ColorRGB", "Color", "").f64(0).f64(0).f64(0);
    pString(props, "DefaultCamera", "Producer Perspective");
    const int mode = timeMode(fps);
    pEnum(props, "TimeMode", mode);
    pEnum(props, "TimeProtocol", 2);
    pEnum(props, "SnapOnFrameMode", 0);
    props.addP("TimeSpanStart", "KTime", "Time", "").i64(spanStart);
    props.addP("TimeSpanStop", "KTime", "Time", "").i64(spanStop);
    pDouble(props, "CustomFrameRate", mode == 14 ? fps : -1.0);
    props.addP("TimeMarker", "Compound", "", "");
    pInt(props, "CurrentTimeMarker", -1);
}

struct ObjectCounts
{
    int models = 0, nulls = 0, limbs = 0, cameras = 0, lights = 0, lodGroups = 0, geometries = 0, materials = 0, textures = 0;
    int deformers = 0, poses = 0, stacks = 0, layers = 0, curveNodes = 0, curves = 0;
};

void writeDefinitions(Record& root, const ObjectCounts& n)
{
    Record& defs = root.add("Definitions");
    defs.add("Version").i32(100);
    defs.add("Count").i32(1 + n.models + n.nulls + n.limbs + n.cameras + n.lights + n.lodGroups + n.geometries + n.materials + 2 * n.textures +
                          n.deformers + n.poses + n.stacks + n.layers + n.curveNodes + n.curves);
    auto type = [&](std::string_view name, int count, std::string_view tmpl) -> Record* {
        Record& t = defs.add("ObjectType");
        t.str(name);
        t.add("Count").i32(count);
        if (tmpl.empty())
            return nullptr;
        Record& pt = t.add("PropertyTemplate");
        pt.str(tmpl);
        return &pt.add("Properties70");
    };
    type("GlobalSettings", 1, "");
    if (n.models)
    {
        Record* p = type("Model", n.models, "FbxNode");
        p3(*p, "Lcl Translation", "Lcl Translation", "", "A", glm::dvec3(0.0));
        p3(*p, "Lcl Rotation", "Lcl Rotation", "", "A", glm::dvec3(0.0));
        p3(*p, "Lcl Scaling", "Lcl Scaling", "", "A", glm::dvec3(1.0));
        pEnum(*p, "RotationOrder", 0);
        p3(*p, "PreRotation", "Vector3D", "Vector", "", glm::dvec3(0.0));
        p3(*p, "PostRotation", "Vector3D", "Vector", "", glm::dvec3(0.0));
        pBool(*p, "Show", true);
        p->addP("Visibility", "Visibility", "", "A").f64(1.0);
    }
    if (n.nulls || n.limbs || n.cameras || n.lights || n.lodGroups)
    {
        Record* p = type("NodeAttribute", n.nulls + n.limbs + n.cameras + n.lights + n.lodGroups,
                         n.nulls ? "FbxNull" : n.limbs ? "FbxSkeleton" : n.cameras ? "FbxCamera" : n.lights ? "FbxLight" : "FbxLODGroup");
        if (n.nulls || n.limbs)
        {
            p3(*p, "Color", "ColorRGB", "Color", "", glm::dvec3(0.8));
            pDouble(*p, "Size", 100.0);
        }
        if (n.nulls)
            pEnum(*p, "Look", 1);
    }
    if (n.geometries)
    {
        Record* p = type("Geometry", n.geometries, "FbxMesh");
        p3(*p, "Color", "ColorRGB", "Color", "", glm::dvec3(0.8));
        pBool(*p, "Primary Visibility", true);
        pBool(*p, "Casts Shadows", true);
        pBool(*p, "Receive Shadows", true);
    }
    if (n.materials)
    {
        Record* p = type("Material", n.materials, "FbxSurfaceLambert");
        p->addP("ShadingModel", "KString", "", "").str("Lambert");
        p3(*p, "DiffuseColor", "Color", "", "A", glm::dvec3(0.8));
        pDouble(*p, "DiffuseFactor", 1.0);
    }
    if (n.textures)
    {
        Record* p = type("Texture", n.textures, "FbxFileTexture");
        pEnum(*p, "WrapModeU", 0);
        pEnum(*p, "WrapModeV", 0);
        type("Video", n.textures, "");
    }
    if (n.deformers)
        type("Deformer", n.deformers, "");
    if (n.poses)
        type("Pose", n.poses, "");
    if (n.stacks)
    {
        Record* p = type("AnimationStack", n.stacks, "FbxAnimStack");
        p->addP("LocalStart", "KTime", "Time", "").i64(0);
        p->addP("LocalStop", "KTime", "Time", "").i64(0);
        p->addP("ReferenceStart", "KTime", "Time", "").i64(0);
        p->addP("ReferenceStop", "KTime", "Time", "").i64(0);
    }
    if (n.layers)
    {
        Record* p = type("AnimationLayer", n.layers, "FbxAnimLayer");
        pDouble(*p, "Weight", 100.0);
    }
    if (n.curveNodes)
        type("AnimationCurveNode", n.curveNodes, "");
    if (n.curves)
        type("AnimationCurve", n.curves, "");
}

std::string toLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

bool parseProfile(std::string_view name, AxisProfile* out)
{
    std::string n = toLower(name);
    if (n == "ue5")
        *out = AxisProfile::Ue5;
    else if (n == "ue6")
        *out = AxisProfile::Ue6;
    else if (n == "generic")
        *out = AxisProfile::Generic;
    else
        return false;
    return true;
}

const char* profileName(AxisProfile p)
{
    switch (p)
    {
    case AxisProfile::Ue5: return "ue5";
    case AxisProfile::Ue6: return "ue6";
    case AxisProfile::Generic: return "generic";
    }
    return "ue5";
}

glm::dmat3 profileBasis(AxisProfile p, math::UpAxis up)
{
    const math::AxisBasis& b = math::axisBasis(up);
    // Rows are the Lila directions that become file X, Y, Z.
    glm::dmat3 rows = fileIsZUp(p, up) ? glm::dmat3(b.forward, -b.right, b.up) : glm::dmat3(-b.right, b.up, b.forward);
    return glm::transpose(rows);
}

glm::dmat3 characterTurn(math::UpAxis up)
{
    const math::AxisBasis& b = math::axisBasis(up);
    // Columns are where right, up and forward go: right becomes forward, forward becomes -right.
    const glm::dmat3 from(b.right, b.up, b.forward);
    const glm::dmat3 to(b.forward, b.up, -b.right);
    return to * glm::transpose(from);
}

bool parseKind(std::string_view name, ExportKind* out)
{
    std::string n = toLower(name);
    if (n == "static")
        *out = ExportKind::Static;
    else if (n == "skeletal")
        *out = ExportKind::Skeletal;
    else if (n == "animation")
        *out = ExportKind::Animation;
    else if (n == "sequence")
        *out = ExportKind::Sequence;
    else
        return false;
    return true;
}

const char* kindName(ExportKind k)
{
    switch (k)
    {
    case ExportKind::Static: return "static";
    case ExportKind::Skeletal: return "skeletal";
    case ExportKind::Animation: return "animation";
    case ExportKind::Sequence: return "sequence";
    }
    return "static";
}

namespace
{
using scene::Graph;
using scene::Node;
using scene::NodeId;

std::optional<ExportCamera> cameraOf(const Node& n)
{
    using scene::Camera;
    if (!n.type().isA("camera"))
        return std::nullopt;
    ExportCamera c;
    c.focalLength = n.as<double>(Camera::kFocalLength);
    c.filmWidth = n.as<double>(Camera::kHorizontalFilmAperture);
    c.filmHeight = n.as<double>(Camera::kVerticalFilmAperture);
    c.nearClip = n.as<double>(Camera::kNearClipPlane);
    c.farClip = n.as<double>(Camera::kFarClipPlane);
    c.ortho = n.as<bool>(Camera::kOrthographic);
    c.orthoWidth = n.as<double>(Camera::kOrthographicWidth);
    c.filmFit = int(scene::camera::filmFit(n));
    return c;
}

bool channelsDriven(const Graph& g, NodeId id)
{
    using scene::Transform;
    for (int attr : {int(Transform::kTranslate), int(Transform::kRotate), int(Transform::kScale)})
        for (int c = -1; c < 3; ++c)
            if (g.source({id, attr, c}))
                return true;
    return false;
}

/** The scene side of an export while it is being gathered. */
struct Gather
{
    Graph& graph;
    const ExportOptions& opts;
    ExportScene& out;
    std::unordered_map<NodeId, int> indexOf;
    std::vector<glm::dmat4> rootInv; // per node: its export root's inverse placement
    std::vector<NodeId> skinOf;      // per node: its skinCluster, or kNoNode
    /** Joints some skinCluster takes as an influence; they go out whatever their deform flag says. */
    std::set<NodeId> weighted;
    std::vector<std::string> weightedNonDeform;
    /** Export meshes by instance root, so the members of one instance group carry the same snapshot. */
    std::unordered_map<NodeId, scene::MeshPtr> exportMeshes;

    scene::MeshPtr exportMeshOf(NodeId id)
    {
        const NodeId root = scene::instancing::root(graph, id);
        auto [it, added] = exportMeshes.emplace(root, nullptr);
        if (added)
            it->second = scene::modifiers::exportMesh(graph, root);
        return it->second;
    }

    glm::dmat4 world(NodeId id) const { return graph.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix); }

    /** The node's local matrix relative to its exported parent, from the current evaluation. */
    glm::dmat4 localOf(size_t i) const
    {
        const ExportNode& e = out.nodes[i];
        const Node* n = graph.node(e.source);
        if (e.parent < 0)
        {
            if (const NodeId above = graph.parent(e.source); opts.localRoots && above != scene::kNoNode && above != opts.relativeTo)
                return scene::spaces::hasSpaces(graph, e.source) ? glm::inverse(world(above)) * world(e.source)
                                                                 : n->as<glm::dmat4>(scene::Transform::kMatrix);
            if (opts.relativeTo != scene::kNoNode && graph.node(opts.relativeTo))
                return glm::inverse(world(opts.relativeTo)) * world(e.source);
            return rootInv[i] * world(e.source);
        }
        NodeId parentSource = out.nodes[size_t(e.parent)].source;
        // A node in another space has its local matrix in that space's frame, not its parent's.
        if (parentSource == graph.parent(e.source) && !scene::spaces::hasSpaces(graph, e.source))
            return n->as<glm::dmat4>(scene::Transform::kMatrix);
        return glm::inverse(world(parentSource)) * world(e.source);
    }

    /** True when the exported local is the node's own local matrix, so its channels and curves apply directly. */
    bool ownLocal(size_t i) const
    {
        const ExportNode& e = out.nodes[i];
        if (e.parent >= 0)
            return out.nodes[size_t(e.parent)].source == graph.parent(e.source);
        if (const NodeId above = graph.parent(e.source); opts.localRoots && above != scene::kNoNode && above != opts.relativeTo)
            return !scene::spaces::hasSpaces(graph, e.source);
        if (opts.relativeTo != scene::kNoNode)
            return graph.parent(e.source) == opts.relativeTo && !scene::spaces::hasSpaces(graph, e.source);
        return graph.parent(e.source) == scene::kNoNode && nearMat(rootInv[i], glm::dmat4(1.0), 1e-12);
    }

    void walk(NodeId id, int parent, const glm::dmat4& inv, bool isRoot, bool children)
    {
        using namespace scene;
        Node* n = graph.node(id);
        if (!n || !n->type().isA("transform") || indexOf.count(id) || proxy::excludedFromExport(graph, id))
            return;
        if (opts.deformOnly && (opts.kind == ExportKind::Skeletal || opts.kind == ExportKind::Animation) && !rigel::isDeform(*n))
        {
            if (!weighted.count(id))
            {
                // Its children hang off the nearest exported ancestor instead.
                if (children)
                    for (NodeId c : graph.children(id))
                        walk(c, parent, inv, isRoot, true);
                return;
            }
            weightedNonDeform.push_back(n->name());
        }
        const bool flatten = opts.worldSpace && opts.kind == ExportKind::Static;
        ExportNode e;
        e.name = n->name();
        e.source = id;
        e.unrealSocket = scene::pins::socketOf(*n);
        e.properties = scene::attrComponents::exportProperties(graph, id);
        if (n->type().isA("mesh") && !n->type().isA("lattice") && opts.kind != ExportKind::Sequence)
            e.mesh = exportMeshOf(id);
        e.camera = cameraOf(*n);
        if (n->type().isA("light"))
            e.light = scene::lights::settings(*n);
        if (lod::isGroup(*n) && !flatten)
        {
            e.lodGroup = true;
            if (!lod::parseSizes(n->as<std::string>(LodGroup::kScreenSizes)).empty())
                e.lodScreenSizes = lod::screenSizes(*n, lod::levels(graph, id).size());
        }
        const glm::dmat4 w = world(id);
        if (flatten)
        {
            glm::dmat4 rel = inv * w;
            if (e.mesh)
                e.bake = rel;
            else
                e.local = rel;
        }
        else
        {
            e.parent = parent;
            if (const NodeId up = graph.parent(id); isRoot && opts.localRoots && up != kNoNode && up != opts.relativeTo)
                e.local = spaces::hasSpaces(graph, id) ? glm::inverse(world(up)) * w : n->as<glm::dmat4>(Transform::kMatrix);
            else if (isRoot)
                e.local = inv * w;
            else if (const NodeId above = out.nodes[size_t(parent)].source; above != graph.parent(id))
                e.local = glm::inverse(world(above)) * w;
            else if (spaces::hasSpaces(graph, id) && graph.parent(id) != kNoNode)
                e.local = glm::inverse(world(graph.parent(id))) * w;
            else
                e.local = n->as<glm::dmat4>(Transform::kMatrix);
        }
        e.joint = rig::isJoint(*n);
        e.pre = static_cast<const Transform*>(n)->preRotation();
        e.post = static_cast<const Transform*>(n)->rotateAxisMatrix();
        e.rotateOrder = RotateOrder(std::clamp<int64_t>(n->as<int64_t>(Transform::kRotateOrder), 0, 5));
        e.translate = n->as<glm::dvec3>(Transform::kTranslate);
        e.rotate = n->as<glm::dvec3>(Transform::kRotate);
        e.scale = n->as<glm::dvec3>(Transform::kScale);
        e.plainTrs = !flatten && plainChannels(*n) && nearMat(e.local, n->as<glm::dmat4>(Transform::kMatrix));
        int index = int(out.nodes.size());
        indexOf[id] = index;
        out.nodes.push_back(std::move(e));
        rootInv.push_back(inv);
        skinOf.push_back(kNoNode);
        if (!children)
            return;
        for (NodeId c : graph.children(id))
            walk(c, flatten ? -1 : index, inv, false, true);
        if (arrays::isArray(*n) && opts.kind == ExportKind::Static)
            addArrayCopies(id, index, inv, glm::dmat4(1.0), "", flatten, w);
    }

    /**
     * The generated copies of an array (static exports): each source's subtree again per copy, named with the copy's
     * number, sharing the source's export meshes so they write one Geometry. `xf` places the array itself when it is a
     * copy inside another array, and `arrayWorld` is where it then sits.
     */
    void addArrayCopies(NodeId array, int index, const glm::dmat4& inv, const glm::dmat4& xf, const std::string& suffix, bool flatten,
                        const glm::dmat4& arrayWorld)
    {
        const std::vector<glm::dmat4> copies = scene::arrays::copyTransforms(*graph.node(array));
        for (size_t k = 1; k < copies.size(); ++k)
            for (NodeId s : scene::arrays::sources(graph, array))
                walkCopy(s, flatten ? -1 : index, inv, xf * copies[k], suffix + std::format("_{}", k), flatten, arrayWorld);
    }

    void walkCopy(NodeId id, int parent, const glm::dmat4& inv, const glm::dmat4& xf, const std::string& suffix, bool flatten,
                  const glm::dmat4& parentWorld)
    {
        using namespace scene;
        Node* n = graph.node(id);
        if (!n || !n->type().isA("transform"))
            return;
        ExportNode e;
        e.name = n->name() + suffix;
        e.source = id;
        e.unrealSocket = scene::pins::socketOf(*n);
        e.properties = scene::attrComponents::exportProperties(graph, id);
        if (n->type().isA("mesh") && !n->type().isA("lattice"))
            e.mesh = exportMeshOf(id);
        const glm::dmat4 w = xf * world(id);
        if (flatten)
        {
            if (e.mesh)
                e.bake = inv * w;
            else
                e.local = inv * w;
        }
        else
        {
            e.parent = parent;
            e.local = parent < 0 ? inv * w : glm::inverse(parentWorld) * w;
        }
        e.joint = rig::isJoint(*n);
        e.pre = static_cast<const Transform*>(n)->preRotation();
        e.post = static_cast<const Transform*>(n)->rotateAxisMatrix();
        e.rotateOrder = RotateOrder(std::clamp<int64_t>(n->as<int64_t>(Transform::kRotateOrder), 0, 5));
        e.rotate = n->as<glm::dvec3>(Transform::kRotate);
        const int index = int(out.nodes.size());
        out.nodes.push_back(std::move(e));
        rootInv.push_back(inv);
        skinOf.push_back(kNoNode);
        for (NodeId c : graph.children(id))
            walkCopy(c, flatten ? -1 : index, inv, xf, suffix, flatten, w);
        if (arrays::isArray(*n))
            addArrayCopies(id, index, inv, xf, suffix, flatten, w);
    }

    /** Exported node index and its morph, for the meshes that export morph targets. */
    std::vector<std::pair<size_t, NodeId>> morphed;

    /** Where each of out.curveNames is sampled from: a drivenChannel's output, one weight of a morph, or a plug (tension). */
    struct CurveSource
    {
        NodeId channel = scene::kNoNode;
        NodeId morph = scene::kNoNode;
        size_t index = 0;
        scene::PlugRef plug{scene::kNoNode, -1, -1};
    };
    std::vector<CurveSource> curveSources;

    void addSkeletons();
    void buildMorphs();
    void buildCurves();
    void buildSkins();
    void applyBindPose();
    void keepSkeletonOnly();
    void sampleTake();
    std::optional<ExportTake> sampleRange(const std::string& name, double start, double end, double fps, bool keep);
};

/** Brings in the joints of every skin that the given roots did not cover, from their top joint down. */
void Gather::addSkeletons()
{
    LILA_ZONE();

    using namespace scene;
    for (size_t i = 0; i < out.nodes.size(); ++i)
    {
        if (!out.nodes[i].mesh)
            continue;
        NodeId skin = skin::find(graph, out.nodes[i].source);
        if (skin == kNoNode)
            continue;
        skinOf[i] = skin;
        for (const skin::Influence& inf : skin::influences(graph, skin))
        {
            if (inf.joint == kNoNode || indexOf.count(inf.joint))
                continue;
            NodeId top = inf.joint;
            while (graph.parent(top) != kNoNode && rig::isJoint(*graph.node(graph.parent(top))))
                top = graph.parent(top);
            walk(top, -1, rootInv[i], true, true);
            if (!indexOf.count(inf.joint))
                out.warnings.push_back(std::format("{}: influence {} could not be exported", out.nodes[i].name, inf.name));
        }
    }
}

/** Skin clusters per skinned mesh, weights pruned to the influence limit; the mesh goes out in its bind shape. */
void Gather::buildSkins()
{
    LILA_ZONE();

    using namespace scene;
    for (size_t i = 0; i < out.nodes.size(); ++i)
    {
        NodeId skinId = skinOf[i];
        if (skinId == kNoNode)
            continue;
        ExportNode& e = out.nodes[i];
        MeshPtr bind = skin::inputMesh(graph, skinId);
        if (!bind)
            continue;
        // Unreal skins linearly, so whatever the skin's method and the deformers after it add goes out as fitted weights.
        std::optional<WeightTable> routed;
        std::string routeNote;
        try
        {
            routed = skinbake::exportWeights(graph, skinId, &routeNote);
        }
        catch (const std::exception& ex)
        {
            out.warnings.push_back(std::format("{}: fitting linear weights to its deformation failed: {}", e.name, ex.what()));
        }
        if (routed)
            out.warnings.push_back(std::format("{}: {}", e.name, routeNote));
        else if (auto src = graph.source({e.source, MeshNode::kInMesh, -1}); !src || src->node != skinId)
            out.warnings.push_back(std::format("{}: modifiers after the skinCluster are not exported", e.name));
        e.mesh = bind;

        auto skin = std::make_shared<ExportSkin>();
        const Node* sc = graph.node(skinId);
        skin->dualQuaternion = sc->as<int64_t>(SkinClusterNode::kSkinningMethod) == 1;
        skin->meshBindWorld = rootInv[i] * sc->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);

        std::unordered_map<uint32_t, size_t> clusterOf;
        for (const skin::Influence& inf : skin::influences(graph, skinId))
        {
            auto it = indexOf.find(inf.joint);
            if (inf.joint == kNoNode || it == indexOf.end())
                continue;
            ExportSkin::Cluster c;
            c.joint = it->second;
            glm::dmat4 bindPre = graph.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
            c.bindWorld = rootInv[size_t(c.joint)] * glm::inverse(bindPre);
            clusterOf[inf.index] = skin->clusters.size();
            skin->clusters.push_back(std::move(c));
        }

        WeightTable t = routed ? std::move(*routed) : skin::table(graph, skinId);
        int pruned = 0;
        int32_t vertex = 0;
        for (geo::Index p = 0; p < bind->pointCapacity(); ++p)
        {
            if (!bind->pointValid(p))
                continue;
            if (p < t.points.size())
            {
                std::erase_if(t.points[p], [&](const WeightTable::Entry& en) { return !clusterOf.count(en.first); });
                if (opts.maxInfluences > 0 && int(t.points[p].size()) > opts.maxInfluences)
                    ++pruned;
                t.prune(p, opts.maxInfluences, 1e-5f, {});
                for (const WeightTable::Entry& en : t.points[p])
                {
                    ExportSkin::Cluster& c = skin->clusters[clusterOf[en.first]];
                    c.vertices.push_back(vertex);
                    c.weights.push_back(double(en.second));
                }
            }
            ++vertex;
        }
        if (pruned)
            out.warnings.push_back(std::format("{}: {} point(s) pruned to {} influences", e.name, pruned, opts.maxInfluences));
        e.skin = std::move(skin);
    }
}

/**
 * Morph targets from each mesh's morph deformer. The exported base is the neutral shape: the current morph weights are
 * taken back out of the points the morph fed, so the targets add on top of it as they do in Unreal.
 */
void Gather::buildMorphs()
{
    LILA_ZONE();

    using namespace scene;
    for (size_t i = 0; i < out.nodes.size(); ++i)
    {
        ExportNode& e = out.nodes[i];
        if (!e.mesh)
            continue;
        NodeId bs = morphs::find(graph, e.source);
        if (bs == kNoNode || !modifiers::exportEnabled(*graph.node(bs)) || !graph.node(bs)->as<bool>(DeformerNode::kEnabled))
            continue;
        std::vector<morphs::Target> targets = morphs::targets(graph, bs);
        if (targets.empty())
            continue;
        MeshPtr input = std::get<MeshPtr>(graph.value({bs, DeformerNode::kInMesh, -1}));
        if (!input || input->pointCapacity() != e.mesh->pointCapacity())
        {
            out.warnings.push_back(std::format("{}: the points change after the morph; morph targets not exported", e.name));
            continue;
        }
        // Anything between the morph and what is exported moves points the deltas don't know about.
        const std::vector<NodeId> stack = modifiers::stack(graph, e.source);
        const NodeId stop = skinOf[i];
        for (auto it = std::find(stack.begin(), stack.end(), bs) + 1; it < stack.end() && *it != stop; ++it)
            if (const auto* m = static_cast<const ModifierNode*>(graph.node(*it)); m->as<bool>(m->enabledAttr) && modifiers::exportEnabled(*m))
            {
                out.warnings.push_back(std::format("{}: {} sits after the morph; its effect is baked into the base at the current pose",
                                                   e.name, m->name()));
                break;
            }

        const std::vector<float> map = deformers::pointWeights(graph, bs, input->pointCapacity());
        std::vector<int32_t> vertexOf(input->pointCapacity(), -1);
        int32_t next = 0;
        for (geo::Index p = 0; p < input->pointCapacity(); ++p)
            if (e.mesh->pointValid(p))
                vertexOf[p] = next++;

        auto base = std::make_shared<geo::Mesh>(*e.mesh);
        auto morphs = std::make_shared<ExportMorphs>();
        for (const morphs::Target& t : targets)
        {
            ExportMorphs::Target mt;
            mt.name = t.name;
            mt.weight = t.weight;
            for (size_t k = 0; k < t.indices.size(); ++k)
            {
                uint32_t p = t.indices[k];
                if (p >= vertexOf.size() || vertexOf[p] < 0)
                    continue;
                glm::dvec3 d = t.deltas[k] * double(map[p]);
                base->setPosition(p, base->position(p) - d * t.weight);
                mt.vertices.push_back(vertexOf[p]);
                mt.deltas.push_back(d);
            }
            morphs->targets.push_back(std::move(mt));
        }
        if (base->cornerAttrs().has(geo::attr::kNormal))
            geo::updateCornerNormals(*base);
        e.mesh = std::move(base);
        e.morphs = std::move(morphs);
        morphed.push_back({i, bs});
    }
}

/**
 * Named curves for Unreal: every driven channel with a curveName, and in animation exports the morph weights of meshes
 * skinned to the exported joints, which have no mesh to carry them. They hang off the first root joint.
 */
void Gather::buildCurves()
{
    LILA_ZONE();

    using namespace scene;
    for (size_t i = 0; i < out.nodes.size() && out.curveHolder < 0; ++i)
        if (out.nodes[i].joint && out.nodes[i].parent < 0)
            out.curveHolder = int(i);
    if (out.curveHolder < 0 && !out.nodes.empty())
        out.curveHolder = 0;
    if (out.curveHolder < 0)
        return;
    std::set<std::string> names;
    for (NodeId c : drivers::channels(graph))
    {
        const std::string& name = graph.node(c)->as<std::string>(drivers::kCurveName);
        if (!name.empty() && names.insert(name).second)
        {
            out.curveNames.push_back(name);
            curveSources.push_back({c, kNoNode, 0});
        }
    }
    auto onSkeleton = [&](NodeId mesh) {
        const NodeId s = mesh != kNoNode ? skin::find(graph, mesh) : kNoNode;
        if (s == kNoNode)
            return false;
        for (const skin::Influence& inf : skin::influences(graph, s))
            if (indexOf.count(inf.joint))
                return true;
        return false;
    };
    for (const deformers::TensionCurve& t : deformers::tensionCurves(graph))
        if (onSkeleton(t.mesh) && names.insert(t.name).second)
        {
            out.curveNames.push_back(t.name);
            curveSources.push_back({kNoNode, kNoNode, 0, {t.deformer, t.attr, -1}});
        }
    if (opts.kind != ExportKind::Animation)
        return;
    for (NodeId id : graph.nodes())
    {
        if (!graph.node(id)->type().isA("morph") || !modifiers::exportEnabled(*graph.node(id)))
            continue;
        if (!onSkeleton(modifiers::meshOf(graph, id)))
            continue;
        std::vector<morphs::Target> targets = morphs::targets(graph, id);
        for (size_t k = 0; k < targets.size(); ++k)
            if (names.insert(targets[k].name).second)
            {
                out.curveNames.push_back(targets[k].name);
                curveSources.push_back({kNoNode, id, k});
            }
    }
}

/** Influence joints take their bind-time locals, the reference pose Unreal builds the skeleton from. */
void Gather::applyBindPose()
{
    LILA_ZONE();

    std::vector<std::optional<glm::dmat4>> bind(out.nodes.size());
    for (const ExportNode& e : out.nodes)
        if (e.skin)
            for (const ExportSkin::Cluster& c : e.skin->clusters)
            {
                auto& slot = bind[size_t(c.joint)];
                if (!slot)
                    slot = c.bindWorld;
                else if (!nearMat(*slot, c.bindWorld, 1e-5))
                    out.warnings.push_back(std::format("{}: bound in different poses by different skins; using the first",
                                                       out.nodes[size_t(c.joint)].name));
            }
    for (size_t i = 0; i < out.nodes.size(); ++i)
    {
        ExportNode& e = out.nodes[i];
        glm::dmat4 parentBind = e.parent >= 0 ? out.nodes[size_t(e.parent)].bindWorld : glm::dmat4(1.0);
        if (bind[i])
        {
            glm::dmat4 local = glm::inverse(parentBind) * *bind[i];
            if (!nearMat(local, e.local, 1e-6))
            {
                e.local = local;
                e.plainTrs = false;
            }
        }
        e.bindWorld = parentBind * e.local;
    }
}

/** Animation kind: joints and the nodes above them, without meshes; every transform when there are no joints. */
void Gather::keepSkeletonOnly()
{
    std::vector<char> keep(out.nodes.size(), 0);
    bool anyJoint = false;
    for (size_t i = 0; i < out.nodes.size(); ++i)
        if (out.nodes[i].joint)
        {
            anyJoint = true;
            for (int k = int(i); k >= 0 && !keep[size_t(k)]; k = out.nodes[size_t(k)].parent)
                keep[size_t(k)] = 1;
        }
    std::vector<int> remap(out.nodes.size(), -1);
    std::vector<ExportNode> nodes;
    std::vector<glm::dmat4> inv;
    for (size_t i = 0; i < out.nodes.size(); ++i)
    {
        if (anyJoint && !keep[i])
            continue;
        ExportNode e = std::move(out.nodes[i]);
        e.mesh.reset();
        e.skin.reset();
        e.parent = e.parent >= 0 ? remap[size_t(e.parent)] : -1;
        remap[i] = int(nodes.size());
        nodes.push_back(std::move(e));
        inv.push_back(rootInv[i]);
    }
    out.nodes = std::move(nodes);
    rootInv = std::move(inv);
    indexOf.clear();
    for (size_t i = 0; i < out.nodes.size(); ++i)
        indexOf[out.nodes[i].source] = int(i);
}

/**
 * The node's translate/rotate/scale curves, when nothing but curves drives its local matrix and they hold their end
 * values outside their keys (or cover the range). Unanimated channels get one key at their value.
 */
std::optional<std::array<anim::Curve, 9>> directCurves(Graph& g, NodeId id, double start, double end)
{
    using namespace scene;
    Node* n = g.node(id);
    std::array<anim::Curve, 9> out;
    bool any = false;
    const int channels[3] = {Transform::kTranslate, Transform::kRotate, Transform::kScale};
    for (int a = 0; a < int(n->type().attrs.size()); ++a)
    {
        const AttrDef& def = n->type().attrs[size_t(a)];
        if (!(def.flags & AttrInput))
            continue;
        const int group = a == channels[0] ? 0 : a == channels[1] ? 1 : a == channels[2] ? 2 : -1;
        if (group < 0)
        {
            if (a != Transform::kParentMatrix && a != Transform::kVisibility && g.source({id, a, -1}))
                return std::nullopt;
            continue;
        }
        if (auto whole = g.source({id, a, -1}))
        {
            Node* cn = g.node(whole->node);
            if (group != 1 || !cn || !anim::isRotationCurve(*cn))
                return std::nullopt;
            // FBX has only Euler curves: the rotation curve's own keys, plus frames only where those miss it.
            anim::RotationCurve rc = anim::rotationCurveOf(*cn).looped();
            // Modifiers change the output away from the keys, so such curves export sampled.
            if (rc.keys.empty() || !anim::modifiersOf(*cn).empty())
                return std::nullopt;
            bool covered = rc.keys.front().time <= start + 1e-9 && rc.keys.back().time >= end - 1e-9;
            if (!covered && (rc.pre != anim::Infinity::Constant || rc.post != anim::Infinity::Constant))
                return std::nullopt;
            std::vector<double> times;
            const double fps = anim::playback().fps > 0.0 ? anim::playback().fps : 30.0;
            for (long long f = 0, count = std::llround((end - start) * fps); f <= count; ++f)
                times.push_back(start + double(f) / fps);
            std::array<anim::Curve, 3> euler = anim::eulerKeysFor(rc, times);
            for (int c = 0; c < 3; ++c)
            {
                euler[size_t(c)].pre = euler[size_t(c)].post = anim::Infinity::Constant;
                euler[size_t(c)].loop = false;
                out[size_t(3 + c)] = std::move(euler[size_t(c)]);
            }
            any = true;
            continue;
        }
        glm::dvec3 rest = n->as<glm::dvec3>(a);
        for (int c = 0; c < 3; ++c)
        {
            anim::Curve& curve = out[size_t(group * 3 + c)];
            auto src = g.source({id, a, c});
            if (!src)
            {
                curve.keys.push_back({.time = start, .value = rest[c]});
                continue;
            }
            Node* cn = g.node(src->node);
            if (!anim::isCurve(*cn))
                return std::nullopt;
            curve = anim::curveOf(*cn).looped();
            if (curve.keys.empty() || !anim::modifiersOf(*cn).empty())
                return std::nullopt;
            bool covered = curve.keys.front().time <= start + 1e-9 && curve.keys.back().time >= end - 1e-9;
            if (!covered && (curve.pre != anim::Infinity::Constant || curve.post != anim::Infinity::Constant))
                return std::nullopt;
            any = true;
        }
    }
    if (!any)
        return std::nullopt;
    return out;
}

void Gather::sampleTake()
{
    LILA_ZONE();

    anim::Playback& pb = anim::playback();
    std::vector<NodeId> chosen;
    if (!opts.takes.empty())
        chosen = anim::selectTakes(graph, opts.takes.size() == 1 ? nlohmann::json(opts.takes[0]) : nlohmann::json(opts.takes));
    if (chosen.empty())
    {
        std::string name = opts.takeName;
        if (name.empty())
            for (NodeId t : anim::activeTakes(graph))
            {
                name = graph.node(t)->name();
                break;
            }
        out.take = sampleRange(name.empty() ? "Take 001" : name, std::isnan(opts.start) ? pb.minTime : opts.start,
                               std::isnan(opts.end) ? pb.maxTime : opts.end, pb.fps, false);
        return;
    }
    for (NodeId t : chosen)
    {
        std::optional<ExportTake> sampled;
        anim::withTake(graph, t, [&] {
            const scene::Node& n = *graph.node(t);
            sampled = sampleRange(n.name(), n.as<double>(anim::TakeNode::kStart), n.as<double>(anim::TakeNode::kEnd),
                                  n.as<double>(anim::TakeNode::kFps), true);
        });
        if (!out.take)
            out.take = std::move(sampled);
        else
            out.extraTakes.push_back(std::move(*sampled));
    }
}

std::optional<ExportTake> Gather::sampleRange(const std::string& name, double start, double end, double fps, bool keep)
{
    LILA_ZONE();

    ExportTake take;
    take.name = name;
    take.fps = fps > 0.0 ? fps : 30.0;
    take.start = start;
    take.end = end;
    if (take.end < take.start)
        std::swap(take.start, take.end);
    const long long frames = std::llround((take.end - take.start) * take.fps);
    for (long long f = 0; f <= frames; ++f)
        take.times.push_back(take.start + double(f) / take.fps);
    take.end = take.times.back();

    const size_t count = out.nodes.size();
    take.locals.assign(count, {});
    take.keyed.assign(count, 0);
    for (size_t i = 0; i < count; ++i)
        take.keyed[i] = channelsDriven(graph, out.nodes[i].source);
    take.curves.assign(count, std::nullopt);
    std::vector<char> sampled(count, 1);
    // A camera's or light's file frame is turned from its channels, and so is its children's parent frame.
    auto turned = [&](size_t i) { return out.nodes[i].camera || out.nodes[i].light; };
    auto reframed = [&](size_t i) { return turned(i) || (out.nodes[i].parent >= 0 && turned(size_t(out.nodes[i].parent))); };
    if (!opts.bakeAnimation)
        for (size_t i = 0; i < count; ++i)
            if (!reframed(i) && plainChannels(*graph.node(out.nodes[i].source)) && ownLocal(i) &&
                !graph.source({out.nodes[i].source, scene::Joint::kIkBlend, -1}) &&
                !graph.source({out.nodes[i].source, scene::Joint::kIkStretch, -1}))
                if ((take.curves[i] = directCurves(graph, out.nodes[i].source, take.start, take.end)))
                    sampled[i] = 0;

    const double saved = graph.time();
    for (size_t i = 0; i < count; ++i)
        if (sampled[i])
            take.locals[i].reserve(take.times.size());
    take.cameras.assign(count, {});
    take.morphWeights.assign(count, {});
    for (const auto& [i, bs] : morphed)
        take.morphWeights[i].assign(out.nodes[i].morphs->targets.size(), {});
    take.namedCurves.assign(curveSources.size(), {});
    // Meshes a meshCache drives, unless a skin or morphs already carry their motion.
    std::vector<size_t> cached;
    for (size_t i = 0; i < count; ++i)
        if (const ExportNode& e = out.nodes[i]; e.mesh && !e.skin && !e.morphs && scene::meshCache::find(graph, e.source) != scene::kNoNode)
            cached.push_back(i);
    take.meshPoints.assign(count, {});
    for (double t : take.times)
    {
        graph.setTime(t);
        graph.evaluate();
        for (size_t i : cached)
            if (const scene::MeshPtr m = scene::modifiers::exportMesh(graph, scene::instancing::root(graph, out.nodes[i].source));
                m && m->pointCapacity() == out.nodes[i].mesh->pointCapacity())
                take.meshPoints[i].emplace_back(m->positions().begin(), m->positions().end());
        for (size_t i = 0; i < count; ++i)
        {
            if (sampled[i])
                take.locals[i].push_back(localOf(i));
            if (out.nodes[i].camera)
                take.cameras[i].push_back(*cameraOf(*graph.node(out.nodes[i].source)));
        }
        std::map<NodeId, std::vector<double>> morphNow;
        for (size_t c = 0; c < curveSources.size(); ++c)
        {
            const CurveSource& src = curveSources[c];
            double v = 0.0;
            if (src.channel != scene::kNoNode)
                v = std::get<double>(graph.value({src.channel, scene::drivers::kChannelOutput, -1}));
            else if (src.plug.node != scene::kNoNode)
                v = std::get<double>(graph.value(src.plug));
            else
            {
                auto it = morphNow.find(src.morph);
                if (it == morphNow.end())
                    it = morphNow.emplace(src.morph, scene::morphs::weights(graph, src.morph)).first;
                v = src.index < it->second.size() ? it->second[src.index] : 0.0;
            }
            take.namedCurves[c].push_back(v);
        }
        for (const auto& [i, bs] : morphed)
        {
            std::vector<double> weights = scene::morphs::weights(graph, bs);
            auto& perTarget = take.morphWeights[i];
            for (size_t k = 0; k < perTarget.size(); ++k)
                perTarget[k].push_back(k < weights.size() ? weights[k] : 0.0);
        }
    }
    graph.setTime(saved);
    graph.evaluate();
    for (size_t i : cached)
        if (take.meshPoints[i].size() != take.times.size())
        {
            out.warnings.push_back(std::format("{}: its point count changes over the range; its point cache is not exported", out.nodes[i].name));
            take.meshPoints[i].clear();
        }
    for (const auto& [i, bs] : morphed)
    {
        auto& perTarget = take.morphWeights[i];
        const auto& targets = out.nodes[i].morphs->targets;
        bool still = true;
        for (size_t k = 0; k < perTarget.size() && still; ++k)
            still = std::all_of(perTarget[k].begin(), perTarget[k].end(), [&](double w) { return std::abs(w - targets[k].weight) < 1e-9; });
        if (still)
            perTarget.clear();
    }

    bool animated = false;
    for (size_t i = 0; i < count; ++i)
    {
        auto& samples = take.locals[i];
        bool still = std::all_of(samples.begin(), samples.end(), [&](const glm::dmat4& m) { return nearMat(m, out.nodes[i].local, 1e-9); });
        if (still)
            samples.clear();
        auto& cams = take.cameras[i];
        if (std::all_of(cams.begin(), cams.end(), [&](const ExportCamera& c) { return c == *out.nodes[i].camera; }))
            cams.clear();
        animated = animated || !samples.empty() || take.curves[i] || !take.morphWeights[i].empty() || !cams.empty() || !take.meshPoints[i].empty();
    }
    animated = animated || !curveSources.empty();
    if (animated || keep || opts.kind == ExportKind::Animation)
        return take;
    return std::nullopt;
}
} // namespace

namespace
{
/**
 * FBX and engines address bones and meshes by name, so names that are only unique among siblings in the scene can
 * collide in one file. Warns once per clashing name; fixDuplicateJointNames renames clashing joints.
 */
void warnDuplicateNames(scene::Graph& graph, ExportScene& out)
{
    std::map<std::pair<bool, std::string>, std::vector<scene::NodeId>> byName; // (joint, name)
    for (const ExportNode& e : out.nodes)
        if (e.joint || e.mesh)
            byName[{e.joint, e.name}].push_back(e.source);
    for (auto& [key, ids] : byName)
    {
        if (ids.size() < 2)
            continue;
        std::string list;
        for (scene::NodeId id : ids)
            list += (list.empty() ? "" : ", ") + graph.fullPath(id);
        if (key.first)
            out.warnings.push_back(std::format("joint name '{}' is used {} times in one skeleton ({}); bone names must be unique, "
                                               "run fixDuplicateJointNames to rename them",
                                               key.second, ids.size(), list));
        else
            out.warnings.push_back(std::format("{} exported meshes are named '{}' ({}); importers may merge or rename them",
                                               ids.size(), key.second, list));
    }
}
} // namespace

namespace
{
/** ExportOptions::textures: the material behind every slot the exported meshes use. */
void gatherMaterials(scene::Graph& graph, ExportScene& out)
{
    using namespace scene;
    auto map = [](const Node& n, int attr) -> std::optional<ExportTexture> {
        const std::string& stored = n.as<std::string>(attr);
        if (stored.empty())
            return std::nullopt;
        const material::TextureSampling s = materials::sampling(n, attr);
        // FBX has only repeat and clamp: mirror repeats, black clamps.
        return ExportTexture{materials::resolvePath(stored), s.wrapU == material::TextureWrap::Clamp || s.wrapU == material::TextureWrap::Black,
                             s.wrapV == material::TextureWrap::Clamp || s.wrapV == material::TextureWrap::Black};
    };
    for (const ExportNode& e : out.nodes)
    {
        if (!e.mesh)
            continue;
        for (const std::string& slot : e.mesh->materialNames())
        {
            const Node* n = materials::find(graph, slot);
            if (!n || out.materials.count(slot))
                continue;
            ExportMaterial& m = out.materials[slot];
            m.baseColor = n->as<glm::dvec3>(material::kBaseColor);
            m.emissive = n->as<glm::dvec3>(material::kEmissiveColor) * n->as<double>(material::kEmissiveIntensity);
            m.baseColorMap = map(*n, material::kBaseColorMap);
            m.normalMap = map(*n, material::kNormalMap);
            m.emissiveMap = map(*n, material::kEmissiveMap);
        }
    }
}
} // namespace

namespace
{
/** The collision sources' convex meshes beside each exported mesh, named the way Unreal pairs them with it. */
void addCollision(scene::Graph& graph, ExportScene& out)
{
    const std::vector<std::string> ids = collisionSources().idsInOrder();
    if (ids.empty())
        return;
    const size_t count = out.nodes.size();
    for (size_t i = 0; i < count; ++i)
    {
        if (!out.nodes[i].mesh || out.nodes[i].source == scene::kNoNode || out.nodes[i].joint)
            continue;
        std::vector<geo::Mesh> hulls;
        for (const std::string& id : ids)
            if (const CollisionSource* s = collisionSources().find(id); s && *s)
                for (geo::Mesh& m : (*s)(graph, out.nodes[i].source))
                    hulls.push_back(std::move(m));
        for (size_t k = 0; k < hulls.size(); ++k)
        {
            const ExportNode& owner = out.nodes[i];
            ExportNode c;
            c.name = std::format("UCX_{}_{:02}", owner.name, k + 1);
            c.parent = owner.parent;
            c.local = owner.local;
            c.bake = owner.bake;
            c.mesh = std::make_shared<const geo::Mesh>(std::move(hulls[k]));
            out.nodes.push_back(std::move(c));
        }
    }
}
} // namespace

Registry<CollisionSource>& collisionSources()
{
    static Registry<CollisionSource> r;
    return r;
}

bool addCollisionSource(ext::Context& ctx, const std::string& id, CollisionSource source)
{
    if (collisionSources().contains(id))
    {
        log::error("extension {}: collision source '{}' is already registered", ctx.owner(), id);
        return false;
    }
    ctx.add(collisionSources(), "collisionSource", id, std::move(source));
    return true;
}

ExportScene gatherExport(scene::Graph& graph, const std::vector<scene::NodeId>& roots, const ExportOptions& opts)
{
    LILA_ZONE();

    using namespace scene;
    graph.evaluate();
    ExportScene out;
    out.upAxis = math::upAxis();
    out.fps = anim::playback().fps;
    Gather g{graph, opts, out, {}, {}, {}};

    std::set<NodeId> wanted;
    for (NodeId id : roots)
        if (Node* n = graph.node(id); n && n->type().isA("transform"))
            wanted.insert(id);
    auto coveredByAncestor = [&](NodeId id) {
        for (NodeId p = graph.parent(id); p != kNoNode; p = graph.parent(p))
            if (wanted.count(p) && !opts.childless.count(p))
                return true;
        return false;
    };

    if (opts.kind != ExportKind::Static && opts.deformOnly)
        for (NodeId id : graph.nodes())
            if (Node* n = graph.node(id); n && n->type().isA("skinCluster"))
                for (const skin::Influence& inf : skin::influences(graph, id))
                    if (inf.joint != kNoNode)
                        g.weighted.insert(inf.joint);

    std::set<NodeId> done;
    for (NodeId id : roots)
    {
        if (!wanted.count(id) || (opts.children && coveredByAncestor(id)) || !done.insert(id).second)
            continue;
        glm::dmat4 rootInv(1.0);
        if (opts.atOrigin)
            rootInv = glm::inverse(g.world(id));
        else if (opts.relativeTo != kNoNode && graph.node(opts.relativeTo))
            rootInv = glm::inverse(g.world(opts.relativeTo));
        g.walk(id, -1, rootInv, true, opts.children && !opts.childless.count(id));
    }
    warnDuplicateNames(graph, out);
    if (opts.textures)
        gatherMaterials(graph, out);
    if (opts.collision && opts.kind == ExportKind::Static)
        addCollision(graph, out);

    if (opts.kind == ExportKind::Static || opts.kind == ExportKind::Sequence)
    {
        for (ExportNode& e : out.nodes)
            e.bindWorld = e.parent >= 0 ? out.nodes[size_t(e.parent)].bindWorld * e.local : e.local;
        if (opts.kind == ExportKind::Sequence && opts.animation)
            g.sampleTake();
        return out;
    }
    if (opts.worldSpace)
        out.warnings.push_back("worldSpace is ignored for skeletal and animation exports");
    g.addSkeletons();
    if (!g.weightedNonDeform.empty())
    {
        std::string list;
        for (const std::string& n : g.weightedNonDeform)
            list += (list.empty() ? "" : ", ") + n;
        out.warnings.push_back(std::format("{} non-deform joint(s) carry skin weights and were exported: {}", g.weightedNonDeform.size(), list));
    }
    g.buildSkins();
    g.applyBindPose();
    if (opts.kind == ExportKind::Skeletal)
        g.buildMorphs();
    if (opts.kind == ExportKind::Animation)
        g.keepSkeletonOnly();
    g.buildCurves();
    if (opts.animation)
        g.sampleTake();
    return out;
}

namespace
{
/**
 * Post-multiplied onto a camera's file-space local matrix: an FBX camera looks down its local +X with +Y up, a Lila
 * camera down -Z, so the file frame is the Lila one turned 90 degrees about its local Y (then into file axes).
 */
glm::dmat4 exportCameraFrame(const glm::dmat3& basis)
{
    const glm::dmat3 lilaFromFbx(glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0), glm::dvec3(1, 0, 0));
    return glm::dmat4(basis * lilaFromFbx);
}

/** The same for lights: an FBX light shines down its local -Y with -Z up, a Lila light down -Z with +Y up. */
glm::dmat4 exportLightFrame(const glm::dmat3& basis)
{
    const glm::dmat3 lilaFromFbx(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, -1, 0));
    return glm::dmat4(basis * lilaFromFbx);
}

scene::lights::Settings inUnits(scene::lights::Settings l, double k)
{
    l.range *= k;
    l.width *= k;
    l.height *= k;
    l.sourceRadius *= k;
    return l;
}

ExportCamera inUnits(ExportCamera c, double k)
{
    c.nearClip *= k;
    c.farClip *= k;
    c.orthoWidth *= k;
    return c;
}

/** The snapshot in file units: every length divided by cmPerUnit, rotations and scales unchanged. */
ExportScene inUnits(const ExportScene& in, double cmPerUnit)
{
    LILA_ZONE();

    ExportScene out = in;
    const double k = 1.0 / cmPerUnit;
    const glm::dmat4 s = glm::scale(glm::dmat4(1.0), glm::dvec3(k));
    const glm::dmat4 sInv = glm::scale(glm::dmat4(1.0), glm::dvec3(cmPerUnit));
    auto conj = [&](const glm::dmat4& m) { return s * m * sInv; };
    for (ExportNode& n : out.nodes)
    {
        n.local = conj(n.local);
        n.translate *= k;
        n.bake = s * n.bake;
        n.bindWorld = conj(n.bindWorld);
        if (n.camera)
            n.camera = inUnits(*n.camera, k);
        if (n.light)
            n.light = inUnits(*n.light, k);
        if (n.skin)
        {
            auto skin = std::make_shared<ExportSkin>(*n.skin);
            skin->meshBindWorld = conj(skin->meshBindWorld);
            for (ExportSkin::Cluster& c : skin->clusters)
                c.bindWorld = conj(c.bindWorld);
            n.skin = std::move(skin);
        }
    }
    std::vector<ExportTake*> all;
    if (out.take)
        all.push_back(&*out.take);
    for (ExportTake& t : out.extraTakes)
        all.push_back(&t);
    for (ExportTake* tk : all)
    {
        for (std::vector<glm::dmat4>& frames : tk->locals)
            for (glm::dmat4& m : frames)
                m = conj(m);
        for (std::vector<ExportCamera>& frames : tk->cameras)
            for (ExportCamera& c : frames)
                c = inUnits(c, k);
        for (auto& curves : tk->curves)
            if (curves)
                for (int c = 0; c < 3; ++c)
                    for (scene::anim::Key& key : (*curves)[size_t(c)].keys)
                    {
                        key.value *= k;
                        // Fixed tangent angles live in (seconds, value) space, so the slope scales with the value.
                        key.inAngle = glm::degrees(std::atan(std::tan(glm::radians(key.inAngle)) * k));
                        key.outAngle = glm::degrees(std::atan(std::tan(glm::radians(key.outAngle)) * k));
                    }
    }
    return out;
}
} // namespace

std::string encodeFbx(const ExportScene& sceneIn, const ExportOptions& opts)
{
    LILA_ZONE();

    std::optional<ExportScene> scaled;
    if (opts.unitScale > 0.0 && opts.unitScale != 1.0)
        scaled = inUnits(sceneIn, opts.unitScale);
    const ExportScene& scene = scaled ? *scaled : sceneIn;

    const bool character = (opts.kind == ExportKind::Skeletal || opts.kind == ExportKind::Animation) && fileIsZUp(opts.profile, scene.upAxis);
    const glm::dmat3 basis3 = character ? profileBasis(opts.profile, scene.upAxis) * glm::transpose(characterTurn(scene.upAxis))
                                        : profileBasis(opts.profile, scene.upAxis);
    const glm::dmat4 basis(basis3);
    const glm::dmat4 basisInv = glm::transpose(basis);
    const AxisMap axes = axisMap(basis3);
    const std::string creator = "Lila FBX exporter (" + std::string(profileName(opts.profile)) + ")";
    const ExportTake* take = scene.take ? &*scene.take : nullptr;
    auto toFile = [&](const glm::dmat4& m) { return basis * m * basisInv; };
    const glm::dmat4 camFrame = exportCameraFrame(basis3), lightFrm = exportLightFrame(basis3);
    /** The turn between a node's Lila frame and its file frame: cameras look down +X and lights shine down -Y in FBX. */
    auto frameOf = [&](const ExportNode& n) { return n.camera ? camFrame : n.light ? lightFrm : glm::dmat4(1.0); };
    auto turned = [&](const ExportNode& n) { return n.camera || n.light; };
    /** A node's local matrix in the file, with cameras and lights turned and their children turned back. */
    auto fileLocal = [&](size_t i, const glm::dmat4& local) {
        const ExportNode& n = scene.nodes[i];
        glm::dmat4 m = toFile(local);
        if (n.parent >= 0 && turned(scene.nodes[size_t(n.parent)]))
            m = glm::transpose(frameOf(scene.nodes[size_t(n.parent)])) * m;
        return m * frameOf(n);
    };

    Record root("");
    writeHeader(root, creator);
    const int64_t spanStart = take ? ktime(take->start) : 0;
    const int64_t spanStop = take ? ktime(take->end) : 46186158000;
    writeGlobalSettings(root, fileIsZUp(opts.profile, scene.upAxis) ? kZUp : kYUp, take ? take->fps : scene.fps, spanStart, spanStop,
                        opts.unitScale);

    Record& docs = root.add("Documents");
    docs.add("Count").i32(1);
    Record& doc = docs.add("Document");
    int64_t nextId = 1000000000;
    doc.i64(nextId++).str("Scene").str("Scene");
    Record& docProps = doc.add("Properties70");
    docProps.addP("SourceObject", "object", "", "");
    pString(docProps, "ActiveAnimStackName", take ? take->name : "");
    doc.add("RootNode").i64(0);
    root.add("References");

    Record objects("Objects");
    Record connections("Connections");
    auto connect = [&](int64_t child, int64_t parent) { connections.add("C").str("OO").i64(child).i64(parent); };
    auto connectProp = [&](int64_t child, int64_t parent, std::string_view prop) {
        connections.add("C").str("OP").i64(child).i64(parent).str(prop);
    };

    std::vector<int64_t> modelIds(scene.nodes.size()), geomIds(scene.nodes.size(), 0), attrIds(scene.nodes.size(), 0);
    std::vector<ModelInfo> info(scene.nodes.size());
    std::map<std::string, int64_t> materialIds;
    struct SharedGeometry
    {
        const geo::Mesh* mesh = nullptr; // null for one a skin or morphs deform, which no other model may share
        glm::dmat4 bake{1.0};
        int64_t id = 0;
        std::vector<std::string> slots;
    };
    std::vector<SharedGeometry> sharedGeometry;
    ObjectCounts counts;
    bool anySkin = false;
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const ExportNode& n = scene.nodes[i];
        modelIds[i] = nextId++;
        ++counts.models;
        const bool isMesh = n.mesh && n.mesh->numFaces() > 0;
        const bool keyed = take && take->curves[i];

        // Joints and keyed nodes keep their rotate order through the axis swap, so channels map one to one.
        ModelInfo& mi = info[i];
        const bool lodGroup = n.lodGroup && !isMesh && !n.joint && !n.camera;
        mi.type = isMesh ? "Mesh" : n.joint ? "LimbNode" : lodGroup ? "LodGroup" : n.light ? "Light" : "Null";
        mi.order = n.joint || keyed ? axes.order(n.rotateOrder) : scene::RotateOrder::XYZ;
        const glm::dmat3 pre = basis3 * glm::dmat3(n.pre) * glm::transpose(basis3);
        mi.pre = cleanVec(scene::matrixToEuler(pre, scene::RotateOrder::XYZ));
        const glm::dmat3 preExact(scene::eulerToMatrix(mi.pre, scene::RotateOrder::XYZ));
        const glm::dmat3 post = basis3 * glm::transpose(glm::dmat3(n.post)) * glm::transpose(basis3);
        mi.post = cleanVec(scene::matrixToEuler(post, scene::RotateOrder::XYZ));
        const glm::dmat3 postExact(scene::eulerToMatrix(mi.post, scene::RotateOrder::XYZ));
        const bool reframed = turned(n) || (n.parent >= 0 && turned(scene.nodes[size_t(n.parent)]));
        if (n.plainTrs && !reframed && mi.order == axes.order(n.rotateOrder))
            mi.trs = {axes.translate(n.translate), axes.rotate(n.rotate), axes.scale(n.scale)};
        else
            mi.trs = fileTrs(fileLocal(i, n.local), preExact, mi.order, axes.rotate(n.rotate), postExact);
        std::vector<std::pair<std::string, double>> userProps;
        if (int(i) == scene.curveHolder)
            for (size_t c = 0; c < scene.curveNames.size(); ++c)
                userProps.push_back({scene.curveNames[c], take && c < take->namedCurves.size() && !take->namedCurves[c].empty()
                                                              ? take->namedCurves[c].front()
                                                              : 0.0});
        writeModel(objects, modelIds[i], n.name, mi, userProps, n.unrealSocket, n.properties);
        connect(modelIds[i], n.parent >= 0 ? modelIds[size_t(n.parent)] : 0);
        if (!isMesh)
        {
            int64_t attrId = attrIds[i] = nextId++;
            if (n.camera && !n.joint)
            {
                writeCameraAttribute(objects, attrId, n.name, *n.camera);
                ++counts.cameras;
            }
            else if (n.light && !n.joint)
            {
                writeLightAttribute(objects, attrId, n.name, *n.light);
                ++counts.lights;
            }
            else if (n.joint)
            {
                writeLimbAttribute(objects, attrId, n.name);
                ++counts.limbs;
            }
            else if (lodGroup)
            {
                const size_t levels = size_t(std::count_if(scene.nodes.begin(), scene.nodes.end(), [&](const ExportNode& c) { return c.parent == int(i); }));
                writeLodGroupAttribute(objects, attrId, n.name, n.lodScreenSizes, levels);
                ++counts.lodGroups;
            }
            else
            {
                writeNullAttribute(objects, attrId, n.name);
                ++counts.nulls;
            }
            connect(attrId, modelIds[i]);
            continue;
        }
        // Instances (one snapshot under the same bake) share one Geometry, connected to each of their models.
        size_t geometry = sharedGeometry.size();
        if (!n.skin && !n.morphs)
            for (size_t k = 0; k < sharedGeometry.size(); ++k)
                if (sharedGeometry[k].mesh == n.mesh.get() && nearMat(sharedGeometry[k].bake, n.bake))
                    geometry = k;
        if (geometry == sharedGeometry.size())
        {
            Geometry g = buildGeometry(*n.mesh, basis * n.bake, opts);
            int64_t id = nextId++;
            writeGeometry(objects, id, n.name, g);
            ++counts.geometries;
            sharedGeometry.push_back({n.skin || n.morphs ? nullptr : n.mesh.get(), n.bake, id, g.slots});
        }
        const int64_t geomId = geomIds[i] = sharedGeometry[geometry].id;
        connect(geomId, modelIds[i]);
        // Material order on the model is what the polygons' slot indices refer to.
        for (const std::string& slot : sharedGeometry[geometry].slots)
        {
            auto [it, added] = materialIds.emplace(slot, 0);
            if (added)
            {
                it->second = nextId++;
                auto m = scene.materials.find(slot);
                writeMaterial(objects, it->second, slot, m == scene.materials.end() ? nullptr : &m->second);
                if (m != scene.materials.end())
                {
                    const std::pair<const std::optional<ExportTexture>*, const char*> maps[] = {
                        {&m->second.baseColorMap, "DiffuseColor"}, {&m->second.normalMap, "NormalMap"}, {&m->second.emissiveMap, "EmissiveColor"}};
                    for (const auto& [map, prop] : maps)
                    {
                        if (!*map)
                            continue;
                        const int64_t textureId = nextId++, videoId = nextId++;
                        writeTexture(objects, textureId, videoId, textureName((*map)->path), **map);
                        connect(videoId, textureId);
                        connectProp(textureId, it->second, prop);
                        ++counts.textures;
                    }
                }
            }
            connect(it->second, modelIds[i]);
        }
        anySkin = anySkin || n.skin;
    }

    // Skins: one Skin deformer per mesh, one Cluster per influence, matrices as Maya writes them.
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const ExportNode& n = scene.nodes[i];
        if (!n.skin || !geomIds[i])
            continue;
        const int64_t geomId = geomIds[i];
        int64_t skinId = nextId++;
        Record& skin = objects.add("Deformer");
        skin.i64(skinId).str(bin::objectName(n.name, "Deformer")).str("Skin");
        skin.add("Version").i32(101);
        skin.add("Link_DeformAcuracy").f64(50.0);
        skin.add("SkinningType").str(n.skin->dualQuaternion ? "DualQuaternion" : "Linear");
        connect(skinId, geomId);
        ++counts.deformers;
        const glm::dmat4 meshBind = toFile(n.skin->meshBindWorld);
        for (const ExportSkin::Cluster& c : n.skin->clusters)
        {
            const ExportNode& joint = scene.nodes[size_t(c.joint)];
            int64_t clusterId = nextId++;
            Record& cl = objects.add("Deformer");
            cl.i64(clusterId).str(bin::objectName(joint.name, "SubDeformer")).str("Cluster");
            cl.add("Version").i32(100);
            cl.add("UserData").str("").str("");
            cl.add("Indexes").i32s(c.vertices);
            cl.add("Weights").f64s(c.weights);
            const glm::dmat4 link = toFile(c.bindWorld);
            cl.add("Transform").f64s(flat(glm::inverse(link) * meshBind));
            cl.add("TransformLink").f64s(flat(link));
            connect(clusterId, skinId);
            connect(modelIds[size_t(c.joint)], clusterId);
            ++counts.deformers;
        }
    }
    // Morph targets: a BlendShape per mesh, a BlendShapeChannel per target holding one Shape of point offsets.
    std::vector<std::vector<int64_t>> channelIds(scene.nodes.size());
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const ExportNode& n = scene.nodes[i];
        if (!n.morphs || !geomIds[i])
            continue;
        const glm::dmat3 lin(basis * n.bake);
        int64_t blendId = nextId++;
        Record& blend = objects.add("Deformer");
        blend.i64(blendId).str(bin::objectName(n.name, "Deformer")).str("BlendShape");
        blend.add("Version").i32(100);
        connect(blendId, geomIds[i]);
        ++counts.deformers;
        for (const ExportMorphs::Target& t : n.morphs->targets)
        {
            int64_t channelId = nextId++;
            Record& ch = objects.add("Deformer");
            ch.i64(channelId).str(bin::objectName(t.name, "SubDeformer")).str("BlendShapeChannel");
            Record& props = ch.add("Properties70");
            props.addP("DeformPercent", "Number", "", "A").f64(t.weight * 100.0);
            ch.add("Version").i32(100);
            ch.add("DeformPercent").f64(t.weight * 100.0);
            ch.add("FullWeights").f64s(std::vector<double>{100.0});
            connect(channelId, blendId);
            channelIds[i].push_back(channelId);
            ++counts.deformers;

            std::vector<double> offsets;
            offsets.reserve(t.deltas.size() * 3);
            for (const glm::dvec3& d : t.deltas)
            {
                glm::dvec3 f = lin * d;
                offsets.insert(offsets.end(), {f.x, f.y, f.z});
            }
            int64_t shapeId = nextId++;
            Record& shape = objects.add("Geometry");
            shape.i64(shapeId).str(bin::objectName(t.name, "Geometry")).str("Shape");
            shape.add("Version").i32(100);
            shape.add("Indexes").i32s(t.vertices);
            shape.add("Vertices").f64s(offsets);
            connect(shapeId, channelId);
            ++counts.geometries;
        }
    }

    if (anySkin)
    {
        Record& pose = objects.add("Pose");
        pose.i64(nextId++).str(bin::objectName("BIND_POSES", "Pose")).str("BindPose");
        pose.add("Type").str("BindPose");
        pose.add("Version").i32(100);
        pose.add("NbPoseNodes").i32(int32_t(scene.nodes.size()));
        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            const ExportNode& n = scene.nodes[i];
            Record& pn = pose.add("PoseNode");
            pn.add("Node").i64(modelIds[i]);
            pn.add("Matrix").f64s(flat(toFile(n.skin ? n.skin->meshBindWorld : n.bindWorld) * frameOf(n)));
        }
        ++counts.poses;
    }

    std::vector<const ExportTake*> allTakes;
    if (take)
        allTakes.push_back(take);
    for (const ExportTake& extra : scene.extraTakes)
        allTakes.push_back(&extra);
    counts.stacks = counts.layers = int(allTakes.size());
    // One AnimStack per take, each with a single layer holding its transform and morph weight curves.
    for (const ExportTake* tk : allTakes)
    {
        int64_t stackId = nextId++;
        Record& stack = objects.add("AnimationStack");
        stack.i64(stackId).str(bin::objectName(tk->name, "AnimStack")).str("");
        Record& sp = stack.add("Properties70");
        sp.addP("LocalStart", "KTime", "Time", "").i64(ktime(tk->start));
        sp.addP("LocalStop", "KTime", "Time", "").i64(ktime(tk->end));
        sp.addP("ReferenceStart", "KTime", "Time", "").i64(ktime(tk->start));
        sp.addP("ReferenceStop", "KTime", "Time", "").i64(ktime(tk->end));
        int64_t layerId = nextId++;
        Record& layer = objects.add("AnimationLayer");
        layer.i64(layerId).str(bin::objectName("BaseLayer", "AnimLayer")).str("");
        layer.add("Properties70");
        connect(layerId, stackId);

        static constexpr const char* kGroupProp[3] = {"Lcl Translation", "Lcl Rotation", "Lcl Scaling"};
        static constexpr const char* kGroupName[3] = {"T", "R", "S"};
        static constexpr const char* kAxisProp[3] = {"d|X", "d|Y", "d|Z"};
        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            const ModelInfo& mi = info[i];
            const glm::dvec3 lcl[3] = {mi.trs.t, mi.trs.r, mi.trs.s};
            // keys[group][file axis], or nothing to write for that channel.
            std::array<std::array<std::optional<CurveKeys>, 3>, 3> keys;
            if (tk->curves[i])
            {
                const auto& lila = *tk->curves[i];
                for (int g = 0; g < 3; ++g)
                    for (int a = 0; a < 3; ++a)
                        keys[size_t(g)][size_t(axes.to[a])] = curveKeys(lila[size_t(g * 3 + a)], axes.factor(g, a));
            }
            else if (!tk->locals[i].empty())
            {
                const glm::dmat3 preExact(scene::eulerToMatrix(mi.pre, scene::RotateOrder::XYZ));
                const glm::dmat3 postExact(scene::eulerToMatrix(mi.post, scene::RotateOrder::XYZ));
                std::vector<std::array<double, 9>> frames;
                frames.reserve(tk->times.size());
                glm::dvec3 ref = mi.trs.r;
                for (const glm::dmat4& local : tk->locals[i])
                {
                    Trs trs = fileTrs(fileLocal(i, local), preExact, mi.order, ref, postExact);
                    ref = trs.r;
                    frames.push_back({trs.t.x, trs.t.y, trs.t.z, trs.r.x, trs.r.y, trs.r.z, trs.s.x, trs.s.y, trs.s.z});
                }
                for (int g = 0; g < 3; ++g)
                    for (int a = 0; a < 3; ++a)
                    {
                        const int ch = g * 3 + a;
                        const double tol = g == 2 ? 1e-7 : 1e-6;
                        bool still = std::all_of(frames.begin(), frames.end(),
                                                 [&](const std::array<double, 9>& f) { return std::abs(f[size_t(ch)] - lcl[g][a]) <= tol; });
                        if (still)
                            continue;
                        std::vector<double> values;
                        values.reserve(frames.size());
                        for (const auto& f : frames)
                            values.push_back(f[size_t(ch)]);
                        keys[size_t(g)][size_t(a)] = sampledKeys(tk->times, values);
                    }
            }
            for (int g = 0; g < 3; ++g)
            {
                if (!keys[size_t(g)][0] && !keys[size_t(g)][1] && !keys[size_t(g)][2])
                    continue;
                int64_t nodeId = nextId++;
                writeCurveNode(objects, nodeId, kGroupName[g], lcl[g]);
                connect(nodeId, layerId);
                connectProp(nodeId, modelIds[i], kGroupProp[g]);
                ++counts.curveNodes;
                for (int a = 0; a < 3; ++a)
                {
                    const auto& k = keys[size_t(g)][size_t(a)];
                    if (!k)
                        continue;
                    int64_t curveId = nextId++;
                    writeCurve(objects, curveId, *k, lcl[g][a]);
                    connectProp(curveId, nodeId, kAxisProp[a]);
                    ++counts.curves;
                }
            }
        }

        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            if (i >= tk->cameras.size() || tk->cameras[i].empty() || !attrIds[i] || !scene.nodes[i].camera)
                continue;
            const auto& frames = tk->cameras[i];
            for (const CameraChannel& ch : kCameraChannels)
            {
                const double rest = (*scene.nodes[i].camera).*ch.value;
                std::vector<double> values(frames.size());
                for (size_t f = 0; f < frames.size(); ++f)
                    values[f] = frames[f].*ch.value;
                if (std::all_of(values.begin(), values.end(), [&](double v) { return std::abs(v - rest) <= 1e-9 * std::max(1.0, std::abs(rest)); }))
                    continue;
                int64_t nodeId = nextId++;
                writeCurveNode1(objects, nodeId, ch.prop, ch.prop, rest);
                connect(nodeId, layerId);
                connectProp(nodeId, attrIds[i], ch.prop);
                ++counts.curveNodes;
                int64_t curveId = nextId++;
                writeCurve(objects, curveId, sampledKeys(tk->times, values), rest);
                connectProp(curveId, nodeId, std::string("d|") + ch.prop);
                ++counts.curves;
            }
        }

        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            if (i >= tk->morphWeights.size() || tk->morphWeights[i].empty())
                continue;
            const auto& weights = tk->morphWeights[i];
            for (size_t k = 0; k < weights.size() && k < channelIds[i].size(); ++k)
            {
                std::vector<double> percent(weights[k].size());
                for (size_t f = 0; f < percent.size(); ++f)
                    percent[f] = weights[k][f] * 100.0;
                const double rest = scene.nodes[i].morphs->targets[k].weight * 100.0;
                int64_t nodeId = nextId++;
                writeCurveNode1(objects, nodeId, "DeformPercent", "DeformPercent", rest);
                connect(nodeId, layerId);
                connectProp(nodeId, channelIds[i][k], "DeformPercent");
                ++counts.curveNodes;
                int64_t curveId = nextId++;
                writeCurve(objects, curveId, sampledKeys(tk->times, percent), rest);
                connectProp(curveId, nodeId, "d|DeformPercent");
                ++counts.curves;
            }
        }

        for (size_t c = 0; c < scene.curveNames.size() && scene.curveHolder >= 0; ++c)
        {
            if (c >= tk->namedCurves.size() || tk->namedCurves[c].empty())
                continue;
            const std::string& name = scene.curveNames[c];
            const double first = tk->namedCurves[c].front();
            int64_t nodeId = nextId++;
            writeCurveNode1(objects, nodeId, name, name, first);
            connect(nodeId, layerId);
            connectProp(nodeId, modelIds[size_t(scene.curveHolder)], name);
            ++counts.curveNodes;
            int64_t curveId = nextId++;
            writeCurve(objects, curveId, sampledKeys(tk->times, tk->namedCurves[c]), first);
            connectProp(curveId, nodeId, "d|" + name);
            ++counts.curves;
        }
    }

    counts.materials = int(materialIds.size());
    writeDefinitions(root, counts);
    root.add("Objects") = std::move(objects);
    root.add("Connections") = std::move(connections);
    Record& takes = root.add("Takes");
    takes.add("Current").str(take ? take->name : "");
    for (const ExportTake* tk : allTakes)
    {
        Record& t = takes.add("Take");
        t.str(tk->name);
        std::string file = tk->name;
        std::replace(file.begin(), file.end(), ' ', '_');
        t.add("FileName").str(file + ".tak");
        t.add("LocalTime").i64(ktime(tk->start)).i64(ktime(tk->end));
        t.add("ReferenceTime").i64(ktime(tk->start)).i64(ktime(tk->end));
    }
    return bin::encodeFile(root, 7400, opts.compress);
}

bool writeFbx(const ExportScene& scene, const fs::path& path, const ExportOptions& opts, std::string* error)
{
    LILA_ZONE();

    if (scene.nodes.empty())
    {
        if (error)
            *error = "nothing to export";
        return false;
    }
    std::string bytes = encodeFbx(scene, opts);
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    // Write beside the target and rename, so a reader never sees half a file.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            if (error)
                *error = "can't write " + path.string();
            return false;
        }
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f)
        {
            if (error)
                *error = "write failed: " + path.string();
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        if (error)
            *error = "can't replace " + path.string();
        return false;
    }
    return true;
}
} // namespace lila::fbx
