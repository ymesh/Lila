#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "fbx/FbxBinary.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"

#include <ufbx.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <sstream>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
namespace fs = std::filesystem;

namespace
{
std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Node* addNode(Graph& g, const char* type, const std::string& name, NodeId parent = kNoNode)
{
    return g.createNode(*types::find(type), name, parent, false);
}

Node* addMesh(Graph& g, const std::string& name, geo::Mesh m, NodeId parent = kNoNode)
{
    Node* n = addNode(g, "mesh", name, parent);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n;
}

void setTrs(Graph& g, Node* n, glm::dvec3 t, glm::dvec3 r = glm::dvec3(0.0), glm::dvec3 s = glm::dvec3(1.0))
{
    g.setValue({n->id(), Transform::kTranslate, -1}, t);
    g.setValue({n->id(), Transform::kRotate, -1}, r);
    g.setValue({n->id(), Transform::kScale, -1}, s);
}

/** A cube with two materials, a colour layer and a second uv set. */
geo::Mesh decoratedCube(double size)
{
    geo::Mesh m = geo::makeCube({.width = size, .height = size, .depth = size});
    m.materialNames() = {"M_Body", "M_Trim"};
    std::span<int32_t> mat = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (size_t f = 0; f < mat.size(); ++f)
        mat[f] = int32_t(f % 2);
    std::span<glm::vec4> col = geo::colors::add(m, "Paint");
    for (size_t c = 0; c < col.size(); ++c)
        col[c] = glm::vec4(float(c % 3) / 2.0f, 0.25f, 1.0f, 1.0f);
    std::vector<glm::vec2> uv0(m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).begin(),
                               m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).end());
    std::span<glm::vec2> uv1 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(1));
    for (size_t c = 0; c < uv1.size(); ++c)
        uv1[c] = uv0[c] * 0.5f + glm::vec2(0.25f);
    return m;
}

void buildScene(Graph& g)
{
    Node* body = addMesh(g, "Body", decoratedCube(100.0));
    setTrs(g, body, {10, 20, 30}, {15, 30, 45}, {1, 2, 1.5});
    Node* ucx = addMesh(g, "UCX_Body_00", geo::makeCube({.width = 20, .height = 20, .depth = 20}), body->id());
    setTrs(g, ucx, {0, 50, 0}, {0, 90, 0});
    Node* socket = addNode(g, "locator", "SOCKET_Top", body->id());
    setTrs(g, socket, {0, 100, 5}, {0, 0, 30});
    g.evaluate();
}

struct Snapshot
{
    std::string parent;
    glm::dmat4 local, world;
    MeshPtr mesh;
};

std::map<std::string, Snapshot> snapshot(Graph& g)
{
    g.evaluate();
    std::map<std::string, Snapshot> out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n->type().isA("transform"))
            continue;
        Snapshot s;
        NodeId p = g.parent(id);
        s.parent = p ? g.node(p)->name() : "";
        s.local = n->as<glm::dmat4>(Transform::kMatrix);
        s.world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        if (n->type().isA("mesh"))
            s.mesh = n->as<MeshPtr>(MeshNode::kOutMesh);
        out[n->name()] = s;
    }
    return out;
}

bool nearRelative(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-6)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0, std::abs(a[c][r])))
                return false;
    return true;
}

bool nearRelative(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) <= eps * std::max(1.0, glm::length(a)); }

void checkMeshesMatch(const geo::Mesh& a, const geo::Mesh& b, bool sameTopology)
{
    if (!sameTopology)
    {
        CHECK(a.numPoints() == b.numPoints());
        return;
    }
    REQUIRE(a.numPoints() == b.numPoints());
    REQUIRE(a.numFaces() == b.numFaces());
    REQUIRE(a.numCorners() == b.numCorners());
    for (geo::Index p = 0; p < a.pointCapacity(); ++p)
        CHECK(nearRelative(a.position(p), b.position(p)));

    for (int set = 0; set < 2; ++set)
    {
        std::span<const glm::vec2> ua = a.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set));
        std::span<const glm::vec2> ub = b.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set));
        REQUIRE(ua.size() == ub.size());
        for (size_t c = 0; c < ua.size(); ++c)
            CHECK(glm::length(ua[c] - ub[c]) < 1e-6f);
    }

    std::vector<glm::vec3> na = geo::cornerNormals(a);
    std::span<const glm::vec3> nb = b.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(nb.size() == na.size());
    for (size_t c = 0; c < na.size(); ++c)
        CHECK(glm::dot(na[c], nb[c]) > 0.9999f);

    std::span<const bool> ha = a.edgeAttrs().get<bool>(geo::attr::kHard);
    std::span<const bool> hb = b.edgeAttrs().get<bool>(geo::attr::kHard);
    if (!ha.empty())
    {
        REQUIRE(!hb.empty());
        for (geo::Index e = 0; e < a.edgeCapacity(); ++e)
        {
            geo::Index eb = b.findEdge(a.edgePoint(e, 0), a.edgePoint(e, 1));
            REQUIRE(eb != geo::kInvalid);
            CHECK(ha[e] == hb[eb]);
        }
    }

    // A mesh without materials goes out with one "Default" slot, since Unreal wants at least one.
    if (a.materialNames().empty())
        CHECK(b.materialNames() == std::vector<std::string>{"Default"});
    else
    {
        std::span<const int32_t> ma = a.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        std::span<const int32_t> mb = b.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        REQUIRE(mb.size() == ma.size());
        for (size_t f = 0; f < ma.size(); ++f)
            CHECK(a.materialNames()[size_t(ma[f])] == b.materialNames()[size_t(mb[f])]);
    }

    CHECK(geo::colors::sets(a) == geo::colors::sets(b));
    std::span<const glm::vec4> ca = geo::colors::values(a, geo::colors::current(a));
    std::span<const glm::vec4> cb = geo::colors::values(b, geo::colors::current(b));
    REQUIRE(ca.size() == cb.size());
    for (size_t c = 0; c < ca.size(); ++c)
        CHECK(glm::length(ca[c] - cb[c]) < 1e-6f);
}

void roundTrip(fbx::AxisProfile profile, math::UpAxis up, bool triangulate)
{
    std::string profileLabel = fbx::profileName(profile);
    CAPTURE(profileLabel);
    CAPTURE(int(up));
    CAPTURE(triangulate);
    math::setUpAxis(up);
    Graph& g = Graph::current();
    g.clear();
    buildScene(g);
    auto before = snapshot(g);

    fbx::ExportOptions opts;
    opts.profile = profile;
    opts.triangulate = triangulate;
    fs::path path = tempFile(std::string("roundtrip_") + fbx::profileName(profile) + (up == math::UpAxis::Z ? "_z" : "_y") +
                             (triangulate ? "_tri" : "") + ".fbx");
    std::string err;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {g.find("Body")->id()}, opts), path, opts, &err));
    const std::vector<glm::vec3> preview = fbx::previewTriangles(path);
    CHECK(!preview.empty());
    CHECK(preview.size() % 3 == 0);

    g.clear();
    fbx::ImportResult r = fbx::importFbx(g, path);
    REQUIRE_MESSAGE(r.ok, r.error);
    for (const std::string& w : r.warnings)
        MESSAGE(w);
    CHECK(r.roots.size() == 1);
    CHECK(r.nodes.size() == 3);
    auto after = snapshot(g);
    REQUIRE(after.size() == before.size());
    for (auto& [name, b] : before)
    {
        CAPTURE(name);
        REQUIRE(after.count(name));
        const Snapshot& a = after[name];
        CHECK(a.parent == b.parent);
        CHECK(nearRelative(a.local, b.local));
        CHECK(nearRelative(a.world, b.world));
        CHECK(bool(a.mesh) == bool(b.mesh));
        if (a.mesh && b.mesh)
            checkMeshesMatch(*b.mesh, *a.mesh, !triangulate);
    }
    math::setUpAxis(math::UpAxis::Y);
}
} // namespace

TEST_CASE("fbx binary records encode the 7.4 layout")
{
    fbx::bin::Record root("");
    fbx::bin::Record& n = root.add("Test");
    n.i32(7).f64(1.5).str("hi");
    n.add("Child").i16(3);
    std::string bytes = fbx::bin::encodeFile(root);

    REQUIRE(bytes.size() > 27 + 160);
    CHECK(bytes.compare(0, 21, std::string("Kaydara FBX Binary  \0", 21)) == 0);
    CHECK(uint8_t(bytes[21]) == 0x1a);
    uint32_t version;
    std::memcpy(&version, bytes.data() + 23, 4);
    CHECK(version == 7400);

    // First record: end offset, 3 properties, name "Test".
    uint32_t end, count, listLen;
    std::memcpy(&end, bytes.data() + 27, 4);
    std::memcpy(&count, bytes.data() + 31, 4);
    std::memcpy(&listLen, bytes.data() + 35, 4);
    CHECK(count == 3);
    CHECK(listLen == (1 + 4) + (1 + 8) + (1 + 4 + 2));
    CHECK(uint8_t(bytes[39]) == 4);
    CHECK(bytes.compare(40, 4, "Test") == 0);
    // It has a child, so it ends with a 13-byte null record.
    CHECK(bytes.compare(end - 13, 13, std::string(13, '\0')) == 0);
    CHECK(uint8_t(bytes.back()) == 0x0b);
}

TEST_CASE("fbx export round trips through ufbx for every profile and up axis")
{
    for (fbx::AxisProfile p : {fbx::AxisProfile::Ue5, fbx::AxisProfile::Ue6, fbx::AxisProfile::Generic})
        for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
        {
            roundTrip(p, up, false);
            roundTrip(p, up, true);
        }
}

TEST_CASE("ue5 and ue6 files hold Lila's semantic axes where Unreal expects them")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        CAPTURE(int(up));
        math::setUpAxis(up);
        const math::AxisBasis& b = math::axisBasis(up);
        Graph& g = Graph::current();
        g.clear();
        // One point per semantic direction: 100 forward, 50 right, 25 up.
        glm::dvec3 pts[3] = {b.forward * 100.0, b.right * 50.0, b.up * 25.0};
        geo::Index sizes[1] = {3};
        geo::Index idx[3] = {0, 1, 2};
        addMesh(g, "Probe", geo::Mesh::fromPolygons(pts, sizes, idx));

        struct Case
        {
            fbx::AxisProfile profile;
            glm::dvec3 forward, right, up;
            ufbx_coordinate_axis fileUp, fileFront;
        };
        // ue5: Unreal reads Z-up right-handed and negates Y, so right is stored at -Y.
        const Case cases[] = {
            {fbx::AxisProfile::Ue5, {100, 0, 0}, {0, -50, 0}, {0, 0, 25}, UFBX_COORDINATE_AXIS_POSITIVE_Z, UFBX_COORDINATE_AXIS_NEGATIVE_Y},
            {fbx::AxisProfile::Ue6, {0, 0, 100}, {-50, 0, 0}, {0, 25, 0}, UFBX_COORDINATE_AXIS_POSITIVE_Y, UFBX_COORDINATE_AXIS_POSITIVE_Z},
        };
        for (const Case& c : cases)
        {
            std::string profileLabel = fbx::profileName(c.profile);
            CAPTURE(profileLabel);
            fbx::ExportOptions opts;
            opts.profile = c.profile;
            fs::path path = tempFile(std::string("axes_") + fbx::profileName(c.profile) + ".fbx");
            std::string err;
            REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {g.find("Probe")->id()}, opts), path, opts, &err));
            UfbxScene s(path);
            REQUIRE(s.scene);
            CHECK(s.scene->settings.axes.up == c.fileUp);
            CHECK(s.scene->settings.axes.front == c.fileFront);
            CHECK(s.scene->settings.unit_meters == doctest::Approx(0.01));
            REQUIRE(s.scene->meshes.count == 1);
            const ufbx_mesh* m = s.scene->meshes.data[0];
            REQUIRE(m->vertices.count == 3);
            auto v = [&](size_t i) { return glm::dvec3(m->vertices.data[i].x, m->vertices.data[i].y, m->vertices.data[i].z); };
            CHECK(nearRelative(v(0), c.forward));
            CHECK(nearRelative(v(1), c.right));
            CHECK(nearRelative(v(2), c.up));
        }
    }
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("fbx export is deterministic and carries tangents, sockets and collision names")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    buildScene(g);
    fbx::ExportOptions opts;
    fs::path a = tempFile("det_a.fbx"), b = tempFile("det_b.fbx");
    std::string err;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {g.find("Body")->id()}, opts), a, opts, &err));
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {g.find("Body")->id()}, opts), b, opts, &err));
    CHECK(readAll(a) == readAll(b));

    UfbxScene s(a);
    REQUIRE(s.scene);
    CHECK(s.scene->metadata.warnings.count == 0);
    std::map<std::string, const ufbx_node*> byName;
    for (size_t i = 0; i < s.scene->nodes.count; ++i)
        byName[std::string(s.scene->nodes.data[i]->name.data)] = s.scene->nodes.data[i];
    REQUIRE(byName.count("Body"));
    REQUIRE(byName.count("UCX_Body_00"));
    REQUIRE(byName.count("SOCKET_Top"));
    CHECK(byName["SOCKET_Top"]->attrib_type == UFBX_ELEMENT_EMPTY);
    CHECK(byName["UCX_Body_00"]->parent == byName["Body"]);

    const ufbx_mesh* body = byName["Body"]->mesh;
    REQUIRE(body);
    CHECK(body->num_triangles == 12);
    CHECK(body->num_faces == 12);
    CHECK(body->uv_sets.count == 2);
    CHECK(body->color_sets.count == 1);
    CHECK(byName["Body"]->materials.count == 2);
    REQUIRE(body->vertex_tangent.exists);
    REQUIRE(body->vertex_bitangent.exists);
    for (size_t i = 0; i < body->num_indices; ++i)
    {
        ufbx_vec3 n = ufbx_get_vertex_vec3(&body->vertex_normal, i);
        ufbx_vec3 t = ufbx_get_vertex_vec3(&body->vertex_tangent, i);
        glm::dvec3 nn(n.x, n.y, n.z), tt(t.x, t.y, t.z);
        CHECK(std::abs(glm::length(tt) - 1.0) < 1e-5);
        CHECK(std::abs(glm::dot(nn, tt)) < 1e-4);
    }
}

TEST_CASE("world-space export bakes mirrored transforms with outward winding")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    Node* n = addMesh(g, "Mirrored", geo::makeCube({.width = 100, .height = 100, .depth = 100}));
    setTrs(g, n, {200, 0, 0}, {0, 0, 0}, {-1, 1, 1});
    fbx::ExportOptions opts;
    opts.worldSpace = true;
    fs::path path = tempFile("mirrored.fbx");
    std::string err;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {n->id()}, opts), path, opts, &err));

    g.clear();
    fbx::ImportResult r = fbx::importFbx(g, path);
    REQUIRE(r.ok);
    g.evaluate();
    Node* m = g.find("Mirrored");
    REQUIRE(m);
    CHECK(nearRelative(m->as<glm::dmat4>(Transform::kWorldMatrix), glm::dmat4(1.0)));
    MeshPtr mesh = m->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(mesh);
    CHECK(mesh->bounds().center().x == doctest::Approx(200.0));
    std::span<const glm::vec3> normals = mesh->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
    {
        glm::dvec3 fn = mesh->faceNormal(f);
        glm::dvec3 out = glm::normalize(mesh->faceCentroid(f) - mesh->bounds().center());
        CHECK(glm::dot(fn, out) > 0.5);
        mesh->forEachFaceCorner(f, [&](geo::Index c) { CHECK(glm::dot(glm::dvec3(normals[c]), fn) > 0.99); });
    }
}

namespace
{
MeshPtr importedMesh(const fs::path& path, const std::string& name)
{
    Graph& g = Graph::current();
    g.clear();
    fbx::ImportResult r = fbx::importFbx(g, path);
    REQUIRE_MESSAGE(r.ok, r.error);
    g.evaluate();
    Node* n = g.find(name);
    REQUIRE(n);
    return n->as<MeshPtr>(MeshNode::kOutMesh);
}

template <class T> bool sameCorners(const geo::Mesh& a, const geo::Mesh& b, std::string_view attr)
{
    std::span<const T> va = a.cornerAttrs().get<T>(attr);
    std::span<const T> vb = b.cornerAttrs().get<T>(attr);
    return va.size() == vb.size() && std::equal(va.begin(), va.end(), vb.begin());
}
} // namespace

TEST_CASE("compressed fbx arrays import back exactly")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    Node* n = addMesh(g, "Dense", geo::makeSphere({.subdivAxis = 200, .subdivHeight = 200}));
    g.evaluate();
    MeshPtr source = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(source);
    fs::path packed = tempFile("compressed.fbx"), raw = tempFile("uncompressed.fbx");
    ops::execute("exportFbx", {{"args", {packed.string(), "Dense"}}, {"triangulate", false}});
    ops::execute("exportFbx", {{"args", {raw.string(), "Dense"}}, {"triangulate", false}, {"compress", false}});
    MESSAGE("compressed " << fs::file_size(packed) << " bytes, raw " << fs::file_size(raw));
    CHECK(fs::file_size(packed) < fs::file_size(raw) * 3 / 4);
    {
        UfbxScene s(packed);
        REQUIRE(s.scene);
        CHECK(s.scene->metadata.warnings.count == 0);
    }

    MeshPtr a = importedMesh(packed, "Dense");
    MeshPtr b = importedMesh(raw, "Dense");
    REQUIRE(a);
    REQUIRE(b);
    checkMeshesMatch(*source, *a, true);
    REQUIRE(a->pointCapacity() == b->pointCapacity());
    REQUIRE(a->faceCapacity() == b->faceCapacity());
    REQUIRE(a->cornerCapacity() == b->cornerCapacity());
    bool samePoints = true, sameTopology = true;
    for (geo::Index p = 0; p < a->pointCapacity(); ++p)
        samePoints = samePoints && a->position(p) == b->position(p);
    for (geo::Index c = 0; c < a->cornerCapacity(); ++c)
        sameTopology = sameTopology && a->cornerPoint(c) == b->cornerPoint(c);
    for (geo::Index f = 0; f < a->faceCapacity(); ++f)
        sameTopology = sameTopology && a->faceCorner(f) == b->faceCorner(f);
    CHECK(samePoints);
    CHECK(sameTopology);
    CHECK(sameCorners<glm::vec3>(*a, *b, geo::attr::kNormal));
    CHECK(sameCorners<glm::vec2>(*a, *b, geo::attr::uvName(0)));
}

TEST_CASE("zlibDeflate writes one zlib stream across many blocks")
{
    std::vector<uint8_t> data(3 * 1024 * 1024 + 12345);
    uint32_t x = 1;
    for (size_t i = 0; i < data.size(); ++i)
    {
        x = x * 1664525u + 1013904223u;
        data[i] = uint8_t((i / 7) % 251) ^ uint8_t(x >> 30);
    }
    for (size_t blockBytes : {size_t(64 * 1024), fbx::bin::kDeflateBlockBytes, data.size() * 2})
    {
        CAPTURE(blockBytes);
        std::string z = fbx::bin::zlibDeflate(data, blockBytes);
        CHECK(z.size() < data.size());

        std::vector<uint8_t> back(data.size());
        uLongf n = uLongf(back.size());
        REQUIRE(uncompress(back.data(), &n, reinterpret_cast<const Bytef*>(z.data()), uLong(z.size())) == Z_OK);
        CHECK(n == data.size());
        CHECK(back == data);

        std::vector<uint8_t> viaUfbx(data.size());
        ufbx_inflate_input in{};
        in.total_size = z.size();
        in.data = z.data();
        in.data_size = z.size();
        ufbx_inflate_retain retain;
        retain.initialized = false;
        CHECK(ufbx_inflate(viaUfbx.data(), viaUfbx.size(), &in, &retain) == ptrdiff_t(data.size()));
        CHECK(viaUfbx == data);
    }
}

TEST_CASE("exportFbx and importFbx commands")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    buildScene(g);
    fs::path path = tempFile("command.fbx");
    json res = ops::execute("exportFbx", {{"args", {path.string(), "Body"}}, {"profile", "ue5"}, {"atOrigin", true}});
    CHECK(res.get<std::string>() == path.string());
    CHECK_THROWS_AS(ops::execute("exportFbx", {{"args", {path.string(), "Body"}}, {"profile", "ue7"}}), CommandError);

    g.clear();
    json report = ops::execute("importFbx", {{"args", {path.string()}}, {"report", true}});
    REQUIRE(report["nodes"].size() == 3);
    CHECK(report["roots"] == json::array({"Body"}));
    CHECK(report["skipped"].empty());
    Node* body = g.find("Body");
    REQUIRE(body);
    CHECK(nearRelative(body->as<glm::dvec3>(Transform::kTranslate), glm::dvec3(0.0)));
    CHECK(g.selection() == std::vector<NodeId>{body->id()});

    json names = ops::execute("importFbx", {{"args", {path.string()}}});
    CHECK(names.size() == 3);
    CHECK(names[0].get<std::string>() != "Body"); // made unique
}

TEST_CASE("fbx export writes a plain color layer as vertex colors")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    geo::Mesh m = geo::makeCube({.width = 100.0, .height = 100.0, .depth = 100.0});
    std::span<glm::vec4> col = m.cornerAttrs().getOrAdd<glm::vec4>(geo::attr::kColor);
    for (size_t c = 0; c < col.size(); ++c)
        col[c] = glm::vec4(0.25f, 0.5f, 0.75f, 1.0f);
    REQUIRE(geo::colors::sets(m).empty());
    addMesh(g, "Tinted", std::move(m));
    const fs::path path = tempFile("plaincolor.fbx");
    ops::execute("exportFbx", {{"args", {path.string(), "Tinted"}}, {"profile", "ue5"}});
    g.clear();
    ops::execute("importFbx", {{"args", {path.string()}}});
    Node* n = g.find("Tinted");
    REQUIRE(n);
    const MeshPtr back = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(back);
    const std::vector<std::string> sets = geo::colors::sets(*back);
    REQUIRE(sets.size() == 1);
    for (const glm::vec4& c : geo::colors::values(*back, sets[0]))
        CHECK(std::abs(c.g - 0.5f) < 1e-3f);
}

TEST_CASE("fbx textures carry their wrap modes out and back")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    Node* box = g.createNode(*types::find("mesh"), "Crate", kNoNode, false);
    g.setValue({box->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube({.width = 10, .height = 10, .depth = 10}))));
    const fs::path base = tempFile("crate_bc.png"), normal = tempFile("crate_n.png");
    std::ofstream(base, std::ios::binary) << "png";
    std::ofstream(normal, std::ios::binary) << "png";
    Node* m = materials::create(g, "Wood");
    g.setValue({m->id(), material::kBaseColor, -1}, glm::dvec3(0.25, 0.5, 0.75));
    g.setValue({m->id(), material::kBaseColorMap, -1}, base.generic_string());
    g.setValue({m->id(), material::kNormalMap, -1}, normal.generic_string());
    materials::setSampling(g, m->id(), material::kBaseColorMap, {material::TextureWrap::Clamp, material::TextureWrap::Repeat, {}});
    materials::setSampling(g, m->id(), material::kNormalMap, {material::TextureWrap::Repeat, material::TextureWrap::Black, {}});
    ops::execute("assignMaterial", {{"args", {"Wood", "Crate"}}});

    const fs::path path = tempFile("textures.fbx");
    ops::execute("exportFbx", {{"args", {path.string(), "Crate"}}, {"textures", true}});
    {
        UfbxScene s(path);
        REQUIRE(s.scene);
        REQUIRE(s.scene->materials.count == 1);
        const ufbx_material& fm = *s.scene->materials.data[0];
        REQUIRE(fm.fbx.diffuse_color.texture);
        CHECK(fm.fbx.diffuse_color.texture->wrap_u == UFBX_WRAP_CLAMP);
        CHECK(fm.fbx.diffuse_color.texture->wrap_v == UFBX_WRAP_REPEAT);
        CHECK(fm.fbx.diffuse_color.value_vec3.y == doctest::Approx(0.5));
        REQUIRE(fm.fbx.normal_map.texture);
        CHECK(fm.fbx.normal_map.texture->wrap_v == UFBX_WRAP_CLAMP);
    }

    g.clear();
    ops::execute("importFbx", {{"args", {path.string()}}});
    Node* back = materials::find(g, "Wood");
    REQUIRE(back);
    CHECK(back->as<std::string>(material::kBaseColorMap).ends_with("crate_bc.png"));
    const material::TextureSampling bs = materials::sampling(*back, material::kBaseColorMap);
    CHECK(bs.wrapU == material::TextureWrap::Clamp);
    CHECK(bs.wrapV == material::TextureWrap::Repeat);
    const material::TextureSampling ns = materials::sampling(*back, material::kNormalMap);
    CHECK(ns.wrapU == material::TextureWrap::Repeat);
    CHECK(ns.wrapV == material::TextureWrap::Clamp);

    // Without textures the materials are plain names, as before.
    ops::execute("exportFbx", {{"args", {path.string(), "Crate"}}});
    UfbxScene plain(path);
    REQUIRE(plain.scene);
    CHECK(plain.scene->textures.count == 0);
}

TEST_CASE("exportForUnreal writes the roots at the origin")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    buildScene(g);
    fs::path path = tempFile("unreal.fbx");
    std::string err;
    REQUIRE(fbx::exportForUnreal({g.find("Body")->id()}, path, "ue5", &err));
    CHECK_FALSE(fbx::exportForUnreal({g.find("Body")->id()}, path, "generic", &err));

    UfbxScene s(path);
    REQUIRE(s.scene);
    const ufbx_node* root = s.scene->root_node->children.data[0];
    CHECK(std::string(root->name.data) == "Body");
    CHECK(root->local_transform.translation.x == doctest::Approx(0.0));
    CHECK(root->local_transform.translation.y == doctest::Approx(0.0));
    CHECK(root->local_transform.translation.z == doctest::Approx(0.0));
}

TEST_CASE("fbx material slots are Lila's material names, with unassigned faces on Default")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    geo::Mesh assigned = geo::makeCube();
    scene::materials::assign(assigned, "M_Hull", {});
    geo::Mesh mixed = geo::makeCube();
    scene::materials::assign(mixed, "M_Hull", {0, 1});
    scene::materials::assign(mixed, "M_Glass", {2});
    Node* a = addMesh(g, "Assigned", std::move(assigned));
    Node* u = addMesh(g, "Unassigned", geo::makeCube());
    Node* m = addMesh(g, "Mixed", std::move(mixed));
    g.evaluate();

    fs::path path = tempFile("slots.fbx");
    std::string err;
    REQUIRE(fbx::exportForUnreal({a->id(), u->id(), m->id()}, path, "ue5", &err));
    UfbxScene s(path);
    REQUIRE(s.scene);

    // Triangles per material name, from the face material indices.
    auto slots = [&](const char* node) {
        std::map<std::string, size_t> out;
        const ufbx_node* n = ufbx_find_node(s.scene, node);
        REQUIRE(n);
        REQUIRE(n->mesh);
        const ufbx_mesh* mesh = n->mesh;
        for (size_t f = 0; f < mesh->faces.count; ++f)
        {
            uint32_t slot = mesh->face_material.count ? mesh->face_material.data[f] : 0;
            REQUIRE(slot < n->materials.count);
            out[std::string(n->materials.data[slot]->name.data)] += mesh->faces.data[f].num_indices - 2;
        }
        return out;
    };
    CHECK(slots("Assigned") == std::map<std::string, size_t>{{"M_Hull", 12}});
    CHECK(slots("Unassigned") == std::map<std::string, size_t>{{"Default", 12}});
    CHECK(slots("Mixed") == std::map<std::string, size_t>{{"Default", 6}, {"M_Glass", 2}, {"M_Hull", 4}});
    // One FBX material per name, shared by the meshes using it.
    std::set<std::string> names;
    for (size_t i = 0; i < s.scene->materials.count; ++i)
        CHECK(names.insert(s.scene->materials.data[i]->name.data).second);
    CHECK(names == std::set<std::string>{"Default", "M_Glass", "M_Hull"});
}

TEST_CASE("imports FBX files written by other tools (LILA_FBX_EXTERNAL_DIR)")
{
    const char* dir = std::getenv("LILA_FBX_EXTERNAL_DIR");
    if (!dir)
        return;
    math::setUpAxis(math::UpAxis::Y);
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.path().extension() != ".fbx")
            continue;
        std::string file = entry.path().filename().string();
        CAPTURE(file);
        Graph& g = Graph::current();
        g.clear();
        fbx::ImportResult r = fbx::importFbx(g, entry.path());
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(!r.nodes.empty());
        g.evaluate();
        MESSAGE(file << ": axes " << r.sourceAxes << ", unit " << r.sourceUnitMeters << " m");
        for (const std::string& s : r.skipped)
            MESSAGE("  skipped " << s);
        for (const std::string& w : r.warnings)
            MESSAGE("  warning " << w);
        for (NodeId id : r.nodes)
        {
            Node* n = g.node(id);
            glm::dvec3 p(n->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
            std::string line = "  " + n->name() + " at (" + std::to_string(p.x) + ", " + std::to_string(p.y) + ", " + std::to_string(p.z) + ")";
            if (n->type().isA("mesh"))
            {
                MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
                REQUIRE(m);
                CHECK(m->validate(geo::kValidateStructure).empty());
                size_t hard = 0;
                for (bool h : m->edgeAttrs().get<bool>(geo::attr::kHard))
                    hard += h;
                line += " points " + std::to_string(m->numPoints()) + " faces " + std::to_string(m->numFaces()) + " hard " +
                        std::to_string(hard) + " uvSets " + std::to_string(m->cornerAttrs().has("uv0") + m->cornerAttrs().has("uv1")) + " materials " +
                        std::to_string(m->materialNames().size());
            }
            MESSAGE(line);
        }
    }
}
