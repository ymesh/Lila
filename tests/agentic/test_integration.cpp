#include <doctest/doctest.h>

#include "mcp/AgentIntegration.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lila::mcp::agents;
using json = nlohmann::json;

namespace
{
std::string slurp(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("mergeMcpJson adds lila without touching other servers or writing the token")
{
    json existing = {{"mcpServers", {{"other", {{"type", "stdio"}, {"command", "x"}}}}}, {"extra", 1}};
    json merged = json::parse(mergeMcpJson(existing.dump(), 8740));
    CHECK(merged["extra"] == 1);
    CHECK(merged["mcpServers"]["other"]["command"] == "x");
    json lila = merged["mcpServers"]["lila"];
    CHECK(lila["type"] == "http");
    CHECK(lila["url"] == "${LILA_MCP_URL:-http://127.0.0.1:8740/mcp}");
    CHECK_FALSE(lila.contains("headers"));
    CHECK(lila["headersHelper"] == tokenHelperCommand());
    CHECK(tokenHelperCommand().find("mcp.json") != std::string::npos);
    CHECK(tokenHelperCommand().find("${") == std::string::npos);
    CHECK(json::parse(mergeMcpJson("", 9000))["mcpServers"]["lila"]["url"] == "${LILA_MCP_URL:-http://127.0.0.1:9000/mcp}");
    CHECK_THROWS(mergeMcpJson("[1, 2]", 8740));
}

TEST_CASE("upsertBlock appends once, replaces in place, and keeps user text")
{
    std::string user = "# My project\n\nRules here.\n";
    std::string once = upsertBlock(user, "Lila v1\n");
    CHECK(once.starts_with(user));
    CHECK(once.find("Lila v1") != std::string::npos);
    std::string twice = upsertBlock(once, "Lila v1\n");
    CHECK(twice == once);
    std::string tail = once + "\nMore user text.\n";
    std::string replaced = upsertBlock(tail, "Lila v2\n");
    CHECK(replaced.find("Lila v1") == std::string::npos);
    CHECK(replaced.find("Lila v2") != std::string::npos);
    CHECK(replaced.find("Rules here.") != std::string::npos);
    CHECK(replaced.find("More user text.") != std::string::npos);
    CHECK(upsertBlock("", "x").starts_with("<!-- lila:begin"));
}

TEST_CASE("setUp writes .mcp.json, the instruction blocks and the skill")
{
    auto root = std::filesystem::temp_directory_path() / "lila_agentic_ws";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "CLAUDE.md", std::ios::binary) << "# Mine\n";
    std::ofstream(root / ".mcp.json") << R"({"mcpServers": {"keep": {"type": "http", "url": "http://x"}}})";

    SetupResult r = setUp(root, {});
    CHECK(r.errors.empty());
    json mcp = json::parse(slurp(root / ".mcp.json"));
    CHECK(mcp["mcpServers"].contains("keep"));
    CHECK(mcp["mcpServers"].contains("lila"));
    std::string claude = slurp(root / "CLAUDE.md");
    CHECK(claude.starts_with("# Mine\n"));
    CHECK(claude.find("lila://guide/modeling") != std::string::npos);
    CHECK(std::filesystem::exists(root / "AGENTS.md"));
    CHECK(slurp(root / ".claude" / "skills" / "lila-modeling" / "SKILL.md").find("name: lila-modeling") != std::string::npos);

    // Running it again changes nothing in the markdown.
    setUp(root, {});
    CHECK(slurp(root / "CLAUDE.md") == claude);
    std::filesystem::remove_all(root);
}

TEST_CASE("setupRoots writes to the game project around a workspace")
{
    namespace fs = std::filesystem;
    auto base = fs::weakly_canonical(fs::temp_directory_path()) / "lila_agentic_roots";
    fs::remove_all(base);

    // The nearest .uproject or .git above the workspace.
    fs::create_directories(base / "Repo" / ".git");
    fs::create_directories(base / "Repo" / "Game" / "Lila");
    std::ofstream(base / "Repo" / "Game" / "Game.uproject") << "{}";
    CHECK(setupRoot(base / "Repo" / "Game" / "Lila") == base / "Repo" / "Game");
    fs::create_directories(base / "Repo" / "Art");
    CHECK(setupRoot(base / "Repo" / "Art") == base / "Repo");

    // The workspace is the project.
    fs::create_directories(base / "Loose");
    std::ofstream(base / "Loose" / "Loose.uproject") << "{}";
    CHECK(setupRoot(base / "Loose") == base / "Loose");

    // Every shipped skill is installed.
    setUp(base / "Loose", {});
    for (const char* skill : {"lila-modeling", "lila-geometry-nodes", "lila-materials", "lila-terrain", "lila-set-dressing"})
        CHECK(fs::exists(base / "Loose" / ".claude" / "skills" / skill / "SKILL.md"));
    fs::remove_all(base);
}

TEST_CASE("shell variables point agents at the server")
{
    bool url = false, token = false;
    for (auto& [k, v] : shellVariables())
    {
        url |= k == "LILA_MCP_URL" && v.starts_with("http://127.0.0.1:");
        token |= k == "LILA_MCP_TOKEN" && !v.empty();
    }
    CHECK(url);
    CHECK(token);
}
