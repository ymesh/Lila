#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace lila::mcp::agents
{
namespace fs = std::filesystem;

inline constexpr const char* kBlockBegin = "<!-- lila:begin (managed by Lila: Help > Set Up Agent Integration; edits inside are replaced) -->";
inline constexpr const char* kBlockEnd = "<!-- lila:end -->";

/**
 * The "lila" server entry merged into a workspace's .mcp.json. The URL comes from LILA_MCP_URL when Lila's terminal sets it;
 * the token is read at connect time by tokenHelperCommand() from Lila's mcp.json, so the file holds no secret, is safe under
 * source control and works from any terminal.
 */
LILA_SCENE_API std::string mergeMcpJson(const std::string& existing, int port);

/** Shell command for .mcp.json's headersHelper: prints {"Authorization": "Bearer <token>"} from <config dir>/mcp.json. */
LILA_SCENE_API std::string tokenHelperCommand();

/**
 * Where setup writes for a workspace: the project around it (the nearest folder up with .git or a .uproject, else the
 * workspace). Agents look for .mcp.json, CLAUDE.md and skills from their start folder upward, so one copy there serves
 * an agent started anywhere in the project, the workspace included.
 */
LILA_SCENE_API fs::path setupRoot(const fs::path& workspace);

/** existing with the delimited Lila block replaced by block, or appended when it has none. User text is never touched. */
LILA_SCENE_API std::string upsertBlock(const std::string& existing, const std::string& block);

struct SetupOptions
{
    bool mcpJson = true;      // <root>/.mcp.json
    bool instructions = true; // Lila block in <root>/CLAUDE.md and <root>/AGENTS.md
    bool skill = true;        // <root>/.claude/skills/lila-*/SKILL.md
    int port = 8740;
};

struct SetupResult
{
    std::vector<std::string> written;
    std::vector<std::string> errors; // read-only (checked-in) files and the like
};

/** Writes what the options ask for into the workspace root. Reads templates from resources/agents. */
LILA_SCENE_API SetupResult setUp(const fs::path& root, const SetupOptions& options);

/** Variables for shells started from Lila: LILA, LILA_MCP_URL, LILA_MCP_TOKEN, LILA_WORKSPACE, LILA_BIN. */
LILA_SCENE_API std::vector<std::pair<std::string, std::string>> shellVariables();

/** Per workspace answer to the setup prompt, in prefs: "" (never asked), "done" or "declined". */
LILA_SCENE_API std::string workspaceState(const fs::path& root);
LILA_SCENE_API void setWorkspaceState(const fs::path& root, const std::string& state);
} // namespace lila::mcp::agents
