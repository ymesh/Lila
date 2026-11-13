#include "ops/Command.h"
#include "mcp/McpServer.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <atomic>
#include <future>
#include <map>
#include <optional>
#include <mutex>
#include <random>
#include <thread>

namespace lila::mcp
{
namespace
{
constexpr const char* kProtocolVersion = "2025-06-18";
constexpr auto kToolTimeout = std::chrono::seconds(120);

json rpcResult(const json& id, json result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcError(const json& id, int code, const std::string& msg)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}

std::string randomToken()
{
    std::random_device rd;
    std::uniform_int_distribution<int> d(0, 15);
    std::string s;
    for (int i = 0; i < 40; ++i)
        s += "0123456789abcdef"[d(rd)];
    return s;
}

json textContent(const std::string& text)
{
    return json::array({{{"type", "text"}, {"text", text}}});
}

/** Set while a streamed tools/call waits: sends a notifications/progress for the long operation on the main thread. */
thread_local std::function<void(const progress::Snapshot&)> tProgress;

/** The long operation the main thread is busy with, if any. */
std::optional<progress::Snapshot> mainOperation()
{
    for (const progress::Snapshot& s : progress::snapshot())
        if (s.mainThread && !s.silent)
            return s;
    return std::nullopt;
}
} // namespace

struct Server::Impl
{
    std::mutex toolsMutex; // tools, resources, prompts, instructions
    std::map<std::string, Tool> tools;
    std::map<std::string, std::string> toolOwners; // extension tools only
    std::map<std::string, Resource> resources;
    std::map<std::string, std::string> resourceOwners; // extension resources only
    std::map<std::string, Prompt> prompts;
    std::map<std::string, std::string> promptOwners; // extension prompts only
    std::string instructions = "Lila DCC. Use execute_python to drive the app with the lila package "
                               "(lila.ops.* mirrors every command). list_commands shows what exists.";
    std::unique_ptr<httplib::Server> http;
    std::thread thread;
    std::string token;
    int port = 0;
    /** False while the host is still starting: main-thread calls wait for it instead of running inside startup's event pumping. */
    std::atomic<bool> ready{true};

    json handle(const json& req)
    {
        json id = req.value("id", json(nullptr));
        std::string method = req.value("method", "");
        if (method.rfind("notifications/", 0) == 0)
            return nullptr;

        if (method == "initialize")
        {
            std::string version = req.value("/params/protocolVersion"_json_pointer, std::string(kProtocolVersion));
            std::scoped_lock lock(toolsMutex);
            return rpcResult(id, {
                {"protocolVersion", version},
                {"capabilities", {{"tools", json::object()}, {"resources", json::object()}, {"prompts", json::object()}}},
                {"serverInfo", {{"name", "lila"}, {"version", "0.1.0"}}},
                {"instructions", instructions},
            });
        }
        if (method == "ping")
            return rpcResult(id, json::object());
        if (method == "tools/list")
        {
            json list = json::array();
            std::scoped_lock lock(toolsMutex);
            for (auto& [name, t] : tools)
                list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema}});
            return rpcResult(id, {{"tools", list}});
        }
        if (method == "tools/call")
        {
            std::string name = req.value("/params/name"_json_pointer, std::string());
            json args = req.value("/params/arguments"_json_pointer, json::object());
            if (args.is_null())
                args = json::object();
            Tool tool;
            {
                std::scoped_lock lock(toolsMutex);
                auto it = tools.find(name);
                if (it == tools.end())
                    return rpcError(id, -32602, "unknown tool " + name);
                tool = it->second;
            }
            auto [content, isError] = onMain([tool, args] {
                ops::AgentScope agent;
                return tool.runContent ? tool.runContent(args) : textContent(tool.run(args));
            });
            return rpcResult(id, {{"content", content}, {"isError", isError}});
        }
        if (method == "resources/list")
        {
            json list = json::array();
            std::scoped_lock lock(toolsMutex);
            for (auto& [uri, r] : resources)
                list.push_back({{"uri", r.uri}, {"name", r.name}, {"description", r.description}, {"mimeType", r.mimeType}});
            return rpcResult(id, {{"resources", list}});
        }
        if (method == "resources/templates/list")
            return rpcResult(id, {{"resourceTemplates", json::array()}});
        if (method == "resources/read")
        {
            std::string uri = req.value("/params/uri"_json_pointer, std::string());
            Resource r;
            {
                std::scoped_lock lock(toolsMutex);
                auto it = resources.find(uri);
                if (it == resources.end())
                    return rpcError(id, -32002, "resource not found: " + uri);
                r = it->second;
            }
            auto [content, isError] = onMain([r] { return textContent(r.read()); });
            if (isError)
                return rpcError(id, -32603, content[0].value("text", "failed"));
            return rpcResult(id, {{"contents", json::array({{{"uri", r.uri}, {"mimeType", r.mimeType}, {"text", content[0]["text"]}}})}});
        }
        if (method == "prompts/list")
        {
            json list = json::array();
            std::scoped_lock lock(toolsMutex);
            for (auto& [name, p] : prompts)
                list.push_back({{"name", p.name}, {"description", p.description}, {"arguments", p.arguments}});
            return rpcResult(id, {{"prompts", list}});
        }
        if (method == "prompts/get")
        {
            std::string name = req.value("/params/name"_json_pointer, std::string());
            json args = req.value("/params/arguments"_json_pointer, json::object());
            if (args.is_null())
                args = json::object();
            Prompt p;
            {
                std::scoped_lock lock(toolsMutex);
                auto it = prompts.find(name);
                if (it == prompts.end())
                    return rpcError(id, -32602, "unknown prompt " + name);
                p = it->second;
            }
            auto [content, isError] = onMain([p, args] { return textContent(p.get(args)); });
            if (isError)
                return rpcError(id, -32603, content[0].value("text", "failed"));
            return rpcResult(id, {{"description", p.description},
                                  {"messages", json::array({{{"role", "user"}, {"content", content[0]}}})}});
        }
        return rpcError(id, -32601, "method not found: " + method);
    }

    /** Runs fn on the main thread and waits; an exception becomes an error text block. */
    std::pair<json, bool> onMain(std::function<json()> fn)
    {
        const auto startupDeadline = std::chrono::steady_clock::now() + kToolTimeout;
        while (!ready.load())
        {
            if (std::chrono::steady_clock::now() > startupDeadline)
                return {textContent("timed out waiting for Lila to finish starting"), true};
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        auto promise = std::make_shared<std::promise<std::pair<json, bool>>>();
        auto future = promise->get_future();
        mainq::post([fn, promise] {
            LILA_ZONE_N("mcp call");
            try
            {
                promise->set_value({fn(), false});
            }
            catch (const std::exception& e)
            {
                promise->set_value({textContent(e.what()), true});
            }
        });
        // A long operation reporting progress keeps the call alive past the timeout.
        auto deadline = std::chrono::steady_clock::now() + kToolTimeout;
        while (future.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready)
        {
            if (std::optional<progress::Snapshot> s = mainOperation())
            {
                deadline = std::chrono::steady_clock::now() + kToolTimeout;
                if (tProgress)
                    tProgress(*s);
            }
            if (std::chrono::steady_clock::now() > deadline)
                return {textContent("timed out waiting for the main thread (is a modal dialog open?)"), true};
        }
        return future.get();
    }

    void serve(httplib::Server& srv)
    {
        srv.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req))
            {
                res.status = 401;
                return;
            }
            json body = json::parse(req.body, nullptr, false);
            if (body.is_discarded())
            {
                res.set_content(rpcError(nullptr, -32700, "parse error").dump(), "application/json");
                return;
            }
            // A tools/call with a progress token, from a client that takes a stream, gets progress notifications.
            if (body.is_object() && body.value("method", "") == "tools/call" &&
                body.contains("/params/_meta/progressToken"_json_pointer) &&
                req.get_header_value("Accept").find("text/event-stream") != std::string::npos)
            {
                const json token = body["/params/_meta/progressToken"_json_pointer];
                res.set_chunked_content_provider("text/event-stream", [this, body, token](size_t, httplib::DataSink& sink) {
                    auto send = [&sink](const json& msg) {
                        const std::string event = "event: message\ndata: " + msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
                        return sink.write(event.data(), event.size());
                    };
                    double last = -1.0;
                    std::string lastMessage;
                    tProgress = [&](const progress::Snapshot& s) {
                        // Progress must only grow; an indeterminate stretch repeats the last value with a new message.
                        const double value = std::max(last, s.determinate ? s.fraction * 100.0 : 0.0);
                        const std::string message = s.status.empty() ? s.title : s.title + " " + s.status;
                        if (value == last && message == lastMessage)
                            return;
                        last = value;
                        lastMessage = message;
                        send({{"jsonrpc", "2.0"},
                              {"method", "notifications/progress"},
                              {"params", {{"progressToken", token}, {"progress", value}, {"total", 100}, {"message", message}}}});
                    };
                    json out = handle(body);
                    tProgress = nullptr;
                    send(out);
                    sink.done();
                    return true;
                });
                return;
            }
            json out;
            if (body.is_array())
            {
                out = json::array();
                for (const json& r : body)
                    if (json x = handle(r); !x.is_null())
                        out.push_back(std::move(x));
            }
            else
                out = handle(body);

            if (out.is_null() || (out.is_array() && out.empty()))
            {
                res.status = 202;
                return;
            }
            res.set_content(out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
        });
        srv.Get("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 405; });
        srv.Delete("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 405; });
    }

    bool authorized(const httplib::Request& req) const
    {
        if (token.empty())
            return true;
        std::string auth = req.get_header_value("Authorization");
        return auth == "Bearer " + token || req.get_header_value("x-lila-token") == token;
    }
};

Server::Server() : m(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

Server& Server::get()
{
    static Server s;
    return s;
}

void Server::addTool(Tool tool)
{
    std::scoped_lock lock(m->toolsMutex);
    std::string name = tool.name;
    m->toolOwners.erase(name);
    m->tools[name] = std::move(tool);
}

bool Server::addTool(ext::Context& ctx, Tool tool)
{
    std::string name = tool.name;
    {
        std::scoped_lock lock(m->toolsMutex);
        if (m->tools.contains(name))
        {
            log::error("extension {}: MCP tool '{}' is already registered", ctx.owner(), name);
            return false;
        }
        m->tools[name] = std::move(tool);
        m->toolOwners[name] = ctx.owner();
    }
    std::string owner = ctx.owner();
    ctx.track("mcpTool:" + name, [this, name, owner] {
        {
            std::scoped_lock lock(m->toolsMutex);
            auto it = m->toolOwners.find(name);
            if (it == m->toolOwners.end() || it->second != owner)
                return;
        }
        removeTool(name);
    });
    return true;
}

bool Server::removeTool(const std::string& name)
{
    std::scoped_lock lock(m->toolsMutex);
    m->toolOwners.erase(name);
    return m->tools.erase(name) > 0;
}

bool Server::hasTool(const std::string& name) const
{
    std::scoped_lock lock(m->toolsMutex);
    return m->tools.contains(name);
}

std::vector<Resource> Server::resources(std::string_view prefix) const
{
    std::scoped_lock lock(m->toolsMutex);
    std::vector<Resource> out;
    for (const auto& [uri, r] : m->resources)
        if (uri.starts_with(prefix))
            out.push_back(r);
    return out;
}

void Server::addResource(Resource resource)
{
    std::scoped_lock lock(m->toolsMutex);
    std::string uri = resource.uri;
    m->resourceOwners.erase(uri);
    m->resources[uri] = std::move(resource);
}

namespace
{
/** Adds an extension's entry to `entries`, owned by ctx: unloading removes it unless something else took the key since. */
template <class T>
bool addOwnedMcpEntry(ext::Context& ctx, std::mutex& mutex, std::map<std::string, T>& entries, std::map<std::string, std::string>& owners,
              const std::string& key, T entry, const char* what)
{
    {
        std::scoped_lock lock(mutex);
        if (entries.contains(key))
        {
            log::error("extension {}: MCP {} '{}' is already registered", ctx.owner(), what, key);
            return false;
        }
        entries[key] = std::move(entry);
        owners[key] = ctx.owner();
    }
    std::string owner = ctx.owner();
    ctx.track(std::string("mcp ") + what + ":" + key, [&mutex, &entries, &owners, key, owner] {
        std::scoped_lock lock(mutex);
        auto it = owners.find(key);
        if (it == owners.end() || it->second != owner)
            return;
        owners.erase(it);
        entries.erase(key);
    });
    return true;
}
} // namespace

bool Server::addResource(ext::Context& ctx, Resource resource)
{
    std::string uri = resource.uri;
    return addOwnedMcpEntry(ctx, m->toolsMutex, m->resources, m->resourceOwners, uri, std::move(resource), "resource");
}

void Server::addPrompt(Prompt prompt)
{
    std::scoped_lock lock(m->toolsMutex);
    std::string name = prompt.name;
    m->promptOwners.erase(name);
    m->prompts[name] = std::move(prompt);
}

bool Server::addPrompt(ext::Context& ctx, Prompt prompt)
{
    std::string name = prompt.name;
    return addOwnedMcpEntry(ctx, m->toolsMutex, m->prompts, m->promptOwners, name, std::move(prompt), "prompt");
}

void Server::setInstructions(std::string text)
{
    std::scoped_lock lock(m->toolsMutex);
    m->instructions = std::move(text);
}

json Server::handle(const json& request)
{
    return m->handle(request);
}

bool Server::start(int port, const std::string& token)
{
    if (running())
        return true;
    m->token = token;
    m->http = std::make_unique<httplib::Server>();
    m->serve(*m->http);
#ifdef _WIN32
    // httplib's default SO_REUSEADDR lets a second Lila bind the same port and steal half the requests.
    m->http->set_socket_options([](socket_t sock) {
        int on = 1;
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    });
#endif
    if (!m->http->bind_to_port("127.0.0.1", port))
    {
        log::error("mcp: port {} is in use", port);
        m->http.reset();
        return false;
    }
    m->port = port;
    m->thread = std::thread([this] {
        LILA_THREAD_NAME("MCP");
        m->http->listen_after_bind();
    });

    json info = {{"url", url()}, {"token", token}};
    std::ofstream(paths::userConfigDir() / "mcp.json") << info.dump(2);
    log::info("mcp: listening on {}", url());
    return true;
}

void Server::stop()
{
    if (!m->http)
        return;
    m->http->stop();
    if (m->thread.joinable())
        m->thread.join();
    m->http.reset();
    log::info("mcp: stopped");
}

bool Server::running() const { return m->http != nullptr; }

void Server::holdUntilReady() { m->ready = false; }

void Server::setReady()
{
    if (!m->ready.exchange(true))
        log::info("mcp: ready");
}
int Server::port() const { return m->port; }
std::string Server::url() const { return "http://127.0.0.1:" + std::to_string(m->port) + "/mcp"; }
std::string Server::token() const { return m->token; }

void registerBuiltinTools();
void registerAgentResources();
void registerTextureTools();

void registerBuiltins()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerBuiltinTools();
    registerAgentResources();
    registerTextureTools();
}

std::string tokenFromPrefs()
{
    PrefStore& prefs = PrefStore::get();
    std::string token = prefs.value<std::string>("general", "/mcp/token", "");
    if (token.empty())
    {
        token = randomToken();
        prefs.set("general", "/mcp/token", token);
    }
    return token;
}

int portFromPrefs()
{
    int port = PrefStore::get().value<int>("general", "/mcp/port", 8740);
    // Lets a second instance (a test build) serve on its own port.
    if (const char* env = std::getenv("LILA_MCP_PORT"); env && std::atoi(env) > 0)
        port = std::atoi(env);
    return port;
}

void startFromPrefs()
{
    registerBuiltins();
    std::string token = tokenFromPrefs();
    if (PrefStore::get().value<bool>("general", "/mcp/enabled", false))
        Server::get().start(portFromPrefs(), token);
}
} // namespace lila::mcp
