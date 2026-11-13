#include "mcp/AgentIntegration.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "mcp/McpServer.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace lila::mcp::agents
{
namespace
{
using json = nlohmann::json;

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAll(const fs::path& p, const std::string& text, std::string* err)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        *err = p.string() + ": cannot write (read-only or checked in? check it out and run Set Up Agent Integration again)";
        return false;
    }
    out << text;
    return bool(out);
}

std::string utf8(const fs::path& p)
{
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

std::string mergeMcpJson(const std::string& existing, int port)
{
    json doc = existing.empty() ? json::object() : json::parse(existing, nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
        throw std::runtime_error(".mcp.json is not a JSON object; fix or remove it first");
    if (!doc.contains("mcpServers") || !doc["mcpServers"].is_object())
        doc["mcpServers"] = json::object();
    doc["mcpServers"]["lila"] = {
        {"type", "http"},
        {"url", "${LILA_MCP_URL:-http://127.0.0.1:" + std::to_string(port) + "/mcp}"},
        {"headersHelper", tokenHelperCommand()},
    };
    return doc.dump(2) + "\n";
}

std::string tokenHelperCommand()
{
    // Claude Code runs it through the platform shell (cmd.exe on Windows) and expands ${...} in .mcp.json, so the PowerShell
    // form has no double quotes inside and the sh form no ${...}.
#ifdef _WIN32
    return "powershell -NoProfile -NonInteractive -Command \"$d=$env:LILA_CONFIG_DIR; if (!$d) { $d=Join-Path $env:APPDATA 'Lila' }; "
           "$t=(Get-Content -Raw (Join-Path $d 'mcp.json') | ConvertFrom-Json).token; @{Authorization='Bearer '+$t} | "
           "ConvertTo-Json -Compress\"";
#else
#ifdef __APPLE__
    const char* fallback = "$HOME/Library/Application Support/Lila";
#else
    const char* fallback = "$HOME/.config/Lila";
#endif
    return std::string("d=\"$LILA_CONFIG_DIR\"; [ -n \"$d\" ] || d=\"") + fallback +
           "\"; printf '{\"Authorization\":\"Bearer %s\"}' \"$(sed -n 's/.*\"token\": *\"\\([^\"]*\\)\".*/\\1/p' \"$d/mcp.json\")\"";
#endif
}

fs::path setupRoot(const fs::path& workspace)
{
    std::error_code ec;
    fs::path ws = fs::weakly_canonical(workspace, ec);
    if (ec)
        ws = workspace;
    auto isProject = [](const fs::path& dir) {
        std::error_code e;
        if (fs::exists(dir / ".git", e))
            return true;
        for (const auto& entry : fs::directory_iterator(dir, e))
            if (entry.path().extension() == ".uproject")
                return true;
        return false;
    };
    for (fs::path p = ws;; p = p.parent_path())
    {
        if (isProject(p))
            return p;
        if (p == p.parent_path())
            break;
    }
    return ws;
}

std::string upsertBlock(const std::string& existing, const std::string& block)
{
    std::string wrapped = std::string(kBlockBegin) + "\n" + block + (block.ends_with('\n') ? "" : "\n") + kBlockEnd + "\n";
    size_t b = existing.find(kBlockBegin);
    // Any earlier version of the begin marker counts, so its wording can change without leaving duplicates.
    if (b == std::string::npos)
        b = existing.find("<!-- lila:begin");
    size_t e = b == std::string::npos ? std::string::npos : existing.find(kBlockEnd, b);
    if (b != std::string::npos && e != std::string::npos)
    {
        size_t end = e + std::string(kBlockEnd).size();
        if (end < existing.size() && existing[end] == '\r')
            ++end;
        if (end < existing.size() && existing[end] == '\n')
            ++end;
        return existing.substr(0, b) + wrapped + existing.substr(end);
    }
    if (existing.empty())
        return wrapped;
    std::string sep = existing.ends_with("\n\n") ? "" : existing.ends_with('\n') ? "\n" : "\n\n";
    return existing + sep + wrapped;
}

SetupResult setUp(const fs::path& root, const SetupOptions& o)
{
    SetupResult r;
    fs::path agents = paths::resourcesDir() / "agents";
    std::string err;
    if (o.mcpJson)
    {
        fs::path p = root / ".mcp.json";
        try
        {
            if (writeAll(p, mergeMcpJson(fs::exists(p) ? readAll(p) : std::string(), o.port), &err))
                r.written.push_back(utf8(p));
            else
                r.errors.push_back(err);
        }
        catch (const std::exception& e)
        {
            r.errors.push_back(utf8(p) + ": " + e.what());
        }
    }
    if (o.instructions)
    {
        std::string block = readAll(agents / "workspace-block.md");
        if (block.empty())
            r.errors.push_back("missing resources/agents/workspace-block.md");
        else
            for (const char* name : {"CLAUDE.md", "AGENTS.md"})
            {
                fs::path p = root / name;
                std::string before = fs::exists(p) ? readAll(p) : std::string();
                std::string after = upsertBlock(before, block);
                if (after == before)
                    continue;
                if (writeAll(p, after, &err))
                    r.written.push_back(utf8(p));
                else
                    r.errors.push_back(err);
            }
    }
    if (o.skill)
    {
        std::error_code dirEc;
        std::vector<fs::path> skills;
        for (const auto& entry : fs::directory_iterator(agents / "skills", dirEc))
            if (entry.is_directory())
                skills.push_back(entry.path());
        if (skills.empty())
            r.errors.push_back("missing " + utf8(agents / "skills"));
        for (const fs::path& src : skills)
        {
            fs::path dst = root / ".claude" / "skills" / src.filename();
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(src, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                fs::path to = dst / fs::relative(entry.path(), src);
                if (writeAll(to, readAll(entry.path()), &err))
                    r.written.push_back(utf8(to));
                else
                    r.errors.push_back(err);
            }
            if (ec)
                r.errors.push_back("missing " + utf8(src));
        }
    }
    return r;
}

std::vector<std::pair<std::string, std::string>> shellVariables()
{
    std::vector<std::pair<std::string, std::string>> out{{"LILA", "1"}, {"LILA_BIN", utf8(paths::executableDir().make_preferred())}};
    Server& s = Server::get();
    int port = s.running() ? s.port() : portFromPrefs();
    out.push_back({"LILA_MCP_URL", "http://127.0.0.1:" + std::to_string(port) + "/mcp"});
    out.push_back({"LILA_MCP_TOKEN", s.running() ? s.token() : tokenFromPrefs()});
    if (Workspace::get().isOpen())
    {
        fs::path root = Workspace::get().root();
        out.push_back({"LILA_WORKSPACE", utf8(root.make_preferred())});
    }
    return out;
}

std::string workspaceState(const fs::path& root)
{
    json all = PrefStore::get().value("general", "/agents/workspaces", json::object());
    std::string key = utf8(fs::weakly_canonical(root));
    return all.is_object() && all.contains(key) && all[key].is_string() ? all[key].get<std::string>() : std::string();
}

void setWorkspaceState(const fs::path& root, const std::string& state)
{
    json all = PrefStore::get().value("general", "/agents/workspaces", json::object());
    if (!all.is_object())
        all = json::object();
    all[utf8(fs::weakly_canonical(root))] = state;
    PrefStore::get().set("general", "/agents/workspaces", all);
}
} // namespace lila::mcp::agents
