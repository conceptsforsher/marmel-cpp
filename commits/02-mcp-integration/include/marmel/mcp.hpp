#pragma once
// MCP (Model Context Protocol) client: JSON-RPC 2.0 over stdio and HTTP+SSE.
// Rust origin: src/mcp/{mod,client}.rs (8+383 lines) + src/mcp/http.rs (364).
//
// Minimal synchronous MCP: spawn `command args` with piped stdin/stdout
// (stderr inherited) for `command` servers, or POST JSON-RPC 2.0 to `url`
// servers (Streamable HTTP, SSE responses), handshake initialize{protocolVersion
// 2024-11-05, capabilities.tools, clientInfo marmel/<version>} +
// notifications/initialized, tools/list (inputSchema default {"type":"object"}),
// tools/call with text concatenation (isError=true => error). 30s per-response
// timeout, mismatched ids ignored, EOF => closed error. Discovered tools are
// keyed and exposed under `<server>__<tool>` qualified names; agents see only
// the servers listed in their `mcp_servers` config. boot() is tolerant:
// spawn/list failures warn + continue. shutdown() SIGKILLs the child.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "marmel/config.hpp"
#include "marmel/json.hpp"

namespace marmel::mcp {

struct McpTool {
    std::string name;
    std::optional<std::string> description;
    Json input_schema = Json::object();
    std::string server_name;

    /// Fully-qualified tool name: `<server>__<tool>`. Unique across servers,
    /// even when several of them expose identically named tools.
    std::string qualified_name() const { return server_name + "__" + name; }
};

class McpClient {
public:
    virtual ~McpClient() = default;
    virtual std::vector<McpTool> list_tools() = 0;
    virtual std::string call_tool(const std::string& name, const Json& args) = 0;
    virtual void shutdown() = 0;
    static std::shared_ptr<McpClient> spawn_stdio(const std::string& server_name,
                                                 const std::string& command,
                                                 const std::vector<std::string>& args,
                                                 const std::map<std::string, std::string>& env);
    /// Connect to a remote MCP server over HTTP + SSE (`url` config).
    static std::shared_ptr<McpClient> spawn_http_sse(const std::string& server_name,
                                                    const McpServerConfig& cfg);
};

/// Test double (Rust: `McpClient::Mock`, `#[cfg(test)]`): records the raw tool
/// names it was called with and always answers "mock-result".
class MockMcpClient : public McpClient {
public:
    std::vector<std::string> called_names;

    std::vector<McpTool> list_tools() override { return {}; }
    std::string call_tool(const std::string& name, const Json&) override {
        called_names.push_back(name);
        return "mock-result";
    }
    void shutdown() override {}
};

class McpManager {
public:
    McpManager();
    /// Test seam (Rust tests mutate `manager.tools` inside the crate).
    struct TestInit {
        std::map<std::string, McpTool> tools;            // keyed by qualified name
        std::map<std::string, std::shared_ptr<McpClient>> clients;
    };
    explicit McpManager(TestInit init);

    static McpManager boot(const std::map<std::string, McpServerConfig>& servers);
    std::vector<McpTool> tools() const;
    /// All discovered tools whose server is in `servers`; empty `servers`
    /// means "none" (no implicit global visibility).
    std::vector<McpTool> tools_for_servers(const std::vector<std::string>& servers) const;
    bool has_tool(const std::string& name) const;
    std::string call_tool(const std::string& name, const Json& args) const;
    void shutdown() const;

private:
    struct State {
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<McpClient>> clients;
        std::map<std::string, McpTool> tools;
    };
    std::shared_ptr<State> st_;
};

} // namespace marmel::mcp
