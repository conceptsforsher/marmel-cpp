// Rust origin: src/mcp/client.rs
#include "marmel/mcp.hpp"

#include "marmel/harness.hpp"
#include "marmel/mcp_http.hpp"
#include "marmel/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef __unix__
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace marmel::mcp {
namespace {

#ifdef __unix__
std::string read_line_timeout(int fd, std::chrono::seconds timeout) {
    std::string line;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char c = 0;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw std::runtime_error("mcp: response timeout (30s)");
        int ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                                 .count());
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int r = ::poll(&pfd, 1, std::min(ms, 200));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("mcp: poll failed");
        }
        if (r == 0) continue;
        ssize_t n = ::read(fd, &c, 1);
        if (n == 0) throw std::runtime_error("mcp: server closed the connection");
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("mcp: read failed");
        }
        if (c == '\n') break;
        if (c != '\r') line += c;
    }
    return line;
}
#endif

class StdioMcpConnection {
public:
    StdioMcpConnection(std::string server, pid_t pid, int wfd, int rfd)
        : server_(std::move(server)), pid_(pid), wfd_(wfd), rfd_(rfd) {}
    ~StdioMcpConnection() { kill(); }

    static std::unique_ptr<StdioMcpConnection> spawn(const std::string& server,
                                                    const McpServerConfig& cfg) {
        if (!cfg.command || cfg.command->empty())
            throw std::runtime_error("mcp: server '" + server + "' has no command");
#ifdef __unix__
        int to_child[2], from_child[2];
        if (::pipe(to_child) != 0 || ::pipe(from_child) != 0)
            throw std::runtime_error("mcp: pipe failed");
        pid_t pid = ::fork();
        if (pid < 0) throw std::runtime_error("mcp: fork failed");
        if (pid == 0) {
            ::dup2(to_child[0], STDIN_FILENO);
            ::dup2(from_child[1], STDOUT_FILENO);
            ::close(to_child[0]);
            ::close(to_child[1]);
            ::close(from_child[0]);
            ::close(from_child[1]);
            for (auto& [k, v] : cfg.env) ::setenv(k.c_str(), v.c_str(), 1);
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(cfg.command->c_str()));
            std::vector<std::string> args = cfg.args;
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        ::close(to_child[0]);
        ::close(from_child[1]);
        auto conn =
            std::unique_ptr<StdioMcpConnection>(new StdioMcpConnection(server, pid, to_child[1], from_child[0]));
        conn->initialize();
        return conn;
#else
        (void)pid;
        (void)wfd;
        (void)rfd;
        throw std::runtime_error("mcp: stdio servers are not supported on this platform");
#endif
    }

    Json send_request(const std::string& method, const Json& params) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint64_t id = next_id_.fetch_add(1);
        // Shared JSON-RPC envelope (Rust: `use super::http::JsonRpcRequest`).
        JsonRpcRequest envelope;
        envelope.jsonrpc = "2.0";
        envelope.id = id;
        envelope.method = method;
        if (!params.is_null()) envelope.params = params;
        std::string line = envelope.to_json().dump() + "\n";
#ifdef __unix__
        const char* p = line.c_str();
        std::size_t left = line.size();
        while (left > 0) {
            ssize_t n = ::write(wfd_, p, left);
            if (n <= 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("mcp: write to server failed");
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        while (true) {
            std::string resp = read_line_timeout(rfd_, std::chrono::seconds(30));
            if (resp.empty()) continue;
            Json v;
            try {
                v = Json::parse(resp);
            } catch (...) {
                continue;
            }
            if (!v.is_object()) continue;
            // Shared response envelope: mismatched ids (numeric or string) are
            // skipped, errors bubble out with code, message and server name.
            JsonRpcResponse resp = JsonRpcResponse::from_json(v);
            if (!resp.id_matches(id)) continue;
            return resp.into_result(server_);
        }
#else
        throw std::runtime_error("mcp: unsupported platform");
#endif
    }

    void send_notification(const std::string& method) {
        std::lock_guard<std::mutex> lock(mutex_);
        JsonRpcNotification envelope;
        envelope.jsonrpc = "2.0";
        envelope.method = method;
        std::string line = envelope.to_json().dump() + "\n";
#ifdef __unix__
        const char* p = line.c_str();
        std::size_t left = line.size();
        while (left > 0) {
            ssize_t n = ::write(wfd_, p, left);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
#endif
    }

    void initialize() {
        Json::Object caps_tools;
        Json::Object caps;
        caps.emplace("tools", Json(std::move(caps_tools)));
        Json::Object client_info;
        client_info.emplace("name", Json("marmel"));
        client_info.emplace("version", Json("0.1.0"));
        Json::Object params;
        params.emplace("protocolVersion", Json("2024-11-05"));
        params.emplace("capabilities", Json(std::move(caps)));
        params.emplace("clientInfo", Json(std::move(client_info)));
        send_request("initialize", Json(std::move(params)));
        send_notification("notifications/initialized");
    }

    std::vector<McpTool> list_tools() {
        Json result = send_request("tools/list", Json());
        std::vector<McpTool> out;
        if (result.is_object() && result.contains("tools") && result.at("tools").is_array()) {
            for (auto& t : result.at("tools").as_array()) {
                if (!t.is_object()) continue;
                McpTool tool;
                tool.name = t.str_or("name", "");
                if (tool.name.empty()) continue;
                if (t.contains("description") && t.at("description").is_string())
                    tool.description = t.at("description").as_string();
                if (t.contains("inputSchema"))
                    tool.input_schema = t.at("inputSchema");
                else
                    tool.input_schema = Json::parse(R"({"type":"object"})");
                tool.server_name = server_;
                out.push_back(std::move(tool));
            }
        }
        return out;
    }

    std::string call_tool(const std::string& name, const Json& args) {
        Json::Object params;
        params.emplace("name", Json(name));
        params.emplace("arguments", args);
        Json result = send_request("tools/call", Json(std::move(params)));
        // Exact content extraction: any item with a string "text" field
        // contributes it, otherwise the item JSON; '\n' after EVERY item;
        // top-level "text"; else the whole result unless null.
        std::string out;
        if (result.is_object() && result.contains("content") &&
            result.at("content").is_array()) {
            for (auto& c : result.at("content").as_array()) {
                if (c.is_object() && c.contains("text") && c.at("text").is_string()) {
                    out += c.at("text").as_string();
                } else {
                    out += c.dump();
                }
                out += "\n";
            }
        } else if (result.is_object() && result.contains("text") &&
                   result.at("text").is_string()) {
            out += result.at("text").as_string();
        } else if (!result.is_null()) {
            out += result.dump();
        }
        bool is_error = result.is_object() && result.contains("isError") &&
                        result.at("isError").is_bool() && result.at("isError").as_bool(false);
        // Trim (mirrors output.trim()).
        auto not_space = [](char ch) {
            return ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r';
        };
        std::size_t b = 0;
        while (b < out.size() && !not_space(out[b])) b++;
        std::size_t e = out.size();
        while (e > b && !not_space(out[e - 1])) e--;
        out = out.substr(b, e - b);
        if (is_error) throw std::runtime_error(out);
        return out;
    }

    void kill() {
#ifdef __unix__
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, WNOHANG);
            pid_ = -1;
        }
        if (wfd_ >= 0) {
            ::close(wfd_);
            wfd_ = -1;
        }
        if (rfd_ >= 0) {
            ::close(rfd_);
            rfd_ = -1;
        }
#endif
    }

private:
    std::string server_;
    pid_t pid_ = -1;
    int wfd_ = -1;
    int rfd_ = -1;
    std::atomic<std::uint64_t> next_id_{1};
    std::mutex mutex_;
};

class StdioClient final : public McpClient {
public:
    explicit StdioClient(std::unique_ptr<StdioMcpConnection> conn) : conn_(std::move(conn)) {}
    std::vector<McpTool> list_tools() override {
        std::lock_guard<std::mutex> l(mutex_);
        return conn_->list_tools();
    }
    std::string call_tool(const std::string& name, const Json& args) override {
        std::lock_guard<std::mutex> l(mutex_);
        return conn_->call_tool(name, args);
    }
    void shutdown() override {
        std::lock_guard<std::mutex> l(mutex_);
        conn_->kill();
    }

private:
    std::mutex mutex_;
    std::unique_ptr<StdioMcpConnection> conn_;
};

/// HTTP + SSE backend (Rust: `McpClient::HttpSse(Mutex<HttpSseConnection>)`).
class HttpSseClient : public McpClient {
public:
    explicit HttpSseClient(HttpSseConnection conn) : conn_(std::move(conn)) {}

    std::vector<McpTool> list_tools() override {
        std::lock_guard<std::mutex> l(mutex_);
        return conn_.list_tools();
    }
    std::string call_tool(const std::string& name, const Json& args) override {
        std::lock_guard<std::mutex> l(mutex_);
        return conn_.call_tool(name, args);
    }
    void shutdown() override {
        std::lock_guard<std::mutex> l(mutex_);
        conn_.shutdown();
    }

private:
    std::mutex mutex_;
    HttpSseConnection conn_;
};

} // namespace

std::shared_ptr<McpClient> McpClient::spawn_stdio(const std::string& server_name,
                                                 const std::string& command,
                                                 const std::vector<std::string>& args,
                                                 const std::map<std::string, std::string>& env) {
    McpServerConfig cfg;
    cfg.command = command;
    cfg.args = args;
    cfg.env = env;
    return std::make_shared<StdioClient>(StdioMcpConnection::spawn(server_name, cfg));
}

std::shared_ptr<McpClient> McpClient::spawn_http_sse(const std::string& server_name,
                                                    const McpServerConfig& cfg) {
    return std::make_shared<HttpSseClient>(HttpSseConnection::connect(server_name, cfg));
}

McpManager::McpManager() : st_(std::make_shared<State>()) {}

McpManager::McpManager(TestInit init) : st_(std::make_shared<State>()) {
    st_->tools = std::move(init.tools);
    st_->clients = std::move(init.clients);
}

McpManager McpManager::boot(const std::map<std::string, McpServerConfig>& servers) {
    McpManager m;
    std::vector<types::ToolDef> defs;
    for (auto& [name, cfg] : servers) {
        const bool has_command = cfg.command && !cfg.command->empty();
        const bool has_url = cfg.url && !cfg.url->empty();
        if (!has_command && !has_url) continue; // neither spawnable nor reachable
        std::shared_ptr<McpClient> client;
        try {
            if (has_command)
                client = McpClient::spawn_stdio(name, *cfg.command, cfg.args, cfg.env);
            else
                client = McpClient::spawn_http_sse(name, cfg);
        } catch (const std::exception& e) {
            std::cerr << "[marmel] mcp: failed to connect to server '" << name
                      << "': " << e.what() << "\n";
            continue;
        }
        std::vector<McpTool> tools;
        try {
            tools = client->list_tools();
        } catch (const std::exception& e) {
            std::cerr << "[marmel] mcp: tools/list failed for '" << name << "': " << e.what()
                      << "\n";
        }
        {
            std::lock_guard<std::mutex> l(m.st_->mutex);
            m.st_->clients[name] = client;
            for (auto& t : tools) {
                m.st_->tools[t.qualified_name()] = t;
                defs.push_back(types::ToolDef::from_mcp(
                    t.qualified_name(), t.description.value_or(""), t.input_schema));
            }
        }
    }
    harness::set_mcp_tool_defs(defs);
    // Bridge into the harness dispatcher (MCP overrides built-ins). The
    // shared state outlives the call, so the callbacks stay valid for the
    // process lifetime (matches Rust's global set_mcp_manager).
    std::shared_ptr<State> st = m.st_;
    harness::set_mcp_tool_checker(
        [st](const std::string& name) {
            std::lock_guard<std::mutex> l(st->mutex);
            return st->tools.count(name) != 0;
        },
        [st](const std::string& name, const Json& args) {
            std::shared_ptr<McpClient> client;
            std::string server;
            std::string raw_name = name;
            {
                std::lock_guard<std::mutex> l(st->mutex);
                auto it = st->tools.find(name);
                if (it == st->tools.end())
                    return harness::ToolResult::err("MCP tool error: unknown MCP tool: " + name);
                auto cit = st->clients.find(it->second.server_name);
                if (cit == st->clients.end())
                    return harness::ToolResult::err("MCP tool error: server gone: " + name);
                client = cit->second;
                raw_name = it->second.name;
            }
            try {
                return harness::ToolResult::ok(client->call_tool(raw_name, args));
            } catch (const std::exception& e) {
                return harness::ToolResult::err("MCP tool error: " + std::string(e.what()));
            }
        });
    return m;
}

std::vector<McpTool> McpManager::tools() const {
    std::lock_guard<std::mutex> l(st_->mutex);
    std::vector<McpTool> out;
    for (auto& [k, v] : st_->tools) out.push_back(v);
    return out;
}
std::vector<McpTool> McpManager::tools_for_servers(const std::vector<std::string>& servers) const {
    if (servers.empty()) return {}; // explicit empty list => nothing is exposed
    std::lock_guard<std::mutex> l(st_->mutex);
    std::vector<McpTool> out;
    for (auto& [k, tool] : st_->tools) {
        if (std::find(servers.begin(), servers.end(), tool.server_name) != servers.end())
            out.push_back(tool);
    }
    return out;
}
bool McpManager::has_tool(const std::string& name) const {
    std::lock_guard<std::mutex> l(st_->mutex);
    return st_->tools.count(name) != 0;
}
std::string McpManager::call_tool(const std::string& name, const Json& args) const {
    std::shared_ptr<McpClient> client;
    std::string raw_name = name;
    {
        std::lock_guard<std::mutex> l(st_->mutex);
        auto it = st_->tools.find(name);
        if (it == st_->tools.end()) throw std::runtime_error("unknown MCP tool: " + name);
        auto cit = st_->clients.find(it->second.server_name);
        if (cit == st_->clients.end()) throw std::runtime_error("MCP server gone: " + name);
        client = cit->second;
        raw_name = it->second.name;
    }
    // The qualified name is stripped before the call is forwarded: servers
    // always receive the original, unprefixed tool name.
    return client->call_tool(raw_name, args);
}
void McpManager::shutdown() const {
    std::lock_guard<std::mutex> l(st_->mutex);
    for (auto& [k, c] : st_->clients) {
        try {
            c->shutdown();
        } catch (...) {
        }
    }
}

} // namespace marmel::mcp
