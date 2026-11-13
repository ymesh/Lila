#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/ExtensionContext.h"
#include "core/MainQueue.h"
#include "mcp/McpServer.h"
#include "render/Renderer.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <set>
#include <thread>

using namespace lila;
using json = nlohmann::json;

namespace
{
constexpr int kPort = 18741;
constexpr const char* kToken = "test-token";

mcp::Server& server()
{
    mcp::Server& s = mcp::Server::get();
    if (!s.running())
        REQUIRE(s.start(kPort, kToken));
    return s;
}

struct Reply
{
    int status = 0;
    json body;
};

/** POSTs from a worker thread while this (main) thread drains the queue tool calls hop onto. */
Reply post(const json& body, const std::string& token = kToken)
{
    server();
    auto fut = std::async(std::launch::async, [body, token] {
        httplib::Client c("127.0.0.1", kPort);
        c.set_read_timeout(120, 0);
        httplib::Headers h;
        if (!token.empty())
            h.emplace("Authorization", "Bearer " + token);
        auto res = c.Post("/mcp", h, body.dump(), "application/json");
        Reply r;
        if (res)
        {
            r.status = res->status;
            r.body = json::parse(res->body, nullptr, false);
        }
        return r;
    });
    while (fut.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        mainq::drain();
    return fut.get();
}

json call(const std::string& method, const json& params = json::object())
{
    Reply r = post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}});
    REQUIRE(r.status == 200);
    return r.body;
}

std::string base64Decode(const std::string& in)
{
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : in)
    {
        size_t i = chars.find(c);
        if (i == std::string::npos)
            break;
        val = (val << 6) + int(i);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
} // namespace

TEST_CASE("mcp: auth, initialize with instructions, and every capability listed")
{
    CHECK(post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, "").status == 401);

    json init = call("initialize", {{"protocolVersion", "2025-06-18"}});
    json caps = init["result"]["capabilities"];
    CHECK(caps.contains("tools"));
    CHECK(caps.contains("resources"));
    CHECK(caps.contains("prompts"));
    std::string instructions = init["result"]["instructions"];
    CHECK(instructions.find("Blender") != std::string::npos);
    CHECK(instructions.find("lila://guide/modeling") != std::string::npos);

    std::set<std::string> tools;
    json listed = call("tools/list");
    for (const json& t : listed["result"]["tools"])
        tools.insert(t["name"].get<std::string>());
    for (const char* t : {"execute_python", "list_commands", "scene_summary", "mesh_info", "mesh_check", "viewport_capture", "capture_turntable", "read_guide"})
        CHECK_MESSAGE(tools.contains(t), t);
}

TEST_CASE("mcp: guides as resources, model_asset as a prompt")
{
    std::set<std::string> uris;
    json listed = call("resources/list");
    for (const json& r : listed["result"]["resources"])
        uris.insert(r["uri"].get<std::string>());
    CHECK(uris.contains("lila://guide/modeling"));
    CHECK(uris.contains("lila://guide/api"));
    CHECK(uris.contains("lila://examples/low-poly-dog"));

    json guide = call("resources/read", {{"uri", "lila://guide/modeling"}});
    std::string text = guide["result"]["contents"][0]["text"];
    CHECK(text.find("selectComponentsWhere") != std::string::npos);
    std::string api = call("resources/read", {{"uri", "lila://guide/api"}})["result"]["contents"][0]["text"];
    CHECK(api.find("findComponents") != std::string::npos); // the generated command index
    CHECK(call("resources/read", {{"uri", "lila://nope"}}).contains("error"));

    auto guideText = [](json args) {
        return call("tools/call", {{"name", "read_guide"}, {"arguments", args}})["result"]["content"][0]["text"].get<std::string>();
    };
    const std::string materials = guideText({{"topic", "materials"}});
    CHECK(materials.find("texBuildGraph") != std::string::npos);
    CHECK(materials.find("`materialBlend`") != std::string::npos); // the generated texture node catalogue
    CHECK(guideText({{"topic", "geometry-nodes"}}).find("gnBuildGroup") != std::string::npos);
    const std::string list = guideText(json::object());
    for (const char* g : {"modeling", "materials", "geometry-nodes", "settlement", "masonry", "foliage", "terrain"})
        CHECK_MESSAGE(list.find(std::string("- ") + g + ":") != std::string::npos, g);
    CHECK(guideText({{"topic", "nope"}}).starts_with("No guide 'nope'"));

    json prompts = call("prompts/list")["result"]["prompts"];
    REQUIRE(prompts.size() >= 2);
    std::set<std::string> names;
    for (const json& pr : prompts)
        names.insert(pr["name"].get<std::string>());
    CHECK(names.count("model_asset") == 1);
    CHECK(names.count("dress_scene") == 1);
    json p = call("prompts/get", {{"name", "model_asset"}, {"arguments", {{"subject", "a small cute dog"}}}});
    std::string msg = p["result"]["messages"][0]["content"]["text"];
    CHECK(msg.find("a small cute dog") != std::string::npos);
    CHECK(msg.find("{{") == std::string::npos);
}

TEST_CASE("mcp: an extension's resources and prompts leave with it")
{
    mcp::Server& s = mcp::Server::get();
    auto listed = [&s](const char* method, const char* key, const char* field, const std::string& value) {
        json r = s.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}})["result"][key];
        return std::any_of(r.begin(), r.end(), [&](const json& e) { return e[field] == value; });
    };
    {
        ext::Context ctx("mcpTestExtension");
        CHECK(s.addResource(ctx, {.uri = "lila://test/owned", .name = "Owned", .read = [] { return std::string("x"); }}));
        CHECK(s.addPrompt(ctx, {.name = "owned_prompt", .get = [](const json&) { return std::string("y"); }}));
        ext::Context other("otherExtension");
        CHECK_FALSE(s.addResource(other, {.uri = "lila://test/owned", .name = "Taken"}));
        CHECK_FALSE(s.addPrompt(other, {.name = "owned_prompt"}));
        CHECK(listed("resources/list", "resources", "uri", "lila://test/owned"));
        CHECK(listed("prompts/list", "prompts", "name", "owned_prompt"));
        ctx.unloadAll();
    }
    CHECK_FALSE(listed("resources/list", "resources", "uri", "lila://test/owned"));
    CHECK_FALSE(listed("prompts/list", "prompts", "name", "owned_prompt"));
}

TEST_CASE("mcp: scene_summary and mesh_info describe meshes")
{
    ops::execute("newScene", json::object());
    ops::execute("polyCube", {{"name", "Box"}, {"subdivisionsX", 2}});
    ops::execute("setSymmetry", {{"args", {"Box"}}, {"axis", "x"}});
    json summary = call("tools/call", {{"name", "scene_summary"}, {"arguments", json::object()}});
    CHECK(summary["result"]["isError"] == false);
    std::string text = summary["result"]["content"][0]["text"];
    CHECK(text.find("Box (mesh)") != std::string::npos);
    CHECK(text.find("10 faces") != std::string::npos);
    CHECK(text.find("symmetry x") != std::string::npos);

    json info = call("tools/call", {{"name", "mesh_info"}, {"arguments", {{"mesh", "Box"}}}});
    json stats = json::parse(info["result"]["content"][0]["text"].get<std::string>());
    CHECK(stats["closed"] == true);
    CHECK(stats["quads"] == 10);

    json check = call("tools/call", {{"name", "mesh_check"}, {"arguments", {{"mesh", "Box"}}}});
    std::string report = check["result"]["content"][0]["text"];
    CHECK(report.starts_with("Box: OK | 10 faces"));
    CHECK(report.find("1 shell |") != std::string::npos);
    ops::execute("polyCube", {{"name", "Bit"}, {"position", {0, 5, 0}}});
    ops::execute("polyCombine", {{"args", {"Box", "Bit"}}, {"name", "Two"}});
    report = call("tools/call", {{"name", "mesh_check"}, {"arguments", {{"mesh", "Two"}}}})["result"]["content"][0]["text"];
    CHECK(report.find("PROBLEMS") != std::string::npos);
    CHECK(report.find("shell 1: 6 faces") != std::string::npos);
    CHECK(report.find("ISSUE: 2 shells") != std::string::npos);

    json bad =call("tools/call", {{"name", "mesh_info"}, {"arguments", {{"mesh", "Nope"}}}});
    CHECK(bad["result"]["isError"] == true);
}

TEST_CASE("mcp: viewport_capture returns a PNG image block from the offscreen renderer")
{
    if (!render::Renderer::get().running() && !render::Renderer::get().start())
    {
        MESSAGE("no Vulkan device: offscreen capture not exercised");
        return;
    }
    ops::execute("newScene", json::object());
    ops::execute("polyCube", {{"name", "Box"}});
    json r = call("tools/call", {{"name", "viewport_capture"}, {"arguments", {{"view", "front"}, {"width", 64}, {"height", 48}}}});
    REQUIRE(r["result"]["isError"] == false);
    json img = r["result"]["content"][0];
    CHECK(img["type"] == "image");
    CHECK(img["mimeType"] == "image/png");
    std::string png = base64Decode(img["data"].get<std::string>());
    REQUIRE(png.size() > 24);
    CHECK(png.substr(1, 3) == "PNG");
    // IHDR width and height, big endian.
    auto be32 = [&](size_t at) { return (uint8_t(png[at]) << 24) | (uint8_t(png[at + 1]) << 16) | (uint8_t(png[at + 2]) << 8) | uint8_t(png[at + 3]); };
    CHECK(be32(16) == 64);
    CHECK(be32(20) == 48);

    json sheet = call("tools/call", {{"name", "capture_turntable"}, {"arguments", {{"count", 4}, {"width", 32}, {"height", 32}}}});
    REQUIRE(sheet["result"]["isError"] == false);
    std::string sheetPng = base64Decode(sheet["result"]["content"][0]["data"].get<std::string>());
    CHECK(be32(16) == 64); // the first image is untouched
    auto be32s = [&](size_t at) { return (uint8_t(sheetPng[at]) << 24) | (uint8_t(sheetPng[at + 1]) << 16) | (uint8_t(sheetPng[at + 2]) << 8) | uint8_t(sheetPng[at + 3]); };
    CHECK(be32s(16) == 128); // four 32px tiles in a row
    CHECK(be32s(20) == 32);
    std::string caption = sheet["result"]["content"][1]["text"];
    CHECK(caption.find("yaw 90") != std::string::npos);
}
