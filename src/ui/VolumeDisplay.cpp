#include "ui/VolumeDisplay.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "gn/Volume.h"
#include "render/FrameState.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/SceneIndex.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

namespace lila::ui::volumeDisplay
{
namespace
{
using namespace scene::material;

/** Voxels across the longest side of a grid the renderer takes; denser grids are averaged down. */
constexpr int kMaxCells = 160;

const scene::Node* volumeMaterial(const geo::Mesh& mesh)
{
    scene::Graph& g = scene::Graph::current();
    for (const std::string& slot : mesh.materialNames())
        if (const scene::Node* m = scene::materials::find(g, slot); m && m->as<int64_t>(kShadingModel) == int64_t(ShadingModel::Volume))
            return m;
    return nullptr;
}

/** A grid as the renderer takes it: density values no more than kMaxCells across, averaged down by whole voxels. */
struct Grid
{
    glm::dmat4 indexToWorld{1.0};
    glm::ivec3 dims{0};
    std::shared_ptr<const std::vector<float>> values;
    float peak = 0.0f;
};

Grid reduce(const gn::Volume& v)
{
    Grid out;
    const int longest = std::max({v.dims.x, v.dims.y, v.dims.z});
    const int step = std::max(1, (longest + kMaxCells - 1) / kMaxCells);
    out.dims = (v.dims + step - 1) / step;
    auto values = std::make_shared<std::vector<float>>(size_t(out.dims.x) * out.dims.y * out.dims.z, 0.0f);
    for (int k = 0; k < out.dims.z; ++k)
        for (int j = 0; j < out.dims.y; ++j)
            for (int i = 0; i < out.dims.x; ++i)
            {
                double sum = 0.0;
                int count = 0;
                for (int c = 0; c < step; ++c)
                    for (int b = 0; b < step; ++b)
                        for (int a = 0; a < step; ++a)
                            if (v.inside(i * step + a, j * step + b, k * step + c))
                            {
                                sum += std::max(v.at(i * step + a, j * step + b, k * step + c), 0.0f);
                                ++count;
                            }
                const float d = count ? float(sum / count) : 0.0f;
                (*values)[size_t(i) + size_t(out.dims.x) * (size_t(j) + size_t(out.dims.y) * k)] = d;
                out.peak = std::max(out.peak, d);
            }
    // Averaged voxel (i, j, k) is centred between the first and last source voxels it covers.
    const double offset = 0.5 * double(step - 1);
    out.indexToWorld = v.indexToWorld * glm::translate(glm::dmat4(1.0), glm::dvec3(offset)) * glm::scale(glm::dmat4(1.0), glm::dvec3(step));
    out.values = std::move(values);
    return out;
}

/** A volume as a renderer grid, kept while the volume lives. */
Grid gridOf(const std::shared_ptr<const gn::Volume>& v)
{
    static std::mutex mutex;
    static std::map<const gn::Volume*, std::pair<std::weak_ptr<const gn::Volume>, Grid>> cache;
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](const auto& e) { return e.second.first.expired(); });
    if (auto it = cache.find(v.get()); it != cache.end())
        return it->second.second;
    LILA_ZONE_N("Volume grid");
    Grid g = reduce(*v);
    cache[v.get()] = {v, g};
    return g;
}
} // namespace

bool isVolumeMesh(const geo::Mesh& mesh)
{
    return volumeMaterial(mesh) != nullptr;
}

void apply(render::FrameState& s)
{
    LILA_ZONE();
    s.volumes.clear();
    scene::Graph& g = scene::Graph::current();
    const scene::sceneIndex::Hierarchy& hierarchy = scene::sceneIndex::hierarchy(g, true);
    // Material slot name to its volume material (or none), looked up once a frame: many meshes share few materials.
    std::unordered_map<std::string, const scene::Node*> volumeBySlot;
    auto volumeOf = [&](const geo::Mesh& mesh) -> const scene::Node* {
        for (const std::string& slot : mesh.materialNames())
        {
            auto it = volumeBySlot.find(slot);
            if (it == volumeBySlot.end())
            {
                const scene::Node* m = scene::materials::find(g, slot);
                it = volumeBySlot.emplace(slot, m && m->as<int64_t>(kShadingModel) == int64_t(ShadingModel::Volume) ? m : nullptr).first;
            }
            if (it->second)
                return it->second;
        }
        return nullptr;
    };
    for (const scene::Node* n : scene::sceneIndex::ofType(g, "mesh"))
    {
        if (!hierarchy.visible(n->id()))
            continue;
        const scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        const scene::Node* material = mesh ? volumeOf(*mesh) : nullptr;
        if (!material)
            continue;
        if (s.volumes.size() >= render::kMaxVolumes)
        {
            log::warn("volume: only the first {} volumes draw", render::kMaxVolumes);
            break;
        }
        const std::shared_ptr<const gn::Volume> volume = gn::meshVolume(g, n->id());
        if (!volume)
            continue;
        const Grid gr = gridOf(volume);
        if (!gr.values || gr.peak <= 0.0f)
            continue;
        render::VolumeDraw d;
        d.indexToWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix) * gr.indexToWorld;
        d.dims = gr.dims;
        d.values = gr.values;
        d.peak = gr.peak;
        d.density = float(material->as<double>(kVolumeDensity)) * 0.01f;
        d.scatter = glm::vec3(material->as<glm::dvec3>(kVolumeScatterColor));
        d.absorption = glm::vec3(material->as<glm::dvec3>(kVolumeAbsorptionColor));
        d.anisotropy = float(std::clamp(material->as<double>(kVolumeAnisotropy), -0.95, 0.95));
        d.emission = glm::vec3(material->as<glm::dvec3>(kVolumeEmissionColor)) * float(material->as<double>(kVolumeEmissionStrength));
        d.node = uint64_t(n->id());
        s.volumes.push_back(std::move(d));
    }
}
} // namespace lila::ui::volumeDisplay
