# 02 — mcp integration (`69c234f`, 2026-08-31): C++ translation

Faithful C++20 port of upstream commit **#2** of
[Na1w/marmel](https://github.com/Na1w/marmel) — `mcp integration`
(`69c234fd52e71075412d25b8785f6afbdb3e15e3`, +874/−55 lines), applied on top of
`commits/01-marmel-v0.1`. Everything from upstream #1 stays; this folder adds
the Model Context Protocol layer: **remote HTTP/SSE servers**, **per-server tool
qualification**, **per-agent MCP scoping** and the first `tests` for them.

```sh
cmake -S . -B build && cmake --build build -j
./build/marmel_tests        # 37 tests, no external framework
ctest --test-dir build
./build/marmel --help
```

---

## 1. Author's chain of thought (reconstructed from this commit)

The diff is small but changes the *identity* of a tool:

1. **Tool names must be unique across servers.** Upstream v0.1 keyed MCP tools
   by their raw name, so two servers exposing `read_file` silently collided.
   This commit introduces `qualified_name()` = `<server>__<tool>`: registration,
   `has_tool`, the exposed `ToolDef` and the harness dispatcher all use the
   qualified name, and the prefix is stripped *only* when the call is forwarded
   to the server. *Why:* the model sees a flat tool namespace, so it has to be
   made collision-free; the server must still see the name it advertised.
2. **Visibility is a policy decision, not a side effect.** Previously every MCP
   tool was visible to every agent. Now each specialist (and the validator)
   declares `mcp_servers = [...]`, and `tools_for_servers` narrows discovery for
   that call. *Why:* a debugging agent should not see the filesystem agent's
   tools — fewer tools means better tool selection on small local models.
3. **One transport config per server, chosen explicitly.** A server entry sets
   either `command` (stdio, spawns a process) or `url` (remote HTTP + SSE),
   never both — the example config spells this rule out verbatim. The new
   `src/mcp/http.rs` gives the remote case a full JSON-RPC 2.0 client with
   session-id capture, SSE frame demux and `Accept: application/json,
   text/event-stream`.
4. **Config beats code for scoping.** `mcp_servers` lands on
   `SpecialistConfig` (per agent), on `OrchestrationConfig` (top-level default,
   propagated into `StreamConfig`) and on the example config; specialists may
   opt in, and nothing is exposed by default (`tools_for_servers([])` → empty).
5. **Tolerant boot stays.** Spawn/list failures still warn and continue: a dead
   MCP server must not take the harness down.

## 2. File correspondence (upstream commit #2 → this translation)

| Upstream delta | This translation | Notes |
|---|---|---|
| `marmel.toml.example` (+32) | `marmel.toml.example` (appended verbatim) | "External MCP servers" block, both examples |
| `src/config.rs` (+59) | `include/marmel/config.hpp`, `src/config.cpp` | `mcp_servers` on `SpecialistConfig` + `OrchestrationConfig`, array parsing, merge (non-empty overwrites) |
| `src/types.rs` (+34) | used via `ToolDef::from_mcp` with the qualified name | upstream composes `function.name` from `tool.qualified_name()` |
| `src/llm/stream.rs` (~5) | `include/marmel/llm.hpp`, `src/llm.cpp` | `StreamConfig::mcp_servers` from `orchestration.mcp_servers`; `build_request` filters via `tools_for_servers` |
| `src/agents/mod.rs` (~13) | `src/agents.cpp` | specialist loop uses its own servers; validation loop uses validator's list, else specialist's, else none |
| `src/mcp/mod.rs` (+4) | unchanged (`mcp.hpp`) | module already exposed |
| `src/mcp/client.rs` (+242/−55) | `include/marmel/mcp.hpp`, `src/mcp.cpp` | `qualified_name`, `tools_for_servers`, `MockMcpClient`, `HttpSseClient`, qualified registration + raw-name dispatch, `url` branch in `boot()` |
| `src/mcp/http.rs` (new, 364) | `include/marmel/mcp_http.hpp`, `src/mcp_http.cpp` | JSON-RPC envelopes, session id, SSE demux, `initialize`/`tools/list`/`tools/call`, libcurl transport with stub fallback |
| `src/mcp/http_tests.rs` (new, 176) | `tests/test_marmel.cpp` (5 new cases) | envelopes, id matching, `into_result`, `tools`/`call` payload parsing, config parsing |

## 3. Rust → C++ mapping for the new code

| Rust | C++ here |
|---|---|
| `serde(skip_serializing_if = "Option::is_none")` | `JsonRpcRequest::to_json()` omits `params` when `std::nullopt` |
| `reqwest` client (30 s) + `eventsource-stream` | libcurl easy handle (`CURLOPT_TIMEOUT 30`) + whole-body SSE frame parser; stub when no libcurl headers |
| `AtomicU64` request id | plain `std::uint64_t` (the port is single-threaded, see §4) |
| `HashMap<String, McpTool>` | `std::map` keyed by qualified name (deterministic order) |
| `Arc<Mutex<HttpSseConnection>>` | `HttpSseClient` holding the connection behind a `std::mutex` |
| `#[cfg(test)] McpClient::Mock` | `mcp::MockMcpClient` (records raw names, returns `"mock-result"`) |
| tests mutating `manager.tools` in-crate | `McpManager::TestInit` constructor — the documented test seam |

## 4. Documented deviations

- **Transport is synchronous.** Upstream streams SSE with `tokio` +
  `eventsource-stream`; the port performs one blocking POST per request and
  demuxes the complete `data:` frame set afterwards. Same frames, same
  mismatched-id skipping, same "closed before response" error — but no
  incremental delivery and no cancellation mid-response.
- **Request ids are not atomic** (`AtomicU64` → `std::uint64_t`): the port has
  no concurrent request in flight.
- **No libcurl ⇒ HTTP MCP unavailable.** When `MARMEL_HAVE_CURL` is not
  defined, `http_post_json` reports
  `"libcurl unavailable: MCP HTTP transport is disabled in this build"` through
  the same error path as upstream's transport failure (same message shape:
  `HTTP request to MCP server '<x>' for '<method>' failed: …`).
- **`inputSchema: null` becomes `{"type":"object"}`** in `parse_tools_list`
  (upstream's own test asserts this after filtering nulls; the port filters in
  the helper rather than in the test).
- **Tool ordering is deterministic** (sorted qualified names) where upstream
  iterates a `HashMap`.
- **`Json::object` ordering is sorted**, so serialized envelopes list keys as
  `id, jsonrpc, method, params` rather than struct order; field values, presence
  rules and absence of `params` are asserted, not the byte string.
- **`ToolDef::from_mcp(name, description, schema)`** keeps the v0.1 signature;
  callers pass `tool.qualified_name()`, which is exactly what upstream composes
  inside `from_mcp` (avoids a `types → mcp` dependency that Rust does not need).
- **Error data rendering** uses the JSON dump instead of Rust's `{:?}` of
  `Option<Value>` (`None` when absent); code, message and server name — the
  parts upstream asserts — are textual and identical.

## 5. Config (this commit's additions)

```toml
[orchestration]
mcp_servers = ["filesystem"]          # default set for top-level turns

[orchestration.specialists.coder]
mcp_servers = ["filesystem", "db"]    # what this specialist may call

[mcp_servers.filesystem]              # stdio transport
command = "npx"
args = ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]

[mcp_servers.remote]                  # remote HTTP+SSE transport
url = "https://example.com/mcp"
```

Discovered tools reach the model as `<server>__<tool>` (`filesystem__read_file`);
the prefix is stripped before the call reaches the server.
