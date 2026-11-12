// Geometry-node volumes to and from OpenVDB files (.vdb), the format Unreal, Blender, Houdini and EmberGen read.

#include "Volumes.h"

#include <lila/Command.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Paths.h>

#include <openvdb/openvdb.h>

#include <filesystem>
#include <list>
#include <mutex>

using namespace lila;

namespace nanovdbext
{
namespace
{
using json = nlohmann::json;

/** OpenVDB maps row vectors (index * M), so its matrix holds glm's columns as rows. */
openvdb::math::Mat4d toVdb(const glm::dmat4& m)
{
    openvdb::math::Mat4d out;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out(i, j) = m[i][j];
    return out;
}

glm::dmat4 fromVdb(const openvdb::math::Mat4d& m)
{
    glm::dmat4 out;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = m(i, j);
    return out;
}

openvdb::GridBase::Ptr toVdbVectorGrid(const gn::Volume& v)
{
    openvdb::Vec3SGrid::Ptr grid = openvdb::Vec3SGrid::create(openvdb::Vec3s(0.0f));
    grid->setName(v.name);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(toVdb(v.indexToWorld)));
    auto acc = grid->getAccessor();
    for (int k = 0; k < v.dims.z; ++k)
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
                if (glm::vec3 x = v.vectors[v.index(i, j, k)]; x != glm::vec3(0.0f))
                    acc.setValue(openvdb::Coord(i, j, k), openvdb::Vec3s(x.x, x.y, x.z));
    return grid;
}

openvdb::GridBase::Ptr toVdbGrid(const gn::Volume& v)
{
    if (v.isVector())
        return toVdbVectorGrid(v);
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(v.background);
    grid->setName(v.name);
    grid->setGridClass(v.isSdf() ? openvdb::GRID_LEVEL_SET : openvdb::GRID_FOG_VOLUME);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(toVdb(v.indexToWorld)));
    auto acc = grid->getAccessor();
    for (int k = 0; k < v.dims.z; ++k)
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
                if (float x = v.values[v.index(i, j, k)]; x != v.background)
                    acc.setValue(openvdb::Coord(i, j, k), x);
    return grid;
}

/** A float or vector grid made dense over its active voxels. Throws past about 64 million voxels. */
template <class GridT> gn::Volume fromVdbGrid(const GridT& grid)
{
    constexpr bool vector = std::is_same_v<GridT, openvdb::Vec3SGrid>;
    gn::Volume v;
    v.name = grid.getName();
    if constexpr (vector)
        v.kind = gn::Volume::Kind::Vector;
    else
    {
        v.kind = grid.getGridClass() == openvdb::GRID_LEVEL_SET ? gn::Volume::Kind::Sdf : gn::Volume::Kind::Density;
        v.background = grid.background();
    }
    const openvdb::CoordBBox box = grid.evalActiveVoxelBoundingBox();
    if (box.empty())
        return v;
    const openvdb::Coord lo = box.min(), hi = box.max();
    v.dims = {hi.x() - lo.x() + 1, hi.y() - lo.y() + 1, hi.z() - lo.z() + 1};
    if (double(v.dims.x) * v.dims.y * v.dims.z > 64.0e6)
        throw std::runtime_error("the grid is too big to read whole (" + std::to_string(v.dims.x) + " x " + std::to_string(v.dims.y) + " x " +
                                 std::to_string(v.dims.z) + " voxels)");
    auto affine = grid.transform().baseMap()->getAffineMap();
    v.indexToWorld = fromVdb(affine->getMat4()) * glm::dmat4(glm::dvec4(1, 0, 0, 0), glm::dvec4(0, 1, 0, 0), glm::dvec4(0, 0, 1, 0),
                                                              glm::dvec4(lo.x(), lo.y(), lo.z(), 1));
    v.values.assign(size_t(v.dims.x) * size_t(v.dims.y) * size_t(v.dims.z), v.background);
    auto acc = grid.getConstAccessor();
    if constexpr (vector)
    {
        std::vector<glm::vec3> vectors(v.values.size());
        for (int k = 0; k < v.dims.z; ++k)
            for (int j = 0; j < v.dims.y; ++j)
                for (int i = 0; i < v.dims.x; ++i)
                {
                    openvdb::Vec3s x = acc.getValue(openvdb::Coord(lo.x() + i, lo.y() + j, lo.z() + k));
                    vectors[v.index(i, j, k)] = glm::vec3(x.x(), x.y(), x.z());
                }
        v.setVectors(std::move(vectors));
    }
    else
        for (int k = 0; k < v.dims.z; ++k)
            for (int j = 0; j < v.dims.y; ++j)
                for (int i = 0; i < v.dims.x; ++i)
                    v.values[v.index(i, j, k)] = acc.getValue(openvdb::Coord(lo.x() + i, lo.y() + j, lo.z() + k));
    return v;
}

std::shared_ptr<const gn::Volume> readVdb(const std::filesystem::path& file, const std::string& gridName)
{
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const gn::Volume>>> recent;
    std::error_code ec;
    auto stamp = std::filesystem::last_write_time(file, ec);
    if (ec)
        throw std::runtime_error("can't find " + paths::toUtf8(file));
    std::string key = paths::toUtf8(file) + "|" + gridName + "|" + std::to_string(paths::timeKey(stamp));
    {
        std::lock_guard lock(mutex);
        for (auto& [k, v] : recent)
            if (k == key)
                return v;
    }
    openvdb::io::File f(paths::toUtf8(file));
    f.open();
    openvdb::FloatGrid::Ptr grid;
    openvdb::Vec3SGrid::Ptr vectorGrid;
    if (!gridName.empty())
    {
        openvdb::GridBase::Ptr base = f.readGrid(gridName);
        grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
        vectorGrid = openvdb::gridPtrCast<openvdb::Vec3SGrid>(base);
    }
    else
    {
        for (auto it = f.beginName(); it != f.endName() && !grid; ++it)
            grid = openvdb::gridPtrCast<openvdb::FloatGrid>(f.readGrid(it.gridName()));
        for (auto it = f.beginName(); it != f.endName() && !grid && !vectorGrid; ++it)
            vectorGrid = openvdb::gridPtrCast<openvdb::Vec3SGrid>(f.readGrid(it.gridName()));
    }
    f.close();
    if (!grid && !vectorGrid)
        throw std::runtime_error(paths::toUtf8(file) + " has no float or vector grid" + (gridName.empty() ? "" : " named " + gridName));
    auto v = std::make_shared<const gn::Volume>(grid ? fromVdbGrid(*grid) : fromVdbGrid(*vectorGrid));
    std::lock_guard lock(mutex);
    recent.emplace_front(key, v);
    if (recent.size() > 8)
        recent.pop_back();
    return v;
}
} // namespace

gn::NodeDecl readVdbNode()
{
    using T = gn::SocketType;
    return {
        .type = "ReadVDB",
        .label = "Read VDB",
        .category = "Volume",
        .doc = "A float or vector grid from an OpenVDB file (.vdb, from Houdini, Blender or EmberGen) as a volume: level sets as signed "
               "distances, fog volumes as densities, vec3 grids such as velocity as vectors. Read whole over the grid's active voxels.",
        .sockets = {.inputs = {{.id = "path", .type = T::String, .defaultValue = "", .doc = "The .vdb file; relative paths are against the scene."},
                               {.id = "grid", .type = T::String, .defaultValue = "", .doc = "Grid name, such as density; empty reads the first float grid, else the first vector grid."}},
                    .outputs = {{.id = "volume", .type = T::Geometry}}},
        .exec =
            [](gn::ExecContext& c) {
                std::string path = c.s("path");
                if (path.empty())
                {
                    c.out("volume", gn::makeGeometry({}));
                    return;
                }
                try
                {
                    gn::Geometry g;
                    g.volume = readVdb(paths::resolvePath(path), c.s("grid"));
                    c.out("volume", gn::makeGeometry(std::move(g)));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
            },
    };
}

void registerVdbCommands(ext::Context& ctx)
{
    openvdb::initialize();
    ctx.registerCommand({
        .name = "exportVDB",
        .doc = "Write the volume a mesh's geometry nodes make (or a geometry-nodes modifier's) to an OpenVDB file: a sparse grid, a "
               "level set for signed distances, a fog volume for densities and a vec3 grid for vectors, for Unreal's sparse volume textures, Houdini or Blender. "
               "Returns the file written.",
        .positionalDoc = "target path",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
                throw CommandError("exportVDB: target and path required");
            scene::Graph& g = scene::Graph::current();
            scene::Node* target = g.find(a.positional[0].get<std::string>());
            if (!target)
                throw CommandError("exportVDB: no node '" + a.positional[0].get<std::string>() + "'");
            std::shared_ptr<const gn::Volume> v = volumeOf(g, *target);
            if (!v)
                throw CommandError("exportVDB: " + target->name() + " has no geometry-nodes volume");
            std::filesystem::path path = paths::fromUtf8(a.positional[1].get<std::string>());
            if (path.extension().empty())
                path += ".vdb";
            try
            {
                if (path.has_parent_path())
                    std::filesystem::create_directories(path.parent_path());
                openvdb::io::File f(paths::toUtf8(path));
                f.write({toVdbGrid(*v)});
                f.close();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("exportVDB: ") + e.what());
            }
            return paths::toUtf8(path);
        },
    });
}
} // namespace nanovdbext
