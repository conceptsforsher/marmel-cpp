#pragma once
// MCP HTTP + SSE transport for remote (external) MCP servers.
// Rust origin: src/mcp/http.rs (364 lines, added by upstream commit 69c234f
// "mcp integration").
//
// Implements the MCP "Streamable HTTP" transport: JSON-RPC 2.0 requests are
// POSTed to a remote endpoint and responses come back either directly in the
// HTTP body (`application/json`) or as Server-Sent Events
// (`text/event-stream`). The session id advertised by the server via the
// `Mcp-Session-Id` header is captured on `initialize` and echoed back on every
// later request.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "marmel/config.hpp"
#include "marmel/json.hpp"
#include "marmel/mcp.hpp"

namespace marmel::mcp {

/// Shared JSON-RPC 2.0 request envelope.
struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    std::uint64_t id = 0;
    std::string method;
    std::optional<Json> params;

    /// Envelope as Json; `params` is omitted when not engaged (mirrors serde's
    /// `skip_serializing_if = "Option::is_none"`).
    Json to_json() const;
};

/// Shared JSON-RPC 2.0 notification envelope (no id).
struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<Json> params;

    Json to_json() const;
};

struct JsonRpcError {
    long long code = 0;
    std::string message;
    std::optional<Json> data;

    static JsonRpcError from_json(const Json& v);
};

/// Shared JSON-RPC 2.0 response envelope.
struct JsonRpcResponse {
    std::optional<std::string> jsonrpc;
    std::optional<Json> id;
    std::optional<Json> result;
    std::optional<JsonRpcError> error;

    static JsonRpcResponse from_json(const Json& v);

    /// Whether this response carries the given request id.
    bool id_matches(std::uint64_t id) const;

    /// The `result` value; throws with code, message and server name on error.
    Json into_result(const std::string& server_name) const;
};

/// `tools/list` result -> McpTool list. Entries without a string `name` are
/// skipped; `description` stays nullopt when absent and `inputSchema` falls
/// back to `{"type":"object"}` when absent or null.
std::vector<McpTool> parse_tools_list(const std::string& server_name, const Json& result);

/// Whether a `tools/call` result carries `isError = true`.
bool call_result_is_error(const Json& result);

/// `tools/call` result -> concatenated text: `content[].text` joined by '\n'
/// (non-text items are JSON-dumped), else `result.text`, else the raw JSON.
/// Throws with that text when the result is an error result.
std::string render_call_tool_result(const Json& result);

/// Active connection to a remote MCP server over HTTP + SSE.
class HttpSseConnection {
public:
    HttpSseConnection(std::string server_name, std::string endpoint);

    /// Connect to a remote MCP server configured via `url` and run
    /// `initialize`. Throws when `url` is missing, the client cannot be built
    /// or the handshake fails.
    static HttpSseConnection connect(const std::string& server_name, const McpServerConfig& cfg);

    /// Discover the tools exposed by the remote MCP server.
    std::vector<McpTool> list_tools();

    /// Invoke a tool on the remote MCP server.
    std::string call_tool(const std::string& name, const Json& arguments);

    /// HTTP/SSE has no shutdown handshake; the connection is simply dropped.
    void shutdown();

private:
    Json post(const std::string& method, const std::optional<Json>& params, std::uint64_t id);
    Json read_sse_response(const std::string& body, const std::string& method, std::uint64_t id);
    void send_notification(const std::string& method, const std::optional<Json>& params);
    void initialize();

    std::string server_name_;
    std::string endpoint_;
    std::optional<std::string> session_id_;
    std::uint64_t request_id_ = 1; // Rust: AtomicU64 (single-threaded port)
};

} // namespace marmel::mcp
