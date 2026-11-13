#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::mcp
{
using json = nlohmann::json;

struct Tool
{
    std::string name;
    std::string description;
    json inputSchema;
    /** Runs on the main thread. Return text for the model; throw to report an error. */
    std::function<std::string(const json& args)> run;
    /** Instead of run: returns an MCP content array (text, image blocks). Runs on the main thread. */
    std::function<json(const json& args)> runContent;
};

/** A read-only document clients can list and fetch, such as an agent guide. */
struct Resource
{
    std::string uri; // "lila://guide/modeling"
    std::string name;
    std::string description;
    std::string mimeType = "text/markdown";
    /** Runs on the main thread. */
    std::function<std::string()> read;
};

/** A prompt template; clients show these as slash commands. */
struct Prompt
{
    std::string name;
    std::string description;
    json arguments = json::array(); // [{"name", "description", "required"}]
    /** Returns the prompt text for the given arguments. Runs on the main thread. */
    std::function<std::string(const json& args)> get;
};

/**
 * MCP over streamable HTTP (JSON responses, no SSE) on localhost. Requests arrive on the server thread and tool
 * calls hop to the main thread. Writes <userConfigDir>/mcp.json with url + token so clients can be pointed at it.
 */
class LILA_SCENE_API Server
{
public:
    static Server& get();

    /** Adds, or replaces the tool of that name. */
    void addTool(Tool tool);
    /** For extensions: owned by ctx and removed when it unloads. False, with a log message, if the name is taken. */
    bool addTool(ext::Context& ctx, Tool tool);
    bool removeTool(const std::string& name);
    bool hasTool(const std::string& name) const;
    void addResource(Resource resource);
    /** For extensions: owned by ctx and removed when it unloads. False, with a log message, if the uri is taken. */
    bool addResource(ext::Context& ctx, Resource resource);
    /** Copies of the resources whose uri starts with prefix, by uri. */
    std::vector<Resource> resources(std::string_view prefix = {}) const;
    void addPrompt(Prompt prompt);
    /** For extensions: owned by ctx and removed when it unloads. False, with a log message, if the name is taken. */
    bool addPrompt(ext::Context& ctx, Prompt prompt);
    /** Sent to clients on initialize; agents read it as standing instructions for the server. */
    void setInstructions(std::string text);

    bool start(int port, const std::string& token);
    void stop();
    bool running() const;
    int port() const;
    std::string url() const;
    std::string token() const;
    /**
     * Holds tool calls that run on the main thread until setReady: a host whose startup pumps events (opening a scene,
     * progress cards) calls this before starting the server, so a call never runs inside a half-finished startup.
     */
    void holdUntilReady();
    void setReady();

    /** One JSON-RPC request as the HTTP endpoint would answer it (null for notifications). Not on the main thread. */
    json handle(const json& request);

    ~Server();

private:
    Server();
    struct Impl;
    std::unique_ptr<Impl> m;
};

/** Registers the built-in tools and starts the server if prefs say so (general.json /mcp/enabled). */
LILA_SCENE_API void startFromPrefs();

/** Registers the built-in tools, resources and prompts without starting anything (batch mode, tests). Idempotent. */
LILA_SCENE_API void registerBuiltins();

/** The token from prefs, created on first use. */
LILA_SCENE_API std::string tokenFromPrefs();
LILA_SCENE_API int portFromPrefs();
} // namespace lila::mcp
