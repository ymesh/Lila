#include <doctest/doctest.h>

#include "TestUtil.h"
#include "FakeServer.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "link/Exporters.h"
#include "link/RpcClient.h"
#include "link/UnrealLink.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace lila;
using namespace lila::link;
using namespace linktest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
/** Registries the commands need, set up once on the thread doctest runs tests on. */
struct Setup
{
    Setup()
    {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        link::registerCommands();
    }
};

void setup()
{
    static Setup once;
}

json helloResult()
{
    return {{"engine", {{"major", 5}, {"minor", 8}, {"patch", 2}, {"string", "5.8.2"}}},
            {"project", "TestGame"},
            {"projectDir", "C:/Projects/TestGame/"},
            {"contentDir", "C:/Projects/TestGame/Content/"},
            {"pluginVersion", "0.1.0"},
            {"axisProfile", "ue5"},
            {"features", {"progress"}}};
}

/** Answers hello and ping; everything else goes to `extra`. */
FakeServer::Handler editor(std::function<std::optional<json>(const json&)> extra = nullptr)
{
    return [extra](const json& req) -> std::optional<json> {
        std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, helloResult());
        if (m == "ping")
            return FakeServer::reply(req, "pong");
        if (extra)
            return extra(req);
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "Unknown method"}}}};
    };
}

template <class F> bool waitUntil(F&& f, int ms = 3000)
{
    for (int i = 0; i < ms / 10; ++i)
    {
        mainq::drain();
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("framing: split, batched and out-of-order replies match their requests")
{
    setup();
    std::mutex held;
    json first;
    FakeServer server([&](const json& req) -> std::optional<json> {
        // Hold "a" back and answer it together with "b", b first.
        if (req["method"] == "a")
        {
            std::scoped_lock lock(held);
            first = req;
            return std::nullopt;
        }
        std::scoped_lock lock(held);
        return json::array({FakeServer::reply(req, "B"), FakeServer::reply(first, "A")});
    });
    server.rawWrite = [](TcpSocket& s, const std::string& data) {
        // One byte per write, with a CRLF thrown in, to exercise reassembly.
        for (char c : data)
        {
            if (c == '\n')
                s.sendAll("\r");
            s.sendAll(std::string_view(&c, 1));
        }
    };

    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));

    std::atomic<int> done = 0;
    RpcResult ra, rb;
    client.callAsync("a", json::object(), 3000, [&](RpcResult r) { ra = std::move(r); ++done; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.callAsync("b", {{"x", 1}}, 3000, [&](RpcResult r) { rb = std::move(r); ++done; });
    REQUIRE(waitUntil([&] { return done == 2; }));

    CHECK(ra.ok);
    CHECK(ra.result == "A");
    CHECK(rb.ok);
    CHECK(rb.result == "B");

    auto reqs = server.requests();
    REQUIRE(reqs.size() == 2);
    CHECK(reqs[0]["jsonrpc"] == "2.0");
    CHECK(reqs[0]["id"] != reqs[1]["id"]);
    CHECK(reqs[1]["params"]["x"] == 1);
}

TEST_CASE("errors come back as failed results with the server's code")
{
    setup();
    FakeServer server(editor());
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));
    RpcResult r = client.call("nope", json::object(), 2000);
    CHECK_FALSE(r.ok);
    CHECK(r.code == -32601);
    CHECK(r.error == "Unknown method");
}

TEST_CASE("timeouts fail the request and a late reply is ignored")
{
    setup();
    std::mutex m;
    json slowReq;
    FakeServer server(editor([&](const json& req) -> std::optional<json> {
        if (req["method"] == "slow")
        {
            std::scoped_lock lock(m);
            slowReq = req;
            return std::nullopt;
        }
        // Answer the abandoned request first, then this one.
        std::scoped_lock lock(m);
        return json::array({FakeServer::reply(slowReq, "late"), FakeServer::reply(req, "fast")});
    }));

    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));

    auto t0 = std::chrono::steady_clock::now();
    RpcResult slow = client.call("slow", json::object(), 200);
    auto took = std::chrono::steady_clock::now() - t0;
    CHECK_FALSE(slow.ok);
    CHECK(slow.code == RpcResult::kTimeout);
    CHECK(took >= std::chrono::milliseconds(150));
    CHECK(took < std::chrono::milliseconds(2000));

    RpcResult fast = client.call("fast", json::object(), 2000);
    CHECK(fast.ok);
    CHECK(fast.result == "fast");
    CHECK(client.connected());
}

TEST_CASE("pending requests fail when the server goes away")
{
    setup();
    auto server = std::make_unique<FakeServer>([](const json&) -> std::optional<json> { return std::nullopt; });
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server->port(), 1000));

    std::atomic<bool> got = false;
    RpcResult r;
    client.callAsync("hang", json::object(), 10000, [&](RpcResult x) { r = std::move(x); got = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.reset();
    REQUIRE(waitUntil([&] { return got.load(); }));
    CHECK(r.code == RpcResult::kDisconnected);
    CHECK_FALSE(client.connected());
}

TEST_CASE("connect fails cleanly when nothing listens")
{
    setup();
    TcpListener l;
    REQUIRE(l.listen(0));
    uint16_t port = l.port();
    l.close();
    RpcClient client;
    std::string err;
    CHECK_FALSE(client.connect("127.0.0.1", port, 300, &err));
    CHECK_FALSE(err.empty());
    RpcResult r = client.call("ping");
    CHECK(r.code == RpcResult::kDisconnected);
}

TEST_CASE("Link says hello and reports status")
{
    setup();
    FakeServer server(editor());
    Link::ConnectOptions o;
    o.port = server.port();
    json h = Link::get().connect(o);
    CHECK(h["project"] == "TestGame");
    CHECK(h["axisProfile"] == "ue5");
    CHECK(h["port"] == server.port());

    json s = Link::get().status();
    CHECK(s["connected"] == true);
    CHECK(s["engine"]["major"] == 5);

    o.project = "OtherGame";
    CHECK_THROWS_AS(Link::get().connect(o), std::runtime_error);

    Link::get().disconnect();
    CHECK(Link::get().status()["connected"] == false);
}

TEST_CASE("port files are discovered from configured project dirs")
{
    setup();
    fs::path proj = tempDir("portfile");
    fs::create_directories(proj / "Saved" / "LilaLink");
    std::ofstream(proj / "Saved" / "LilaLink" / "port.json")
        << R"({"port": 8799, "pid": 1234, "engineVersion": "5.8.2", "project": "Crates"})";
    std::ofstream(proj / "Crates.uproject") << "{}";

    std::vector<Instance> found = readPortFiles({proj / "Crates.uproject", tempDir("noport")});
    REQUIRE(found.size() == 1);
    CHECK(found[0].port == 8799);
    CHECK(found[0].pid == 1234);
    CHECK(found[0].project == "Crates");
    CHECK(found[0].source == "portFile");
    fs::remove_all(proj);
}

TEST_CASE("unrealImport sends importAsset with the mapped options")
{
    setup();
    json importResult = {{"assets", {"/Game/Props/SM_Crate.SM_Crate"}}, {"warnings", json::array()}, {"errors", json::array()}};
    FakeServer server(editor([&](const json& req) -> std::optional<json> {
        if (req["method"] == "importAsset")
            return FakeServer::reply(req, importResult);
        return std::nullopt;
    }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    fs::path file = tempDir("import") / "crate.fbx";
    std::ofstream(file) << "fbx";

    json out = ops::execute("unrealImport", {{"args", {file.string()}},
                                              {"contentPath", "/Game/Props/SM_Crate"},
                                              {"lodGroup", "LargeProp"},
                                              {"autoCollision", false},
                                              {"wait", true}});
    CHECK(out["assets"][0] == "/Game/Props/SM_Crate.SM_Crate");

    json req;
    for (const json& r : server.requests())
        if (r["method"] == "importAsset")
            req = r;
    REQUIRE(req.is_object());
    const json& p = req["params"];
    CHECK(fs::path(p["file"].get<std::string>()) == fs::path(file.generic_string()));
    CHECK(p["contentPath"] == "/Game/Props/SM_Crate");
    CHECK(p["kind"] == "static");
    CHECK(p["options"]["lodGroup"] == "LargeProp");
    CHECK(p["options"]["autoCollision"] == false);
    CHECK_FALSE(p["options"].contains("skeleton"));

    // Queued form: returns at once, the reply lands through mainq.
    json queued = ops::execute("unrealImport", {{"args", {file.string()}}, {"contentPath", "/Game/Props/SM_Crate2"}});
    CHECK(queued["queued"] == true);
    CHECK(waitUntil([&] {
        int n = 0;
        for (const json& r : server.requests())
            n += r["method"] == "importAsset";
        return n == 2;
    }));

    CHECK_THROWS_AS(ops::execute("unrealImport", {{"args", {file.string()}}}), CommandError);
    CHECK_THROWS_AS(ops::execute("unrealImport", {{"args", {file.string()}}, {"contentPath", "/Game/X"}, {"kind", "mesh"}}), CommandError);

    Link::get().disconnect();
    fs::remove_all(file.parent_path());
}

TEST_CASE("mappings live on their nodes and drive auto-sync on save")
{
    setup();
    fs::path ws = tempDir("workspace");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    PrefStore::get().set("unreal", "/cacheDir", (ws / "cache").generic_string(), PrefScope::Workspace);

    scene::Graph& g = scene::Graph::current();
    scene::Node* crate = g.createNode(*scene::types::find("mesh"), "crate");
    REQUIRE(crate);
    std::string crateName = crate->name();

    json m = ops::execute("unrealMap", {{"args", {crateName}}, {"contentPath", "/Game/Props/SM_Crate"}, {"lodGroup", "SmallProp"}});
    CHECK(m["node"] == crateName);
    CHECK(ops::execute("unrealMappings", json::object()).size() == 1);
    // Re-mapping replaces rather than duplicates.
    ops::execute("unrealMap", {{"args", {crateName}}, {"contentPath", "/Game/Props/SM_Crate"}, {"kind", "static"}});
    REQUIRE(mappings().size() == 1);
    CHECK_FALSE(mappings()[0].contains("lodGroup"));
    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {"nobody"}}, {"contentPath", "/Game/X"}}), CommandError);

    // No exporter yet: nothing is queued.
    unregisterExporter("fbx");
    CHECK(syncMappings(true) == 0);

    std::mutex mx;
    std::vector<fs::path> exported;
    registerExporter("fbx", [&](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string*) {
        REQUIRE(nodes.size() == 1);
        std::ofstream(path) << "fbx";
        std::scoped_lock lock(mx);
        exported.push_back(path);
        return true;
    });

    FakeServer server(editor([](const json& req) -> std::optional<json> {
        if (req["method"] == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        return std::nullopt;
    }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    onSceneSaved();
    Link::get().waitIdle();
    mainq::drain();

    {
        std::scoped_lock lock(mx);
        REQUIRE(exported.size() == 1);
        CHECK(exported[0] == ws / "cache" / (crateName + ".fbx"));
        CHECK(fs::exists(exported[0]));
    }
    json req;
    for (const json& r : server.requests())
        if (r["method"] == "importAsset")
            req = r;
    REQUIRE(req.is_object());
    CHECK(req["params"]["contentPath"] == "/Game/Props/SM_Crate");
    CHECK(fs::path(req["params"]["file"].get<std::string>()) == fs::path((ws / "cache" / (crateName + ".fbx")).generic_string()));

    // The pref turns save-triggered sync off; unrealSync still forces it.
    PrefStore::get().set("unreal", "/autoSync", false, PrefScope::Workspace);
    onSceneSaved();
    Link::get().waitIdle();
    CHECK(exported.size() == 1);
    CHECK(ops::execute("unrealSync", json::object()) == 1);
    Link::get().waitIdle();
    CHECK(exported.size() == 2);

    CHECK(ops::execute("unrealUnmap", {{"args", {crateName}}}) == 1);
    CHECK(mappings().empty());

    unregisterExporter("fbx");
    Link::get().disconnect();
    g.deleteNode(crate->id());
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("mappings follow renames and deletes, refuse meshless nodes, and adopt the workspace's old ones")
{
    setup();
    fs::path ws = tempDir("mapping_nodes");
    PrefStore::get().setWorkspaceDir(ws / ".lila");

    scene::Graph& g = scene::Graph::current();
    scene::Node* group = g.createNode(*scene::types::find("transform"), "hero");
    scene::Node* body = g.createNode(*scene::types::find("mesh"), "heroBody", group->id());
    scene::Node* joint = g.createNode(*scene::types::find("transform"), "heroRoot");
    REQUIRE((group && body && joint));

    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {joint->name()}}, {"contentPath", "/Game/X/SM_Root"}}), CommandError);
    ops::execute("unrealMap", {{"args", {group->name()}}, {"contentPath", "/Game/Chars/SK_Hero"}, {"kind", "skeletal"}, {"facing", "y"}});
    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {group->name()}}, {"contentPath", "/Game/X"}, {"facing", "z"}}), CommandError);

    g.rename(group->id(), "heroRenamed");
    json list = mappings();
    REQUIRE(list.size() == 1);
    CHECK(list[0]["node"] == g.fullPath(group->id()));
    CHECK(list[0]["facing"] == "y");
    CHECK(importParams("x.fbx", list[0])["options"]["facing"] == "y");

    CHECK(ops::execute("unrealUnmap", {{"args", {group->name()}}}) == 1);
    CHECK(mappings().empty());
    CHECK(UndoStack::get().undo());
    CHECK(mappings().size() == 1);

    // Old workspace entries move onto the nodes they name; a meshless one is dropped, an unresolved one waits for its scene.
    ops::execute("unrealUnmap", {{"args", {group->name()}}});
    PrefStore::get().set("unreal", "/mappings",
                         json::array({{{"node", g.fullPath(body->id())}, {"contentPath", "/Game/M/SM_Body"}, {"kind", "static"}},
                                      {{"node", g.fullPath(joint->id())}, {"contentPath", "/Game/M/SM_Root"}, {"kind", "static"}},
                                      {{"node", "|elsewhere"}, {"contentPath", "/Game/M/SM_Else"}, {"kind", "static"}}}),
                         PrefScope::Workspace);
    adoptWorkspaceMappings();
    list = mappings();
    REQUIRE(list.size() == 1);
    CHECK(list[0]["contentPath"] == "/Game/M/SM_Body");
    PrefStore::get().set("unreal", "/autoSync", false, PrefScope::Workspace);
    onSceneSaved();
    json left = PrefStore::get().value("unreal", "/mappings", json::array());
    REQUIRE(left.size() == 1);
    CHECK(left[0]["node"] == "|elsewhere");

    g.deleteNode(group->id());
    CHECK(mappings().empty());
    g.deleteNode(joint->id());
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("auto-sync sends the mapping's material options and the materials its meshes use")
{
    setup();
    fs::path ws = tempDir("materials");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    PrefStore::get().set("unreal", "/cacheDir", (ws / "cache").generic_string(), PrefScope::Workspace);

    scene::Graph& g = scene::Graph::current();
    scene::Node* hull = scene::materials::create(g, "M_Hull");
    scene::Node* glass = scene::materials::create(g, "M_Glass");
    REQUIRE(hull);
    REQUIRE(glass);
    g.setValue({hull->id(), scene::material::kBaseColor, -1}, glm::dvec3(0.8, 0.1, 0.1));
    g.setValue({hull->id(), scene::material::kBaseColorMap, -1}, (ws / "hull_bc.png").generic_string());
    g.setValue({glass->id(), scene::material::kRoughness, -1}, 0.1);

    auto addMesh = [&](const char* name, scene::NodeId parent, auto&& assign) {
        geo::Mesh m = geo::makeCube();
        assign(m);
        scene::Node* n = g.createNode(*scene::types::find("mesh"), name, parent);
        g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        return n;
    };
    // Two named materials plus unassigned faces on one mesh, a child with only unassigned faces.
    scene::Node* ship = addMesh("ship", scene::kNoNode, [](geo::Mesh& m) {
        scene::materials::assign(m, "M_Hull", {0, 1, 2});
        scene::materials::assign(m, "M_Glass", {3});
    });
    scene::Node* plain = addMesh("plain", ship->id(), [](geo::Mesh&) {});
    // An unmapped mesh's materials stay out of the request.
    scene::Node* other = addMesh("other", scene::kNoNode, [](geo::Mesh& m) { scene::materials::assign(m, "M_Other", {}); });
    std::string shipName = ship->name();

    json mats = materialParams(g, {ship->id()});
    REQUIRE(mats.size() == 2);
    CHECK(mats[0]["name"] == "M_Hull");
    CHECK(mats[0]["baseColor"][0].get<double>() == doctest::Approx(0.8));
    CHECK(fs::path(mats[0]["baseColorMap"].get<std::string>()) == fs::path((ws / "hull_bc.png").generic_string()));
    CHECK(mats[0]["blendMode"] == "opaque");
    CHECK(mats[1]["name"] == "M_Glass");
    CHECK(mats[1]["roughness"].get<double>() == doctest::Approx(0.1));
    CHECK_FALSE(mats[1].contains("baseColorMap"));
    CHECK(materialParams(g, {plain->id()}).empty());

    ops::execute("unrealMap", {{"args", {shipName}},
                                {"contentPath", "/Game/Ships/SM_Ship"},
                                {"materialFolder", "/Game/Ships/Mats"},
                                {"keepMaterials", false},
                                {"importMaterials", true}});
    registerExporter("fbx", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    FakeServer server(editor([](const json& req) -> std::optional<json> {
        if (req["method"] == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        return std::nullopt;
    }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    CHECK(syncMappings(true) == 1);
    Link::get().waitIdle();
    mainq::drain();

    json req;
    for (const json& r : server.requests())
        if (r["method"] == "importAsset")
            req = r;
    REQUIRE(req.is_object());
    const json& p = req["params"];
    CHECK(p["options"]["materialFolder"] == "/Game/Ships/Mats");
    CHECK(p["options"]["keepMaterials"] == false);
    CHECK(p["options"]["importMaterials"] == true);
    REQUIRE(p["materials"].size() == 2);
    CHECK(p["materials"][0]["name"] == "M_Hull");
    CHECK(p["materials"][1]["name"] == "M_Glass");

    // A mapping without the options leaves them to the plugin's defaults; a mesh with no assignments still sends an
    // empty list so the plugin manages its slots, while a plain file import leaves materials to Interchange.
    ops::execute("unrealMap", {{"args", {shipName}}, {"contentPath", "/Game/Ships/SM_Ship"}});
    json params = importParams(ws / "x.fbx", mappings()[0], materialParams(g, {plain->id()}));
    CHECK_FALSE(params["options"].contains("materialFolder"));
    CHECK_FALSE(params["options"].contains("keepMaterials"));
    CHECK(params["materials"] == json::array());
    CHECK_FALSE(importParams(ws / "x.fbx", mappings()[0]).contains("materials"));

    unregisterExporter("fbx");
    Link::get().disconnect();
    ops::execute("unrealUnmap", {{"args", {shipName}}});
    for (scene::Node* n : {other, plain, ship, glass, hull})
        g.deleteNode(n->id());
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("a server that takes the request and never answers fails on the idle timeout")
{
    setup();
    FakeServer server([](const json&) -> std::optional<json> { return std::nullopt; });
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));

    auto t0 = std::chrono::steady_clock::now();
    RpcResult r = client.call("importAsset", json::object(), CallOptions{.timeoutMs = 60000, .idleTimeoutMs = 200});
    auto took = std::chrono::steady_clock::now() - t0;
    CHECK_FALSE(r.ok);
    CHECK(r.code == RpcResult::kTimeout);
    CHECK(r.error.find("stopped responding to importAsset") != std::string::npos);
    CHECK(took >= std::chrono::milliseconds(150));
    CHECK(took < std::chrono::milliseconds(3000));
    CHECK(client.connected());
}

TEST_CASE("progress keeps a slow call alive and reaches onProgress on the calling thread")
{
    setup();
    FakeServer* self = nullptr;
    FakeServer server([&](const json& req) -> std::optional<json> {
        // Longer in total than the idle timeout, but never quiet for that long.
        for (int i = 0; i < 6; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            self->send(FakeServer::progress(req, "importing", 0.06 * (i + 1)));
        }
        // Progress for a request nobody made is ignored.
        self->send(FakeServer::progress({{"id", 9999}}, "importing", 1.0));
        return FakeServer::reply(req, "done");
    });
    self = &server;
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));

    std::vector<json> seen;
    bool sameThread = true;
    auto caller = std::this_thread::get_id();
    CallOptions opts{.timeoutMs = 5000, .idleTimeoutMs = 200};
    opts.onProgress = [&](const json& p) {
        sameThread = sameThread && std::this_thread::get_id() == caller;
        seen.push_back(p);
    };
    RpcResult r = client.call("importAsset", json::object(), opts);
    INFO("code " << r.code << ": " << r.error);
    CHECK(r.ok);
    CHECK(r.result == "done");
    REQUIRE(seen.size() == 6);
    CHECK(seen.back()["stage"] == "importing");
    CHECK(sameThread);
}

TEST_CASE("progress does not extend the overall timeout")
{
    setup();
    std::atomic<bool> stop = false;
    FakeServer* self = nullptr;
    FakeServer server([&](const json& req) -> std::optional<json> {
        for (int i = 0; i < 40 && !stop; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            self->send(FakeServer::progress(req, "importing", 0.0));
        }
        return std::nullopt;
    });
    self = &server;
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));

    RpcResult r = client.call("importAsset", json::object(), CallOptions{.timeoutMs = 300, .idleTimeoutMs = 200});
    stop = true;
    CHECK_FALSE(r.ok);
    CHECK(r.code == RpcResult::kTimeout);
    CHECK(r.error.find("importAsset timed out") != std::string::npos);
}

TEST_CASE("callOnMain posts progress and the result to the main thread")
{
    setup();
    FakeServer* self = nullptr;
    FakeServer server([&](const json& req) -> std::optional<json> {
        self->send(FakeServer::progress(req, "importing", 0.1));
        return FakeServer::reply(req, 1);
    });
    self = &server;
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));

    auto mainThread = std::this_thread::get_id();
    int progressCalls = 0;
    bool done = false, onMain = true;
    CallOptions opts{.timeoutMs = 3000, .idleTimeoutMs = 1000};
    opts.onProgress = [&](const json&) {
        onMain = onMain && std::this_thread::get_id() == mainThread;
        ++progressCalls;
    };
    client.callOnMain("importAsset", json::object(), opts, [&](RpcResult r) {
        onMain = onMain && std::this_thread::get_id() == mainThread;
        done = r.ok;
    });
    REQUIRE(waitUntil([&] { return done; }));
    CHECK(progressCalls == 1);
    CHECK(onMain);
}

TEST_CASE("auto-sync against an editor that never answers fails with a clear error instead of hanging")
{
    setup();
    fs::path ws = tempDir("hang");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    PrefStore::get().set("unreal", "/cacheDir", (ws / "cache").generic_string(), PrefScope::Workspace);
    PrefStore::get().set("unreal", "/importIdleTimeoutMs", 300, PrefScope::Workspace);

    scene::Graph& g = scene::Graph::current();
    scene::Node* cube = g.createNode(*scene::types::find("mesh"), "cube");
    REQUIRE(cube);
    std::string name = cube->name();
    ops::execute("unrealMap", {{"args", {name}}, {"contentPath", "/Game/Meshes/SM_Cube"}});
    registerExporter("fbx", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });

    // Answers hello, then swallows importAsset like an editor whose game thread is stuck.
    FakeServer server(editor([](const json&) -> std::optional<json> { return std::nullopt; }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    uint64_t before = log::since(0, 100000).empty() ? 0 : log::since(0, 100000).back().seq;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(ops::execute("unrealSync", json::object()) == 1);
    Link::get().waitIdle();
    mainq::drain();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));

    bool reported = false;
    for (const LogEntry& e : log::since(before))
        reported = reported || (e.level == LogLevel::Error && e.text.find("stopped responding to importAsset") != std::string::npos &&
                                e.text.find(name) != std::string::npos);
    CHECK(reported);
    CHECK(Link::get().connected());

    unregisterExporter("fbx");
    Link::get().disconnect();
    ops::execute("unrealUnmap", {{"args", {name}}});
    g.deleteNode(cube->id());
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("an object set maps as one asset: its members export together through fbx:set")
{
    setup();
    fs::path ws = tempDir("setmap");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    PrefStore::get().set("unreal", "/cacheDir", (ws / "cache").generic_string(), PrefScope::Workspace);

    ops::execute("newScene", json::object());
    std::string a = ops::execute("spaceLocator", {{"name", "rockA"}}).get<std::string>();
    std::string b = ops::execute("spaceLocator", {{"name", "rockB"}}).get<std::string>();
    std::string set = ops::execute("createSet", {{"args", {a, b}}, {"name", "Rocks"}}).get<std::string>();
    ops::execute("unrealMap", {{"args", {set}}, {"contentPath", "/Game/Props/SM_Rocks"}});

    std::mutex mx;
    std::vector<std::vector<scene::NodeId>> plain, together;
    registerExporter("fbx", [&](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        std::scoped_lock lock(mx);
        plain.push_back(nodes);
        return true;
    });
    registerExporter("fbx:set:static", [&](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        std::scoped_lock lock(mx);
        together.push_back(nodes);
        return true;
    });
    FakeServer server(editor([](const json& req) -> std::optional<json> {
        if (req["method"] == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        return std::nullopt;
    }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    CHECK(syncMappings(true) == 1);
    Link::get().waitIdle();
    mainq::drain();
    {
        std::scoped_lock lock(mx);
        CHECK(plain.empty());
        REQUIRE(together.size() == 1);
        scene::Graph& g = scene::Graph::current();
        CHECK(together[0] == std::vector<scene::NodeId>{g.find(a)->id(), g.find(b)->id()});
    }
    CHECK(fs::exists(ws / "cache" / "Rocks.fbx"));

    // An empty set has nothing to send.
    ops::execute("removeFromSet", {{"args", {set, a, b}}});
    CHECK(syncMappings(true) == 0);

    unregisterExporter("fbx");
    unregisterExporter("fbx:set:static");
    Link::get().disconnect();
    ops::execute("unrealUnmap", {{"args", {set}}});
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("meshSequence frame names and the frames left over when the range shrinks")
{
    SequenceAssets a = sequenceAssets({{"node", "spike"}, {"contentPath", "/Game/FX/Spike/SM_Spike"}}, 3);
    CHECK(a.folder == "/Game/FX/Spike");
    CHECK(a.base == "SM_Spike");
    CHECK(a.contentPaths == std::vector<std::string>{"/Game/FX/Spike/SM_Spike_000", "/Game/FX/Spike/SM_Spike_001", "/Game/FX/Spike/SM_Spike_002"});
    CHECK(a.objectPaths[1] == "/Game/FX/Spike/SM_Spike_001.SM_Spike_001");
    SequenceAssets b = sequenceAssets({{"node", "spike"}, {"contentPath", "/Game/FX/"}, {"sequence", {{"padding", 2}}}}, 1);
    CHECK(b.contentPaths == std::vector<std::string>{"/Game/FX/spike_00"});

    json listed = json::array();
    for (const char* n : {"SM_Spike_000", "SM_Spike_002", "SM_Spike_003", "SM_Spike_0010", "SM_Spike_Old", "SM_Spikey_009", "SM_Spike_004_LOD"})
        listed.push_back({{"path", std::string("/Game/FX/Spike/") + n + "." + n}, {"class", "StaticMesh"}});
    CHECK(surplusFrames(listed, "SM_Spike", 3) ==
          std::vector<std::string>{"/Game/FX/Spike/SM_Spike_003.SM_Spike_003", "/Game/FX/Spike/SM_Spike_0010.SM_Spike_0010"});
    CHECK(surplusFrames(listed, "SM_Spike", 11).empty());
}

TEST_CASE("a meshSequence mapping imports every frame, feeds the Niagara renderer and drops surplus frames")
{
    setup();
    fs::path ws = tempDir("meshseq");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    PrefStore::get().set("unreal", "/cacheDir", (ws / "cache").generic_string(), PrefScope::Workspace);

    ops::execute("newScene", json::object());
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "spike");
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
    const std::string spike = n->name();

    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {spike}}, {"contentPath", "/Game/FX/SM_Spike"}, {"kind", "meshSequence"}, {"sequence", 3}}),
                    CommandError);
    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {spike}}, {"contentPath", "/Game/FX/SM_Spike"}, {"kind", "flipbook"}}), CommandError);
    json m = ops::execute("unrealMap", {{"args", {spike}},
                                        {"contentPath", "/Game/FX/SM_Spike"},
                                        {"kind", "meshSequence"},
                                        {"sequence", {{"start", 32}, {"end", 34}, {"scale", 0.1}}},
                                        {"niagaraSystem", "/Game/FX/NS_Spike"}});
    CHECK(m["sequence"]["end"] == 34);
    CHECK(m["niagaraSystem"] == "/Game/FX/NS_Spike");

    FakeServer server(editor([](const json& req) -> std::optional<json> {
        const std::string method = req["method"];
        if (method == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        if (method == "setNiagaraMeshes")
            return FakeServer::reply(req, {{"renderers", 1}, {"updated", {"Emitter/MeshRenderer"}}, {"errors", json::array()}});
        if (method == "listAssets")
        {
            json out = json::array();
            for (const char* a : {"SM_Spike_000", "SM_Spike_001", "SM_Spike_002", "SM_Spike_003", "SM_Spike_004"})
                out.push_back({{"path", std::string("/Game/FX/") + a + "." + a}, {"class", "StaticMesh"}});
            return FakeServer::reply(req, out);
        }
        if (method == "deleteAssets")
            return FakeServer::reply(req, {{"deleted", req["params"]["paths"].size()}, {"missing", json::array()}});
        return std::nullopt;
    }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    // No FBX exporter is needed: the sequence writes its own frames.
    unregisterExporter("fbx");
    CHECK(syncMappings(true) == 1);
    Link::get().waitIdle();
    mainq::drain();

    std::vector<json> imports, niagara, deletes;
    for (const json& r : server.requests())
    {
        if (r["method"] == "importAsset")
            imports.push_back(r["params"]);
        else if (r["method"] == "setNiagaraMeshes")
            niagara.push_back(r["params"]);
        else if (r["method"] == "deleteAssets")
            deletes.push_back(r["params"]);
    }
    REQUIRE(imports.size() == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK(imports[i]["kind"] == "static");
        CHECK(imports[i]["contentPath"] == "/Game/FX/SM_Spike_00" + std::to_string(i));
        CHECK(fs::exists(imports[i]["file"].get<std::string>()));
        CHECK(imports[i]["file"].get<std::string>().ends_with(".fbx"));
    }
    REQUIRE(niagara.size() == 1);
    CHECK(niagara[0]["system"] == "/Game/FX/NS_Spike");
    CHECK(niagara[0]["meshes"] == json{"/Game/FX/SM_Spike_000.SM_Spike_000", "/Game/FX/SM_Spike_001.SM_Spike_001", "/Game/FX/SM_Spike_002.SM_Spike_002"});
    REQUIRE(deletes.size() == 1);
    CHECK(deletes[0]["paths"] == json{"/Game/FX/SM_Spike_003.SM_Spike_003", "/Game/FX/SM_Spike_004.SM_Spike_004"});

    // geometryCache sends one .abc of the same range.
    ops::execute("unrealMap", {{"args", {spike}}, {"contentPath", "/Game/FX/GC_Spike"}, {"kind", "geometryCache"}, {"sequence", {{"start", 1}, {"end", 4}}}});
    size_t before = server.requests().size();
    CHECK(syncMappings(true) == 1);
    Link::get().waitIdle();
    mainq::drain();
    std::vector<json> after = server.requests();
    REQUIRE(after.size() > before);
    json cacheReq;
    for (size_t i = before; i < after.size(); ++i)
        if (after[i]["method"] == "importAsset")
            cacheReq = after[i]["params"];
    REQUIRE(cacheReq.is_object());
    CHECK(cacheReq["kind"] == "geometryCache");
    CHECK(cacheReq["contentPath"] == "/Game/FX/GC_Spike");
    CHECK_FALSE(cacheReq.contains("materials"));
    CHECK(cacheReq["file"].get<std::string>().ends_with(".abc"));
    CHECK(fs::exists(cacheReq["file"].get<std::string>()));

    Link::get().disconnect();
    ops::execute("unrealUnmap", {{"args", {spike}}});
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("a vertexAnimation mapping bakes the textures and sends importVertexAnimation")
{
    setup();
    fs::path ws = tempDir("vat");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    PrefStore::get().set("unreal", "/cacheDir", (ws / "cache").generic_string(), PrefScope::Workspace);

    ops::execute("newScene", json::object());
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "blob");
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
    const std::string blob = n->name();

    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {blob}}, {"contentPath", "/Game/FX/SM_Blob"}, {"kind", "vertexAnimation"}, {"vat", 3}}),
                    CommandError);
    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {blob}}, {"contentPath", "/Game/FX/SM_Blob"}, {"kind", "vertexAnimation"},
                                               {"vat", {{"format", "tga"}}}}),
                    CommandError);
    json m = ops::execute("unrealMap", {{"args", {blob}},
                                        {"contentPath", "/Game/FX/SM_Blob"},
                                        {"kind", "vertexAnimation"},
                                        {"sequence", {{"start", 1}, {"end", 4}}},
                                        {"vat", {{"format", "png16"}, {"materialParent", "/Game/FX/M_MyVat"}}}});
    CHECK(m["vat"]["format"] == "png16");

    FakeServer server(editor([](const json& req) -> std::optional<json> {
        if (req["method"] == "importVertexAnimation")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        return std::nullopt;
    }));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    unregisterExporter("fbx");
    CHECK(syncMappings(true) == 1);
    Link::get().waitIdle();
    mainq::drain();

    json req;
    for (const json& r : server.requests())
        if (r["method"] == "importVertexAnimation")
            req = r["params"];
    REQUIRE(req.is_object());
    CHECK(req["contentPath"] == "/Game/FX/SM_Blob");
    CHECK(req["materialParent"] == "/Game/FX/M_MyVat");
    for (const char* k : {"fbx", "positions", "normals", "metaFile"})
        CHECK(fs::exists(req[k].get<std::string>()));
    CHECK(req["positions"].get<std::string>().ends_with("_pos.png"));
    CHECK(req["meta"]["frameCount"] == 4);
    CHECK(req["meta"]["mode"] == "constant");
    CHECK(req["meta"]["vertexCount"] == 8);
    // The texture parameters name the assets the plugin makes beside the mesh.
    CHECK(req["meta"]["materialParams"]["PositionTexture"] == "/Game/FX/T_Blob_VATPos.T_Blob_VATPos");
    CHECK(req["meta"]["materialParams"]["NormalTexture"] == "/Game/FX/T_Blob_VATNrm.T_Blob_VATNrm");
    CHECK(req["meta"]["materialParams"]["HDR"] == 0);

    Link::get().disconnect();
    ops::execute("unrealUnmap", {{"args", {blob}}});
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}
