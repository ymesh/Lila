#include "TexTest.h"

#include "core/MainQueue.h"
#include "link/TextureSetSend.h"
#include "mcp/McpServer.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/Library.h"
#include "tex/TextureSet.h"

#include <chrono>
#include <filesystem>
#include <future>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** A layered set: a grey base fill under a red half-covered layer. */
void buildLayered()
{
    run("newScene");
    run("texCreateSet", {{"args", {"Crate"}}, {"resolution", 32}});
    run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Base"}, {"material", {{"baseColor", "#808080"}, {"roughness", 0.6}}}});
    const std::string red = run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Red"}, {"material", {{"baseColor", "#FF0000"}, {"roughness", 0.2}}}})["id"];
    run("texAddMask", {{"args", {"Crate", red, "fill"}}, {"value", 0.5}});
}

json callTool(const std::string& name, const json& args)
{
    // Tool calls hop to the main thread: ask from a worker while this thread drains the queue.
    const json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", args}}}};
    auto fut = std::async(std::launch::async, [request] { return mcp::Server::get().handle(request); });
    while (fut.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        mainq::drain();
    const json reply = fut.get();
    REQUIRE(reply.contains("result"));
    return reply["result"];
}

std::string toolText(const json& result)
{
    for (const json& c : result.value("content", json::array()))
        if (c.value("type", std::string()) == "text")
            return c.value("text", std::string());
    return {};
}
} // namespace

TEST_CASE("tex pipeline: incremental export skips unchanged channels")
{
    buildLayered();
    const fs::path dir = tempDir("incremental");
    json first = run("texExport", {{"args", {"Crate"}}, {"folder", dir.string()}, {"incremental", true}, {"backend", "cpu"}});
    REQUIRE(first["files"].size() >= 2);
    CHECK(first["written"].size() == first["files"].size());
    for (const json& o : first["outputs"])
        CHECK(o["changed"] == true);

    json again = run("texExport", {{"args", {"Crate"}}, {"folder", dir.string()}, {"incremental", true}, {"backend", "cpu"}});
    CHECK(again["written"].empty());
    CHECK(again["unchanged"].size() == first["files"].size());

    // Roughness feeds ORM only: base colour and normal stay as they are.
    const std::string red = run("texLayerStack", {{"args", {"Crate"}}})["layers"][1]["id"];
    run("texSetLayer", {{"args", {"Crate", red, {{"material", {{"roughness", 0.9}}}}}}});
    json third = run("texExport", {{"args", {"Crate"}}, {"folder", dir.string()}, {"incremental", true}, {"backend", "cpu"}});
    REQUIRE(third["written"].size() == 1);
    CHECK(third["written"][0].get<std::string>().find("ORM") != std::string::npos);

    // What goes to Unreal follows: only the changed texture, with the slot's instance.
    const json params = link::textureSetParams(third, {{"name", "Crate"}}, {{"folder", "/Game/T"}, {"materialParent", "/Game/M_Base"}}, false);
    REQUIRE(params["textures"].size() == 1);
    CHECK(params["materialMode"] == "instance");
    CHECK(params["materials"][0]["parameters"].contains("ORMMap"));
    const json painterly = link::textureSetParams(first, {{"name", "Crate"}, {"shadingModel", "painterly"}}, json::object(), true);
    CHECK(painterly["materialParent"] == "lila:painterly");
    CHECK(painterly["materials"][0]["parameters"].size() == 1);
    CHECK(painterly["materials"][0]["scalars"]["UseBaseColorMap"] == 1.0);
}

TEST_CASE("tex pipeline: PSD export round trips through readPsdLayers and imports as layers")
{
    buildLayered();
    const fs::path dir = tempDir("psd");
    json r = run("texExportPsd", {{"args", {"Crate"}}, {"channels", {"baseColor"}}, {"folder", dir.string()}, {"backend", "cpu"}});
    REQUIRE(r["files"].size() == 1);
    std::string err;
    auto layers = image::readPsdLayers(fs::path(r["files"][0].get<std::string>()), &err);
    REQUIRE_MESSAGE(layers.size() == 3, err);
    // Stored bottom first: Base (the stack's base value), then the layers.
    CHECK(layers[0].first == "Base");
    CHECK(layers[1].first == "Base");
    CHECK(layers[2].first == "Red");
    const image::Image& top = layers[2].second;
    REQUIRE(top.width == 32);
    CHECK(texel(top, 5, 5)[0] == doctest::Approx(1.0).epsilon(0.01));
    CHECK(texel(top, 5, 5)[1] == doctest::Approx(0.0).epsilon(0.01));
    CHECK(texel(top, 5, 5)[3] == doctest::Approx(0.5).epsilon(0.02));

    json imported = run("texImportImage", {{"args", {"Crate", r["files"][0]}}, {"folder", (dir / "imported").string()}});
    CHECK(imported["layers"].size() == 3);
    const json stack = run("texLayerStack", {{"args", {"Crate"}}});
    CHECK(stack["layers"].size() == 5);
    CHECK(stack["layers"].back()["kind"] == "paint");

    const fs::path png = dir / "rough.png";
    json single = run("texExportImage", {{"args", {"Crate", "roughness", png.string()}}, {"backend", "cpu"}});
    CHECK(single["width"] == 32);
    CHECK(fs::exists(png));
}

TEST_CASE("tex pipeline: MCP texture tools")
{
    mcp::registerBuiltins();
    // material_lint runs materialLint, a library command the test main doesn't register.
    if (!ops::find("materialLint"))
        tex::library::registerCommands();
    buildLayered();

    json d = callTool("describe_texture_set", {{"set", "Crate"}});
    CHECK_FALSE(d.value("isError", false));
    const json desc = json::parse(toolText(d));
    CHECK(desc["set"] == "Crate");
    CHECK(desc["materials"].empty());
    CHECK(desc["mode"] == "layers");
    CHECK(desc["layers"].size() == 2);
    CHECK(desc["channels"].is_array());
    CHECK(desc["bindings"].contains("meshMaps"));
    CHECK(desc["bindings"].contains("masks"));
    CHECK(desc["paintLayers"].is_array());

    json l = callTool("material_lint", {{"material", "Crate"}, {"size", 32}, {"backend", "cpu"}});
    CHECK_FALSE(l.value("isError", false));
    const json lint = json::parse(toolText(l));
    CHECK(lint.contains("passed"));
    CHECK(lint["checks"].is_array());

    // Painting needs the renderer, which this test binary doesn't have: an error, not a crash.
    json p = callTool("paint_stroke", {{"set", "Crate"}, {"stroke", json::object()}});
    CHECK(p.value("isError", false));

    json missing = callTool("describe_texture_set", {{"set", "Nope"}});
    CHECK(missing.value("isError", false));
}
