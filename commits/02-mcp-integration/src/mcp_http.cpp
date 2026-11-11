// Rust origin: src/mcp/http.rs (364 lines, upstream commit 69c234f).
#include "marmel/mcp_http.hpp"

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef MARMEL_HAVE_CURL
#include <curl/curl.h>
#endif

namespace marmel::mcp {
namespace {

/// reqwest client timeout: 30s per request.
constexpr long kRequestTimeoutSecs = 30;

/// Client version reported in `initialize` (Rust: env!("CARGO_PKG_VERSION")).
constexpr const char* kClientVersion = "0.1.0";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_copy(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r'))
        e--;
    return s.substr(b, e - b);
}

std::string data_repr(const std::optional<Json>& data) {
    return data ? data->dump() : std::string("None");
}

struct HttpResponse {
    long status = 0;
    std::map<std::string, std::string> headers; // lowercased keys
    std::string body;
};

/// POST `payload` to `url`; on transport failure `error` is set and status 0.
HttpResponse http_post_json(const std::string& url, const std::string& payload,
                            const std::map<std::string, std::string>& headers, std::string& error) {
    HttpResponse out;
#ifdef MARMEL_HAVE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return out;
    }
    struct curl_slist* list = nullptr;
    std::vector<std::string> lines;
    for (const auto& kv : headers) lines.push_back(kv.first + ": " + kv.second);
    for (const auto& l : lines) list = curl_slist_append(list, l.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, std::size_t size, std::size_t nmemb, void* userdata) -> std::size_t {
            std::string* body = static_cast<std::string*>(userdata);
            body->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(
        curl, CURLOPT_HEADERFUNCTION,
        +[](char* ptr, std::size_t size, std::size_t nmemb, void* userdata) -> std::size_t {
            std::string line(ptr, size * nmemb);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                auto* hdrs = static_cast<std::map<std::string, std::string>*>(userdata);
                std::string key = lower(line.substr(0, colon));
                std::string val = trim_copy(line.substr(colon + 1));
                (*hdrs)[key] = val;
            }
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSecs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.status = status;
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
#else
    (void)url;
    (void)payload;
    (void)headers;
    error = "libcurl unavailable: MCP HTTP transport is disabled in this build";
#endif
    return out;
}

} // namespace

Json JsonRpcRequest::to_json() const {
    Json o = Json::object();
    o["jsonrpc"] = jsonrpc;
    o["id"] = Json(static_cast<std::size_t>(id));
    o["method"] = method;
    if (params) o["params"] = *params;
    return o;
}

Json JsonRpcNotification::to_json() const {
    Json o = Json::object();
    o["jsonrpc"] = jsonrpc;
    o["method"] = method;
    if (params) o["params"] = *params;
    return o;
}

JsonRpcError JsonRpcError::from_json(const Json& v) {
    JsonRpcError e;
    e.code = v.value("code", Json(0)).as_int(0);
    e.message = v.str_or("message", "");
    if (v.contains("data") && !v.at("data").is_null()) e.data = v.at("data");
    return e;
}

JsonRpcResponse JsonRpcResponse::from_json(const Json& v) {
    JsonRpcResponse r;
    if (v.contains("jsonrpc") && v.at("jsonrpc").is_string()) r.jsonrpc = v.at("jsonrpc").as_string();
    if (v.contains("id")) r.id = v.at("id");
    if (v.contains("result")) r.result = v.at("result");
    if (v.contains("error") && !v.at("error").is_null())
        r.error = JsonRpcError::from_json(v.at("error"));
    return r;
}

bool JsonRpcResponse::id_matches(std::uint64_t want) const {
    if (!id) return false;
    const Json& v = *id;
    // Numeric ids compare as integers; string ids are parsed (Rust:
    // `Value::String(s) => s.parse::<u64>().ok() == Some(id)`).
    if (v.is_integer()) return v.as_u64(0) == want;
    if (v.is_string()) {
        try {
            return std::stoull(v.as_string()) == want;
        } catch (...) {
            return false;
        }
    }
    return false;
}

Json JsonRpcResponse::into_result(const std::string& server_name) const {
    if (error) {
        throw std::runtime_error("MCP error (" + std::to_string(error->code) + ") from '" +
                                 server_name + "': " + error->message + " (data: " +
                                 data_repr(error->data) + ")");
    }
    return result ? *result : Json();
}

std::vector<McpTool> parse_tools_list(const std::string& server_name, const Json& result) {
    std::vector<McpTool> tools;
    if (!result.contains("tools")) return tools;
    const Json& arr = result.at("tools");
    if (!arr.is_array()) return tools;
    for (const Json& t : arr.as_array()) {
        if (!t.is_object()) continue;
        const Json& nv = t.value("name", Json());
        if (!nv.is_string()) continue;
        McpTool tool;
        tool.name = nv.as_string();
        const Json& dv = t.value("description", Json());
        if (dv.is_string()) tool.description = dv.as_string();
        const Json& sv = t.value("inputSchema", Json());
        tool.input_schema = sv.is_object() ? sv : Json::object({{"type", Json("object")}});
        tool.server_name = server_name;
        tools.push_back(std::move(tool));
    }
    return tools;
}

bool call_result_is_error(const Json& result) {
    return result.is_object() && result.value("isError", Json(false)).as_bool(false);
}

std::string render_call_tool_result(const Json& result) {
    std::string out;
    bool have_content = result.is_object() && result.contains("content") &&
                        result.at("content").is_array();
    if (have_content) {
        for (const Json& item : result.at("content").as_array()) {
            const Json& tv = item.value("text", Json());
            if (item.is_object() && tv.is_string())
                out += tv.as_string();
            else
                out += item.dump();
            out += '\n';
        }
    } else if (result.is_object() && result.value("text", Json()).is_string()) {
        out += result.value("text", Json()).as_string();
    } else if (!result.is_null()) {
        out += result.dump();
    }
    std::string trimmed = trim_copy(out);
    if (call_result_is_error(result)) throw std::runtime_error(trimmed);
    return trimmed;
}

HttpSseConnection::HttpSseConnection(std::string server_name, std::string endpoint)
    : server_name_(std::move(server_name)), endpoint_(std::move(endpoint)) {}

HttpSseConnection HttpSseConnection::connect(const std::string& server_name,
                                             const McpServerConfig& cfg) {
    if (!cfg.url || cfg.url->empty())
        throw std::runtime_error("missing `url` for HTTP MCP server '" + server_name + "'");
    HttpSseConnection conn(server_name, *cfg.url);
    conn.initialize();
    return conn;
}

Json HttpSseConnection::post(const std::string& method, const std::optional<Json>& params,
                             std::uint64_t id) {
    JsonRpcRequest req;
    req.jsonrpc = "2.0";
    req.id = id;
    req.method = method;
    req.params = params;
    const std::string payload = req.to_json().dump();

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json, text/event-stream";
    if (session_id_) headers["Mcp-Session-Id"] = *session_id_;

    std::string error;
    HttpResponse resp = http_post_json(endpoint_, payload, headers, error);
    if (!error.empty())
        throw std::runtime_error("HTTP request to MCP server '" + server_name_ + "' for '" + method +
                                 "' failed: " + error);

    auto sid = resp.headers.find("mcp-session-id");
    if (sid != resp.headers.end()) session_id_ = sid->second;

    auto ct = resp.headers.find("content-type");
    const std::string content_type = ct == resp.headers.end() ? std::string() : ct->second;

    if (content_type.find("text/event-stream") != std::string::npos)
        return read_sse_response(resp.body, method, id);

    if (resp.status < 200 || resp.status >= 300)
        throw std::runtime_error("HTTP " + std::to_string(resp.status) + " from '" + server_name_ +
                                 "' for '" + method + "': " + resp.body);
    Json parsed;
    try {
        parsed = Json::parse(resp.body);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid JSON-RPC response from '" + server_name_ + "' for '" +
                                 method + "'");
    }
    return JsonRpcResponse::from_json(parsed).into_result(server_name_);
}

Json HttpSseConnection::read_sse_response(const std::string& body, const std::string& method,
                                          std::uint64_t id) {
    std::vector<std::string> events;
    std::istringstream in(body);
    std::string line;
    std::string cur;
    bool have = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (have) {
                events.push_back(cur);
                cur.clear();
                have = false;
            }
            continue;
        }
        if (line.rfind("data:", 0) == 0) {
            if (have) cur += '\n';
            cur += trim_copy(line.substr(5));
            have = true;
        }
    }
    if (have) events.push_back(cur);

    for (const std::string& data : events) {
        Json parsed;
        try {
            parsed = Json::parse(data);
        } catch (const std::exception&) {
            continue; // non-JSON SSE frames are ignored (as in Rust)
        }
        JsonRpcResponse resp = JsonRpcResponse::from_json(parsed);
        if (resp.id_matches(id)) return resp.into_result(server_name_);
    }
    throw std::runtime_error("SSE stream from '" + server_name_ +
                             "' closed before response for '" + method + "'");
}

void HttpSseConnection::send_notification(const std::string& method,
                                          const std::optional<Json>& params) {
    JsonRpcNotification notif;
    notif.jsonrpc = "2.0";
    notif.method = method;
    notif.params = params;
    const std::string payload = notif.to_json().dump();

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json, text/event-stream";
    if (session_id_) headers["Mcp-Session-Id"] = *session_id_;

    std::string error;
    HttpResponse resp = http_post_json(endpoint_, payload, headers, error);
    if (!error.empty())
        throw std::runtime_error("HTTP request to MCP server '" + server_name_ +
                                 "' for notification '" + method + "' failed: " + error);

    auto sid = resp.headers.find("mcp-session-id");
    if (sid != resp.headers.end()) session_id_ = sid->second;

    if (resp.status < 200 || resp.status >= 300)
        throw std::runtime_error("HTTP " + std::to_string(resp.status) + " from '" + server_name_ +
                                 "' for notification '" + method + "': " + resp.body);
}

void HttpSseConnection::initialize() {
    Json params = Json::object({
        {"protocolVersion", Json("2024-11-05")},
        {"capabilities", Json::object({{"tools", Json::object()}})},
        {"clientInfo", Json::object({{"name", Json("marmel")},
                                    {"version", Json(std::string(kClientVersion))}})},
    });
    post("initialize", params, 0);
    send_notification("notifications/initialized", std::nullopt);
}

std::vector<McpTool> HttpSseConnection::list_tools() {
    const std::uint64_t id = request_id_++;
    Json result = post("tools/list", std::nullopt, id);
    return parse_tools_list(server_name_, result);
}

std::string HttpSseConnection::call_tool(const std::string& name, const Json& arguments) {
    const std::uint64_t id = request_id_++;
    Json params = Json::object({{"name", Json(name)}, {"arguments", arguments}});
    Json result = post("tools/call", params, id);
    return render_call_tool_result(result);
}

void HttpSseConnection::shutdown() {
    // No explicit handshake: dropping the connection is the shutdown.
}

} // namespace marmel::mcp
