#include "tex/Generators.h"

#include "bake/BakeNodes.h"
#include "bake/Maps.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/NodeDef.h"

#include <algorithm>

namespace lila::tex
{
namespace
{
LiveMeshMapReader& liveReader()
{
    static LiveMeshMapReader fn;
    return fn;
}

LiveMeshMapMetaReader& liveMetaReader()
{
    static LiveMeshMapMetaReader fn;
    return fn;
}

BakedMapsListener& bakedListener()
{
    static BakedMapsListener fn;
    return fn;
}

scene::Node* findProject(scene::Graph& g, const std::string& name)
{
    for (scene::NodeId id : bake::projects(g))
        if (scene::Node* n = g.node(id); n && n->name() == name)
            return n;
    return nullptr;
}
} // namespace

std::string bakeMapName(std::string_view external) { return external == "worldNormal" ? std::string("objectNormal") : std::string(external); }

void setLiveMeshMapReader(LiveMeshMapReader fn) { liveReader() = std::move(fn); }

void setLiveMeshMapMetaReader(LiveMeshMapMetaReader fn) { liveMetaReader() = std::move(fn); }

json liveMeshMapMeta(const std::string& bakeProject, const std::string& map)
{
    if (bakeProject.empty() || !liveMetaReader())
        return json();
    return liveMetaReader()(bakeProject, bakeMapName(map));
}

void setBakedMapsListener(BakedMapsListener fn) { bakedListener() = std::move(fn); }

void bakedMapsChanged(const std::string& bakeProject)
{
    if (bakedListener())
        bakedListener()(bakeProject);
}

image::Image readLiveMeshMap(const std::string& bakeProject, const std::string& map, int udim)
{
    if (bakeProject.empty() || !liveReader())
        return {};
    return liveReader()(bakeProject, bakeMapName(map), udim);
}

const std::vector<MeshRole>& meshRoles()
{
    static const std::vector<MeshRole> roles = {
        {"curvature", "meshCurvature", "out", PortType::Gray, "curvature"},
        {"ao", "meshAo", "out", PortType::Gray, "ao"},
        {"thickness", "meshThickness", "out", PortType::Gray, "thickness"},
        {"cavity", "meshCavity", "out", PortType::Gray, "cavity"},
        {"height", "meshHeight", "out", PortType::Gray, "height"},
        {"position", "position", "position", PortType::Vector, "position"},
        {"worldNormal", "meshWorldNormal", "out", PortType::Vector, "objectNormal"},
        {"id", "meshId", "out", PortType::Color, "id"},
        {"uvIslands", "meshUvIslands", "out", PortType::Color, "uvIslands"},
        {"vertexColor", "meshVertexColor", "out", PortType::Color, "vertexColor"},
    };
    return roles;
}

const MeshRole* findMeshRole(std::string_view name)
{
    for (const MeshRole& r : meshRoles())
        if (r.name == name)
            return &r;
    return nullptr;
}

std::vector<std::string> generatorTypes()
{
    std::vector<std::string> out;
    for (const NodeDef* d : allNodes())
        if (d->category == "Generator")
            out.push_back(d->type);
    return out;
}

std::vector<std::string> missingBakeMaps(scene::Graph& g, const std::string& bakeProject, const std::vector<std::string>& maps)
{
    scene::Node* project = bakeProject.empty() ? nullptr : findProject(g, bakeProject);
    if (!project)
        return {};
    bake::registerBuiltinMaps();
    const bake::ProjectSettings settings = bake::projectSettings(*project);
    std::vector<std::string> out;
    for (const std::string& external : maps)
    {
        const std::string m = bakeMapName(external);
        if (!bake::findMap(m) || std::find(out.begin(), out.end(), m) != out.end())
            continue;
        const bool have = std::any_of(settings.maps.begin(), settings.maps.end(), [&](const bake::MapSettings& s) { return s.map == m && s.enabled; });
        if (!have)
            out.push_back(m);
    }
    return out;
}

std::vector<std::string> addBakeMaps(scene::Graph& g, const std::string& bakeProject, const std::vector<std::string>& maps)
{
    std::vector<std::string> missing = missingBakeMaps(g, bakeProject, maps);
    if (missing.empty())
        return {};
    scene::Node* project = findProject(g, bakeProject);
    bake::ProjectSettings settings = bake::projectSettings(*project);
    for (const std::string& m : missing)
    {
        auto it = std::find_if(settings.maps.begin(), settings.maps.end(), [&](const bake::MapSettings& s) { return s.map == m; });
        if (it != settings.maps.end())
            it->enabled = true;
        else
        {
            bake::MapSettings s;
            s.map = m;
            settings.maps.push_back(std::move(s));
        }
    }
    bake::setProjectSettings(g, project->id(), settings);
    return missing;
}
} // namespace lila::tex
