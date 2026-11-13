#include "scene/GeoSidecar.h"
#include "scene/MeshNodes.h"
#include "ops/Command.h"
#include "core/FileBytes.h"
#include "core/Paths.h"
#include "core/Progress.h"
#include "geo/ColorSets.h"
#include "geo/PlyIO.h"
#include "geo/StlIO.h"
#include "math/MathCore.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

#include <tbb/parallel_for.h>

#include <filesystem>

namespace lila::scene
{
namespace
{
namespace fs = std::filesystem;

/** Centimetres per file unit. */
double unitScale(const std::string& unit, const char* cmd)
{
    if (unit == "mm")
        return 0.1;
    if (unit == "cm")
        return 1.0;
    if (unit == "m")
        return 100.0;
    if (unit == "in")
        return 2.54;
    throw CommandError(std::string(cmd) + ": unit must be mm, cm, m or in");
}

/** File space to the scene's: unit scale plus the turn between the file's up axis and the scene's. */
glm::dmat4 fileToScene(const Args& a, const char* cmd)
{
    const double s = unitScale(a.get<std::string>("unit"), cmd);
    const std::string up = a.get<std::string>("upAxis");
    if (up != "y" && up != "z")
        throw CommandError(std::string(cmd) + ": upAxis must be y or z");
    const bool fileZ = up == "z", sceneZ = math::upAxis() == math::UpAxis::Z;
    glm::dmat4 m(s);
    m[3][3] = 1.0;
    if (fileZ && !sceneZ) // (x, y, z) -> (x, z, -y)
        m = glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, -s, 0), glm::dvec4(0, s, 0, 0), glm::dvec4(0, 0, 0, 1));
    else if (!fileZ && sceneZ) // (x, y, z) -> (x, -z, y)
        m = glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, s, 0), glm::dvec4(0, -s, 0, 0), glm::dvec4(0, 0, 0, 1));
    return m;
}

/** Positions by m, file normals (corner and point) by its inverse transpose. */
void transformMesh(geo::Mesh& mesh, const glm::dmat4& m)
{
    tbb::parallel_for(geo::Index(0), mesh.pointCapacity(), [&](geo::Index p) {
        if (mesh.pointValid(p))
            mesh.setPosition(p, glm::dvec3(m * glm::dvec4(mesh.position(p), 1.0)));
    });
    const glm::mat3 n(glm::transpose(glm::inverse(glm::dmat3(m))));
    const bool flip = glm::determinant(glm::dmat3(m)) < 0.0;
    for (geo::AttributeSet* set : {&mesh.cornerAttrs(), &mesh.pointAttrs()})
    {
        std::span<glm::vec3> normals = set->get<glm::vec3>(geo::attr::kNormal);
        tbb::parallel_for(size_t(0), normals.size(), [&](size_t i) {
            if (glm::dot(normals[i], normals[i]) > 0.0f)
                normals[i] = glm::normalize(n * normals[i]);
        });
    }
    if (flip)
        for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
            if (mesh.faceValid(f))
                mesh.flipFace(f);
}

std::string readPath(const Args& a, const char* cmd)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": path required");
    return a.positional[0].get<std::string>();
}

json addMeshes(std::vector<std::pair<std::string, geo::Mesh>>& meshes)
{
    Graph& g = Graph::current();
    Graph::Batch batch(g);
    json names = json::array();
    std::vector<NodeId> ids;
    for (auto& [name, mesh] : meshes)
    {
        Node* n = g.createNode(*types::find("mesh"), Graph::validName(name) ? name : "Mesh", kNoNode, true);
        g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
        names.push_back(n->path());
        ids.push_back(n->id());
    }
    g.select(ids);
    return names;
}

/** The evaluated meshes of the named nodes and the meshes under them (the selection by default), in file space. */
std::vector<std::pair<std::string, geo::Mesh>> gatherMeshes(const Args& a, const char* cmd, const glm::dmat4& sceneToFile)
{
    Graph& g = Graph::current();
    std::vector<NodeId> roots;
    for (size_t k = 1; k < a.positional.size(); ++k)
    {
        Node* n = g.find(a.positional[k].get<std::string>());
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + a.positional[k].get<std::string>() + "'");
        roots.push_back(n->id());
    }
    if (roots.empty())
        roots = g.selection();
    std::vector<NodeId> nodes;
    for (NodeId r : roots)
    {
        nodes.push_back(r);
        for (NodeId d : g.descendants(r))
            nodes.push_back(d);
    }
    g.evaluate();
    std::vector<std::pair<std::string, geo::Mesh>> out;
    std::vector<NodeId> seen;
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || std::ranges::find(seen, id) != seen.end())
            continue;
        seen.push_back(id);
        MeshPtr m = modifiers::exportMesh(g, id);
        if (!m)
            continue;
        geo::Mesh mesh = *m;
        transformMesh(mesh, sceneToFile * n->as<glm::dmat4>(Transform::kWorldMatrix));
        out.emplace_back(n->name(), std::move(mesh));
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": no meshes to export");
    return out;
}

void writeFile(const std::string& path, const std::string& data, const char* cmd)
{
    try
    {
        writeFileBytes(paths::fromUtf8(path), data);
    }
    catch (const std::runtime_error&)
    {
        throw CommandError(std::string(cmd) + ": can't write " + path);
    }
}

FlagDef unitFlag(const char* fallback, const char* doc) { return {"unit", "", ArgType::String, fallback, doc, {"mm", "cm", "m", "in"}}; }
FlagDef upFlag() { return {"upAxis", "", ArgType::String, "z", "The file's up axis; converted to the scene's.", {"y", "z"}}; }
} // namespace

void registerMeshFileCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "importStl",
        .doc = "Import an STL file, binary or ASCII: one mesh per solid. Triangles sharing positions weld into a connected mesh, edges "
               "bent past hardAngle are hard, and binary per-triangle colors (VisCAM and Materialise) become a color set. Returns "
               "the new mesh names.",
        .positionalDoc = "path",
        .flags = {unitFlag("mm", "The file's length unit. STL has none; 3D printing assumes mm."), upFlag(),
                  {"weld", "", T::Bool, true, "Merge identical positions into shared points."},
                  {"hardAngle", "", T::Float, 30.0, "Edges bent more than this many degrees are hard; -1 leaves all soft.", {}, {}, -1.0, 180.0}},
        .run = [](const Args& a) -> json {
            const std::string path = readPath(a, "importStl");
            const glm::dmat4 m = fileToScene(a, "importStl");
            progress::Scope scope("Importing " + path + "...");
            geo::StlReadResult r;
            progress::runOnWorker([&] {
                r = geo::readStlFile(path, {.weld = a.get<bool>("weld", true), .hardAngle = a.get<double>("hardAngle", 30.0)});
                for (geo::StlSolid& s : r.solids)
                    transformMesh(s.mesh, m);
            });
            if (!r.ok())
                throw CommandError("importStl: " + r.error);
            std::vector<std::pair<std::string, geo::Mesh>> meshes;
            const std::string stem = fs::path(path).stem().string();
            for (geo::StlSolid& s : r.solids)
            {
                meshes.emplace_back(s.name.empty() || r.binary ? stem : s.name, std::move(s.mesh));
            }
            return addMeshes(meshes);
        },
    });

    ops::registerCommand({
        .name = "exportStl",
        .doc = "Export meshes to STL in world space, triangulated, defaulting to the selection and the meshes under it. Binary "
               "merges them into one solid; ASCII writes a solid per mesh. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {unitFlag("mm", "The file's length unit. 3D printing assumes mm."), upFlag(),
                  {"binary", "", T::Bool, true, "Binary STL; off writes ASCII."},
                  {"colors", "", T::Bool, false, "Binary only: the current color set as per-triangle VisCAM colors."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string path = readPath(a, "exportStl");
            const glm::dmat4 toFile = glm::inverse(fileToScene(a, "exportStl"));
            std::vector<geo::StlSolid> solids;
            for (auto& [name, mesh] : gatherMeshes(a, "exportStl", toFile))
                solids.push_back({name, std::move(mesh)});
            progress::Scope scope("Exporting " + path + "...");
            std::string data;
            progress::runOnWorker([&] { data = geo::writeStl(solids, {.binary = a.get<bool>("binary", true), .colors = a.get<bool>("colors")}); });
            writeFile(path, data, "exportStl");
            return path;
        },
    });

    ops::registerCommand({
        .name = "importPly",
        .doc = "Import a PLY file (ASCII or binary, either byte order) as one mesh: polygons with normals, UVs (per vertex or "
               "MeshLab's per-corner texcoord), vertex colors as colorSet and face colors as faceColor. Other numeric vertex and "
               "face properties (quality, intensity, confidence...) become float attributes of the same name. A file with only "
               "vertices comes in as a point cloud: a mesh of loose points. Returns the new mesh names, or with report=True a "
               "dict of nodes, pointCloud, textureFiles and skipped.",
        .positionalDoc = "path",
        .flags = {unitFlag("m", "The file's length unit. Scanners and photogrammetry write metres."), upFlag(),
                  {"normals", "", T::Bool, true, "Keep the file's normals."},
                  {"georeference", "", T::Bool, true,
                   "Place it by the georeference files beside it (metadata.xml, _offset.xyz, .rcinfo, .prj): it sets the scene's "
                   "georeference or moves into it, and unit and upAxis are the CRS's."},
                  {"report", "", T::Bool, false, "Return the import report instead of just the names."}},
        .run = [](const Args& a) -> json {
            const std::string path = readPath(a, "importPly");
            const glm::dmat4 m = fileToScene(a, "importPly");
            const bool georeferenced = a.get<bool>("georeference", true) && geoSidecar::find(path);
            progress::Scope scope("Importing " + path + "...");
            geo::PlyReadResult r;
            progress::runOnWorker([&] {
                r = geo::readPlyFile(path, {.keepNormals = a.get<bool>("normals", true)});
                if (r.ok() && !georeferenced)
                    transformMesh(r.mesh, m);
            });
            if (!r.ok())
                throw CommandError("importPly: " + r.error);
            std::optional<std::string> placed;
            if (georeferenced)
                placed = geoSidecar::place(path, {&r.mesh});
            std::vector<std::pair<std::string, geo::Mesh>> meshes;
            meshes.emplace_back(fs::path(path).stem().string(), std::move(r.mesh));
            json names = addMeshes(meshes);
            if (!a.get<bool>("report"))
                return names;
            json report = {{"nodes", names}, {"pointCloud", r.pointCloud}, {"textureFiles", r.textureFiles}, {"skipped", r.skipped}};
            if (placed)
                report["georeference"] = *placed;
            return report;
        },
    });

    ops::registerCommand({
        .name = "exportPly",
        .doc = "Export meshes to PLY in world space, merged into one mesh, defaulting to the selection and the meshes under it: "
               "polygons as they are, normals, uv0 and the current color set per vertex (points split where those differ). Loose "
               "points and edges are written too. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {unitFlag("m", "The file's length unit."), upFlag(),
                  {"binary", "", T::Bool, true, "Binary little endian; off writes ASCII."},
                  {"normals", "", T::Bool, true, "Write normals."},
                  {"uvs", "", T::Bool, true, "Write uv0 as s, t."},
                  {"colors", "", T::Bool, true, "Write the current color set as 8-bit RGBA."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string path = readPath(a, "exportPly");
            const glm::dmat4 toFile = glm::inverse(fileToScene(a, "exportPly"));
            std::vector<std::pair<std::string, geo::Mesh>> meshes = gatherMeshes(a, "exportPly", toFile);
            geo::Mesh merged = std::move(meshes[0].second);
            for (size_t i = 1; i < meshes.size(); ++i)
                merged.append(meshes[i].second);
            progress::Scope scope("Exporting " + path + "...");
            std::string data;
            progress::runOnWorker([&] {
                data = geo::writePly(merged, {.binary = a.get<bool>("binary", true),
                                              .normals = a.get<bool>("normals", true),
                                              .uvs = a.get<bool>("uvs", true),
                                              .colors = a.get<bool>("colors", true)});
            });
            writeFile(path, data, "exportPly");
            return path;
        },
    });
}
} // namespace lila::scene
