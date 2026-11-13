#include "gltf/Gltf.h"
#include "scene/Georef.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/ImportCommon.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "gltf/GltfDraco.h"
#include "gltf/GltfInternal.h"
#include "gltf/GltfMaterials.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Deformers.h"
#include "scene/Light.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Morphs.h"
#include "scene/Rig.h"
#include "scene/RotationCurve.h"
#include "scene/Skin.h"
#include "scene/Take.h"

#include <cgltf.h>
#include <meshoptimizer.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace lila::gltf
{
namespace
{
using namespace scene;
using geo::Index;
using geo::kInvalid;

constexpr double kCmPerMetre = 100.0;

struct DataDeleter
{
    void operator()(cgltf_data* d) const { cgltf_free(d); }
};

cgltf_result readCallback(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data)
{
    std::string bytes;
    if (!readFile(utf8Path(path), bytes))
        return cgltf_result_file_not_found;
    void* p = malloc(bytes.empty() ? 1 : bytes.size());
    if (!p)
        return cgltf_result_out_of_memory;
    std::memcpy(p, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = p;
    return cgltf_result_success;
}

void releaseCallback(const cgltf_memory_options*, const cgltf_file_options*, void* data) { free(data); }

const char* resultText(cgltf_result r)
{
    switch (r)
    {
    case cgltf_result_data_too_short: return "the file is truncated";
    case cgltf_result_unknown_format: return "not a glTF or GLB file";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_file_not_found: return "a referenced file was not found";
    case cgltf_result_io_error: return "read error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
    default: return "can't read the file";
    }
}


/** EXT_meshopt_compression: each compressed view is decoded into its own memory, which cgltf then reads instead of the buffer. */
void decodeMeshopt(cgltf_data& data, ImportResult& r)
{
    LILA_ZONE();

    for (size_t i = 0; i < data.buffer_views_count; ++i)
    {
        cgltf_buffer_view& view = data.buffer_views[i];
        if (!view.has_meshopt_compression || view.data)
            continue;
        const cgltf_meshopt_compression& mc = view.meshopt_compression;
        const auto* src = mc.buffer && mc.buffer->data ? static_cast<const unsigned char*>(mc.buffer->data) + mc.offset : nullptr;
        if (!src)
        {
            r.errors.push_back(std::format("buffer view {}: EXT_meshopt_compression data is missing", i));
            continue;
        }
        void* dst = malloc(mc.count * mc.stride);
        if (!dst)
        {
            r.errors.push_back(std::format("buffer view {}: out of memory decoding EXT_meshopt_compression", i));
            continue;
        }
        int rc = -1;
        switch (mc.mode)
        {
        case cgltf_meshopt_compression_mode_attributes: rc = meshopt_decodeVertexBuffer(dst, mc.count, mc.stride, src, mc.size); break;
        case cgltf_meshopt_compression_mode_triangles: rc = meshopt_decodeIndexBuffer(dst, mc.count, mc.stride, src, mc.size); break;
        case cgltf_meshopt_compression_mode_indices: rc = meshopt_decodeIndexSequence(dst, mc.count, mc.stride, src, mc.size); break;
        default: break;
        }
        if (rc != 0)
        {
            free(dst);
            r.errors.push_back(std::format("buffer view {}: EXT_meshopt_compression data doesn't decode", i));
            continue;
        }
        switch (mc.filter)
        {
        case cgltf_meshopt_compression_filter_octahedral: meshopt_decodeFilterOct(dst, mc.count, mc.stride); break;
        case cgltf_meshopt_compression_filter_quaternion: meshopt_decodeFilterQuat(dst, mc.count, mc.stride); break;
        case cgltf_meshopt_compression_filter_exponential: meshopt_decodeFilterExp(dst, mc.count, mc.stride); break;
        default: break;
        }
        view.data = dst;
    }
}

std::vector<float> floats(const cgltf_accessor* a)
{
    if (!a)
        return {};
    std::vector<float> out(a->count * cgltf_num_components(a->type));
    cgltf_accessor_unpack_floats(a, out.data(), out.size());
    return out;
}

/** True when the accessor's data can be read: it has a buffer view with data, or is all sparse. */
bool readable(const cgltf_accessor* a)
{
    if (!a)
        return false;
    if (!a->buffer_view)
        return true;
    return cgltf_buffer_view_data(a->buffer_view) != nullptr;
}

glm::dmat4 nodeMatrix(const cgltf_node& n)
{
    if (n.has_matrix)
    {
        glm::dmat4 m(1.0);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = n.matrix[c * 4 + r];
        return m;
    }
    glm::dmat4 m(1.0);
    if (n.has_translation)
        m = glm::translate(m, glm::dvec3(n.translation[0], n.translation[1], n.translation[2]));
    if (n.has_rotation)
        m = m * glm::dmat4(glm::mat3_cast(glm::normalize(glm::dquat(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]))));
    if (n.has_scale)
        m = glm::scale(m, glm::dvec3(n.scale[0], n.scale[1], n.scale[2]));
    return m;
}

bool nearIdentity(const glm::dmat4& m, double eps = 1e-9)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(m[c][r] - (c == r ? 1.0 : 0.0)) > eps)
                return false;
    return true;
}

glm::dvec3 clean(glm::dvec3 v)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(v[i]) < 1e-9)
            v[i] = 0.0;
    return v;
}

// Mesh building ------------------------------------------------------------------------------------------------------

/** One primitive's vertex data, as read. */
struct Prim
{
    const cgltf_primitive* src = nullptr;
    int material = -1;
    size_t count = 0;
    std::vector<float> pos, nrm, tan;
    std::vector<std::vector<float>> uvs;
    std::vector<std::vector<float>> colors; // RGBA each
    std::vector<std::vector<float>> joints, weights; // 4 per vertex each
    std::vector<std::vector<float>> morphs;          // 3 per vertex per target
    std::vector<uint32_t> tris;
};

struct BuiltMesh
{
    MeshPtr mesh;
    std::vector<Prim> prims;
    /** Per point of mesh: the primitive and vertex its data comes from. */
    std::vector<std::pair<uint32_t, uint32_t>> source;
    size_t targets = 0;
    bool skinData = false;
};

std::vector<uint32_t> triangles(const cgltf_primitive& p, size_t vertexCount)
{
    std::vector<uint32_t> idx;
    if (p.indices)
    {
        idx.resize(p.indices->count);
        cgltf_accessor_unpack_indices(p.indices, idx.data(), sizeof(uint32_t), idx.size());
    }
    else
    {
        idx.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
            idx[i] = uint32_t(i);
    }
    std::vector<uint32_t> out;
    switch (p.type)
    {
    case cgltf_primitive_type_triangles:
        out.assign(idx.begin(), idx.begin() + std::ptrdiff_t(idx.size() / 3 * 3));
        break;
    case cgltf_primitive_type_triangle_strip:
        for (size_t i = 2; i < idx.size(); ++i)
            if (i % 2 == 0)
                out.insert(out.end(), {idx[i - 2], idx[i - 1], idx[i]});
            else
                out.insert(out.end(), {idx[i - 1], idx[i - 2], idx[i]});
        break;
    case cgltf_primitive_type_triangle_fan:
        for (size_t i = 2; i < idx.size(); ++i)
            out.insert(out.end(), {idx[i - 1], idx[i], idx[0]});
        break;
    default: break;
    }
    return out;
}

const char* primTypeName(cgltf_primitive_type t)
{
    switch (t)
    {
    case cgltf_primitive_type_points: return "point";
    case cgltf_primitive_type_lines:
    case cgltf_primitive_type_line_loop:
    case cgltf_primitive_type_line_strip: return "line";
    default: return "unknown";
    }
}

std::shared_ptr<BuiltMesh> buildMesh(const cgltf_data& data, const cgltf_mesh& gm, const std::vector<MaterialSlot>& slots, const glm::dmat3& F,
                                     ImportResult& r)
{
    LILA_ZONE();

    auto out = std::make_shared<BuiltMesh>();
    const std::string meshName = gm.name ? gm.name : std::format("mesh{}", cgltf_mesh_index(&data, &gm));
    std::set<std::string> customAttrs;
    bool morphNormals = false;
    size_t skippedPrims = 0;
    for (size_t pi = 0; pi < gm.primitives_count; ++pi)
    {
        const cgltf_primitive& p = gm.primitives[pi];
        if (p.type != cgltf_primitive_type_triangles && p.type != cgltf_primitive_type_triangle_strip &&
            p.type != cgltf_primitive_type_triangle_fan)
        {
            ++skippedPrims;
            r.skipped.push_back(std::format("mesh '{}' primitive {}: {} primitive (Lila meshes are polygons)", meshName, pi, primTypeName(p.type)));
            continue;
        }
        const cgltf_accessor* position = nullptr;
        for (size_t a = 0; a < p.attributes_count; ++a)
            if (p.attributes[a].type == cgltf_attribute_type_position)
                position = p.attributes[a].data;
        if (!position)
        {
            r.warnings.push_back(std::format("mesh '{}' primitive {}: no POSITION, skipped", meshName, pi));
            continue;
        }
        // Draco data wins; an uncompressed fallback is read only when it doesn't decode.
        std::optional<DracoPrimitive> draco;
        if (p.has_draco_mesh_compression)
        {
            std::string err;
            draco.emplace();
            if (!decodeDraco(data, p, *draco, &err))
            {
                draco.reset();
                if (!position->buffer_view && !position->is_sparse)
                {
                    r.errors.push_back(std::format("mesh '{}' primitive {}: its Draco data (KHR_draco_mesh_compression) doesn't decode ({}) and the "
                                                   "file has no uncompressed fallback; the primitive is skipped",
                                                   meshName, pi, err));
                    continue;
                }
                r.warnings.push_back(std::format("mesh '{}' primitive {}: its Draco data doesn't decode ({}); read its uncompressed fallback", meshName,
                                                 pi, err));
            }
        }
        if (!draco && !readable(position))
        {
            r.errors.push_back(std::format("mesh '{}' primitive {}: its vertex data could not be read; skipped", meshName, pi));
            continue;
        }
        auto read = [&](const cgltf_accessor* acc) {
            if (draco)
                if (auto it = draco->values.find(acc); it != draco->values.end())
                    return it->second;
            return floats(acc);
        };

        Prim prim;
        prim.src = &p;
        prim.material = p.material ? int(cgltf_material_index(&data, p.material)) : -1;
        prim.count = draco ? draco->count : position->count;
        prim.pos = read(position);
        if (prim.pos.size() != prim.count * 3)
        {
            r.errors.push_back(std::format("mesh '{}' primitive {}: its positions don't match its point count; skipped", meshName, pi));
            continue;
        }
        for (size_t a = 0; a < p.attributes_count; ++a)
        {
            const cgltf_attribute& at = p.attributes[a];
            const cgltf_accessor* acc = at.data;
            if (!acc || (draco ? !draco->values.count(acc) && acc->count != prim.count : acc->count != prim.count))
                continue;
            switch (at.type)
            {
            case cgltf_attribute_type_normal: prim.nrm = read(acc); break;
            case cgltf_attribute_type_tangent:
                if (acc->type == cgltf_type_vec4)
                    prim.tan = read(acc);
                break;
            case cgltf_attribute_type_texcoord:
                if (at.index >= 0 && at.index < 8)
                {
                    if (prim.uvs.size() <= size_t(at.index))
                        prim.uvs.resize(size_t(at.index) + 1);
                    prim.uvs[size_t(at.index)] = read(acc);
                }
                break;
            case cgltf_attribute_type_color:
                if (at.index >= 0)
                {
                    std::vector<float> c = read(acc);
                    if (acc->type == cgltf_type_vec3)
                    {
                        std::vector<float> rgba(prim.count * 4, 1.0f);
                        for (size_t v = 0; v < prim.count; ++v)
                            std::copy_n(&c[v * 3], 3, &rgba[v * 4]);
                        c = std::move(rgba);
                    }
                    if (prim.colors.size() <= size_t(at.index))
                        prim.colors.resize(size_t(at.index) + 1);
                    prim.colors[size_t(at.index)] = std::move(c);
                }
                break;
            case cgltf_attribute_type_joints:
                if (at.index >= 0)
                {
                    if (prim.joints.size() <= size_t(at.index))
                        prim.joints.resize(size_t(at.index) + 1);
                    prim.joints[size_t(at.index)] = read(acc);
                }
                break;
            case cgltf_attribute_type_weights:
                if (at.index >= 0)
                {
                    if (prim.weights.size() <= size_t(at.index))
                        prim.weights.resize(size_t(at.index) + 1);
                    prim.weights[size_t(at.index)] = read(acc);
                }
                break;
            case cgltf_attribute_type_custom: customAttrs.insert(orEmpty(at.name)); break;
            default: break;
            }
        }
        for (size_t t = 0; t < p.targets_count; ++t)
        {
            std::vector<float> d(prim.count * 3, 0.0f);
            for (size_t a = 0; a < p.targets[t].attributes_count; ++a)
            {
                const cgltf_attribute& at = p.targets[t].attributes[a];
                if (at.type == cgltf_attribute_type_position && at.data && at.data->count == prim.count)
                    d = floats(at.data);
                else if (at.type == cgltf_attribute_type_normal || at.type == cgltf_attribute_type_tangent)
                    morphNormals = true;
            }
            prim.morphs.push_back(std::move(d));
        }
        out->targets = std::max(out->targets, prim.morphs.size());
        prim.tris = draco ? draco->tris : triangles(p, prim.count);
        bool bad = false;
        for (uint32_t i : prim.tris)
            bad = bad || i >= prim.count;
        if (bad)
        {
            r.errors.push_back(std::format("mesh '{}' primitive {}: indices out of range; skipped", meshName, pi));
            continue;
        }
        out->skinData = out->skinData || (!prim.joints.empty() && !prim.weights.empty());
        out->prims.push_back(std::move(prim));
    }
    if (!customAttrs.empty())
    {
        std::string list;
        for (const std::string& a : customAttrs)
            list += (list.empty() ? "" : ", ") + a;
        r.skipped.push_back(std::format("mesh '{}': custom vertex attributes {}", meshName, list));
    }
    if (morphNormals)
        r.skipped.push_back(std::format("mesh '{}': morph target normal and tangent deltas (Lila derives a target's normals from its shape)", meshName));
    if (out->prims.empty())
        return out;

    // Points weld where position, skin and morph data agree, so seams split for normals or UVs become one point.
    auto m = std::make_shared<geo::Mesh>();
    std::unordered_map<std::string, Index> pointOf;
    std::string key;
    auto pointFor = [&](uint32_t pi, uint32_t v) {
        const Prim& p = out->prims[pi];
        key.assign(reinterpret_cast<const char*>(&p.pos[size_t(v) * 3]), sizeof(float) * 3);
        for (const auto& d : p.morphs)
            key.append(reinterpret_cast<const char*>(&d[size_t(v) * 3]), sizeof(float) * 3);
        for (size_t s = 0; s < p.joints.size() && s < p.weights.size(); ++s)
            for (int k = 0; k < 4; ++k)
                if (p.weights[s].size() > size_t(v) * 4 + size_t(k) && p.weights[s][size_t(v) * 4 + size_t(k)] > 0.0f)
                {
                    key.append(reinterpret_cast<const char*>(&p.joints[s][size_t(v) * 4 + size_t(k)]), sizeof(float));
                    key.append(reinterpret_cast<const char*>(&p.weights[s][size_t(v) * 4 + size_t(k)]), sizeof(float));
                }
        auto [it, added] = pointOf.emplace(key, Index(out->source.size()));
        if (added)
        {
            const glm::dvec3 pos(p.pos[size_t(v) * 3], p.pos[size_t(v) * 3 + 1], p.pos[size_t(v) * 3 + 2]);
            m->addPoint(F * pos * kCmPerMetre);
            out->source.push_back({pi, v});
        }
        return it->second;
    };

    struct Tri
    {
        uint32_t prim;
        uint32_t v[3];
    };
    std::vector<Tri> faces;
    size_t degenerate = 0;
    std::vector<int32_t> faceSlot;
    std::vector<std::string>& slotNames = m->materialNames();
    std::map<int, int32_t> slotOfMaterial;
    bool anyMaterial = false;
    for (const Prim& p : out->prims)
        anyMaterial = anyMaterial || p.material >= 0;
    for (uint32_t pi = 0; pi < out->prims.size(); ++pi)
    {
        const Prim& p = out->prims[pi];
        int32_t slot = 0;
        if (anyMaterial)
        {
            auto [it, added] = slotOfMaterial.emplace(p.material, int32_t(slotNames.size()));
            if (added)
                slotNames.push_back(p.material >= 0 && size_t(p.material) < slots.size() ? slots[size_t(p.material)].name
                                                                                       : std::string(materials::kDefaultSlot));
            slot = it->second;
        }
        for (size_t t = 0; t + 2 < p.tris.size(); t += 3)
        {
            const Index pts[3] = {pointFor(pi, p.tris[t]), pointFor(pi, p.tris[t + 1]), pointFor(pi, p.tris[t + 2])};
            if (m->addFace(std::span<const Index>(pts, 3)) == kInvalid)
            {
                ++degenerate;
                continue;
            }
            faces.push_back({pi, {p.tris[t], p.tris[t + 1], p.tris[t + 2]}});
            faceSlot.push_back(slot);
        }
    }
    if (degenerate)
        r.warnings.push_back(std::format("mesh '{}': {} degenerate triangle(s) skipped", meshName, degenerate));

    size_t uvSets = 0, colorSets = 0;
    bool allNormals = true, anyNormals = false, allTangents = true;
    for (const Prim& p : out->prims)
    {
        uvSets = std::max(uvSets, p.uvs.size());
        colorSets = std::max(colorSets, p.colors.size());
        allNormals = allNormals && !p.nrm.empty();
        anyNormals = anyNormals || !p.nrm.empty();
        allTangents = allTangents && !p.tan.empty();
    }

    auto forCorners = [&](auto&& fn) {
        for (size_t f = 0; f < faces.size(); ++f)
        {
            const Index first = m->faceCorner(Index(f));
            for (int k = 0; k < 3; ++k)
                fn(first + Index(k), out->prims[faces[f].prim], faces[f].v[k], f);
        }
    };

    if (anyMaterial)
    {
        std::span<int32_t> mat = m->faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        for (size_t f = 0; f < faces.size(); ++f)
            mat[f] = faceSlot[f];
    }
    if (anyNormals)
    {
        std::span<glm::vec3> nrm = m->cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        forCorners([&](Index c, const Prim& p, uint32_t v, size_t f) {
            glm::dvec3 n;
            if (!p.nrm.empty())
                n = F * glm::dvec3(p.nrm[size_t(v) * 3], p.nrm[size_t(v) * 3 + 1], p.nrm[size_t(v) * 3 + 2]);
            else
            {
                // glTF's rule for a primitive without normals: flat shading.
                const Index fc = m->faceCorner(Index(f));
                const glm::dvec3 a = m->position(m->cornerPoint(fc)), b = m->position(m->cornerPoint(fc + 1)),
                                 d = m->position(m->cornerPoint(fc + 2));
                n = glm::cross(b - a, d - a);
            }
            const double len = glm::length(n);
            nrm[c] = glm::vec3(len > 0.0 ? n / len : glm::dvec3(0, 1, 0));
        });
        if (allTangents)
        {
            std::span<glm::vec4> tan = m->cornerAttrs().getOrAdd<glm::vec4>(geo::attr::kTangent);
            forCorners([&](Index c, const Prim& p, uint32_t v, size_t) {
                const float* t = &p.tan[size_t(v) * 4];
                tan[c] = glm::vec4(glm::vec3(F * glm::dvec3(t[0], t[1], t[2])), t[3] < 0.0f ? -1.0f : 1.0f);
            });
        }
    }
    for (size_t s = 0; s < uvSets; ++s)
    {
        std::span<glm::vec2> uv = m->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(s)));
        forCorners([&](Index c, const Prim& p, uint32_t v, size_t) {
            glm::dvec3 t(0.0, 0.0, 1.0);
            if (s < p.uvs.size() && !p.uvs[s].empty())
                t = glm::dvec3(p.uvs[s][size_t(v) * 2], p.uvs[s][size_t(v) * 2 + 1], 1.0);
            if (p.material >= 0 && size_t(p.material) < slots.size() && slots[size_t(p.material)].transformed &&
                slots[size_t(p.material)].uvSet == int(s))
                t = slots[size_t(p.material)].uvTransform * t;
            uv[c] = glm::vec2(float(t.x), float(1.0 - t.y));
        });
    }
    for (size_t s = 0; s < colorSets; ++s)
    {
        std::string name = geo::colors::uniqueName(*m, "colorSet");
        std::span<glm::vec4> col = geo::colors::add(*m, name);
        forCorners([&](Index c, const Prim& p, uint32_t v, size_t) {
            if (s < p.colors.size() && !p.colors[s].empty())
                col[c] = glm::vec4(p.colors[s][size_t(v) * 4], p.colors[s][size_t(v) * 4 + 1], p.colors[s][size_t(v) * 4 + 2],
                                   p.colors[s][size_t(v) * 4 + 3]);
        });
    }
    if (anyNormals)
        fbx::hardEdgesFromNormals(*m);
    else
    {
        std::span<bool> hard = m->edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        std::fill(hard.begin(), hard.end(), true);
    }

    m->removeAllLoose();
    if (m->hasDeleted())
    {
        geo::MeshRemap remap;
        m->compact(&remap);
        std::vector<std::pair<uint32_t, uint32_t>> kept(m->pointCapacity());
        for (size_t p = 0; p < out->source.size() && p < remap.points.size(); ++p)
            if (remap.points[p] != kInvalid && remap.points[p] < kept.size())
                kept[remap.points[p]] = out->source[p];
        out->source = std::move(kept);
    }
    out->mesh = std::move(m);
    return out;
}

/** A copy of the mesh with its points, normals and tangents taken through m. */
MeshPtr transformed(const geo::Mesh& src, const glm::dmat4& m)
{
    auto out = std::make_shared<geo::Mesh>(src);
    for (Index p = 0; p < out->pointCapacity(); ++p)
        if (out->pointValid(p))
            out->setPosition(p, glm::dvec3(m * glm::dvec4(out->position(p), 1.0)));
    const glm::dmat3 lin(m);
    const glm::dmat3 nm = glm::inverse(glm::transpose(lin));
    if (std::span<glm::vec3> n = out->cornerAttrs().get<glm::vec3>(geo::attr::kNormal); !n.empty())
        for (glm::vec3& v : n)
            v = glm::vec3(glm::normalize(nm * glm::dvec3(v)));
    if (std::span<glm::vec4> t = out->cornerAttrs().get<glm::vec4>(geo::attr::kTangent); !t.empty())
        for (glm::vec4& v : t)
            v = glm::vec4(glm::vec3(glm::normalize(lin * glm::dvec3(v))), v.w);
    return out;
}

// Scene --------------------------------------------------------------------------------------------------------------

/** How one glTF node maps onto its Lila transform. */
struct NodeFrame
{
    NodeId id = kNoNode;
    bool joint = false;
    /** The jointOrient the rest rotation went into, exactly as Lila rebuilds it. */
    glm::dmat3 pre{1.0};
    RotateOrder order = RotateOrder::XYZ;
    fbx::Trs rest;
    /** Lila local = before * (file local in Lila axes) * after: a camera's turn into a Z-up scene, undone on its children. */
    glm::dmat4 before{1.0}, after{1.0};
    /** The morph's target nodes, in the mesh's target order, and their default weights. */
    std::vector<NodeId> targets;
    std::vector<double> targetWeights;
};

/** (node, glTF animation path) pairs. */
using ChannelSet = std::set<std::pair<const cgltf_node*, int>>;

struct Importer
{
    Graph& graph;
    const cgltf_data& data;
    const ImportOptions& opts;
    ImportResult& r;
    glm::dmat3 F;             // file axes to Lila axes
    glm::dmat4 C{1.0}, Cinv;  // F with the unit scale
    std::vector<MaterialSlot> slots;
    std::unordered_map<const cgltf_node*, NodeFrame> frames;
    std::map<const cgltf_mesh*, std::shared_ptr<BuiltMesh>> meshes;
    std::vector<std::pair<const cgltf_node*, NodeId>> meshNodes;
    std::set<const cgltf_node*> joints;
    progress::Scope* build = nullptr;

    glm::dmat4 toLila(const glm::dmat4& m) const { return C * m * Cinv; }

    void create(const cgltf_node* gn, NodeId parent);
    void applyCamera(NodeId id, const cgltf_camera& cam);
    void instances(const cgltf_node* gn, NodeId id, const MeshPtr& mesh);
    void buildSkins();
    void buildMorphs();
    void importAnimations();
    void importAnimation(const cgltf_animation& anim, const std::vector<double>& frames, const ChannelSet* hold);
    void holdRest(const NodeFrame& nf, int path, double time);
};

/**
 * A KHR_lights_punctual light. Its intensity is in candela (point, spot) or lux (directional), its cone half angles in
 * radians and its range in metres; a range of 0 or none means no cutoff.
 */
lights::Settings lightSettings(const cgltf_light& l)
{
    lights::Settings s;
    s.kind = l.type == cgltf_light_type_directional ? lights::Kind::Directional
             : l.type == cgltf_light_type_spot      ? lights::Kind::Spot
                                                    : lights::Kind::Point;
    static constexpr const char* kKinds[] = {"point", "directional", "point", "spot"};
    s.sourceFormat = std::string("gltf:") + kKinds[std::clamp(int(l.type), 0, 3)];
    s.color = glm::dvec3(l.color[0], l.color[1], l.color[2]);
    s.intensity = double(l.intensity);
    s.units = s.kind == lights::Kind::Directional ? lights::Units::Lux : lights::Units::Candela;
    s.range = l.range > 0.0f ? double(l.range) * kCmPerMetre : 0.0;
    if (s.kind == lights::Kind::Spot)
    {
        s.innerConeAngle = glm::degrees(double(l.spot_inner_cone_angle));
        s.outerConeAngle = glm::degrees(double(l.spot_outer_cone_angle));
    }
    return s;
}

void Importer::applyCamera(NodeId id, const cgltf_camera& cam)
{
    auto set = [&](int attr, PlugValue v) { graph.setValue({id, attr, -1}, std::move(v)); };
    const Node* n = graph.node(id);
    const double vfa = n->as<double>(Camera::kVerticalFilmAperture);
    if (cam.type == cgltf_camera_type_perspective)
    {
        const cgltf_camera_perspective& p = cam.data.perspective;
        if (p.has_aspect_ratio && p.aspect_ratio > 0.0f)
            set(Camera::kHorizontalFilmAperture, vfa * double(p.aspect_ratio));
        // yfov is vertical, so the vertical film fit keeps it whatever the view's aspect.
        set(Camera::kFocalLength, vfa * 25.4 * 0.5 / std::tan(double(p.yfov) * 0.5));
        set(Camera::kFilmFit, int64_t(FilmFit::Vertical));
        set(Camera::kNearClipPlane, double(p.znear) * kCmPerMetre);
        if (p.has_zfar && p.zfar > 0.0f)
            set(Camera::kFarClipPlane, double(p.zfar) * kCmPerMetre);
        set(Camera::kOrthographic, false);
    }
    else if (cam.type == cgltf_camera_type_orthographic)
    {
        const cgltf_camera_orthographic& o = cam.data.orthographic;
        set(Camera::kOrthographic, true);
        set(Camera::kOrthographicWidth, 2.0 * double(std::abs(o.xmag)) * kCmPerMetre);
        if (o.ymag != 0.0f && o.xmag != 0.0f)
            set(Camera::kHorizontalFilmAperture, vfa * std::abs(double(o.xmag) / double(o.ymag)));
        set(Camera::kFilmFit, int64_t(FilmFit::Horizontal));
        set(Camera::kNearClipPlane, double(o.znear) * kCmPerMetre);
        set(Camera::kFarClipPlane, double(o.zfar) * kCmPerMetre);
    }
    r.cameras.push_back(id);
}

/** EXT_mesh_gpu_instancing: one child per instance, all sharing the mesh. */
void Importer::instances(const cgltf_node* gn, NodeId id, const MeshPtr& mesh)
{
    const cgltf_mesh_gpu_instancing& gi = gn->mesh_gpu_instancing;
    std::vector<float> t, q, s;
    size_t count = 0;
    for (size_t a = 0; a < gi.attributes_count; ++a)
    {
        const cgltf_attribute& at = gi.attributes[a];
        const std::string name = orEmpty(at.name);
        if (!at.data)
            continue;
        count = std::max(count, at.data->count);
        if (name == "TRANSLATION")
            t = floats(at.data);
        else if (name == "ROTATION")
            q = floats(at.data);
        else if (name == "SCALE")
            s = floats(at.data);
    }
    const NodeType& meshType = *types::find("mesh");
    const std::string base = graph.node(id)->name();
    for (size_t i = 0; i < count; ++i)
    {
        glm::dmat4 m(1.0);
        if (t.size() >= (i + 1) * 3)
            m = glm::translate(m, glm::dvec3(t[i * 3], t[i * 3 + 1], t[i * 3 + 2]));
        if (q.size() >= (i + 1) * 4)
            m = m * glm::dmat4(glm::mat3_cast(glm::normalize(glm::dquat(q[i * 4 + 3], q[i * 4], q[i * 4 + 1], q[i * 4 + 2]))));
        if (s.size() >= (i + 1) * 3)
            m = glm::scale(m, glm::dvec3(s[i * 3], s[i * 3 + 1], s[i * 3 + 2]));
        Node* n = graph.createNode(meshType, std::format("{}_instance{}", base, i), id, false);
        r.nodes.push_back(n->id());
        const fbx::Trs trs = fbx::decompose(toLila(m));
        graph.setValue({n->id(), Transform::kTranslate, -1}, clean(trs.t));
        graph.setValue({n->id(), Transform::kRotate, -1}, clean(trs.r));
        graph.setValue({n->id(), Transform::kScale, -1}, trs.s);
        graph.setValue({n->id(), MeshNode::kInMesh, -1}, mesh);
    }
    if (count)
        r.warnings.push_back(std::format("{}: EXT_mesh_gpu_instancing expanded into {} mesh copies", graph.path(id), count));
}

void Importer::create(const cgltf_node* gn, NodeId parent)
{
    if (build)
    {
        build->step();
        build->checkCancel();
    }
    std::shared_ptr<BuiltMesh> built;
    if (gn->mesh)
    {
        auto it = meshes.find(gn->mesh);
        if (it == meshes.end())
            it = meshes.emplace(gn->mesh, buildMesh(data, *gn->mesh, slots, F, r)).first;
        if (it->second->mesh && it->second->mesh->numFaces() > 0)
            built = it->second;
    }
    const bool instanced = built && gn->has_mesh_gpu_instancing;
    const bool isMesh = built && !instanced;
    const NodeType* jointType = types::find("joint");
    const NodeType* cameraType = types::find("camera");
    const bool isJoint = !isMesh && jointType && joints.count(gn);
    const bool isCamera = !isMesh && !isJoint && gn->camera && cameraType;
    const NodeType* lightType = types::find("light");
    const bool isLight = !isMesh && !isJoint && !isCamera && gn->light && lightType;
    const NodeType& type = isMesh     ? *types::find("mesh")
                           : isJoint  ? *jointType
                           : isCamera ? *cameraType
                           : isLight  ? *lightType
                                      : *types::find("transform");
    std::string raw = orEmpty(gn->name);
    if (raw.empty() && gn->mesh && gn->mesh->name)
        raw = gn->mesh->name;
    if (raw.empty() && gn->camera && gn->camera->name)
        raw = gn->camera->name;
    if (raw.empty() && gn->light && gn->light->name)
        raw = gn->light->name;
    Node* n = graph.createNode(type, fbx::safeName(raw, isMesh ? "Mesh" : isJoint ? "joint" : isCamera ? "camera" : isLight ? "light" : "Transform"),
                               parent, false);
    const NodeId id = n->id();
    r.nodes.push_back(id);
    if (parent == opts.parent)
        r.roots.push_back(id);

    NodeFrame& nf = frames[gn];
    nf.id = id;
    nf.joint = isJoint;
    // Cameras and lights look down their local -Z in both, so only the axes turn between them.
    if (isCamera || isLight)
        nf.after = glm::dmat4(F);
    if (auto above = gn->parent ? frames.find(gn->parent) : frames.end(); above != frames.end())
        nf.before = glm::transpose(above->second.after);
    const glm::dmat4 local = nf.before * toLila(nodeMatrix(*gn)) * nf.after;
    if (isJoint)
    {
        // The rest rotation becomes jointOrient, so the joint rests at zero rotation the way rigs expect.
        r.joints.push_back(id);
        const fbx::Trs plain = fbx::decompose(local);
        const glm::dvec3 jo = clean(plain.r);
        nf.pre = glm::dmat3(eulerToMatrix(jo, RotateOrder::XYZ));
        nf.rest = fbx::decomposeWith(local, nf.pre, nf.order, glm::dvec3(0.0));
        nf.rest.r = clean(nf.rest.r);
        graph.setValue({id, Joint::kJointOrient, -1}, jo);
    }
    else
        nf.rest = fbx::decompose(local);
    if (nf.rest.sheared)
        r.warnings.push_back(std::format("{}: shear dropped from its matrix", n->path()));
    graph.setValue({id, Transform::kTranslate, -1}, clean(nf.rest.t));
    graph.setValue({id, Transform::kRotate, -1}, clean(nf.rest.r));
    graph.setValue({id, Transform::kScale, -1}, nf.rest.s);

    if (isMesh)
    {
        graph.setValue({id, MeshNode::kInMesh, -1}, built->mesh);
        meshNodes.push_back({gn, id});
    }
    else if (instanced)
        instances(gn, id, built->mesh);
    if (isCamera)
        applyCamera(id, *gn->camera);
    else if (gn->camera && cameraType)
    {
        // A camera on a mesh or joint node rides along as a child.
        Node* c = graph.createNode(*cameraType, n->name() + "_camera", id, false);
        r.nodes.push_back(c->id());
        const fbx::Trs trs = fbx::decompose(glm::dmat4(F));
        graph.setValue({c->id(), Transform::kRotate, -1}, clean(trs.r));
        applyCamera(c->id(), *gn->camera);
    }
    if (isLight)
    {
        lights::setSettings(graph, id, lightSettings(*gn->light));
        r.lights.push_back(id);
    }
    else if (gn->light && lightType)
    {
        // A light on a mesh, joint or camera node rides along as a child.
        Node* c = graph.createNode(*lightType, n->name() + "_light", id, false);
        r.nodes.push_back(c->id());
        const fbx::Trs trs = fbx::decompose(glm::dmat4(F));
        graph.setValue({c->id(), Transform::kRotate, -1}, clean(trs.r));
        lights::setSettings(graph, c->id(), lightSettings(*gn->light));
        r.lights.push_back(c->id());
    }
    if (gn->mesh && !built)
        r.warnings.push_back(std::format("{}: its mesh has no triangles to import", n->path()));

    for (size_t i = 0; i < gn->children_count; ++i)
        create(gn->children[i], id);
}

void Importer::buildSkins()
{
    LILA_ZONE();

    if (!opts.skins || !types::find("skinCluster"))
    {
        for (const auto& [gn, id] : meshNodes)
            if (gn->skin)
                r.skipped.push_back(std::format("skin on {} (mesh imported unskinned)", graph.path(id)));
        return;
    }
    graph.evaluate();
    const glm::dmat4 parentWorld = opts.parent != kNoNode ? graph.node(opts.parent)->as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
    const glm::dmat4 parentInv = glm::inverse(parentWorld);
    for (const auto& [gn, meshId] : meshNodes)
    {
        const cgltf_skin* skin = gn->skin;
        const BuiltMesh& bm = *meshes[gn->mesh];
        if (!skin || !bm.skinData)
            continue;
        const std::string meshName = graph.path(meshId);

        // glTF ignores a skinned mesh's own transform: its points are in the joints' bind space.
        const glm::dmat4 W = graph.node(meshId)->as<glm::dmat4>(Transform::kWorldMatrix);
        const glm::dmat4 toObject = glm::inverse(W) * parentWorld;
        const PlugRef top{meshId, MeshNode::kInMesh, -1};
        if (!nearIdentity(toObject))
            graph.setValue(top, transformed(*bm.mesh, toObject));

        std::vector<float> ibm = floats(skin->inverse_bind_matrices);
        Node* sc = graph.createNode(*types::find("skinCluster"), "SkinCluster", kNoNode, true);
        const NodeId skinId = sc->id();
        Node* source = graph.createNode(*types::find("meshSource"), "meshSource", kNoNode, true);
        graph.setValue({source->id(), MeshSourceNode::kMesh, -1}, graph.storedValue(top));
        graph.setValue(top, MeshPtr());
        graph.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {skinId, SkinClusterNode::kInMesh, -1});
        graph.connect({skinId, SkinClusterNode::kOutMesh, -1}, top);
        graph.setValue({skinId, SkinClusterNode::kGeomMatrix}, W);

        std::vector<int64_t> influenceOf(skin->joints_count, -1);
        for (size_t j = 0; j < skin->joints_count; ++j)
        {
            auto it = frames.find(skin->joints[j]);
            if (it == frames.end())
            {
                r.warnings.push_back(std::format("{}: skin joint {} is not in the imported scene; its weights are dropped", meshName,
                                                 orEmpty(skin->joints[j]->name)));
                continue;
            }
            const uint32_t index = skin::addInfluence(graph, skinId, it->second.id);
            influenceOf[j] = index;
            glm::dmat4 bind(1.0);
            if (ibm.size() >= (j + 1) * 16)
                for (int c = 0; c < 4; ++c)
                    for (int k = 0; k < 4; ++k)
                        bind[c][k] = ibm[j * 16 + size_t(c) * 4 + size_t(k)];
            for (const skin::Influence& inf : skin::influences(graph, skinId))
                if (inf.index == index)
                    graph.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, toLila(bind) * parentInv);
        }

        MeshPtr bind = skin::inputMesh(graph, skinId);
        WeightTable t;
        t.points.resize(bind ? bind->pointCapacity() : 0);
        size_t maxUsed = 0;
        for (size_t p = 0; p < t.points.size() && p < bm.source.size(); ++p)
        {
            const auto [pi, v] = bm.source[p];
            const Prim& prim = bm.prims[pi];
            std::map<uint32_t, float> sum;
            for (size_t s = 0; s < prim.joints.size() && s < prim.weights.size(); ++s)
                for (int k = 0; k < 4; ++k)
                {
                    const size_t at = size_t(v) * 4 + size_t(k);
                    if (at >= prim.weights[s].size() || at >= prim.joints[s].size())
                        continue;
                    const float w = prim.weights[s][at];
                    const auto j = size_t(prim.joints[s][at]);
                    if (w > 0.0f && j < influenceOf.size() && influenceOf[j] >= 0)
                        sum[uint32_t(influenceOf[j])] += w;
                }
            float total = 0.0f;
            for (const auto& [inf, w] : sum)
                total += w;
            for (const auto& [inf, w] : sum)
                t.points[p].push_back({inf, total > 0.0f ? w / total : w});
            maxUsed = std::max(maxUsed, t.points[p].size());
        }
        graph.setValue({skinId, SkinClusterNode::kMaxInfluences}, int64_t(std::max<size_t>(4, maxUsed)));
        skin::setWeights(graph, skinId, t);
        r.skins.push_back(skinId);
    }
}

void Importer::buildMorphs()
{
    LILA_ZONE();

    if (!types::find("morph"))
        return;
    graph.evaluate();
    const glm::dmat4 parentWorld = opts.parent != kNoNode ? graph.node(opts.parent)->as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
    for (const auto& [gn, meshId] : meshNodes)
    {
        const BuiltMesh& bm = *meshes[gn->mesh];
        if (!bm.targets)
            continue;
        // Skinned meshes had their points taken into the node's object space; the deltas follow.
        glm::dmat3 lin = F * kCmPerMetre;
        if (gn->skin && bm.skinData && opts.skins)
        {
            const NodeId sc = skin::find(graph, meshId);
            if (sc != kNoNode)
                lin = glm::dmat3(glm::inverse(graph.node(sc)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix)) * parentWorld) * lin;
        }
        const NodeId morph = morphs::create(graph, meshId, "Morph");
        r.morphs.push_back(morph);
        NodeFrame& nf = frames[gn];
        for (size_t t = 0; t < bm.targets; ++t)
        {
            auto deltas = std::make_shared<MorphDeltas>();
            for (size_t p = 0; p < bm.source.size(); ++p)
            {
                const auto [pi, v] = bm.source[p];
                const Prim& prim = bm.prims[pi];
                if (t >= prim.morphs.size())
                    continue;
                const float* d = &prim.morphs[t][size_t(v) * 3];
                if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f)
                    continue;
                deltas->indices.push_back(uint32_t(p));
                deltas->deltas.push_back(lin * glm::dvec3(d[0], d[1], d[2]));
            }
            std::string name = t < gn->mesh->target_names_count && gn->mesh->target_names[t] ? gn->mesh->target_names[t] : std::format("target{}", t);
            double weight = 0.0;
            if (t < gn->weights_count)
                weight = gn->weights[t];
            else if (t < gn->mesh->weights_count)
                weight = gn->mesh->weights[t];
            nf.targets.push_back(morphs::addTarget(graph, morph, name, deltas, weight));
            nf.targetWeights.push_back(weight);
        }
    }
}

// Animation -----------------------------------------------------------------------------------------------------------

struct Track
{
    std::vector<float> times;
    std::vector<float> values; // CUBICSPLINE: in tangent, value, out tangent per key
    size_t comps = 0;
    cgltf_interpolation_type interp = cgltf_interpolation_type_linear;

    size_t keys() const { return times.size(); }
    bool cubic() const { return interp == cgltf_interpolation_type_cubic_spline; }
    const float* value(size_t k) const { return &values[(cubic() ? k * 3 + 1 : k) * comps]; }
    const float* inTangent(size_t k) const { return &values[k * 3 * comps]; }
    const float* outTangent(size_t k) const { return &values[(k * 3 + 2) * comps]; }

    /** The sampler at time t, component c, the way glTF interpolates it (rotations are normalised by the caller). */
    double evaluate(double t, size_t c) const
    {
        if (times.empty())
            return 0.0;
        if (t <= times.front())
            return value(0)[c];
        if (t >= times.back())
            return value(keys() - 1)[c];
        const size_t k = size_t(std::upper_bound(times.begin(), times.end(), float(t)) - times.begin()) - 1;
        const double t0 = times[k], t1 = times[k + 1], dt = t1 - t0;
        const double s = dt > 0.0 ? (t - t0) / dt : 0.0;
        const double v0 = value(k)[c], v1 = value(k + 1)[c];
        switch (interp)
        {
        case cgltf_interpolation_type_step: return v0;
        case cgltf_interpolation_type_cubic_spline:
        {
            const double s2 = s * s, s3 = s2 * s;
            return (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * dt * outTangent(k)[c] + (-2 * s3 + 3 * s2) * v1 +
                   (s3 - s2) * dt * inTangent(k + 1)[c];
        }
        default: return v0 + (v1 - v0) * s;
        }
    }
};

Track readTrack(const cgltf_animation_sampler& s)
{
    Track t;
    t.times = floats(s.input);
    t.values = floats(s.output);
    t.interp = s.interpolation;
    const size_t perKey = t.cubic() ? 3 : 1;
    if (!t.times.empty() && s.output)
        t.comps = s.output->count * cgltf_num_components(s.output->type) / (t.times.size() * perKey);
    if (t.comps == 0 || t.values.size() < t.times.size() * perKey * t.comps)
        t.times.clear();
    return t;
}

/** The fps the keys sit on when they are a common frame rate's frames, else 0. */
double keyFps(const cgltf_data& data)
{
    double step = 0.0;
    for (size_t a = 0; a < data.animations_count; ++a)
        for (size_t s = 0; s < data.animations[a].samplers_count; ++s)
        {
            std::vector<float> t = floats(data.animations[a].samplers[s].input);
            for (size_t i = 1; i < t.size(); ++i)
                if (double d = double(t[i]) - double(t[i - 1]); d > 1e-4 && (step == 0.0 || d < step))
                    step = d;
        }
    if (step <= 0.0)
        return 0.0;
    for (double fps : {24.0, 25.0, 30.0, 48.0, 50.0, 60.0, 120.0, 23.976, 29.97, 59.94, 15.0, 12.0})
    {
        const double k = step * fps;
        if (std::abs(k - std::round(k)) < 2e-3 && std::round(k) >= 1.0)
            return fps;
    }
    return 0.0;
}

/** Keys a channel at its rest value, so a take that doesn't animate it still poses it as glTF does. */
void Importer::holdRest(const NodeFrame& nf, int path, double time)
{
    static constexpr const char* kAxis[3] = {"X", "Y", "Z"};
    const std::string base = graph.node(nf.id)->name();
    auto constant = [&](const PlugRef& plug, const char* type, double value, const std::string& name) {
        if (graph.source(plug))
            return;
        anim::Curve c;
        c.keys.push_back({.time = time, .value = value, .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
        fbx::connectCurve(graph, plug, type, c, name);
        ++r.curves;
    };
    switch (path)
    {
    case cgltf_animation_path_type_translation:
        for (int a = 0; a < 3; ++a)
            constant({nf.id, Transform::kTranslate, a}, "animCurveTL", nf.rest.t[a], base + "_translate" + kAxis[a]);
        break;
    case cgltf_animation_path_type_scale:
        for (int a = 0; a < 3; ++a)
            constant({nf.id, Transform::kScale, a}, "animCurveTU", nf.rest.s[a], base + "_scale" + kAxis[a]);
        break;
    case cgltf_animation_path_type_rotation:
        if (anim::keysAsRotation(graph, {nf.id, Transform::kRotate, -1}))
        {
            if (graph.source({nf.id, Transform::kRotate, -1}))
                break;
            anim::RotationCurve rc;
            rc.interp = anim::RotInterp::Slerp;
            rc.order = nf.order;
            rc.keys.push_back(rc.makeKey(time, nf.rest.r));
            fbx::connectRotationCurve(graph, nf.id, rc);
            ++r.curves;
        }
        else
            for (int a = 0; a < 3; ++a)
                constant({nf.id, Transform::kRotate, a}, "animCurveTA", nf.rest.r[a], base + "_rotate" + kAxis[a]);
        break;
    case cgltf_animation_path_type_weights:
        for (size_t t = 0; t < nf.targets.size(); ++t)
            constant({nf.targets[t], morphs::kWeight, -1}, "animCurveTU", nf.targetWeights[t], graph.node(nf.targets[t])->name() + "_weight");
        break;
    default: break;
    }
}

/**
 * hold: with several takes, every channel any of them animates. Each take keys all of those (the ones it doesn't
 * animate at their rest), so switching takes poses exactly what each glTF animation poses.
 */
void Importer::importAnimation(const cgltf_animation& anim, const std::vector<double>& frameTimes, const ChannelSet* hold)
{
    LILA_ZONE();

    const bool keepAll = hold != nullptr;
    ChannelSet done;

    const glm::dmat3 lin = F * kCmPerMetre;
    glm::dmat3 absF(0.0);
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k)
            absF[c][k] = std::abs(F[c][k]);
    static constexpr const char* kAxis[3] = {"X", "Y", "Z"};
    for (size_t ci = 0; ci < anim.channels_count; ++ci)
    {
        const cgltf_animation_channel& ch = anim.channels[ci];
        auto it = ch.target_node ? frames.find(ch.target_node) : frames.end();
        if (it == frames.end() || !ch.sampler)
        {
            if (ch.target_path == cgltf_animation_path_type_invalid)
                r.skipped.push_back(std::format("animation '{}' channel {}: unsupported target (KHR_animation_pointer?)", orEmpty(anim.name), ci));
            continue;
        }
        const NodeFrame& nf = it->second;
        const Track tr = readTrack(*ch.sampler);
        if (tr.keys() == 0)
            continue;
        done.insert({ch.target_node, int(ch.target_path)});
        const std::string base = graph.node(nf.id)->name();
        // A camera's frame turn (after) and its parent camera's (before), as the rest pose took them.
        const glm::dmat3 before(nf.before), after(nf.after);
        glm::dmat3 absAfterT(0.0);
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                absAfterT[c][k] = std::abs(after[k][c]);

        auto vectorTrack = [&](int attr, const glm::dmat3& map, const glm::dvec3& rest, const char* type, const char* label) {
            for (int a = 0; a < 3; ++a)
            {
                anim::Curve c;
                bool moves = false;
                for (size_t k = 0; k < tr.keys(); ++k)
                {
                    const float* v = tr.value(k);
                    anim::Key key{.time = tr.times[k], .value = (map * glm::dvec3(v[0], v[1], v[2]))[a]};
                    switch (tr.interp)
                    {
                    case cgltf_interpolation_type_step:
                        key.in = anim::Tangent::Linear;
                        key.out = anim::Tangent::Step;
                        break;
                    case cgltf_interpolation_type_cubic_spline:
                    {
                        key.in = key.out = anim::Tangent::Fixed;
                        const float* ti = tr.inTangent(k);
                        const float* to = tr.outTangent(k);
                        key.inAngle = glm::degrees(std::atan((map * glm::dvec3(ti[0], ti[1], ti[2]))[a]));
                        key.outAngle = glm::degrees(std::atan((map * glm::dvec3(to[0], to[1], to[2]))[a]));
                        moves = moves || std::abs(key.inAngle) > 1e-9 || std::abs(key.outAngle) > 1e-9;
                        break;
                    }
                    default: key.in = key.out = anim::Tangent::Linear; break;
                    }
                    moves = moves || std::abs(key.value - rest[a]) > 1e-6;
                    c.keys.push_back(key);
                }
                const PlugRef plug{nf.id, attr, a};
                if ((!moves && !keepAll) || graph.source(plug))
                    continue;
                fbx::connectCurve(graph, plug, type, c, base + "_" + label + kAxis[a]);
                ++r.curves;
            }
        };

        switch (ch.target_path)
        {
        case cgltf_animation_path_type_translation:
            if (tr.comps == 3)
                vectorTrack(Transform::kTranslate, before * lin, nf.rest.t, "animCurveTL", "translate");
            break;
        case cgltf_animation_path_type_scale:
            if (tr.comps == 3)
                vectorTrack(Transform::kScale, absAfterT * absF, nf.rest.s, "animCurveTU", "scale");
            break;
        case cgltf_animation_path_type_rotation:
        {
            if (tr.comps != 4)
                break;
            const PlugRef rotate{nf.id, Transform::kRotate, -1};
            if (graph.source(rotate))
                break;
            anim::RotationCurve rc;
            rc.interp = anim::RotInterp::Slerp;
            rc.order = nf.order;
            // Cubic quaternion splines have no Lila equivalent: they are sampled every frame and at every key.
            std::vector<double> times;
            if (tr.cubic())
            {
                std::set<double> all(frameTimes.begin(), frameTimes.end());
                for (float t : tr.times)
                    all.insert(double(t));
                times.assign(all.begin(), all.end());
            }
            else
                for (float t : tr.times)
                    times.push_back(double(t));
            glm::dvec3 ref = nf.rest.r;
            bool moves = false;
            const glm::dmat3 restRot(eulerToMatrix(nf.rest.r, nf.order));
            for (size_t k = 0; k < times.size(); ++k)
            {
                glm::dquat q;
                if (tr.cubic())
                    q = glm::dquat(tr.evaluate(times[k], 3), tr.evaluate(times[k], 0), tr.evaluate(times[k], 1), tr.evaluate(times[k], 2));
                else
                {
                    const float* v = tr.value(k);
                    q = glm::dquat(v[3], v[0], v[1], v[2]);
                }
                if (glm::length(q) < 1e-12)
                    q = glm::dquat(1, 0, 0, 0);
                const glm::dmat3 R = glm::transpose(nf.pre) * before * F * glm::dmat3(glm::mat3_cast(glm::normalize(q))) * glm::transpose(F) * after;
                for (int c = 0; c < 3 && !moves; ++c)
                    moves = glm::length(R[c] - restRot[c]) > 1e-7;
                ref = eulerNear(R, nf.order, ref);
                anim::RotKey key = rc.makeKey(times[k], ref);
                if (tr.interp == cgltf_interpolation_type_step)
                {
                    key.in = anim::Tangent::Linear;
                    key.out = anim::Tangent::Step;
                }
                else
                    key.in = key.out = anim::Tangent::Linear;
                rc.keys.push_back(key);
            }
            if (!moves && !keepAll)
                break;
            if (anim::keysAsRotation(graph, rotate))
            {
                fbx::connectRotationCurve(graph, nf.id, rc);
                ++r.curves;
                break;
            }
            std::array<anim::Curve, 3> euler = anim::eulerKeysFor(rc, frameTimes.empty() ? times : frameTimes);
            for (int a = 0; a < 3; ++a)
            {
                const anim::Curve& c = euler[size_t(a)];
                if (!keepAll && std::all_of(c.keys.begin(), c.keys.end(), [&](const anim::Key& k) { return std::abs(k.value - nf.rest.r[a]) < 1e-6; }))
                    continue;
                fbx::connectCurve(graph, {nf.id, Transform::kRotate, a}, "animCurveTA", c, base + "_rotate" + kAxis[a]);
                ++r.curves;
            }
            break;
        }
        case cgltf_animation_path_type_weights:
        {
            const size_t n = std::min(tr.comps, nf.targets.size());
            if (tr.comps != nf.targets.size())
                r.warnings.push_back(std::format("animation '{}': {} morph weights for {} targets on {}", orEmpty(anim.name), tr.comps, nf.targets.size(),
                                                 graph.path(nf.id)));
            for (size_t t = 0; t < n; ++t)
            {
                anim::Curve c;
                for (size_t k = 0; k < tr.keys(); ++k)
                {
                    anim::Key key{.time = tr.times[k], .value = tr.value(k)[t]};
                    if (tr.interp == cgltf_interpolation_type_step)
                    {
                        key.in = anim::Tangent::Linear;
                        key.out = anim::Tangent::Step;
                    }
                    else if (tr.cubic())
                    {
                        key.in = key.out = anim::Tangent::Fixed;
                        key.inAngle = glm::degrees(std::atan(double(tr.inTangent(k)[t])));
                        key.outAngle = glm::degrees(std::atan(double(tr.outTangent(k)[t])));
                    }
                    else
                        key.in = key.out = anim::Tangent::Linear;
                    c.keys.push_back(key);
                }
                const PlugRef plug{nf.targets[t], morphs::kWeight, -1};
                if (graph.source(plug))
                    continue;
                fbx::connectCurve(graph, plug, "animCurveTU", c, graph.node(nf.targets[t])->name() + "_weight");
                ++r.curves;
            }
            break;
        }
        default:
            r.skipped.push_back(std::format("animation '{}' channel {}: unsupported target path", orEmpty(anim.name), ci));
            break;
        }
    }
    if (hold)
        for (const auto& [gn, path] : *hold)
            if (!done.count({gn, path}))
                if (auto it = frames.find(gn); it != frames.end())
                    holdRest(it->second, path, frameTimes.empty() ? 0.0 : frameTimes.front());
}

void Importer::importAnimations()
{
    LILA_ZONE();

    std::vector<const cgltf_animation*> chosen;
    for (size_t a = 0; a < data.animations_count; ++a)
    {
        const cgltf_animation& anim = data.animations[a];
        if (opts.take.empty() || orEmpty(anim.name) == opts.take || std::to_string(a) == opts.take)
            chosen.push_back(&anim);
    }
    if (chosen.empty())
    {
        if (!opts.take.empty())
            r.warnings.push_back(std::format("no animation named '{}'", opts.take));
        return;
    }
    if (!opts.take.empty() && chosen.size() > 1)
        chosen.resize(1);

    double fps = keyFps(data);
    if (fps <= 0.0)
        fps = anim::playback().fps > 0.0 ? anim::playback().fps : 30.0;
    auto rangeOf = [&](const cgltf_animation& anim) {
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
        for (size_t s = 0; s < anim.samplers_count; ++s)
            for (float t : floats(anim.samplers[s].input))
            {
                lo = std::min(lo, double(t));
                hi = std::max(hi, double(t));
            }
        if (lo > hi)
            lo = hi = 0.0;
        return std::pair{lo, hi};
    };
    auto framesOf = [&](double lo, double hi) {
        std::vector<double> out;
        const long long count = std::llround((hi - lo) * fps);
        for (long long f = 0; f <= count; ++f)
            out.push_back(lo + double(f) / fps);
        return out;
    };
    auto nameOf = [&](const cgltf_animation& anim) {
        return anim.name && *anim.name ? std::string(anim.name) : std::format("animation{}", cgltf_animation_index(&data, &anim));
    };

    const auto [start, end] = rangeOf(*chosen[0]);
    r.take = nameOf(*chosen[0]);
    r.fps = fps;
    r.start = start;
    r.end = end;
    if (chosen.size() == 1)
    {
        importAnimation(*chosen[0], framesOf(start, end), nullptr);
        if (opts.setPlayback)
            fbx::setPlayback(fps, start, end);
        return;
    }

    // Several animations: one Lila take each, the first left active.
    anim::TakeSettings settings;
    settings.fps = fps;
    if (r.roots.size() == 1)
        settings.root = graph.node(r.roots[0])->path();
    ChannelSet hold;
    for (const cgltf_animation* a : chosen)
        for (size_t c = 0; c < a->channels_count; ++c)
            if (a->channels[c].target_node && a->channels[c].target_path != cgltf_animation_path_type_invalid)
                hold.insert({a->channels[c].target_node, int(a->channels[c].target_path)});
    NodeId first = kNoNode;
    for (const cgltf_animation* a : chosen)
    {
        const auto [lo, hi] = rangeOf(*a);
        settings.start = lo;
        settings.end = hi;
        const NodeId take = anim::createTake(graph, fbx::safeName(nameOf(*a), "Take"), settings);
        anim::setActiveTake(graph, take);
        importAnimation(*a, framesOf(lo, hi), &hold);
        r.takes.push_back(graph.node(take)->name());
        if (first == kNoNode)
            first = take;
    }
    anim::setActiveTake(graph, first);
    if (opts.setPlayback)
        fbx::setPlayback(fps, start, end);
}
} // namespace

ImportResult importGltf(scene::Graph& graph, const fs::path& path, const ImportOptions& opts)
{
    LILA_ZONE();

    ImportResult r;
    progress::Scope scope("Importing " + utf8(path.filename()), 10.0, progress::Cancellable);
    std::string fileBytes;
    if (!readFile(path, fileBytes))
    {
        r.error = "can't read " + utf8(path);
        return r;
    }

    cgltf_options options{};
    options.file.read = readCallback;
    options.file.release = releaseCallback;
    cgltf_data* raw = nullptr;
    cgltf_result res = cgltf_parse(&options, fileBytes.data(), fileBytes.size(), &raw);
    std::unique_ptr<cgltf_data, DataDeleter> data(raw);
    if (res != cgltf_result_success)
    {
        r.error = resultText(res);
        return r;
    }
    const std::string pathUtf8 = utf8(path);
    {
        progress::Scope read("Reading " + utf8(path.filename()), 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { res = cgltf_load_buffers(&options, data.get(), pathUtf8.c_str()); });
    }
    scope.checkCancel();
    if (res != cgltf_result_success)
    {
        r.error = std::string("can't load the file's buffers: ") + resultText(res);
        return r;
    }
    if (cgltf_result v = cgltf_validate(data.get()); v != cgltf_result_success)
    {
        r.error = std::string("the file fails validation: ") + resultText(v);
        return r;
    }
    decodeMeshopt(*data, r);

    r.generator = orEmpty(data->asset.generator);
    static const std::set<std::string> kHandled = {
        "KHR_texture_transform",        "KHR_materials_emissive_strength", "KHR_materials_unlit", "KHR_mesh_quantization",
        "EXT_meshopt_compression",      "KHR_materials_ior",               "KHR_materials_specular", "KHR_lights_punctual",
        "KHR_materials_pbrSpecularGlossiness", "EXT_mesh_gpu_instancing",  "KHR_draco_mesh_compression", "KHR_materials_clearcoat",
        "KHR_materials_transmission",   "KHR_materials_volume",            "KHR_materials_sheen", "KHR_materials_iridescence",
        "KHR_materials_anisotropy",     "KHR_materials_dispersion",        "KHR_materials_diffuse_transmission", "EXT_texture_webp",
        "KHR_texture_basisu"};
    for (size_t i = 0; i < data->extensions_used_count; ++i)
    {
        std::string ext = orEmpty(data->extensions_used[i]);
        r.extensionsUsed.push_back(ext);
        if (!kHandled.count(ext))
            r.skipped.push_back(std::format("extension {} (not supported)", ext));
    }
    for (size_t i = 0; i < data->extensions_required_count; ++i)
        if (std::string ext = orEmpty(data->extensions_required[i]); !kHandled.count(ext))
            r.warnings.push_back(std::format("the file requires {}, which Lila doesn't read; it may import incompletely", ext));

    Graph::Batch batch(graph);
    Importer im{graph, *data, opts, r};
    im.F = glm::transpose(lilaToFile(math::upAxis()));
    im.C = glm::dmat4(im.F) * glm::scale(glm::dmat4(1.0), glm::dvec3(kCmPerMetre));
    im.Cinv = glm::inverse(im.C);

    const fs::path textureDir = opts.textureDir.empty() ? path.parent_path() / utf8Path(utf8(path.stem()) + "_textures") : opts.textureDir;
    ImageStore images(*data, path, textureDir, r);
    im.slots = importMaterials(graph, *data, images, opts, r);

    if (opts.skins && types::find("joint"))
        for (size_t s = 0; s < data->skins_count; ++s)
            for (size_t j = 0; j < data->skins[s].joints_count; ++j)
                im.joints.insert(data->skins[s].joints[j]);

    std::vector<const cgltf_node*> roots;
    const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    if (scene)
        roots.assign(scene->nodes, scene->nodes + scene->nodes_count);
    else
        for (size_t i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent)
                roots.push_back(&data->nodes[i]);
    for (size_t i = 0; i < data->scenes_count; ++i)
        if (&data->scenes[i] != scene)
            r.skipped.push_back(std::format("scene '{}' (only the default scene is imported)", orEmpty(data->scenes[i].name)));

    {
        progress::Scope build("Building " + utf8(path.filename()), double(data->nodes_count), progress::None, 7.0);
        im.build = &build;
        for (const cgltf_node* n : roots)
            im.create(n, opts.parent);
        im.build = nullptr;
    }
    scope.checkCancel();
    im.buildSkins();
    im.buildMorphs();
    // A file Lila wrote from a georeferenced scene says where its origin is.
    if (data->asset.extras.data)
        if (const json extras = json::parse(std::string_view(data->asset.extras.data), nullptr, false); extras.is_object() && extras.contains("lilaGeoreference"))
            if (const auto placed = scene::georef::placeImported(graph, extras["lilaGeoreference"], std::vector<uint64_t>(r.roots.begin(), r.roots.end())))
                r.skipped.push_back("georeference: " + *placed);
    if (opts.animation)
        im.importAnimations();
    else if (data->animations_count)
        r.skipped.push_back(std::format("{} animation(s)", data->animations_count));
    r.ok = true;
    return r;
}
} // namespace lila::gltf
