#include "mcp/server.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace sage::mcp {

// MCP protocol revision we report in the initialize response.
// Reference: https://modelcontextprotocol.io
constexpr const char* PROTOCOL_VERSION = "2025-03-26";

namespace {

// Recursively rewrite a JSON value for `simplify` mode. Preserves the input
// shape on `raw`, drops empty/null/None on `stripped`, applies the same
// drops + collapses {properties:{}} containers on `simplified`. CommonAIExport
// equivalent: the Python post-processor on stripped/simplified .txt outputs.
//
// Field drops (stripped + simplified):
//   - null
//   - empty string ""
//   - empty array []
//   - empty object {}
//   - the literal string "None" (UE FName(NAME_None) round-trip)
//   - the literal string "()" (FStruct ExportText for empty struct)
//
// Additional collapses (simplified only):
//   - {properties: {...}, count: N, ...keep_others} → flatten properties
//     into the outer object when no key collision (rare but useful for
//     audio.read_* whose `properties` block is the bulk of the payload)
//   - count fields are dropped when an adjacent array exists with the
//     same length (redundant)
nlohmann::json simplifyValue(const nlohmann::json& v, bool deep);

bool isDropped(const nlohmann::json& v) {
    if (v.is_null()) return true;
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        return s.empty() || s == "None" || s == "()";
    }
    if (v.is_array())  return v.empty();
    if (v.is_object()) return v.empty();
    return false;
}

nlohmann::json simplifyValue(const nlohmann::json& v, bool deep) {
    if (v.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        // Track array lengths to drop redundant `count` fields in `deep` mode.
        std::map<std::string, std::size_t> arrayLens;
        for (auto it = v.begin(); it != v.end(); ++it) {
            const auto& key = it.key();
            nlohmann::json child = simplifyValue(it.value(), deep);
            if (isDropped(child)) continue;
            if (child.is_array()) arrayLens[key] = child.size();
            out[key] = std::move(child);
        }
        if (deep) {
            // Drop redundant `count` / `<name>_count` fields.
            for (auto it = out.begin(); it != out.end(); ) {
                const auto& key = it.key();
                if (it.value().is_number_integer()) {
                    const std::size_t n = it.value().get<std::size_t>();
                    bool drop = false;
                    if (key == "count" && arrayLens.size() == 1
                        && arrayLens.begin()->second == n) {
                        drop = true;
                    } else if (key.size() > 6
                               && key.compare(key.size()-6, 6, "_count") == 0) {
                        const std::string base(key, 0, key.size()-6);
                        if (auto f = arrayLens.find(base);
                            f != arrayLens.end() && f->second == n) {
                            drop = true;
                        } else if (auto f2 = arrayLens.find(base + "s");
                                   f2 != arrayLens.end() && f2->second == n) {
                            drop = true;
                        }
                    } else if (key == "returned" && arrayLens.size() >= 1) {
                        // world.export / asset.list use `returned` for the
                        // size of their main array; redundant when that array
                        // is present and matches.
                        for (const auto& [_, len] : arrayLens) {
                            if (len == n) { drop = true; break; }
                        }
                    }
                    if (drop) { it = out.erase(it); continue; }
                }
                ++it;
            }
        }
        return out;
    }
    if (v.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : v) {
            nlohmann::json child = simplifyValue(e, deep);
            if (isDropped(child)) continue;
            out.push_back(std::move(child));
        }
        return out;
    }
    return v;
}

}  // namespace


MCPServer::MCPServer(ServerInfo info, std::shared_ptr<ToolRegistry> registry)
    : info_(std::move(info)), registry_(std::move(registry)) {
    if (registry_ == nullptr) {
        throw std::invalid_argument("MCPServer: ToolRegistry must not be null");
    }
}

std::optional<Response> MCPServer::handle(const Request& request) {
    spdlog::debug("MCP handle: method='{}' notification={}",
                  request.method, request.isNotification());

    // Notifications: side-effect only, no response per JSON-RPC 2.0.
    if (request.isNotification()) {
        if (request.method == "notifications/initialized") {
            initialized_ = true;
            spdlog::info("Client signaled initialized");
        }
        return std::nullopt;
    }

    Id id = *request.id;
    const std::string& method = request.method;

    if (method == "initialize") return onInitialize(std::move(id), request.params);
    if (method == "ping")       return onPing(std::move(id));
    if (method == "tools/list") return onToolsList(std::move(id));
    if (method == "tools/call") return onToolsCall(std::move(id), request.params);

    return Response::failure(std::move(id),
        ErrorObject::fromCode(ErrorCode::MethodNotFound,
                              std::string{"unknown method: "} + method));
}

std::optional<nlohmann::json> MCPServer::handleRaw(const nlohmann::json& payload) {
    Request req;
    try {
        req = Request::fromJson(payload);
    } catch (const std::exception& ex) {
        ErrorObject err = ErrorObject::fromCode(ErrorCode::InvalidRequest, ex.what());
        return std::optional<nlohmann::json>{Response::failure(nullptr, std::move(err)).toJson()};
    }
    auto resp = handle(req);
    if (!resp) return std::nullopt;
    return std::optional<nlohmann::json>{resp->toJson()};
}

Response MCPServer::onInitialize(Id id, const nlohmann::json& /*params*/) {
    nlohmann::json result = {
        {"protocolVersion", PROTOCOL_VERSION},
        {"capabilities", {
            {"tools", { {"listChanged", false} }},
        }},
        {"serverInfo", {
            {"name",    info_.name},
            {"version", info_.version},
        }},
    };
    return Response::success(std::move(id), std::move(result));
}

Response MCPServer::onPing(Id id) {
    return Response::success(std::move(id), nlohmann::json::object());
}

Response MCPServer::onToolsList(Id id) {
    // ADR-004 §2 + Milestone 1.5b: every remote (editor-scoped) tool gets an
    // optional `_editor` property injected at list-time. Source tools.cpp files
    // don't have to repeat the property; routing is uniform across the surface.
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tool : registry_->list()) {
        nlohmann::json schema = tool.inputSchema;
        if (tool.remote && schema.is_object()) {
            if (!schema.contains("properties") || !schema["properties"].is_object()) {
                schema["properties"] = nlohmann::json::object();
            }
            if (!schema["properties"].contains("_editor")) {
                schema["properties"]["_editor"] = {
                    {"type", "string"},
                    {"description",
                        "Optional target editor: session_id, label, or instance_id. "
                        "If omitted, the active editor (or the only connected one) is used. "
                        "Use list_editors to discover available editors."},
                };
            }
            if (!schema["properties"].contains("async")) {
                schema["properties"]["async"] = {
                    {"type", "boolean"},
                    {"description",
                        "When true, start this remote editor tool as a detached "
                        "Sage job and return {job_id,state} immediately. Poll "
                        "with jobs.get/jobs.wait/jobs.logs. The plugin receives "
                        "the same args with async removed."},
                };
            }
            if (!schema["properties"].contains("job_timeout_seconds")) {
                schema["properties"]["job_timeout_seconds"] = {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 86400},
                    {"description",
                        "Detached job editor-dispatch timeout. Default 3600 seconds."},
                };
            }
        }
        // Phase 4-r5 export simplifier: every tool may opt into a stripped /
        // simplified response post-process. Server-side middleware — the
        // plugin handler never sees this flag. Default behaviour ("raw") is
        // unchanged. Universal because tool names that aren't verbose dumpers
        // simply have nothing useful to strip and the cost is one walk.
        if (schema.is_object()) {
            if (!schema.contains("properties") || !schema["properties"].is_object()) {
                schema["properties"] = nlohmann::json::object();
            }
            if (!schema["properties"].contains("simplify")) {
                schema["properties"]["simplify"] = {
                    {"type", "string"},
                    {"enum", {"raw", "stripped", "simplified"}},
                    {"description",
                        "Optional response simplifier (server-side). "
                        "'raw' (default): no change. "
                        "'stripped': drop null/empty/None fields recursively. "
                        "'simplified': stripped + collapse redundant `count` "
                        "fields when an adjacent array of the same length "
                        "exists. Adds a `_simplify_meta:{mode,raw_bytes,"
                        "simplified_bytes}` envelope to the response."},
                };
            }
        }
        arr.push_back({
            {"name",        tool.name},
            {"description", tool.description},
            {"inputSchema", std::move(schema)},
        });
    }
    nlohmann::json result = {{"tools", std::move(arr)}};
    return Response::success(std::move(id), std::move(result));
}

void MCPServer::setNotificationSink(NotificationSink sink) {
    std::lock_guard lk(sinkMu_);
    sink_ = std::move(sink);
}

void MCPServer::publishNotification(std::string method, nlohmann::json params) {
    NotificationSink local;
    {
        std::lock_guard lk(sinkMu_);
        local = sink_;
    }
    if (!local) {
        // No transport sink installed — drop. Common during early init or
        // HTTP-only mode without an SSE channel; not an error.
        spdlog::trace("publishNotification dropped (no sink): {}", method);
        return;
    }

    nlohmann::json envelope = {
        {"jsonrpc", "2.0"},
        {"method",  std::move(method)},
        {"params",  std::move(params)},
    };
    try {
        local(envelope);
    } catch (const std::exception& ex) {
        spdlog::warn("Notification sink threw: {}", ex.what());
    }
}

Response MCPServer::onToolsCall(Id id, const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        return Response::failure(std::move(id),
            ErrorObject::fromCode(ErrorCode::InvalidParams,
                                  "missing or invalid 'name' field"));
    }
    const auto toolName  = params["name"].get<std::string>();
    nlohmann::json args  = params.value("arguments", nlohmann::json::object());

    // Pull `_editor` out of the args before forwarding — it's transport-level
    // routing metadata, not part of the tool's domain payload (ADR-004 §2).
    std::string targetEditor;
    if (args.is_object() && args.contains("_editor")) {
        if (args["_editor"].is_string()) {
            targetEditor = args["_editor"].get<std::string>();
        }
        args.erase("_editor");
    }

    // Pull `simplify` out: server-side post-process middleware. Plugin handlers
    // never see this flag. Accepted values: "raw" (default), "stripped",
    // "simplified". Anything else is ignored (downgraded to "raw"). When set
    // to a non-raw mode the response body is rewritten by simplifyValue() and
    // a `_simplify_meta` envelope reports the byte savings.
    std::string simplifyMode = "raw";
    if (args.is_object() && args.contains("simplify")) {
        if (args["simplify"].is_string()) {
            const auto& s = args["simplify"].get_ref<const std::string&>();
            if (s == "raw" || s == "stripped" || s == "simplified") {
                simplifyMode = s;
            }
        }
        args.erase("simplify");
    }

    auto outcome = registry_->dispatch(toolName, args, targetEditor);
    if (!outcome.has_value()) {
        return Response::failure(std::move(id), outcome.error());
    }

    nlohmann::json body = *outcome;
    if (simplifyMode != "raw") {
        const std::string rawDump = body.dump();
        body = simplifyValue(body, /*deep=*/(simplifyMode == "simplified"));
        const std::string outDump = body.dump();
        body["_simplify_meta"] = {
            {"mode",             simplifyMode},
            {"raw_bytes",        rawDump.size()},
            {"simplified_bytes", outDump.size()},
        };
    }

    // MCP `tools/call` shape: content array (text fallback) + structuredContent.
    nlohmann::json content = nlohmann::json::array();
    content.push_back({
        {"type", "text"},
        {"text", body.dump()},
    });
    nlohmann::json wrapped = {
        {"content",           std::move(content)},
        {"isError",           false},
        {"structuredContent", body},
    };
    return Response::success(std::move(id), std::move(wrapped));
}

}  // namespace sage::mcp
