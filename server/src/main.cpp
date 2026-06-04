// Sage server entry point.
//
// Wires:
//   ToolRegistry   ← registerBuiltins (ping)
//   MCPServer      ← registry
//   HTTP+SSE       ← MCPServer  (Claude ↔ server, port 7777)
//   BridgeServer   ← editor sessions (plugin ↔ server WebSocket, port 7778)
//
// Listens until SIGINT / SIGTERM, then graceful shutdown.

#include "bridge/bridge_server.h"
#include "graph/asset_indexer.h"
#include "graph/cypher_subset.h"
#include "graph/graph_store_manager.h"
#include "mcp/server.h"
#include "mcp/tool_registry.h"
#include "tools/builtin.h"
#include "tools/phase4_schemas.h"
#include "tools/restart_orchestrator.h"
#include "tools/source_intelligence_tools.h"
#include "transport/http_sse_server.h"
#include "transport/stdio_mcp.h"
#include "util/crash_handler.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<sage::transport::HttpSseServer*> g_runningHttp{nullptr};
std::atomic<sage::bridge::BridgeServer*>     g_runningBridge{nullptr};

[[nodiscard]] bool hasFlag(int argc, char* argv[], std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::string_view{argv[i]} == flag) return true;
    }
    return false;
}

extern "C" void signalHandler(int signal) {
    // Async-signal-safe surface: only atomics + library stop() functions
    // documented as safe from signal context (cpp-httplib, ixwebsocket).
    if (auto* h = g_runningHttp.load(std::memory_order_acquire)) {
        h->stop();
    }
    if (auto* b = g_runningBridge.load(std::memory_order_acquire)) {
        b->stop();
    }
    spdlog::info("Caught signal {}; shutdown initiated", signal);
}

[[nodiscard]] std::string envOr(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string{v} : std::move(fallback);
}

[[nodiscard]] int envIntOr(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    try {
        return std::stoi(std::string{v});
    } catch (const std::exception&) {
        spdlog::warn("Env {}={} not a valid integer; using default {}", name, v, fallback);
        return fallback;
    }
}

[[nodiscard]] int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

class JobManager : public std::enable_shared_from_this<JobManager> {
public:
    explicit JobManager(sage::bridge::BridgeServer& bridge)
        : bridge_(bridge) {}

    nlohmann::json startRemote(std::string tool,
                               nlohmann::json args,
                               std::string targetEditor,
                               std::chrono::milliseconds timeout) {
        const std::string jobId = nextJobId();
        auto record = createQueuedRecord(jobId, std::move(tool), args, std::move(targetEditor), timeout.count());

        spdlog::info("jobs: queued job_id={} tool={} target={}",
                     jobId, record->tool,
                     record->targetEditor.empty() ? "<active>" : record->targetEditor);

        auto self = shared_from_this();
        std::thread([self, jobId, args = std::move(args), timeout]() mutable {
            self->runRemote(jobId, std::move(args), timeout);
        }).detach();

        return snapshot(jobId, /*includeResult=*/false).value_or(nlohmann::json{
            {"job_id", jobId},
            {"state", "queued"},
        });
    }

    nlohmann::json startLocal(std::string tool,
                              nlohmann::json args,
                              std::function<sage::mcp::ToolResult()> body) {
        const std::string jobId = nextJobId();
        auto record = createQueuedRecord(jobId, std::move(tool), args, std::string{}, 0);

        spdlog::info("jobs: queued local job_id={} tool={}", jobId, record->tool);

        auto self = shared_from_this();
        std::thread([self, jobId, body = std::move(body)]() mutable {
            self->runLocal(jobId, std::move(body));
        }).detach();

        return snapshot(jobId, /*includeResult=*/false).value_or(nlohmann::json{
            {"job_id", jobId},
            {"state", "queued"},
        });
    }

    std::optional<nlohmann::json> snapshot(const std::string& jobId,
                                           bool includeResult = true) const {
        std::lock_guard lk(mu_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) return std::nullopt;
        return toJsonLocked(*it->second, includeResult);
    }

    nlohmann::json list(int limit, bool includeCompletedDetails) const {
        std::lock_guard lk(mu_);
        nlohmann::json arr = nlohmann::json::array();
        if (limit <= 0) limit = 50;
        int emitted = 0;
        for (auto it = order_.rbegin(); it != order_.rend() && emitted < limit; ++it) {
            auto found = jobs_.find(*it);
            if (found == jobs_.end()) continue;
            arr.push_back(toJsonLocked(*found->second, includeCompletedDetails));
            ++emitted;
        }
        return {
            {"jobs", arr},
            {"count", arr.size()},
            {"total", jobs_.size()},
        };
    }

    std::optional<nlohmann::json> wait(const std::string& jobId,
                                       std::chrono::milliseconds timeout) const {
        std::unique_lock lk(mu_);
        auto exists = [&]() { return jobs_.find(jobId) != jobs_.end(); };
        if (!exists()) return std::nullopt;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (exists() && !isTerminalLocked(*jobs_.at(jobId))) {
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
        }
        nlohmann::json out = toJsonLocked(*jobs_.at(jobId), /*includeResult=*/true);
        out["wait_timed_out"] = !isTerminalLocked(*jobs_.at(jobId));
        return out;
    }

    std::optional<nlohmann::json> logs(const std::string& jobId,
                                       std::size_t cursor,
                                       std::size_t limit) const {
        std::lock_guard lk(mu_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) return std::nullopt;
        if (limit == 0 || limit > 500) limit = 100;
        const auto& entries = it->second->logs;
        nlohmann::json arr = nlohmann::json::array();
        for (std::size_t i = cursor; i < entries.size() && arr.size() < limit; ++i) {
            arr.push_back(entries[i]);
        }
        nlohmann::json out = {
            {"job_id", jobId},
            {"cursor", cursor},
            {"next_cursor", std::min(cursor + arr.size(), entries.size())},
            {"log_count", entries.size()},
            {"logs", arr},
        };
        return out;
    }

    std::optional<nlohmann::json> cancel(const std::string& jobId) {
        std::lock_guard lk(mu_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) return std::nullopt;
        auto& job = *it->second;
        job.cancelRequested = true;
        if (!isTerminalLocked(job)) {
            job.state = "cancel_requested";
        }
        job.updatedAtMs = nowEpochMs();
        appendLogLocked(job, "warn", "cancel requested; Unreal operations are best-effort and may continue until the editor tool returns",
                        nlohmann::json::object());
        cv_.notify_all();
        spdlog::warn("jobs: cancel requested job_id={} tool={}", job.id, job.tool);
        return toJsonLocked(job, /*includeResult=*/true);
    }

private:
    struct JobRecord {
        std::string id;
        std::string tool;
        std::string targetEditor;
        std::string state;
        nlohmann::json args;
        nlohmann::json result;
        nlohmann::json error;
        std::vector<nlohmann::json> logs;
        int64_t createdAtMs = 0;
        int64_t updatedAtMs = 0;
        int64_t completedAtMs = 0;
        int64_t timeoutMs = 0;
        bool cancelRequested = false;
    };

    std::shared_ptr<JobRecord> createQueuedRecord(const std::string& jobId,
                                                  std::string tool,
                                                  const nlohmann::json& args,
                                                  std::string targetEditor,
                                                  int64_t timeoutMs) {
        auto record = std::make_shared<JobRecord>();
        record->id = jobId;
        record->tool = std::move(tool);
        record->targetEditor = std::move(targetEditor);
        record->args = args;
        record->state = "queued";
        record->createdAtMs = nowEpochMs();
        record->updatedAtMs = record->createdAtMs;
        record->timeoutMs = timeoutMs;
        appendLogLocked(*record, "info", "queued", nlohmann::json::object());
        {
            std::lock_guard lk(mu_);
            jobs_[jobId] = record;
            order_.push_back(jobId);
            trimLocked();
        }
        cv_.notify_all();
        return record;
    }

    [[nodiscard]] std::string nextJobId() {
        const auto n = ++counter_;
        std::ostringstream oss;
        oss << "job-" << n;
        return oss.str();
    }

    static bool isTerminalLocked(const JobRecord& job) {
        return job.state == "completed" || job.state == "failed"
            || job.state == "editor_crashed" || job.state == "stale";
    }

    void setState(const std::string& jobId,
                  std::string state,
                  std::string message,
                  nlohmann::json data = nlohmann::json::object()) {
        {
            std::lock_guard lk(mu_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end()) return;
            it->second->state = std::move(state);
            it->second->updatedAtMs = nowEpochMs();
            appendLogLocked(*it->second, "info", std::move(message), std::move(data));
        }
        cv_.notify_all();
    }

    void runRemote(const std::string& jobId,
                   nlohmann::json args,
                   std::chrono::milliseconds timeout) {
        std::string tool;
        std::string target;
        {
            std::lock_guard lk(mu_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end()) return;
            tool = it->second->tool;
            target = it->second->targetEditor;
        }

        setState(jobId, "dispatched", "dispatching remote tool to editor",
                 {{"tool", tool}, {"target_editor", target.empty() ? "<active>" : target}});
        setState(jobId, "running", "remote tool call is running");
        setState(jobId, "waiting_editor", "waiting for editor tool result");
        spdlog::info("jobs: dispatch job_id={} tool={} timeout_ms={}",
                     jobId, tool, timeout.count());

        auto outcome = bridge_.dispatchTool(tool, args, timeout, target);
        {
            std::lock_guard lk(mu_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end()) return;
            auto& job = *it->second;
            job.updatedAtMs = nowEpochMs();
            job.completedAtMs = job.updatedAtMs;
            if (outcome.has_value()) {
                job.state = "completed";
                job.result = *outcome;
                appendLogLocked(job, "info", "completed", nlohmann::json::object());
                spdlog::info("jobs: completed job_id={} tool={}", jobId, tool);
            } else {
                job.error = outcome.error().toJson();
                const std::string msg = job.error.value("message", std::string{});
                const int code = job.error.value("code", 0);
                if (code == static_cast<int>(sage::mcp::ErrorCode::EditorNotConnected)
                    && msg.find("disconnected") != std::string::npos) {
                    job.state = "editor_crashed";
                } else {
                    job.state = "failed";
                }
                appendLogLocked(job, "error", msg.empty() ? "failed" : msg, job.error);
                spdlog::warn("jobs: failed job_id={} tool={} state={} error={}",
                             jobId, tool, job.state, msg);
            }
        }
        cv_.notify_all();
    }

    void runLocal(const std::string& jobId,
                  std::function<sage::mcp::ToolResult()> body) {
        std::string tool;
        {
            std::lock_guard lk(mu_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end()) return;
            tool = it->second->tool;
        }
        setState(jobId, "running", "local tool job is running");
        spdlog::info("jobs: running local job_id={} tool={}", jobId, tool);

        sage::mcp::ToolResult outcome = std::unexpected(
            sage::mcp::ErrorObject::fromCode(sage::mcp::ErrorCode::InternalError,
                                             "local job did not run"));
        try {
            outcome = body();
        } catch (const std::exception& ex) {
            outcome = std::unexpected(sage::mcp::ErrorObject::fromCode(
                sage::mcp::ErrorCode::InternalError,
                std::string{"local job exception: "} + ex.what()));
        } catch (...) {
            outcome = std::unexpected(sage::mcp::ErrorObject::fromCode(
                sage::mcp::ErrorCode::InternalError, "local job unknown exception"));
        }

        {
            std::lock_guard lk(mu_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end()) return;
            auto& job = *it->second;
            job.updatedAtMs = nowEpochMs();
            job.completedAtMs = job.updatedAtMs;
            if (outcome.has_value()) {
                job.state = "completed";
                job.result = *outcome;
                appendLogLocked(job, "info", "completed", nlohmann::json::object());
                spdlog::info("jobs: completed local job_id={} tool={}", jobId, tool);
            } else {
                job.state = "failed";
                job.error = outcome.error().toJson();
                const std::string msg = job.error.value("message", std::string{});
                appendLogLocked(job, "error", msg.empty() ? "failed" : msg, job.error);
                spdlog::warn("jobs: failed local job_id={} tool={} error={}",
                             jobId, tool, msg);
            }
        }
        cv_.notify_all();
    }

    static void appendLogLocked(JobRecord& job,
                                std::string level,
                                std::string message,
                                nlohmann::json data) {
        job.logs.push_back({
            {"seq", job.logs.size()},
            {"time_ms", nowEpochMs()},
            {"level", std::move(level)},
            {"state", job.state},
            {"message", std::move(message)},
            {"data", std::move(data)},
        });
        if (job.logs.size() > 500) {
            job.logs.erase(job.logs.begin(), job.logs.begin() + (job.logs.size() - 500));
            for (std::size_t i = 0; i < job.logs.size(); ++i) {
                job.logs[i]["seq"] = i;
            }
        }
    }

    nlohmann::json toJsonLocked(const JobRecord& job, bool includeResult) const {
        nlohmann::json out = {
            {"job_id", job.id},
            {"tool", job.tool},
            {"state", job.state},
            {"cancel_requested", job.cancelRequested},
            {"created_at_ms", job.createdAtMs},
            {"updated_at_ms", job.updatedAtMs},
            {"completed_at_ms", job.completedAtMs == 0 ? nullptr : nlohmann::json(job.completedAtMs)},
            {"timeout_ms", job.timeoutMs},
            {"log_count", job.logs.size()},
            {"terminal", isTerminalLocked(job)},
        };
        out["target_editor"] = job.targetEditor.empty()
            ? nlohmann::json(nullptr)
            : nlohmann::json(job.targetEditor);
        if (!job.logs.empty()) {
            out["last_log"] = job.logs.back();
        }
        if (includeResult) {
            if (!job.result.is_null()) out["result"] = job.result;
            if (!job.error.is_null()) out["error"] = job.error;
        }
        return out;
    }

    void trimLocked() {
        constexpr std::size_t maxJobs = 200;
        while (order_.size() > maxJobs) {
            const std::string oldest = order_.front();
            order_.pop_front();
            auto it = jobs_.find(oldest);
            if (it != jobs_.end() && isTerminalLocked(*it->second)) {
                jobs_.erase(it);
            } else {
                order_.push_back(oldest);
                break;
            }
        }
    }

    sage::bridge::BridgeServer& bridge_;
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::unordered_map<std::string, std::shared_ptr<JobRecord>> jobs_;
    std::deque<std::string> order_;
    std::atomic<std::uint64_t> counter_{0};
};

}  // namespace

int main(int argc, char* argv[]) {
    // Install crash handlers BEFORE any other init. A CRT precondition
    // failure during static/dynamic library init (kuzu DLL load, spdlog
    // sink ctor, etc.) must still produce a minidump and never block on
    // a modal MessageBox (cf. xmemory:209 incident, 2026-05-02).
    sage::installCrashHandlers();

    // Transport selection — stdio is default (Anthropic SDK native, OAuth-free).
    // HTTP+SSE (port 7777) opt-in via `--http` for multi-client / debugging.
    const bool useHttp = hasFlag(argc, argv, "--http");

    // Stdio reserves stdout for the JSON-RPC protocol stream; logs MUST go to
    // stderr (which Claude Code subprocess parent drains). HTTP mode is free
    // to use stdout. Stdio mode also tees logs to a rotating file (%TEMP%/
    // sage-stdio.log on Windows, /tmp/sage-stdio.log elsewhere) — rotating
    // (5MB × 3 backups) instead of truncate-on-start so a crash log survives
    // the next launch by the MCP client. Without rotation, the post-crash
    // restart wipes exactly the breadcrumb you wanted to read.
    std::shared_ptr<spdlog::logger> logger;
    if (useHttp) {
        logger = spdlog::stdout_color_mt("sage");
    } else {
        try {
            const std::string tmp = std::getenv("TEMP") ? std::getenv("TEMP")
                                  : std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                  : "/tmp";
            const std::string logPath = tmp + "/sage-stdio.log";
            auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            auto fileSink   = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                logPath, /*max_size=*/5 * 1024 * 1024, /*max_files=*/3);
            logger = std::make_shared<spdlog::logger>("sage",
                spdlog::sinks_init_list{stderrSink, fileSink});
        } catch (const std::exception&) {
            // File sink failed (path issue, permissions); fall back to stderr-only.
            logger = spdlog::stderr_color_mt("sage");
        }
    }
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");

    const auto levelStr = envOr("SAGE_LOG_LEVEL", "info");
    spdlog::set_level(spdlog::level::from_str(levelStr));

    // Flush every info+ record immediately. Without this, a native crash
    // (e.g. kuzu access violation, std::terminate) leaves the last several
    // log lines buffered and we lose the breadcrumb trail. Cost is one extra
    // file write per log line, negligible at info volume.
    spdlog::flush_on(spdlog::level::info);

    spdlog::info("sage-server starting (version 0.1.0, transport={}, log_level={})",
                 useHttp ? "http+sse" : "stdio", levelStr);

    auto registry = std::make_shared<sage::mcp::ToolRegistry>();
    sage::tools::registerBuiltins(*registry);
    sage::tools::registerPhase4Schemas(*registry);
    sage::tools::registerSourceIntelligenceTools(*registry);
    spdlog::info("Registered {} built-in tool(s)", registry->size());

    sage::mcp::MCPServer mcpServer(
        sage::mcp::ServerInfo{.name = "sage-unreal-mcp", .version = "0.1.0"},
        registry);

    // ---- Bridge (plugin ↔ server WebSocket) -----------------------------
    sage::bridge::BridgeConfig bridgeCfg{
        .host     = envOr("SAGE_WS_HOST", "127.0.0.1"),
        .port     = envIntOr("SAGE_WS_PORT", 7778),
        .endpoint = "/bridge",
    };
    sage::bridge::BridgeServer bridge(bridgeCfg);
    if (!bridge.start()) {
        spdlog::error("Bridge failed to bind {}:{}; aborting",
                      bridgeCfg.host, bridgeCfg.port);
        return 1;
    }
    g_runningBridge.store(&bridge, std::memory_order_release);
    auto jobMgr = std::make_shared<JobManager>(bridge);

    // ---- Wire bridge into tool registry (remote tools route via WS) -----
    // ADR-004 §2 + Milestone 1.5b: dispatcher forwards `_editor` (extracted by
    // MCPServer::onToolsCall) so each call can target a specific editor.
    registry->setRemoteDispatcher(
        [&bridge, jobMgr](std::string_view tool, const nlohmann::json& args,
                          std::string_view targetEditor) {
            if (args.is_object() && args.value("async", false)) {
                nlohmann::json cleanArgs = args;
                cleanArgs.erase("async");
                const int timeoutSeconds = std::clamp(
                    cleanArgs.value("job_timeout_seconds", 3600), 1, 86400);
                cleanArgs.erase("job_timeout_seconds");
                cleanArgs.erase("job_timeout_ms");
                return sage::mcp::ToolResult(jobMgr->startRemote(
                    std::string{tool},
                    std::move(cleanArgs),
                    std::string{targetEditor},
                    std::chrono::seconds(timeoutSeconds)));
            }
            return bridge.dispatchTool(tool, args,
                                       bridge.config().defaultDispatchTimeout,
                                       targetEditor);
        });

    // ---- Knowledge layer (Phase 2) --------------------------------------
    const auto sageDataDir = []() -> std::filesystem::path {
        if (const char* d = std::getenv("SAGE_DATA_DIR")) return std::filesystem::path{d};
        if (const char* h = std::getenv("HOME"))          return std::filesystem::path{h} / ".sage-mcp";
        return std::filesystem::temp_directory_path() / "sage-mcp";
    }();
    const auto graphRoot = sageDataDir / "graph";
    auto graphMgr = std::make_shared<sage::graph::GraphStoreManager>(graphRoot);
    spdlog::info("Knowledge graph root: {}", graphRoot.string());

    // Resolves an explicit slot_id from params, else falls back to the
    // active session, else (when exactly one editor connected) that one.
    auto resolveSlotId = [&bridge](const nlohmann::json& params)
        -> std::expected<std::string, sage::mcp::ErrorObject> {
        if (params.is_object() && params.contains("slot_id")
            && params["slot_id"].is_string()) {
            return params["slot_id"].get<std::string>();
        }
        const auto activeId = bridge.activeSession();
        if (!activeId.empty()) {
            if (auto s = bridge.snapshotSession(activeId)) return s->slot_id;
        }
        const auto sessions = bridge.snapshotSessions();
        if (sessions.size() == 1) return sessions.front().slot_id;
        return std::unexpected(sage::mcp::ErrorObject::fromCode(
            sage::mcp::ErrorCode::EditorNotConnected,
            "no slot_id provided and no unique active editor; "
            "either pass slot_id or call set_active_editor first"));
    };

    // Smoke-test remote tool: round-trips through the connected editor.
    {
        sage::mcp::Tool editorPing{
            .name        = "editor.ping",
            .description = "Round-trips a ping through the connected editor; "
                           "smoke test for bridge dispatch.",
            .inputSchema = nlohmann::json{
                {"type",       "object"},
                {"properties", {{"message", {{"type", "string"}}}}},
                {"additionalProperties", false},
            },
            .handler = nullptr,
            .remote  = true,
        };
        if (auto r = registry->registerTool(std::move(editorPing)); !r.has_value()) {
            spdlog::warn("Failed to register remote tool 'editor.ping'");
        }
    }

    // Actor mutation tools (Milestone 1.3c).
    auto registerRemote = [&registry](sage::mcp::Tool tool) {
        const auto name = tool.name;
        if (auto r = registry->registerTool(std::move(tool)); !r.has_value()) {
            spdlog::warn("Failed to register remote tool '{}'", name);
        }
    };
    auto registerLocal = [&registry](sage::mcp::Tool tool) {
        const auto name = tool.name;
        if (auto r = registry->registerTool(std::move(tool)); !r.has_value()) {
            spdlog::warn("Failed to register local tool '{}'", name);
        }
    };

    registerLocal(sage::mcp::Tool{
        .name = "jobs.list",
        .description = "List detached async Sage jobs started with async:true. "
                       "Returns newest-first job summaries and terminal state.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}},
                {"include_completed_details", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [jobMgr](const nlohmann::json& params) -> sage::mcp::ToolResult {
            const int limit = params.is_object() ? params.value("limit", 50) : 50;
            const bool include = params.is_object()
                && params.value("include_completed_details", false);
            return jobMgr->list(limit, include);
        },
        .remote = false,
    });
    registerLocal(sage::mcp::Tool{
        .name = "jobs.get",
        .description = "Read a detached async Sage job by job_id, including "
                       "final result/error when available.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"job_id", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"job_id"})},
            {"additionalProperties", false},
        },
        .handler = [jobMgr](const nlohmann::json& params) -> sage::mcp::ToolResult {
            const std::string id = params.value("job_id", std::string{});
            if (id.empty()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing job_id"));
            }
            if (auto job = jobMgr->snapshot(id, /*includeResult=*/true)) return *job;
            return std::unexpected(sage::mcp::ErrorObject::fromCode(
                sage::mcp::ErrorCode::InvalidParams, "unknown job_id: " + id));
        },
        .remote = false,
    });
    registerLocal(sage::mcp::Tool{
        .name = "jobs.wait",
        .description = "Wait for a detached async Sage job to reach a terminal "
                       "state, or return the current state with wait_timed_out.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"job_id", {{"type", "string"}}},
                {"timeout_seconds", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}}},
            }},
            {"required", nlohmann::json::array({"job_id"})},
            {"additionalProperties", false},
        },
        .handler = [jobMgr](const nlohmann::json& params) -> sage::mcp::ToolResult {
            const std::string id = params.value("job_id", std::string{});
            const double seconds = params.value("timeout_seconds", 30.0);
            if (id.empty()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing job_id"));
            }
            const auto timeout = std::chrono::milliseconds(
                static_cast<int64_t>(std::clamp(seconds, 0.0, 600.0) * 1000.0));
            if (auto job = jobMgr->wait(id, timeout)) return *job;
            return std::unexpected(sage::mcp::ErrorObject::fromCode(
                sage::mcp::ErrorCode::InvalidParams, "unknown job_id: " + id));
        },
        .remote = false,
    });
    registerLocal(sage::mcp::Tool{
        .name = "jobs.logs",
        .description = "Read structured async job logs from a cursor. Use "
                       "next_cursor for incremental polling.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"job_id", {{"type", "string"}}},
                {"cursor", {{"type", "integer"}, {"minimum", 0}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
            }},
            {"required", nlohmann::json::array({"job_id"})},
            {"additionalProperties", false},
        },
        .handler = [jobMgr](const nlohmann::json& params) -> sage::mcp::ToolResult {
            const std::string id = params.value("job_id", std::string{});
            const std::size_t cursor = static_cast<std::size_t>(
                std::max(0, params.value("cursor", 0)));
            const std::size_t limit = static_cast<std::size_t>(
                std::max(1, params.value("limit", 100)));
            if (id.empty()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing job_id"));
            }
            if (auto logs = jobMgr->logs(id, cursor, limit)) return *logs;
            return std::unexpected(sage::mcp::ErrorObject::fromCode(
                sage::mcp::ErrorCode::InvalidParams, "unknown job_id: " + id));
        },
        .remote = false,
    });
    registerLocal(sage::mcp::Tool{
        .name = "jobs.cancel",
        .description = "Best-effort cancel for a detached async Sage job. "
                       "Unreal editor operations may continue until the active "
                       "tool call returns; state records cancel_requested.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"job_id", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"job_id"})},
            {"additionalProperties", false},
        },
        .handler = [jobMgr](const nlohmann::json& params) -> sage::mcp::ToolResult {
            const std::string id = params.value("job_id", std::string{});
            if (id.empty()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing job_id"));
            }
            if (auto job = jobMgr->cancel(id)) return *job;
            return std::unexpected(sage::mcp::ErrorObject::fromCode(
                sage::mcp::ErrorCode::InvalidParams, "unknown job_id: " + id));
        },
        .remote = false,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "spawn_actor",
        .description = "Spawn an actor in the current editor world. Wrapped in "
                       "FScopedTransaction (undo-friendly). Rejects during PIE "
                       "(api-spec.md §Error Codes -32004). 'class' accepts engine "
                       "paths (/Script/Engine.StaticMeshActor) or Blueprint "
                       "generated-class paths (/Game/.../BP_Foo.BP_Foo_C).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class",    {{"type", "string"},
                              {"description", "UClass path or BP generated-class path"}}},
                {"location", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"label",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "delete_actor",
        .description = "Destroy an actor by full path. FScopedTransaction wrapped. "
                       "Rejects during PIE (-32004). Destructive — pass "
                       "`confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"},
                              {"description", "Full UE path returned by spawn_actor"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "set_transform",
        .description = "Update an actor's transform. At least one of "
                       "location/rotation/scale must be provided. "
                       "FScopedTransaction wrapped. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"}}},
                {"location", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"scale",    {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
            }},
            {"required", nlohmann::json::array({"actor_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "set_visibility",
        .description = "Toggle an actor's editor + game visibility "
                       "(SetActorHiddenInGame + SetIsTemporarilyHiddenInEditor). "
                       "FScopedTransaction wrapped. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"}}},
                {"hidden",   {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "hidden"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "modify_actor_property",
        .description = "Set a UProperty on an actor by name. Phase 1 supports "
                       "primitive types (bool, int, int64, float, double, string, "
                       "name, text, byte). Calls PreEditChange/PostEditChange so "
                       "editor notifications fire. Wrapped in FScopedTransaction. "
                       "Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"}}},
                {"property", {{"type", "string"},
                              {"description", "UProperty name as declared in C++ (e.g. 'bHidden', 'CustomTimeDilation')"}}},
                {"value",    {{"description", "JSON-encoded value matching the property type"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // Component tools (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "add_component",
        .description = "Attach a new UActorComponent to an actor by class. "
                       "RegisterComponent + AddInstanceComponent under "
                       "FScopedTransaction. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id",        {{"type", "string"}}},
                {"component_class", {{"type", "string"},
                                     {"description", "UClass path (e.g. /Script/Engine.StaticMeshComponent)"}}},
                {"component_name",  {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "component_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "remove_component",
        .description = "Destroy an instance component by full path. "
                       "FScopedTransaction wrapped. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"component_id", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"component_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "modify_component_property",
        .description = "Set a UProperty on a component by name. Same primitive "
                       "set as modify_actor_property. PreEditChange/"
                       "PostEditChange + FScopedTransaction.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"component_id", {{"type", "string"}}},
                {"property",     {{"type", "string"}}},
                {"value",        {{"description", "JSON value matching property type"}}},
            }},
            {"required", nlohmann::json::array({"component_id", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "attach",
        .description = "Attach a USceneComponent child to a USceneComponent "
                       "parent (KeepRelativeTransform). Optional 'socket' name. "
                       "FScopedTransaction. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"child_id",  {{"type", "string"}}},
                {"parent_id", {{"type", "string"}}},
                {"socket",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"child_id", "parent_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "detach",
        .description = "Detach a USceneComponent from its parent "
                       "(KeepRelativeTransform). FScopedTransaction. Rejects "
                       "during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"child_id", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"child_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // Asset tools (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "modify_asset_property",
        .description = "Set a UProperty on a content-browser asset by path. "
                       "Same primitive types as modify_actor_property plus "
                       "object refs and TSubclassOf/FClassProperty class paths "
                       "(Blueprint generated classes accepted). "
                       "MarkPackageDirty + FScopedTransaction. Rejects during PIE. "
                       "For UAnimationAsset.Skeleton, routes through "
                       "UAnimationAsset::SetSkeleton with readback diagnostics "
                       "instead of raw reflected assignment.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"},
                                {"description", "Asset path like /Game/MyFolder/MyAsset"}}},
                {"property",   {{"type", "string"}}},
                {"value",      {{"description", "JSON value matching property type"}}},
            }},
            {"required", nlohmann::json::array({"asset_path", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "rename_asset",
        .description = "Rename an asset within the same folder (UEditorAsset"
                       "Subsystem::RenameAsset). FScopedTransaction. PIE rejected. "
                       "Optional overwrite:true requires confirmed:true and "
                       "returns overwrite delete diagnostics if the destination "
                       "had to be cleared first.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
                {"overwrite",   {{"type", "boolean"}}},
                {"confirmed",   {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"source", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "move_asset",
        .description = "Move an asset to a different folder (RenameAsset under "
                       "the hood; semantic alias of rename_asset for cross-folder "
                       "moves). FScopedTransaction. PIE rejected. Optional "
                       "overwrite:true requires confirmed:true and can replace a "
                       "stale/invalid destination package through editor delete "
                       "diagnostics before moving.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
                {"overwrite",   {{"type", "boolean"}}},
                {"confirmed",   {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"source", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "duplicate_asset",
        .description = "Duplicate an asset to a new path. Returns new asset's "
                       "UE path in `new_asset_id`. FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"source", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "delete_asset",
        .description = "Delete an asset by path (UEditorAssetSubsystem::"
                       "DeleteAsset). FScopedTransaction. PIE rejected. "
                       "Pass `confirmed:true` to proceed (destructive). Delete "
                       "failures include blocker diagnostics such as loaded/dirty "
                       "package, redirector, referencer count, unresolved object, "
                       "or read-only package file.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"}}},
                {"confirmed",  {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"asset_path", "confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "save_assets",
        .description = "Save dirty content + world packages. With `paths` array: "
                       "save those specific assets. Without paths: save all dirty. "
                       "`dry_run: true` returns the would-save list without writing. "
                       "Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths",   {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"dry_run", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "get_dirty_assets",
        .description = "List currently-dirty content + world packages "
                       "(in-memory edits not yet saved to disk). Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "discard_changes",
        .description = "Reload packages from disk, discarding in-memory edits. "
                       "Wraps UEditorLoadingAndSavingUtils::ReloadPackages with "
                       "AssumeNegative interaction mode (no UI prompt). Rejects "
                       "during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}}},
            }},
            {"required", nlohmann::json::array({"paths"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // ---- Multi-editor MCP-side query/select (Milestone 1.5a) -----------
    // Local tools — no plugin dispatch needed; query the bridge state.
    auto sessionToJson = [](const sage::bridge::EditorSession& s) {
        return nlohmann::json{
            {"session_id",     s.session_id},
            {"slot_id",        s.slot_id},
            {"label",          s.label},
            {"instance_id",    s.instance_id},
            {"project_id",     s.project_id},
            {"project_path",   s.project_path},
            {"engine_version", s.engine_version},
            {"pid",            s.pid},
        };
    };

    sage::mcp::Tool listEditorsTool{
        .name        = "list_editors",
        .description = "List connected editor sessions (slot, label, instance, "
                       "project, engine version, pid).",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = [&bridge, sessionToJson](const nlohmann::json&) -> sage::mcp::ToolResult {
            const auto sessions = bridge.snapshotSessions();
            nlohmann::json items = nlohmann::json::array();
            for (const auto& s : sessions) items.push_back(sessionToJson(s));
            return nlohmann::json{{"editors", items}, {"count", items.size()}};
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(listEditorsTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'list_editors'");
    }

    sage::mcp::Tool getActiveEditorTool{
        .name        = "get_active_editor",
        .description = "Return the active-editor pointer (server-wide). "
                       "If no active session is set, returns the only connected "
                       "session when there is exactly one, otherwise null.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = [&bridge, sessionToJson](const nlohmann::json&) -> sage::mcp::ToolResult {
            const auto activeId = bridge.activeSession();
            if (!activeId.empty()) {
                if (auto s = bridge.snapshotSession(activeId)) {
                    return sessionToJson(*s);
                }
            }
            // Fallback: if exactly one session connected, return it.
            const auto sessions = bridge.snapshotSessions();
            if (sessions.size() == 1) return sessionToJson(sessions.front());
            return nlohmann::json{{"active", nullptr}, {"connected_count", sessions.size()}};
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(getActiveEditorTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'get_active_editor'");
    }

    sage::mcp::Tool setActiveEditorTool{
        .name        = "set_active_editor",
        .description = "Set the active-editor pointer by session_id or label "
                       "(or instance_id). Returns the resolved editor on success.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"id_or_label", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"id_or_label"})},
            {"additionalProperties", false},
        },
        .handler = [&bridge, sessionToJson](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("id_or_label")
                || !params["id_or_label"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'id_or_label' string"));
            }
            const auto idOrLabel = params["id_or_label"].get<std::string>();
            auto session = bridge.findByIdOrLabel(idOrLabel);
            if (!session.has_value()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::EditorNotConnected,
                    std::string{"no editor matches: "} + idOrLabel));
            }
            bridge.setActiveSession(session->session_id);
            return sessionToJson(*session);
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(setActiveEditorTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'set_active_editor'");
    }

    // ---- Editor state + selection tools (Milestone 1.3c) ---------------
    auto noArgSchema = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json::object()},
        {"additionalProperties", false},
    };

    registerRemote(sage::mcp::Tool{
        .name        = "get_world",
        .description = "Return the current editor world: path, map name, and "
                       "current-level actor count. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_pie_state",
        .description = "Whether PIE is active; if so, returns play-world path, "
                       "live PIE world candidates, and current LevelEditor "
                       "play settings. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "editor.get_play_settings",
        .description = "Return typed ULevelEditorPlaySettings readback for PIE "
                       "automation: PlayNetMode, PlayNumberOfClients, "
                       "RunUnderOneProcess, PrimaryPIEClientIndex, selected "
                       "play mode, and related flags. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_viewport_state",
        .description = "Return basic active viewport metrics (size). Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_selected_actors",
        .description = "Return the currently selected actors as a list of UE "
                       "paths. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "select_actors",
        .description = "Replace the editor selection with the given actor paths. "
                       "Returns selected + not_found arrays.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"actor_ids"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "clear_selection",
        .description = "Clear the editor's current actor selection.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Level tools (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "save_level",
        .description = "Save the current editor world's level package "
                       "(UEditorLoadingAndSavingUtils::SavePackages on the world's "
                       "outermost package). Rejects during PIE.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_current_level",
        .description = "Return current world's level path, map name, actor count, "
                       "and streaming sub-level package names. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // QA / Automation Framework (Milestone 1.7).
    registerRemote(sage::mcp::Tool{
        .name        = "list_tests",
        .description = "List available Automation Framework tests in Editor "
                       "context. Optional 'filter' substring narrows results.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"filter", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "run_tests",
        .description = "Trigger Automation Framework tests in Editor context. "
                       "Filter substring narrows the set; tests run async — the "
                       "tool returns the started list immediately. Result polling "
                       "is deferred to Phase 2.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"filter", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Source control (Milestone 1.7).
    registerRemote(sage::mcp::Tool{
        .name        = "get_source_control_state",
        .description = "Source control module loaded/enabled state and active "
                       "provider name (Perforce, Git, Subversion, ...). Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "checkout_files",
        .description = "Check out files via the active source control provider. "
                       "paths: array of file or asset paths. Returns provider, "
                       "file count, and status (succeeded/failed/cancelled). "
                       "-32005 if SCM disabled or unavailable.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}}},
            }},
            {"required", nlohmann::json::array({"paths"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Compile coordination (Milestone 1.6a — Live Coding wrapper).
    registerRemote(sage::mcp::Tool{
        .name        = "get_live_coding_status",
        .description = "Live Coding availability + state. Cross-platform: returns "
                       "{available:true, ...flags} on Windows when LC module loaded; "
                       "{available:false, reason, platform} elsewhere (UE 5.7's "
                       "Live Coding is Windows-only).",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "compile_and_reload",
        .description = "Trigger Live Coding compile (Windows). Auto-enables for "
                       "session if possible. Async fire-and-forget; poll "
                       "get_live_coding_status to observe completion. Returns "
                       "-32007 LiveCodingUnavailable on macOS/Linux. Full-restart "
                       "orchestration (save→shutdown→UBT→relaunch) deferred to "
                       "Phase 2 per ADR-009.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Optimistic locking (Milestone 1.4c).
    registerRemote(sage::mcp::Tool{
        .name        = "compare_and_set_property",
        .description = "Atomic compare-and-set on a primitive UProperty across "
                       "actor / component / asset targets. Returns -32003 "
                       "VersionConflict if current != expected (with current "
                       "and expected echoed in error.data). On success applies "
                       "value within FScopedTransaction. Same primitive set as "
                       "modify_*_property.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"target_kind", {{"type", "string"},
                                 {"enum", nlohmann::json::array({"actor","component","asset"})}}},
                {"target_id",   {{"type", "string"},
                                 {"description", "UE path; for asset use /Game/.../AssetName"}}},
                {"property",    {{"type", "string"}}},
                {"expected",    {{"description", "Current value the agent expects"}}},
                {"new_value",   {{"description", "Value to write if expected matches"}}},
            }},
            {"required", nlohmann::json::array(
                {"target_kind","target_id","property","expected","new_value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Multi-step transactions (Milestone 1.4b).
    registerRemote(sage::mcp::Tool{
        .name        = "begin_transaction",
        .description = "Open a new editor transaction. All subsequent mutations "
                       "apply within it (their inner FScopedTransaction nests). "
                       "Returns tx_id for commit/rollback. UTransactor is LIFO; "
                       "balance commit/rollback in reverse order of begin.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"label", {{"type", "string"}, {"description", "Display name for Edit > Undo"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "commit_transaction",
        .description = "Finalize an open transaction (GEditor->EndTransaction). "
                       "tx_id must be the most recently opened tx not yet "
                       "committed/rolled-back.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"tx_id", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"tx_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "rollback_transaction",
        .description = "Cancel an open transaction (GEditor->CancelTransaction). "
                       "Reverts every mutation captured since begin_transaction.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"tx_id", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"tx_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_active_transactions",
        .description = "List currently-open Sage transactions (tx_id + UTransactor index). Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Bulk modify (Milestone 1.4a — atomic-by-default multi-op).
    registerRemote(sage::mcp::Tool{
        .name        = "bulk_modify",
        .description = "Apply a sequence of tool calls. atomic=true (default): "
                       "all operations in one FScopedTransaction; first failure "
                       "cancels the transaction (atomic). atomic=false: each op "
                       "runs independently. operations: [{tool, args?}]. "
                       "`bulk_modify` cannot be nested. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"operations", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool", {{"type", "string"}}},
                            {"args", {{"type", "object"}}},
                        }},
                        {"required", nlohmann::json::array({"tool"})},
                    }},
                }},
                {"atomic", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"operations"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // PIE control (Milestone 1.3c → spec'te 1.7'de listelenmişti, hot path).
    registerRemote(sage::mcp::Tool{
        .name        = "run_pie",
        .description = "Start Play-In-Editor. By default uses the editor's "
                       "current play settings in selected viewport. Can use "
                       "transient LevelEditorPlaySettings overrides to force "
                       "standalone/listen/client net mode, client count, "
                       "selected viewport vs new editor window, primary/local "
                       "player index, and single-local-player smoke sessions. "
                       "Does not mutate saved editor defaults. Errors if PIE "
                       "already active.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"force_local_player", {{"type", "boolean"},
                    {"description", "Force standalone, clients=1, RunUnderOneProcess=true, no separate server, and selected viewport unless new_editor_window=true."}}},
                {"single_local_player", {{"type", "boolean"},
                    {"description", "Alias of force_local_player."}}},
                {"net_mode", {{"type", "string"},
                    {"description", "standalone, listen_server, or client. Aliases like listen/client/pie_standalone are accepted."}}},
                {"play_net_mode", {{"type", "string"},
                    {"description", "Alias of net_mode."}}},
                {"clients", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                {"num_clients", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                {"number_of_clients", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                {"play_number_of_clients", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                {"selected_viewport", {{"type", "boolean"}}},
                {"use_selected_viewport", {{"type", "boolean"}}},
                {"new_editor_window", {{"type", "boolean"}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}},
                {"target_local_player_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}},
                {"primary_pie_client_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}},
                {"run_under_one_process", {{"type", "boolean"}}},
                {"launch_separate_server", {{"type", "boolean"}}},
                {"game_gets_mouse_control", {{"type", "boolean"}}},
                {"use_mouse_for_touch", {{"type", "boolean"}}},
                {"allow_online_subsystem", {{"type", "boolean"}}},
                {"restore_settings_after_start", {{"type", "boolean"},
                    {"description", "Accepted for workflow compatibility; run_pie uses transient settings so defaults do not need restoring."}}},
                {"map", {{"type", "string"},
                    {"description", "Optional global map override for the play session."}}},
                {"map_path", {{"type", "string"},
                    {"description", "Alias of map."}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "stop_pie",
        .description = "Request end of the active PIE session "
                       "(GEditor->RequestEndPlayMap). Before teardown it "
                       "purges Python-held PIE UObject wrappers through "
                       "PrepareToCleanseEditorObject, runs Python GC, and "
                       "runs UE GC unless cleanup_python_refs=false. Errors "
                       "if PIE not active.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"cleanup_python_refs", {{"type", "boolean"}}},
                {"clear_python_main_globals", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Reflection (Phase 4.1) ----------------------------------------
    registerRemote(sage::mcp::Tool{
        .name        = "reflect_class",
        .description = "Full UClass dump: name, parent, module, native/abstract/"
                       "interface flags, interfaces implemented, all UProperties "
                       "(type+category+access+replication+tooltip), all UFunctions "
                       "(parameters+return+access+pure+network), and immediate "
                       "child classes. Accepts engine path (/Script/Engine.Pawn) "
                       "or BP generated-class path (/Game/.../BP_Foo.BP_Foo_C). "
                       "Pair with `class_hierarchy` for transitive walks.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class_path",       {{"type", "string"}}},
                {"include_children", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"class_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "reflect_struct",
        .description = "USTRUCT dump: name, module, parent, all fields with full "
                       "type + flags + tooltip. Accepts /Script/CoreUObject.Vector "
                       "or any USTRUCT path.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"struct_path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"struct_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "reflect_enum",
        .description = "UENUM dump: name, module, cpp form, all entries (name + "
                       "value + display name + tooltip). Synthetic _MAX entry "
                       "skipped.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"enum_path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"enum_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "list_classes",
        .description = "Walk the live UClass registry. Filters: substring on name, "
                       "base_class (only subclasses of), include_native, "
                       "include_blueprint. SKEL_/REINST_/HOTRELOADED_ churn "
                       "filtered out. Default 500 max, capped at 5000. Returns "
                       "{name, path, module, is_native, parent}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",            {{"type", "string"}}},
                {"base_class",        {{"type", "string"}}},
                {"include_native",    {{"type", "boolean"}}},
                {"include_blueprint", {{"type", "boolean"}}},
                {"max_results",       {{"type", "integer"},
                                       {"minimum", 1}, {"maximum", 5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "list_structs",
        .description = "Walk the live UScriptStruct registry. Substring filter, "
                       "max_results 1..5000.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",      {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum",1}, {"maximum",5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "list_enums",
        .description = "Walk the live UEnum registry. Substring filter, "
                       "max_results 1..5000.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",      {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum",1}, {"maximum",5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "find_implementers",
        .description = "Classes that implement the given UInterface. Distinct "
                       "from class_hierarchy (which walks INHERITS_FROM); "
                       "interfaces are a separate axis. Returns {name, path, "
                       "module, is_native}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"interface_path", {{"type", "string"}}},
                {"max_results",    {{"type", "integer"}, {"minimum",1}, {"maximum",5000}}},
            }},
            {"required", nlohmann::json::array({"interface_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "class_default_object",
        .description = "Read the class default object's UProperty values as JSON. "
                       "Skips Transient/Deprecated. Read-only; write via Phase 4.2 "
                       "bp.set_cdo_property.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"class_path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"class_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Blueprint authoring (Phase 4.2 — read + write) ---------------
    auto bpPathSchema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"path"})},
        {"additionalProperties", false},
    };
    auto bpPathFnSchema = nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"path",     {{"type", "string"}}},
            {"function", {{"type", "string"}}},
        }},
        {"required", nlohmann::json::array({"path", "function"})},
        {"additionalProperties", false},
    };

    registerRemote(sage::mcp::Tool{
        .name = "bp.read",
        .description = "Blueprint summary: name, parent class, generated class, "
                       "variable/function/event-graph counts, implemented interfaces.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_variables",
        .description = "All UCLASS variables: name, type, default, category, flags "
                       "(EditAnywhere, Replicated, RepNotify, Transient, ...).",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_functions",
        .description = "All BP-side function/event/macro graphs with kind + node count.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_function_graph",
        .description = "All nodes + pins + connections for one graph (function or "
                       "event_graph). Each pin reports direction, type, default, "
                       "and link list with target node id + pin name.",
        .inputSchema = bpPathFnSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_execution_flow",
        .description = "BFS exec-pin walk from FunctionEntry / Event nodes — easier "
                       "for the agent than parsing the full graph. Returns ordered "
                       "[{order, id, class, title}].",
        .inputSchema = bpPathFnSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_components",
        .description = "SCS hierarchy: name, class, parent, attach socket per node. "
                       "Recursive walk from root nodes.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.search_nodes",
        .description = "Substring search across all of a Blueprint's graphs "
                       "(function + event + macro). Returns hits with graph, "
                       "graph_kind, id, title, class.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}}},
                {"keyword", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "keyword"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — variables --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_variable",
        .description = "Add a member variable. type=PinCategory string ('bool', "
                       "'int', 'float', 'string', 'object', 'struct', ...). "
                       "type_object resolves a UClass/UStruct path for object/"
                       "struct types. is_array=true for TArray. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"name",          {{"type", "string"}}},
                {"type",          {{"type", "string"}}},
                {"type_object",   {{"type", "string"}}},
                {"is_array",      {{"type", "boolean"}}},
                {"default_value", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name", "type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_variable",
        .description = "Remove a member variable + fix up references. PIE rejected. "
                       "Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"name",      {{"type", "string"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.rename_variable",
        .description = "Rename a BP-defined member variable in place. Wraps "
                       "FBlueprintEditorUtils::RenameMemberVariable — updates "
                       "the FBPVariableDescription, rewrites every "
                       "K2Node_VariableGet/Set reference, renames the "
                       "OnRep_<Name> RepNotify function. Validation rejects: "
                       "name collision, inherited variable, non-C++-identifier, "
                       "C++ reserved keywords. Idempotent (returns already=true "
                       "when old==new). Returns {old_name, new_name, "
                       "references_updated, compiled}. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"old_name", {{"type", "string"}}},
                {"new_name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "old_name", "new_name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.sanitize_variable_names",
        .description = "Bulk-rename every BP member variable whose FName is not "
                       "a valid C++ identifier into a sanitized form. Algorithm: "
                       "Turkish transliteration → strip non-[A-Za-z0-9_] → "
                       "collapse underscores → leading-digit fix (_) → "
                       "C++-keyword fix (_Var suffix) → 128-char cap. "
                       "Sanitised collisions resolve with _2, _3... suffixes. "
                       "dry_run defaults to true (preview only). Each mapping "
                       "entry carries a comma-separated reason "
                       "(spaces/special_chars/non_ascii/cpp_keyword/number_prefix/"
                       "transliteration/truncated). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}}},
                {"dry_run", {{"type", "boolean"}}},
                {"exclude", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_variable_type",
        .description = "Re-type an existing BP member variable. Wraps "
                       "FBlueprintEditorUtils::ChangeMemberVariableType. type "
                       "values match bp.add_variable (bool/int/real/string/"
                       "object/class/struct/byte/name/text). type_object "
                       "resolves UClass/UStruct path for reference types. "
                       "is_array=true for TArray. Idempotent on same type "
                       "(already=true). Use this to fix the variable-type "
                       "degrade that follows a Component reparent (BP child "
                       "class → C++ parent class). Returns {old_type, new_type, "
                       "default_value_preserved, references_refreshed, "
                       "compiled}. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"name",        {{"type", "string"}}},
                {"type",        {{"type", "string"}}},
                {"type_object", {{"type", "string"}}},
                {"is_array",    {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "name", "type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.clear_graphs",
        .description = "Empty every K2Node out of a Blueprint's event/function/"
                       "macro graphs while preserving graph shells. Step 6 of "
                       "the BP→C++ conversion playbook: once C++ implements the "
                       "behaviour, clearing the BP graphs prevents drift and "
                       "makes the subsequent reparent's pin-orphan errors "
                       "impossible (no nodes left to hold stale references). "
                       "scope ∈ {all, event_graph, functions, macros}. "
                       "keep_entry_nodes (default true) preserves "
                       "UFunctionEntry/Result/Tunnel so function signatures "
                       "survive. keep_event_entries (default false) preserves "
                       "UK2Node_Event placeholders. Recurses into composite "
                       "sub-graphs. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",               {{"type", "string"}}},
                {"scope",              {{"type", "string"},
                                        {"enum", nlohmann::json::array({"all", "event_graph", "functions", "macros"})}}},
                {"keep_entry_nodes",   {{"type", "boolean"}}},
                {"keep_event_entries", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_all_variables",
        .description = "Bulk RemoveMemberVariable across BP->NewVariables. "
                       "Inherited and SCS-component variables are not in "
                       "NewVariables and remain untouched. except[] preserves "
                       "named variables. dry_run lists what would be deleted "
                       "without writing. Returns {deleted[], "
                       "kept{excepted, inherited, scs_generated}, compiled}. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}}},
                {"except",  {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"dry_run", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_variable_default",
        .description = "Set a member variable's default value (string-coerced). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"name",  {{"type", "string"}}},
                {"value", {{"description", "JSON value (string/number/bool)"}}},
            }},
            {"required", nlohmann::json::array({"path", "name", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — local variables (Phase 4.2 round 2b) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_local_variables",
        .description = "Enumerate function-scope (local) variables on the "
                       "function's UK2Node_FunctionEntry. Returns "
                       "{variables: [{name, type, type_object?, is_array?, "
                       "is_map?, is_set?, category, default_value?, flags}], "
                       "count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_local_variable",
        .description = "Add a function-scope variable via "
                       "FBlueprintEditorUtils::AddLocalVariable. Same type "
                       "shape as bp.add_variable: type (PinCategory: bool/"
                       "int/float/double/string/name/object/struct/...), "
                       "type_object? (sub-category UClass/UStruct/UEnum), "
                       "is_array?, default_value?. -32602 on duplicate name. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"function",      {{"type", "string"}}},
                {"name",          {{"type", "string"}}},
                {"type",          {{"type", "string"}}},
                {"type_object",   {{"type", "string"}}},
                {"is_array",      {{"type", "boolean"}}},
                {"default_value", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "name", "type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_local_variable",
        .description = "Remove a function-scope variable from the "
                       "UK2Node_FunctionEntry::LocalVariables array. Returns "
                       "{removed: int} (0 = name not found, no error). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"name",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — interfaces (Phase 4.2 round 2c) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_interfaces",
        .description = "Enumerate UInterface classes implemented by the BP. "
                       "Returns {interfaces: [{name, path, graph_count}], "
                       "count}. Reads UBlueprint::ImplementedInterfaces.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_interface",
        .description = "Implement a UInterface on the BP via "
                       "FBlueprintEditorUtils::ImplementNewInterface. For "
                       "AnimLayerInterface targets, Sage bypasses UE's "
                       "ImplementNewInterface convenience flow, adds the "
                       "interface entry directly, regenerates colliding "
                       "interface graph GUIDs, compiles once inside the tool, "
                       "and restores any engine-side LinkedAnimLayer drift so "
                       "this tool does not mutate AnimGraph call nodes; use "
                       "animation.add_linked_anim_layer_node explicitly for "
                       "new layer calls. "
                       "interface_path: '/Script/Foo.UMyInterface' or BP "
                       "interface asset path. Idempotent — already-implemented "
                       "returns {already: true}. Class must derive from "
                       "UInterface or -32602. PIE rejected. Net linked-layer "
                       "mutation counters remain modified=0/spawned=0; "
                       "snapshot/restored/removed/GUID counters report any UE "
                       "side effects that were prevented or repaired.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",           {{"type", "string"}}},
                {"interface_path", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "interface_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.remove_interface",
        .description = "Remove an implemented UInterface from the BP via "
                       "FBlueprintEditorUtils::RemoveInterface. "
                       "preserve_functions=true keeps the interface's "
                       "function graphs as regular BP functions (default "
                       "false: drops them). Returns {removed: 0|1} — 0 when "
                       "the interface wasn't implemented (idempotent). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",               {{"type", "string"}}},
                {"interface_path",     {{"type", "string"}}},
                {"preserve_functions", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "interface_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — variable props + CDO + deps (Phase 4.2 round 2g/p5) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_variable_properties",
        .description = "Toggle BP member variable property flags + metadata. "
                       "All optional bools layer onto FBPVariableDescription"
                       "::PropertyFlags: instance_editable (CPF_Edit), "
                       "blueprint_readonly (CPF_BlueprintReadOnly), "
                       "replicated (CPF_Net; clears RepNotify on false), "
                       "transient (CPF_Transient), save_game (CPF_SaveGame), "
                       "expose_on_spawn (CPF_ExposeOnSpawn + MD_ExposeOnSpawn "
                       "metadata). Strings: category (MD_FunctionCategory), "
                       "tooltip (MD_Tooltip). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",                {{"type", "string"}}},
                {"name",                {{"type", "string"}}},
                {"instance_editable",   {{"type", "boolean"}}},
                {"blueprint_readonly",  {{"type", "boolean"}}},
                {"replicated",          {{"type", "boolean"}}},
                {"transient",           {{"type", "boolean"}}},
                {"save_game",           {{"type", "boolean"}}},
                {"expose_on_spawn",     {{"type", "boolean"}}},
                {"category",            {{"type", "string"}}},
                {"tooltip",             {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_cdo_properties",
        .description = "Read the Class Default Object of any UClass — "
                       "engine native (e.g. /Script/Engine.Actor) or BP-"
                       "generated. Optional 'properties' array filters to "
                       "specific names. Skips transient. Returns "
                       "{class, class_name, properties: {...}, count}. "
                       "Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class",      {{"type", "string"}}},
                {"properties", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"recurse_instanced", {{"type", "boolean"}}},
                {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
            }},
            {"required", nlohmann::json::array({"class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_dependencies",
        .description = "Forward (default) or reverse (reverse=true) asset "
                       "dependencies via AssetRegistry. Forward also "
                       "enumerates class refs (parent + variable subtypes). "
                       "Returns {dependencies[], dependency_count, "
                       "referenced_classes[]} or {referencers[], "
                       "referencer_count} based on direction.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}}},
                {"reverse", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — diagnostics + dry-run (Phase 4.2 round 2g/p4) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.validate",
        .description = "Compile the BP via FKismetEditorUtilities::"
                       "CompileBlueprint with SkipSave + a silent "
                       "FCompilerResultsLog and return the diagnostics. "
                       "Useful before bp.compile to surface errors / "
                       "warnings without dirtying the package. Returns "
                       "{valid, error_count, warning_count, messages: "
                       "[{severity, message}]} — severity ∈ {error, "
                       "warning, perf, info}. PIE rejected (compile is "
                       "still a write operation under the hood).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.run_construction_script",
        .description = "Spawn a transient instance of the BP into the "
                       "editor world, let SpawnActor + RerunConstruction"
                       "Scripts run, snapshot the resulting components + "
                       "transforms, then destroy. Useful to inspect what "
                       "the construction script produces without leaving "
                       "an actor in the level. location {x,y,z} optional "
                       "(default origin). BP must be Actor-derived. "
                       "Returns {class, components: [{name, class, "
                       "location, rotation, scale, is_root}], count}. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"location", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — SCS component deep CRUD (Phase 4.2 round 2g/p3) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_component_properties",
        .description = "Dump every reflected UProperty on the BP's SCS "
                       "component template (USCS_Node->ComponentTemplate). "
                       "Skips transient / DuplicateTransient. Uses Sage's "
                       "GetUPropertyAsJson — primitives, structs (Vector/"
                       "Rotator/Transform/Color/...), object refs, "
                       "TArray/TMap/TSet, enums all round-tripped. Returns "
                       "{class, properties: {...}, count}. Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"component", {{"type", "string"},
                               {"description", "SCS variable name (e.g. 'Mesh', 'Capsule')"}}},
            }},
            {"required", nlohmann::json::array({"path", "component"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_component_property",
        .description = "Read a single UProperty on a SCS component "
                       "template. Cheaper than bp.read_component_properties "
                       "when only one value is needed. Returns "
                       "{type (FProperty class name), value}. -32602 if "
                       "the property doesn't exist on the component class.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"component", {{"type", "string"}}},
                {"property",  {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "component", "property"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.reparent_component",
        .description = "Move a SCS component under a different parent in "
                       "the BP's component hierarchy (USCS_Node tree). "
                       "Cycle-guarded — rejects with -32602 if the new "
                       "parent is a descendant of the moved component. "
                       "Detaches from the current parent (or root list) "
                       "and attaches to the new parent. PIE rejected. "
                       "Both component and new_parent must be "
                       "USceneComponent descendants.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",       {{"type", "string"}}},
                {"component",  {{"type", "string"}}},
                {"new_parent", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "component", "new_parent"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — T3D node clipboard (Phase 4.2 round 2g/p2) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_component",
        .description = "Add a component template to an Actor Blueprint's "
                       "SimpleConstructionScript. component_class accepts "
                       "native class paths or Blueprint generated classes. "
                       "variable_name is idempotent: an existing component "
                       "with the same class returns already_exists=true; "
                       "class/name collisions are rejected. Optional parent "
                       "may reference a local, inherited, or native scene "
                       "component. Recompiles by default; pass compile:false "
                       "or recompile:false to defer. Pass save:true to save "
                       "the Blueprint asset after mutation.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",            {{"type", "string"}}},
                {"component_class", {{"type", "string"}}},
                {"variable_name",   {{"type", "string"}}},
                {"parent",          {{"type", "string"}}},
                {"compile",         {{"type", "boolean"}}},
                {"recompile",       {{"type", "boolean"}}},
                {"save",            {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "component_class", "variable_name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.copy_component_defaults",
        .description = "Copy named reflected component-template defaults "
                       "from one Blueprint component to another. Resolves "
                       "local SCS templates, inherited SCS templates "
                       "(creating an inheritable override for the target), "
                       "and CDO/native component fallbacks. properties[] is "
                       "preflighted for existence and matching property type "
                       "before mutation. Recompiles target by default; pass "
                       "compile:false or recompile:false to defer. Pass "
                       "save:true to save the target Blueprint asset.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source_path",      {{"type", "string"}}},
                {"from_path",        {{"type", "string"}}},
                {"source_component", {{"type", "string"}}},
                {"from_component",   {{"type", "string"}}},
                {"target_path",      {{"type", "string"}}},
                {"to_path",          {{"type", "string"}}},
                {"target_component", {{"type", "string"}}},
                {"to_component",     {{"type", "string"}}},
                {"properties",       {{"type", "array"},
                                      {"items", {{"type", "string"}}},
                                      {"minItems", 1}}},
                {"compile",          {{"type", "boolean"}}},
                {"recompile",        {{"type", "boolean"}}},
                {"save",             {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"source_path", "source_component", "properties"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.export_nodes_t3d",
        .description = "Export BP graph nodes to UE's T3D ASCII format via "
                       "FEdGraphUtilities::ExportNodesToText. Mirrors the "
                       "editor's Copy operation. node_ids[] selects specific "
                       "nodes by FGuid; omit/empty exports the whole graph. "
                       "Entry/return nodes are skipped (CanDuplicateNode "
                       "filter). Returns {t3d, count, skipped}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"path", "function"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.import_nodes_t3d",
        .description = "Import T3D-formatted nodes into a BP graph via "
                       "FEdGraphUtilities::ImportNodesFromText. Pre-checks "
                       "with CanImportNodesFromText (-32602 on schema "
                       "mismatch / malformed text). Pasted nodes get fresh "
                       "FGuids so re-pasting into the same graph doesn't "
                       "collide. Optional pos_x + pos_y anchors the pasted "
                       "set's centroid at the given position. Returns "
                       "{count, node_ids[], recentered, _warning?}. PIE "
                       "rejected. When the destination graph already has "
                       "entry/return nodes, those nodes in the T3D are "
                       "silently dropped; a `_warning` field is emitted "
                       "with the dropped count.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"t3d",      {{"type", "string"}}},
                {"pos_x",    {{"type", "number"}}},
                {"pos_y",    {{"type", "number"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "t3d"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — event dispatchers (Phase 4.2 round 2g/p1) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_event_dispatchers",
        .description = "Enumerate event dispatchers (multicast delegates) "
                       "on the BP. Reads UBlueprint::DelegateSignatureGraphs. "
                       "Each entry: {name, graph, node_count, parameters: "
                       "[{name, type, direction:input}]}. Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_event_dispatcher",
        .description = "Create a new event dispatcher. Wires up both halves: "
                       "the '<Name>__DelegateSignature' graph in "
                       "DelegateSignatureGraphs (UEdGraphSchema_K2 default "
                       "nodes) AND a member variable of type PC_MCDelegate "
                       "referencing the signature graph. Use "
                       "bp.add_function_parameter on the signature graph to "
                       "configure dispatcher payload types. -32602 on "
                       "duplicate name. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.remove_event_dispatcher",
        .description = "Remove a dispatcher: deletes the signature graph "
                       "AND the member variable. Returns {removed: 0|1} "
                       "(idempotent — 0 when name absent on both sides). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — asset creation (Phase 4.2 round 2f) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.create",
        .description = "Create a new Blueprint asset via UBlueprintFactory + "
                       "IAssetTools::CreateAsset. parent_class accepts a "
                       "full path ('/Script/Engine.Actor') or a short name "
                       "('Actor', 'Pawn', 'Character'); defaults to Actor. "
                       "path: '/Game/Folder/BP_Foo' or '/Game/Folder/BP_Foo."
                       "BP_Foo'. Idempotent — returns {already: true} if "
                       "the asset already exists. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",         {{"type", "string"}}},
                {"parent_class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.create_interface",
        .description = "Create a new Blueprint Interface asset via "
                       "UBlueprintInterfaceFactory. ParentClass is "
                       "UInterface (set by the factory). path same form "
                       "as bp.create. Idempotent. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — function parameter I/O (Phase 4.2 round 2e) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_function_parameters",
        .description = "Enumerate user-defined input/output parameters on "
                       "a BP function. Inputs come from "
                       "UK2Node_FunctionEntry::UserDefinedPins, outputs "
                       "from UK2Node_FunctionResult::UserDefinedPins (may "
                       "be empty if no FunctionResult node yet). Returns "
                       "{inputs: [{name, type, direction, type_object?, "
                       "is_array?, default_value?}], outputs: [...], "
                       "input_count, output_count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_function_parameter",
        .description = "Add a parameter (input or output) to a BP function "
                       "via UK2Node_EditablePinBase::CreateUserDefinedPin. "
                       "direction='input' (default) targets FunctionEntry, "
                       "'output' targets FunctionResult (auto-spawned if "
                       "the function has no result node yet). Same type "
                       "shape as bp.add_variable. -32602 on duplicate name. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"function",    {{"type", "string"}}},
                {"name",        {{"type", "string"}}},
                {"type",        {{"type", "string"}}},
                {"direction",   {{"type", "string"},
                                 {"enum", nlohmann::json::array({"input","output"})}}},
                {"type_object", {{"type", "string"}}},
                {"is_array",    {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path","function","name","type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.remove_function_parameter",
        .description = "Remove a parameter from a BP function via "
                       "RemoveUserDefinedPinByName on the appropriate "
                       "Entry / Result node. direction='input' (default) "
                       "or 'output'. Returns {removed: 0|1} (idempotent — "
                       "0 when name absent or no result node). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"function",  {{"type", "string"}}},
                {"name",      {{"type", "string"}}},
                {"direction", {{"type", "string"},
                               {"enum", nlohmann::json::array({"input","output"})}}},
            }},
            {"required", nlohmann::json::array({"path","function","name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — graph management (Phase 4.2 round 2d) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_graphs",
        .description = "Enumerate all UEdGraphs on the BP. Returns "
                       "{graphs: [{name, kind, node_count}], count} where "
                       "kind ∈ {ubergraph, function, delegate, macro}. "
                       "Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.rename_function",
        .description = "Rename a user function graph via "
                       "FBlueprintEditorUtils::RenameGraph. Reflects on the "
                       "compiled UFunction at next compile. -32602 if "
                       "old_name not found, new_name already exists, or "
                       "old_name == new_name. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"old_name", {{"type", "string"}}},
                {"new_name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "old_name", "new_name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — functions --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_function",
        .description = "Create a new user function graph. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_function",
        .description = "Remove a user function graph. PIE rejected. "
                       "Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"name",      {{"type", "string"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — graph nodes --
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_node",
        .description = "Remove a node from a graph by GUID. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_id",  {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "node_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.connect_pins",
        .description = "Connect two pins. Schema-validated; rejects type-incompatible "
                       "links. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"function",  {{"type", "string"}}},
                {"from_node", {{"type", "string"}}},
                {"from_pin",  {{"type", "string"}}},
                {"to_node",   {{"type", "string"}}},
                {"to_pin",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","function","from_node","from_pin","to_node","to_pin"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — graph node CRUD (Phase 4.2 round 2a) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_node",
        .description = "Spawn a UEdGraphNode subclass into the target graph. "
                       "node_class accepts a full class name (K2Node_*) or one of "
                       "the aliases: CallFunction, Event, CustomEvent, GetVar, "
                       "SetVar, Branch (=K2Node_IfThenElse), If. node_params for "
                       "CallFunction: {function_name, target_class}. node_params "
                       "for GetVar/SetVar: {variable_name}. Returns "
                       "{node_id (guid), node_class, pos, pins[]}. PIE rejected. "
                       "Reject AnimGraph contexts; use "
                       "`animation.add_animgraph_node` instead.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"function",    {{"type", "string"}}},
                {"node_class",  {{"type", "string"}}},
                {"node_x",      {{"type", "number"}}},
                {"node_y",      {{"type", "number"}}},
                {"node_params", {{"type", "object"}}},
            }},
            {"required", nlohmann::json::array({"path","function","node_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_node_property",
        .description = "Set a pin's default value. Goes through the schema's "
                       "TrySetDefaultValue (type-coerces and validates). "
                       "Execution pins are rejected — use bp.connect_pins. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_id",  {{"type", "string"}}},
                {"pin",      {{"type", "string"}}},
                {"value",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path","function","node_id","pin","value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_node_property",
        .description = "Read a pin's default value + type metadata. Returns "
                       "{type, direction, default_value, default_object?, "
                       "default_text?, is_execution, link_count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_id",  {{"type", "string"}}},
                {"pin",      {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path","function","node_id","pin"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_node_types",
        .description = "Enumerate concrete UK2Node subclasses available to the "
                       "BP palette. filter (substring match on class name), max "
                       "(default 200, clamped 1..2000). Returns {types: "
                       "[{name, module}], returned, total}. Excludes Abstract / "
                       "Deprecated / NewerVersionExists classes.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter", {{"type", "string"}}},
                {"max",    {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — class shape --
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_cdo_property",
        .description = "Write a property on the BP's class default object. Goes "
                       "through Phase 4.0 reflection (TArray/TMap/TObjectPtr/"
                       "FStruct/UEnum all supported). Marks BP structurally "
                       "modified. Supports dry_run/validate_only reflection "
                       "validation and reports before/after plus owner class. "
                       "Refreshes stale generated-class layout after reparent "
                       "when an inherited parent property is not yet visible. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"property", {{"type", "string"}}},
                {"value",    {{"description", "JSON value matching property type"}}},
                {"dry_run",  {{"type", "boolean"}}},
                {"validate_only", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.reparent",
        .description = "Change BP's parent class. Refreshes all nodes for the new "
                       "parent's interface and compiles so inherited native CDO "
                       "properties are immediately visible. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",             {{"type", "string"}}},
                {"new_parent_class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "new_parent_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- compile --
    registerRemote(sage::mcp::Tool{
        .name = "bp.compile",
        .description = "Trigger full BP compile. Returns {success, errors, "
                       "warnings, notes}. PIE rejected.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });

    // -- refresh nodes (post-reparent pin cleanup) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.refresh_nodes",
        .description = "Reconstruct every node in every graph of the Blueprint "
                       "(per-node Node->ReconstructNode + final RefreshAllNodes "
                       "+ Compile). Engine's bp.reparent path only refreshes a "
                       "subset; AnimGraph nodes and UMG variable getters keep "
                       "pin descriptors cached against the old parent's "
                       "layout, leaving 'In use pin no longer exists' errors "
                       "after a class-layout change. Use this after reparent "
                       "if compile reports broken-pin errors. Returns "
                       "{blueprint, nodes_reconstructed, graphs_visited}. "
                       "PIE rejected.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });

    // ---- Material graph (Phase 4.3) ------------------------------------
    registerRemote(sage::mcp::Tool{
        .name = "mat.read",
        .description = "Material/instance summary: domain, blend_mode, "
                       "shading_model, two_sided, expression count, base "
                       "material name.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.list_parameters",
        .description = "Scalar/vector/texture/static-switch parameters with name + kind.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.list_expressions",
        .description = "All graph nodes (UMaterialExpression*) with id, class, "
                       "x/y position, and shorthand value when applicable "
                       "(scalar/vector/parameter constants).",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.create_instance",
        .description = "Create a UMaterialInstanceConstant from a parent material. "
                       "destination is the new asset's path "
                       "(e.g. /Game/Mats/MI_Foo). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"parent",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"parent", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.add_expression",
        .description = "Add a UMaterialExpression node. expression_class accepts "
                       "engine-path form (/Script/Engine.MaterialExpressionConstant). "
                       "Returns the new node's expression_id (GUID). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",             {{"type", "string"}}},
                {"expression_class", {{"type", "string"}}},
                {"x",                {{"type", "integer"}}},
                {"y",                {{"type", "integer"}}},
            }},
            {"required", nlohmann::json::array({"path", "expression_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.delete_expression",
        .description = "Remove an expression by GUID. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"expression_id", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "expression_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.connect_expressions",
        .description = "Wire two expression outputs/inputs. from_output / to_input "
                       "are pin name strings. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"from_id",     {{"type", "string"}}},
                {"to_id",       {{"type", "string"}}},
                {"from_output", {{"type", "string"}}},
                {"to_input",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","from_id","to_id","from_output","to_input"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.connect_to_property",
        .description = "Wire an expression output into a material property. "
                       "property: BaseColor, Metallic, Roughness, Specular, "
                       "EmissiveColor, Normal, Opacity, OpacityMask, "
                       "WorldPositionOffset. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"from_id",     {{"type", "string"}}},
                {"from_output", {{"type", "string"}}},
                {"property",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","from_id","from_output","property"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.set_expression_value",
        .description = "Edit a constant in the graph. Constant→number, "
                       "Constant3Vector→[r,g,b]/[r,g,b,a], ScalarParameter→"
                       "default value. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"expression_id", {{"type", "string"}}},
                {"value",         {{"description", "number / [r,g,b] / [r,g,b,a]"}}},
            }},
            {"required", nlohmann::json::array({"path","expression_id","value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.connect_texture",
        .description = "Set the UTexture on a TextureBase expression "
                       "(e.g. TextureSample). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"expression_id", {{"type", "string"}}},
                {"texture",       {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","expression_id","texture"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.set_shading_model",
        .description = "Switch the base material's shading model (Unlit, "
                       "DefaultLit, Subsurface, ClearCoat, Hair, Cloth, Eye, "
                       "ThinTranslucent, ...). Recompiles. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"model", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path","model"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.set_base_color",
        .description = "Adds a Constant3Vector at -300/0 and wires it into BaseColor. "
                       "color: [r,g,b] or [r,g,b,a] (linear, 0..1). Recompiles. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"color", {{"type", "array"}, {"items", {{"type", "number"}}},
                           {"minItems", 3}, {"maxItems", 4}}},
            }},
            {"required", nlohmann::json::array({"path","color"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.validate",
        .description = "Recompile the material (or refresh the instance) and "
                       "report success.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });

    // ---- Asset advanced (Phase 4.5) ------------------------------------
    registerRemote(sage::mcp::Tool{
        .name = "asset.get_mesh_bounds",
        .description = "Static or skeletal mesh bounding box + extent + sphere "
                       "radius + local-space min/max. Read-only.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.get_mesh_collision",
        .description = "Mesh collision primitive counts (box/sphere/capsule/"
                       "convex) + total + collision complexity flag. Read-only.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.list_redirectors",
        .description = "List UObjectRedirector assets under a folder (default "
                       "/Game). Returned for cleanup planning before "
                       "fixup_redirectors.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"folder", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.diagnose_registry",
        .description = "AssetRegistry health snapshot: total, in_memory, "
                       "on_disk_only, transient. Useful when the agent suspects "
                       "stale registry state.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.bulk_rename",
        .description = "Atomic multi-rename in a single transaction. "
                       "renames=[{src, dst}]. Each entry uses the same "
                       "UEditorAssetSubsystem::RenameAsset; failures are "
                       "reported per-row with ok:false. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"renames", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"src", {{"type", "string"}}},
                            {"dst", {{"type", "string"}}},
                        }},
                        {"required", nlohmann::json::array({"src","dst"})},
                    }},
                }},
            }},
            {"required", nlohmann::json::array({"renames"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.move_folder",
        .description = "Move every asset under src into dst, preserving "
                       "subfolder structure. UE leaves redirectors at the "
                       "old paths automatically — pair with "
                       "asset.fixup_redirectors to clean up. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"src", {{"type", "string"}}},
                {"dst", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"src","dst"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.fixup_redirectors",
        .description = "Resolve referencers and remove redirectors under the "
                       "given folders (default /Game). Uses IAssetTools::"
                       "FixupReferencers — referencing assets get re-saved "
                       "to point at the new path, then the redirector is "
                       "deleted. PIE rejected. Pass `confirmed:true` to "
                       "proceed (destructive — mid-refactor redirect chains "
                       "will be lost; FixupReferencers rewrites referencing "
                       "assets and deletes the redirectors).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths",     {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Project introspection (Phase 4.7 batch 1) -----------------------
    registerRemote(sage::mcp::Tool{
        .name = "project.get_info",
        .description = "Read .uproject metadata: project_name, project_dir, "
                       "engine_dir, engine_association, description, "
                       "category, declared_modules[], plugins[]. Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.list_modules",
        .description = "Walk Source/<Module>/<Module>.Build.cs AND "
                       "Plugins/<X>/Source/<Module>/<Module>.Build.cs and "
                       "report each native module: {name, module_dir, "
                       "build_cs_path, header_count, source_count, plugin?}. "
                       "Plugin-owned modules carry the parent plugin's name. "
                       "Lightweight discovery before drilling into a "
                       "specific module via read_cpp_header.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.read_cpp_header",
        .description = "Read a .h file and regex-scan for UCLASS / "
                       "USTRUCT / UENUM declarations + #include directives. "
                       "path is relative to project root or absolute "
                       "(must be under ProjectDir or EngineDir for "
                       "safety). Returns {classes[], structs[], enums[], "
                       "includes[], line_count}. Each declaration is "
                       "{name, line}. NOT a full UHT parse — heuristic "
                       "regex; gets ~95% of common UE headers right.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.read_cpp_source",
        .description = "Read a .h / .cpp / .inl source file as a string, "
                       "capped at max_bytes (default 64KB, clamped "
                       "1KB..512KB). Path same form as read_cpp_header. "
                       "Returns {content, size_bytes, truncated, "
                       "max_bytes}. truncated=true means orig file was "
                       "larger and content has been Left()'d.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"max_bytes", {{"type", "integer"},
                               {"minimum", 1024}, {"maximum", 524288}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Project introspection batch 2 (Phase 4.7 batch 2) -------------
    registerRemote(sage::mcp::Tool{
        .name = "project.search_cpp",
        .description = "Substring search across the project's Source/ subtree "
                       "(.h, .cpp, .inl). Case-sensitive. By default also "
                       "scans every project-local plugin's Source/ folder "
                       "(Plugins/<X>/Source/) — opt out with "
                       "include_plugins=false. Returns {hits, count, capped, "
                       "root, plugin_roots?}. max_results clamped 1..500 "
                       "(default 50). Snippet trimmed/capped at 200 chars.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query",           {{"type", "string"}}},
                {"max_results",     {{"type", "integer"},
                                     {"minimum", 1}, {"maximum", 500}}},
                {"include_plugins", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"query"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.list_engine_modules",
        .description = "Enumerate engine native modules under Engine/Source/"
                       "{Runtime,Editor,Developer,ThirdParty}. Returns "
                       "{name, category, module_dir} per module.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.read_engine_header",
        .description = "Same shape as project.read_cpp_header but the path "
                       "must live under EngineDir (the safety guard rejects "
                       "anywhere else). Convenience alias so the agent's "
                       "intent is clear.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.find_engine_symbol",
        .description = "Substring search across an engine source category "
                       "tree (.h + .cpp). category ∈ {Runtime (default), "
                       "Editor, Developer, ThirdParty}. max_results clamped "
                       "1..500 (default 50). Returns same shape as "
                       "search_cpp plus the category and root.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"symbol",      {{"type", "string"}}},
                {"category",    {{"type", "string"},
                                 {"enum", nlohmann::json::array({
                                     "Runtime","Editor","Developer","ThirdParty"})}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
            }},
            {"required", nlohmann::json::array({"symbol"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Project introspection batch 3 (Phase 4.7 batch 3) -------------
    registerRemote(sage::mcp::Tool{
        .name = "project.read_config",
        .description = "Read a UE INI config file from the project's "
                       "Config/ directory. name accepts 'Game' / "
                       "'DefaultGame' / 'DefaultGame.ini' (all resolve to "
                       "Config/DefaultGame.ini). raw=true returns the "
                       "verbatim file content; default returns parsed "
                       "{sections: [{name, entries: [{key, value, "
                       "modifier?}]}]}. modifier captures leading +/-/!/. "
                       "tokens (UE INI array op syntax). -32602 on missing "
                       "file.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}},
                {"raw",  {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.search_config",
        .description = "Substring search across all *.ini under "
                       "Config/. Returns same {hits, count, capped} shape "
                       "as project.search_cpp.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query",       {{"type", "string"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
            }},
            {"required", nlohmann::json::array({"query"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.list_config_tags",
        .description = "Enumerate gameplay tags declared across all INI "
                       "files in the project's Config/ tree. Parses lines "
                       "of the form '+GameplayTagList=(Tag=\"Foo.Bar\", "
                       "DevComment=\"...\")'. Returns "
                       "{tags: [{tag, dev_comment?, source, line}], count}. "
                       "Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Project introspection batch 4 (Phase 4.7-p4 write) ------------
    registerRemote(sage::mcp::Tool{
        .name = "project.set_config",
        .description = "Write a single key=value into a project INI under "
                       "[section]. Backup-then-rename atomic: <path>.sage_bak "
                       "captures the previous content, <path>.sage_tmp is "
                       "the staging file. modifier ∈ {'', '+', '-', '!', "
                       "'.'} captures UE's INI array-op tokens. If the "
                       "file or section doesn't exist yet, both are "
                       "bootstrapped — set_config can be the first writer. "
                       "Editor must reload the config (restart_editor or "
                       "manual reload) for the change to take effect at "
                       "runtime.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"name",     {{"type", "string"},
                              {"description", "Game / DefaultGame / DefaultGame.ini all resolve same"}}},
                {"section",  {{"type", "string"},
                              {"description", "Bracketed section header without brackets"}}},
                {"key",      {{"type", "string"}}},
                {"value",    {{"type", "string"},
                              {"description", "Verbatim value — quote if it contains commas"}}},
                {"modifier", {{"type", "string"},
                              {"enum", nlohmann::json::array({"", "+", "-", "!", "."})}}},
            }},
            {"required", nlohmann::json::array({"name", "section", "key", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.set_plugin_enabled",
        .description = "Set a plugin's Enabled flag in .uproject's "
                       "Plugins[] array. Updates an existing entry or "
                       "appends a fresh {Name, Enabled} entry. Atomic "
                       "write of the .uproject. Editor restart is required "
                       "for the new state to take effect. Returns "
                       "{plugin, enabled, uproject_path, entry_existed, "
                       "requires_editor_restart, note}. Destructive — "
                       "modifying .uproject Plugins[] during a running "
                       "editor session has non-trivial side effects "
                       "(modules unload, refs dangle, restart required). "
                       "Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"plugin",    {{"type", "string"}}},
                {"enabled",   {{"type", "boolean"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"plugin", "enabled", "confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Asset query (Phase 4.5 round 2 batch 1) -----------------------
    registerRemote(sage::mcp::Tool{
        .name = "asset.list",
        .description = "Enumerate assets under a content directory via "
                       "AssetRegistry. directory defaults to '/Game' "
                       "(project content root); '/Engine', '/Plugin/<Name>' "
                       "also accepted. recursive=true (default). "
                       "Server-side filters: class (FTopLevelAssetPath, "
                       "e.g. '/Script/Engine.Blueprint') and kind (asset "
                       "class simple-name array, e.g. ['Blueprint',"
                       "'AnimBlueprint']). Pagination via offset (>=0, "
                       "default 0) + max_results (1..50000, default 1000). "
                       "fields projects output rows (subset of "
                       "['path','kind','name'], empty = all). Returns "
                       "{assets, returned, offset, total, capped, "
                       "directory, recursive, class?}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"directory",   {{"type", "string"}}},
                {"recursive",   {{"type", "boolean"}}},
                {"class",       {{"type", "string"}}},
                {"kind",        {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"offset",      {{"type", "integer"}, {"minimum", 0}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50000}}},
                {"fields",      {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.search",
        .description = "Substring search assets by name OR path under the "
                       "given directory (case-insensitive). Optional "
                       "class filter is a TopLevelAssetPath (e.g. "
                       "'/Script/Engine.Blueprint'). At least one of "
                       "'query' or 'class' must be provided — if only "
                       "class is given, all assets of that class under "
                       "directory are returned (no substring filter). "
                       "Returns same shape as asset.list plus {query?, "
                       "class?} echo.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query",       {{"type", "string"}}},
                {"class",       {{"type", "string"}}},
                {"directory",   {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.read_properties",
        .description = "Dump every reflected UProperty on an asset's "
                       "UObject. Skips CPF_Transient + DuplicateTransient. "
                       "Goes through Sage's GetUPropertyAsJson — structs "
                       "(Vector/Rotator/Transform/Color), TArray/TMap/"
                       "TSet, enums, soft refs all round-tripped. "
                       "Optional `recurse_instanced` (default false) "
                       "expands UPROPERTY(Instanced) refs (e.g. "
                       "GameFeatureActions, ComponentList) into embedded "
                       "{_class, _path, _props, _count} objects bounded "
                       "by `max_depth` (default 4, range 1..16). Cycles "
                       "short-circuit via a visited set. Returns "
                       "{class, properties: {...}, count, "
                       "recurse_instanced?, max_depth?}. -32602 if path "
                       "doesn't resolve.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"recurse_instanced", {{"type", "boolean"}}},
                {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 2: socket management (StaticMesh + SkeletalMesh)
    registerRemote(sage::mcp::Tool{
        .name = "asset.list_sockets",
        .description = "List sockets on a UStaticMesh or USkeletalMesh. "
                       "Returns {kind: 'static_mesh'|'skeletal_mesh', "
                       "sockets: [{name, location, rotation, scale, "
                       "tag?, bone?, owner, effective, force_always_animated?}], "
                       "count}. Skeletal mesh defaults to mesh-only sockets for "
                       "backward compatibility; pass owner='any' or 'skeleton' "
                       "to include inherited skeleton sockets plus override "
                       "collision diagnostics.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"owner", {{"type", "string"}, {"enum", nlohmann::json::array({"mesh", "skeleton", "any"})}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.add_socket",
        .description = "Add a socket to a UStaticMesh or USkeletalMesh. "
                       "Wrapped in FScopedTransaction (undo-friendly). "
                       "location/rotation/scale default to identity. "
                       "Skeletal mesh accepts optional bone (defaults "
                       "NAME_None). Errors -32602 if a socket with "
                       "that name already exists. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"name",     {{"type", "string"}}},
                {"bone",     {{"type", "string"}}},
                {"location", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"scale",    {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.get_socket",
        .description = "Read one socket by name on a UStaticMesh or USkeletalMesh. "
                       "For skeletal meshes returns mesh_socket, skeleton_socket, "
                       "effective_socket, owner collision flags, and the asset "
                       "that owns each socket.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"owner", {{"type", "string"}, {"enum", nlohmann::json::array({"mesh", "skeleton", "any"})}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.upsert_socket",
        .description = "Create or update a mesh-owned or skeleton-owned socket. "
                       "For skeletal meshes, owner='mesh' can intentionally create "
                       "a mesh-only override of an inherited skeleton socket when "
                       "allow_mesh_override_of_skeleton_socket=true, without "
                       "mutating the shared skeleton. owner='skeleton' requires "
                       "confirmed:true for real writes. Supports dry_run/save and "
                       "before/after readback.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"name",     {{"type", "string"}}},
                {"owner",    {{"type", "string"}, {"enum", nlohmann::json::array({"mesh", "skeleton"})}}},
                {"bone",     {{"type", "string"}}},
                {"parent_bone", {{"type", "string"}}},
                {"location", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"scale",    {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"create_if_missing", {{"type", "boolean"}}},
                {"allow_mesh_override_of_skeleton_socket", {{"type", "boolean"}}},
                {"dry_run", {{"type", "boolean"}}},
                {"save", {{"type", "boolean"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.update_socket",
        .description = "Alias of asset.upsert_socket.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"name",     {{"type", "string"}}},
                {"owner",    {{"type", "string"}, {"enum", nlohmann::json::array({"mesh", "skeleton"})}}},
                {"bone",     {{"type", "string"}}},
                {"parent_bone", {{"type", "string"}}},
                {"location", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"scale",    {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}},
                {"create_if_missing", {{"type", "boolean"}}},
                {"allow_mesh_override_of_skeleton_socket", {{"type", "boolean"}}},
                {"dry_run", {{"type", "boolean"}}},
                {"save", {{"type", "boolean"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.remove_socket",
        .description = "Remove a named socket from a UStaticMesh or "
                       "USkeletalMesh. Wrapped in FScopedTransaction. "
                       "Skeletal mesh removes from the mesh-only socket "
                       "list (skeleton-derived sockets cannot be touched "
                       "from here). Errors -32602 if not found. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 3: textures
    registerRemote(sage::mcp::Tool{
        .name = "asset.list_textures",
        .description = "Enumerate UTexture / UTexture2D assets under a "
                       "content directory (recursive). Returns "
                       "{textures: [{path, name, kind}], returned, total, "
                       "capped}. Use asset.get_texture_info for per-asset "
                       "metadata.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"directory",   {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.get_texture_info",
        .description = "Read a UTexture's import/runtime settings. "
                       "Returns {compression (TC_*), address_x/y "
                       "(Wrap/Clamp/Mirror), filter (Nearest/Bilinear/"
                       "Trilinear/Default), srgb, never_stream, lod_bias, "
                       "compression_quality}. UTexture2D adds {width, "
                       "height, num_mips, pixel_format}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    // Phase 4.5-r2 batch 9: FBX import wrappers
    registerRemote(sage::mcp::Tool{
        .name = "asset.import_static_mesh",
        .description = "Import a UStaticMesh from .fbx/.obj/.gltf via "
                       "UAssetImportTask + IAssetTools::ImportAssetTasks. "
                       "Same args as asset.import_texture; verifies the "
                       "produced asset is a UStaticMesh, otherwise -32000. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"file",             {{"type", "string"}}},
                {"destination",      {{"type", "string"}}},
                {"replace_existing", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"file", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.import_skeletal_mesh",
        .description = "Import a USkeletalMesh from FBX. Verifies the "
                       "produced asset is a USkeletalMesh.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"file",             {{"type", "string"}}},
                {"destination",      {{"type", "string"}}},
                {"replace_existing", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"file", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.import_animation",
        .description = "Import a UAnimSequence from FBX. Verifies the "
                       "produced asset is a UAnimSequence. Requires "
                       "`skeleton` (USkeleton path) — wired into "
                       "UFbxImportUI->Skeleton + bImportAnimations=true. "
                       "-32602 if skeleton missing/wrong class.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"file",             {{"type", "string"}}},
                {"destination",      {{"type", "string"}}},
                {"skeleton",         {{"type", "string"}}},
                {"replace_existing", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"file", "destination", "skeleton"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 8: export
    registerRemote(sage::mcp::Tool{
        .name = "asset.export",
        .description = "Export an asset to disk via "
                       "UExporter::FindExporter + UExporter::"
                       "RunAssetExportTask. The exporter is selected "
                       "by file extension: Texture2D→.png/.tga/.exr; "
                       "StaticMesh/SkeletalMesh→.fbx; SoundWave→.wav; "
                       "etc. Runs in bAutomated mode (no dialogs). "
                       "Returns {exported: bool, exporter (class), "
                       "file_size, errors?: [...]}. Errors -32602 "
                       "if no exporter exists for the asset class + "
                       "extension combination. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"file", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "file"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 7: import + reimport
    registerRemote(sage::mcp::Tool{
        .name = "asset.import_texture",
        .description = "Import an image file (PNG/TGA/JPG/EXR/HDR) "
                       "into a UTexture2D via UAssetImportTask + "
                       "IAssetTools::ImportAssetTasks. file = absolute "
                       "path on disk; destination = '/Game/Folder/Name' "
                       "or '/Game/Folder/Name.Name'. replace_existing "
                       "(default false) overwrites a name collision. "
                       "Returns {imported: [paths], dest_dir, dest_name}. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"file",             {{"type", "string"}}},
                {"destination",      {{"type", "string"}}},
                {"replace_existing", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"file", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.reimport",
        .description = "Reimport an asset from its source file via "
                       "FReimportManager::Reimport. If source_file is "
                       "given, that path overrides the saved source. "
                       "Runs in bAutomated mode (no dialogs). Returns "
                       "{reimported: bool, known_sources: [paths]}. "
                       "Errors -32602 if the asset has no source on "
                       "file AND no source_file is supplied. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"source_file", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 6: datatable read/create/reimport
    registerRemote(sage::mcp::Tool{
        .name = "asset.read_datatable",
        .description = "Read all rows of a UDataTable. Returns "
                       "{row_struct (path), row_count, rows: [{name, "
                       "fields: {<col>: <value>}}], returned, offset, capped}. "
                       "Each row's fields go through Sage's reflection "
                       "(structs/arrays/enums round-trip). max_rows "
                       "clamped 1..100000 (default 1000). Optional `offset` "
                       "(>=0, default 0) skips first N rows. Optional "
                       "`fields` (string array) projects each row's fields "
                       "object to listed columns only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"max_rows", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}}},
                {"offset",   {{"type", "integer"}, {"minimum", 0}}},
                {"fields",   {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.create_datatable",
        .description = "Create an empty UDataTable bound to a "
                       "UScriptStruct row type. row_struct must derive "
                       "from FTableRowBase (UPROPERTY-tagged USTRUCT). "
                       "Wrapped in FScopedTransaction. -32602 if "
                       "row_struct is missing/not a row struct, or "
                       "package already exists. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",       {{"type", "string"}}},
                {"row_struct", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "row_struct"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.reimport_datatable",
        .description = "Repopulate a UDataTable from JSON string or "
                       "from a JSON file on disk. Provide either 'json' "
                       "(inline) OR 'json_file' (filesystem path). "
                       "clear_first defaults true (EmptyTable + "
                       "CreateTableFromJSONString). Returns "
                       "{row_count, problems: [...], cleared}. The "
                       "table's RowStruct must already be set (use "
                       "asset.create_datatable first). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"json",        {{"type", "string"}}},
                {"json_file",   {{"type", "string"}}},
                {"clear_first", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 5: mesh material slots
    registerRemote(sage::mcp::Tool{
        .name = "asset.list_mesh_materials",
        .description = "List material slots on a UStaticMesh or "
                       "USkeletalMesh. Returns {kind, slots: [{index, "
                       "slot_name, imported_name, material}], count}. "
                       "material is the asset path of the bound "
                       "UMaterialInterface or empty string.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.set_mesh_material",
        .description = "Bind a UMaterialInterface to a UStaticMesh "
                       "material slot via UStaticMesh::SetMaterial. "
                       "Empty material clears the slot. Errors -32602 "
                       "if slot is out of range or the material can't "
                       "be resolved. FScopedTransaction wrapped, "
                       "PostEditChange propagated. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"slot",     {{"type", "integer"}, {"minimum", 0}}},
                {"material", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "slot"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.set_sk_material_slots",
        .description = "Bulk-edit USkeletalMesh material slots — for "
                       "each item in slots, sets material (empty=clear) "
                       "and optionally slot_name. Returns {applied: "
                       "[{index, material}], count}. -32602 if any "
                       "index is out of range or material can't be "
                       "resolved (whole batch rolls back). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"slots", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"index",     {{"type", "integer"}, {"minimum", 0}}},
                            {"material",  {{"type", "string"}}},
                            {"slot_name", {{"type", "string"}}},
                        }},
                        {"required", nlohmann::json::array({"index"})},
                    }},
                }},
            }},
            {"required", nlohmann::json::array({"path", "slots"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Phase 4.5-r2 batch 4: write essentials
    registerRemote(sage::mcp::Tool{
        .name = "asset.create_data_asset",
        .description = "Create a new UDataAsset (concrete subclass of "
                       "UDataAsset). path is the destination object path "
                       "/Game/.../AssetName form. class is a class path "
                       "(e.g. '/Script/Engine.DataAsset' or "
                       "'/Game/MyTypes/MyDataAsset.MyDataAsset_C'). "
                       "Errors -32602 if the class isn't a UDataAsset, is "
                       "abstract, or the path lacks a directory. Wrapped "
                       "in FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.delete_batch",
        .description = "Bulk delete a list of assets. Returns per-path "
                       "{status: 'deleted'|'missing'|'failed'} plus failure "
                       "reason/diagnostics for failed deletes and "
                       "rollup counters {deleted, missing, failed, "
                       "total}. Soft-tolerates missing paths (no error). "
                       "Wrapped in single FScopedTransaction so the "
                       "whole batch is one undo step. PIE rejected. "
                       "Pass `confirmed:true` to proceed (destructive — "
                       "deletes every listed asset; only undoable while "
                       "editor is alive).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"minItems", 1},
                }},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"paths", "confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.reload_package",
        .description = "Force reload a UPackage from disk via "
                       "UPackageTools::ReloadPackages. Useful after "
                       "external edits (Python/USD/uassetool). path may "
                       "be either a package path '/Game/Foo/Bar' or a "
                       "full object path '/Game/Foo/Bar.Bar' (the dot "
                       "suffix is stripped automatically). Errors -32602 "
                       "if the package can't be found or loaded. PIE rejected. "
                       "Pass `confirmed:true` to proceed (destructive — "
                       "discards in-memory edits, irreversible without git).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name = "asset.set_texture_settings",
        .description = "Mutate UTexture settings. Each field optional; "
                       "only provided ones are applied. compression "
                       "(case-insensitive: Default/Normalmap/Masks/"
                       "Grayscale/HDR/BC7/HalfFloat/LQ/SingleFloat/"
                       "HDR_F32/...). address_x|y (Wrap/Clamp/Mirror, "
                       "UTexture2D only). filter (Nearest/Bilinear/"
                       "Trilinear/Default). srgb, never_stream (bool). "
                       "lod_bias (int). FScopedTransaction wrapped + "
                       "PostEditChange propagated. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",         {{"type", "string"}}},
                {"compression",  {{"type", "string"}}},
                {"address_x",    {{"type", "string"}}},
                {"address_y",    {{"type", "string"}}},
                {"filter",       {{"type", "string"}}},
                {"srgb",         {{"type", "boolean"}}},
                {"never_stream", {{"type", "boolean"}}},
                {"lod_bias",     {{"type", "integer"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Sequencer (Phase 4.6 round 3 batch 6) ---------------------------
    registerRemote(sage::mcp::Tool{
        .name = "seq.create",
        .description = "Create a ULevelSequence at the given path. "
                       "Calls Initialize() to spawn an empty UMovieScene. "
                       "Wrapped in FScopedTransaction. -32602 if path "
                       "lacks a directory or the package already "
                       "exists. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "seq.list_tracks",
        .description = "List the root UMovieSceneTracks on a "
                       "ULevelSequence's MovieScene. Returns "
                       "{tracks: [{name, class, display_name}], "
                       "track_count, binding_count, possessable_count, "
                       "spawnable_count}. -32602 if not a LevelSequence.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "seq.add_track",
        .description = "Add a UMovieSceneTrack subclass to the root "
                       "of a sequence's MovieScene. track_class is a "
                       "concrete (non-abstract) subclass — e.g. "
                       "'/Script/MovieSceneTracks.MovieSceneCameraCutTrack', "
                       "'/Script/MovieSceneTracks.MovieScene3DTransformTrack'. "
                       "Wrapped in FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"track_class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "track_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- UMG widget authoring (Phase 4.11 round 1) -----------------------
    registerRemote(sage::mcp::Tool{
        .name = "widget.create",
        .description = "Create a new UWidgetBlueprint via "
                       "UWidgetBlueprintFactory. parent_class must be a "
                       "UUserWidget subclass (defaults to UUserWidget). "
                       "path is /Game/Folder/WBP_Name. The factory "
                       "produces a default Canvas root widget unless a "
                       "custom RootWidgetClass is plumbed (future). "
                       "Returns {path, name, parent_class, "
                       "root_widget_class}. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",         {{"type", "string"}}},
                {"parent_class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "widget.list",
        .description = "Enumerate UWidgetBlueprint assets under a "
                       "content directory via AssetRegistry filter on "
                       "/Script/UMGEditor.WidgetBlueprint. Returns "
                       "{widgets: [{path, name}], returned, total, capped}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"directory",   {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "widget.add_widget",
        .description = "Construct a new UWidget and attach it to a "
                       "WidgetBlueprint's tree. widget_class must be a "
                       "concrete UWidget subclass. parent (optional) "
                       "is the name of an existing UPanelWidget to host "
                       "the new child; if omitted, attaches to the "
                       "tree's root panel (or sets it as root if empty). "
                       "name optional. Returns {name, class, attached_to}. "
                       "Wrapped in FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"blueprint",    {{"type", "string"}}},
                {"widget_class", {{"type", "string"}}},
                {"name",         {{"type", "string"}}},
                {"parent",       {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"blueprint", "widget_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "widget.remove_widget",
        .description = "Remove a widget by name from a WidgetBlueprint's "
                       "tree. Walks the tree to find the parent panel, "
                       "calls UPanelWidget::RemoveChild. If the widget "
                       "is the tree's root, clears RootWidget. Marks "
                       "the BP modified afterwards. Returns "
                       "{removed, parent}. -32602 if widget not found. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"blueprint", {{"type", "string"}}},
                {"name",      {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"blueprint", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "widget.set_property",
        .description = "Mutate a UProperty on a named widget inside a "
                       "WidgetBlueprint's tree. Routes through Sage's "
                       "SetUPropertyFromJson — primitives, structs "
                       "(Vector/Rotator/Color/LinearColor/Margin/...), "
                       "TArray/TMap/TSet, enums, soft refs all "
                       "supported. Wrapped in FScopedTransaction + "
                       "MarkBlueprintAsModified + PostEditChange. "
                       "Returns {widget, property, new_value} round-tripped "
                       "through GetUPropertyAsJson. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"blueprint", {{"type", "string"}}},
                {"name",      {{"type", "string"}}},
                {"property",  {{"type", "string"}}},
                {"value",     {{"description", "any JSON value (primitive, struct, array, ...)"}}},
            }},
            {"required", nlohmann::json::array({"blueprint", "name", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "widget.read",
        .description = "Read a UWidgetBlueprint's tree. Returns "
                       "{parent_class, root: {name, class, children: "
                       "[...]}, widgets: [{name, class}], "
                       "widget_count}. root is a recursive tree (panel "
                       "widgets nest); widgets is the flat list from "
                       "WidgetTree::GetAllWidgets. -32602 if not a "
                       "WidgetBlueprint.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Editor automation (Phase 4.6) ---------------------------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.console_command",
        .description = "Execute a UE console command. Default: gated to a "
                       "read-only / view-state whitelist (STAT, SHOW, "
                       "VIEWMODE, CAMERA, R.SCREENPERCENTAGE, MEMREPORT, "
                       "OBJ, LOG, HELP). allow_unsafe=true bypasses — UE "
                       "console can crash the editor with the wrong command, "
                       "use with care. IO-redirecting console forms (FILE=, "
                       "pipes, EXEC) are rejected unless allow_unsafe=true.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"cmd",          {{"type", "string"}}},
                {"allow_unsafe", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"cmd"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.take_screenshot",
        .description = "Capture the active viewport as a PNG. Default path: "
                       "ProjectSavedDir/Screenshots/Sage_<timestamp>.png. "
                       "path must resolve under <Project>/Saved/Screenshots; "
                       "absolute paths and `..` escapes outside that directory "
                       "are rejected. Returns the saved path. Async — UE "
                       "writes the file shortly after the call returns.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_engine_version",
        .description = "Engine + project + build configuration metadata: "
                       "engine_version, compatible_version, "
                       "build_configuration, project_dir, engine_dir.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_project_version",
        .description = "Project name + dir + log/saved/content paths.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_log_file_path",
        .description = "Absolute path of the current editor log file. The "
                       "agent can tail it directly via filesystem when more "
                       "than read_log's slice is needed.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.read_log",
        .description = "Tail recent editor log lines with Windows shared-read "
                       "fallback while the editor is appending. filter "
                       "(substring), max_lines (1..5000, default 200), "
                       "case_sensitive=false, optional path/log_path and "
                       "max_bytes. Returns read_diagnostics with normalized "
                       "path, exists/size, read method, and fallback status.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",         {{"type", "string"}}},
                {"max_lines",      {{"type", "integer"},
                                    {"minimum", 1}, {"maximum", 5000}}},
                {"case_sensitive", {{"type", "boolean"}}},
                {"path",           {{"type", "string"}}},
                {"log_path",       {{"type", "string"}}},
                {"max_bytes",      {{"type", "integer"}, {"minimum", 1}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Editor state control (Phase 4.6 round 3 batch 1) --------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.undo",
        .description = "Run GEditor->UndoTransaction. Reverts the most "
                       "recent transactional change. Returns {undid: bool} "
                       "— false when the undo stack is empty.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.redo",
        .description = "Run GEditor->RedoTransaction. Re-applies the most "
                       "recently undone change. Returns {redid: bool}.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.focus_on_actor",
        .description = "Move the active level viewport's camera to focus on "
                       "the actor identified by actor_id (full path, like "
                       "what spawn_actor returns). active_viewport_only=true "
                       "limits the move to the current viewport (default "
                       "false: all level viewports). -32602 if the actor "
                       "doesn't exist.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id",             {{"type", "string"}}},
                {"active_viewport_only", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"actor_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.set_viewport",
        .description = "Set the active level viewport's camera position "
                       "and/or rotation. Provide location [x,y,z] and/or "
                       "rotation [pitch,yaw,roll]. -32602 if neither is "
                       "supplied. Forces a viewport invalidate so the "
                       "change renders immediately.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"location", {{"type", "array"}, {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Runtime state mutation (Phase 4.6 round 3 batch 2) ------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.set_property",
        .description = "Write a UProperty on any UObject by SoftObjectPath. "
                       "Routes through Sage's SetUPropertyFromJson — "
                       "structured JSON value (numbers / booleans / nested "
                       "structs / TArray / object refs / TSubclassOf class paths) instead of "
                       "ImportText strings. PIE rejected. -32602 on path / "
                       "property miss or type-coercion failure. "
                       "MarkPackageDirty after write so the editor re-saves. "
                       "For UAnimationAsset.Skeleton, uses "
                       "UAnimationAsset::SetSkeleton and fails if readback does "
                       "not match.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"},
                              {"description", "SoftObjectPath (e.g. /Game/Foo.Foo or /Script/Engine.Default__Actor)"}}},
                {"property", {{"type", "string"}}},
                {"value",    {{"description", "Any JSON — Sage coerces by FProperty type"}}},
            }},
            {"required", nlohmann::json::array({"path", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.set_pie_time_scale",
        .description = "Set the PIE world's global time dilation. factor > "
                       "0; lifts AWorldSettings::Min/MaxGlobalTimeDilation "
                       "caps so high values don't clamp silently. -32004 "
                       "if no PIE session is active (start with run_pie). "
                       "Returns {factor, min_cap, max_cap, world}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"factor", {{"type", "number"}, {"exclusiveMinimum", 0}}}}},
            {"required", nlohmann::json::array({"factor"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name = "input.press_key",
        .description = "Simulate a key press in the active PIE viewport/local "
                       "player, then release it immediately or after optional "
                       "duration_seconds. Routes through the PIE GameViewport "
                       "and PlayerController so Enhanced Input mappings see "
                       "pressed/released state. Resolves the same live PIE "
                       "world context used by gameplay readback, accepts "
                       "explicit world/controller/pawn targets, and returns "
                       "target plus route details.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
                {"amount", {{"type", "number"}}},
                {"duration", {{"type", "number"}, {"minimum", 0}}},
                {"duration_seconds", {{"type", "number"}, {"minimum", 0}}},
            }},
            {"required", nlohmann::json::array({"key"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "input.hold_key",
        .description = "Send a key-down event to the active PIE local player. "
                       "Use input.release_key to end the hold, or pass "
                       "duration_seconds to schedule release on the same "
                       "resolved PIE world/controller/pawn target.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
                {"amount", {{"type", "number"}}},
                {"duration", {{"type", "number"}, {"minimum", 0}}},
                {"duration_seconds", {{"type", "number"}, {"minimum", 0}}},
            }},
            {"required", nlohmann::json::array({"key"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "input.release_key",
        .description = "Send a key-up event to the active PIE local player. "
                       "Accepts explicit world/controller/pawn targets and "
                       "returns target plus viewport/controller route details.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"key"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "input.trigger_action",
        .description = "Inject an Enhanced Input action or player mapping in "
                       "PIE. `action`/`action_path` accepts a UInputAction "
                       "asset path; `mapping_name` targets player mappings. "
                       "mode is once/start/update/stop/hold/release; start/"
                       "hold supports duration_seconds for scheduled stop. "
                       "Resolves the live PIE world context and returns target "
                       "identity for stale-world diagnostics.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"}}},
                {"action_path", {{"type", "string"}}},
                {"mapping_name", {{"type", "string"}}},
                {"value", {{"description", "boolean, number, [x,y,z], or {x,y,z}"}}},
                {"mode", {{"type", "string"}}},
                {"continuous", {{"type", "boolean"}}},
                {"duration", {{"type", "number"}, {"minimum", 0}}},
                {"duration_seconds", {{"type", "number"}, {"minimum", 0}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "gameplay.trace_input_action",
        .description = "PIE-safe diagnostic trace around a key or Enhanced "
                       "Input injection. Captures Enhanced Input action/value/"
                       "mapping readback, pawn movement before/after, ASC "
                       "owned tags, Lyra-style pressed/held/released spec "
                       "handle arrays, ability specs, optional reflected "
                       "ProcessAbilityInput, and optional TryActivateAbility "
                       "for matched specs.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"action", {{"type", "string"}}},
                {"action_path", {{"type", "string"}}},
                {"mapping_name", {{"type", "string"}}},
                {"value", {{"description", "boolean, number, [x,y,z], or {x,y,z}"}}},
                {"mode", {{"type", "string"}}},
                {"input_mode", {{"type", "string"}}},
                {"amount", {{"type", "number"}}},
                {"duration", {{"type", "number"}, {"minimum", 0}}},
                {"duration_seconds", {{"type", "number"}, {"minimum", 0}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"actor_id", {{"type", "string"}}},
                {"actor", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
                {"ability", {{"type", "string"}}},
                {"ability_name", {{"type", "string"}}},
                {"ability_path", {{"type", "string"}}},
                {"ability_class", {{"type", "string"}}},
                {"input_tag", {{"type", "string"}}},
                {"tag", {{"type", "string"}}},
                {"input_id", {{"type", "integer"}}},
                {"max_specs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                {"include_raw", {{"type", "boolean"}}},
                {"process_ability_input", {{"type", "boolean"}}},
                {"delta_time", {{"type", "number"}, {"minimum", 0}}},
                {"process_delta_time", {{"type", "number"}, {"minimum", 0}}},
                {"game_paused", {{"type", "boolean"}}},
                {"try_activate", {{"type", "boolean"}}},
                {"allow_remote_activation", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "gas.trace_ability_activation",
        .description = "Alias of gameplay.trace_input_action focused on GAS "
                       "activation diagnostics; accepts the same schema and "
                       "returns the same before/injection/process/try/after "
                       "trace.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"action", {{"type", "string"}}},
                {"action_path", {{"type", "string"}}},
                {"mapping_name", {{"type", "string"}}},
                {"value", {{"description", "boolean, number, [x,y,z], or {x,y,z}"}}},
                {"mode", {{"type", "string"}}},
                {"input_mode", {{"type", "string"}}},
                {"amount", {{"type", "number"}}},
                {"duration", {{"type", "number"}, {"minimum", 0}}},
                {"duration_seconds", {{"type", "number"}, {"minimum", 0}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"actor_id", {{"type", "string"}}},
                {"actor", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
                {"ability", {{"type", "string"}}},
                {"ability_name", {{"type", "string"}}},
                {"ability_path", {{"type", "string"}}},
                {"ability_class", {{"type", "string"}}},
                {"input_tag", {{"type", "string"}}},
                {"tag", {{"type", "string"}}},
                {"input_id", {{"type", "integer"}}},
                {"max_specs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                {"include_raw", {{"type", "boolean"}}},
                {"process_ability_input", {{"type", "boolean"}}},
                {"delta_time", {{"type", "number"}, {"minimum", 0}}},
                {"process_delta_time", {{"type", "number"}, {"minimum", 0}}},
                {"game_paused", {{"type", "boolean"}}},
                {"try_activate", {{"type", "boolean"}}},
                {"allow_remote_activation", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name = "gameplay.simulate_input_tag",
        .description = "PIE-safe Lyra/GAS input-tag simulation. Resolves a "
                       "PIE actor/pawn and input tag, finds the matching "
                       "InputAction from Lyra-style input config data, executes "
                       "the Enhanced Input bound delegate for press/release, "
                       "then calls PlayerController::PostProcessInput so the "
                       "Lyra AbilitySystemComponent input pipeline processes "
                       "the tag. Returns before/after movement and GAS readback.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"input_tag", {{"type", "string"}}},
                {"tag", {{"type", "string"}}},
                {"mode", {{"type", "string"}, {"description", "press, release, or tap"}}},
                {"input_mode", {{"type", "string"}}},
                {"duration", {{"type", "number"}, {"minimum", 0}}},
                {"duration_seconds", {{"type", "number"}, {"minimum", 0}}},
                {"action", {{"type", "string"}}},
                {"action_path", {{"type", "string"}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"actor_id", {{"type", "string"}}},
                {"actor", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
                {"ability", {{"type", "string"}}},
                {"ability_name", {{"type", "string"}}},
                {"ability_path", {{"type", "string"}}},
                {"ability_class", {{"type", "string"}}},
                {"input_id", {{"type", "integer"}}},
                {"max_specs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                {"include_raw", {{"type", "boolean"}}},
                {"post_process_input", {{"type", "boolean"}}},
                {"call_reflected_asc_fallback", {{"type", "boolean"}}},
                {"delta_time", {{"type", "number"}, {"minimum", 0}}},
                {"process_delta_time", {{"type", "number"}, {"minimum", 0}}},
                {"game_paused", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name = "gameplay.spawn_pie_actor_snapshot",
        .description = "Safely spawn a transient native/Blueprint Actor class "
                       "inside a live PIE world, read back requested actor and "
                       "component state, destroy the actor before returning, "
                       "and run the same Python reference cleanup discipline "
                       "used before stop_pie. This is for runtime spawn/readback "
                       "diagnostics; it does not create editor-world actors.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class", {{"type", "string"}}},
                {"class_path", {{"type", "string"}}},
                {"uclass", {{"type", "string"}}},
                {"transform", {{"type", "object"}}},
                {"location", {{"type", "array"}}},
                {"rotation", {{"type", "array"}}},
                {"scale", {{"type", "array"}}},
                {"collision_handling", {{"type", "string"}}},
                {"spawn_collision_handling", {{"type", "string"}}},
                {"replicates", {{"type", "boolean"}}},
                {"replicate_movement", {{"type", "boolean"}}},
                {"always_relevant", {{"type", "boolean"}}},
                {"net_load_on_client", {{"type", "boolean"}}},
                {"include_actor_properties", {{"type", "boolean"}}},
                {"include_components", {{"type", "boolean"}}},
                {"include_component_properties", {{"type", "boolean"}}},
                {"properties", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"components", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"component_properties", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"max_properties", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                {"max_components", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                {"max_depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 8}}},
                {"cleanup_python_refs", {{"type", "boolean"}}},
                {"clear_python_main_globals", {{"type", "boolean"}}},
                {"local_player_index", {{"type", "integer"}, {"minimum", 0}}},
                {"world", {{"type", "string"}}},
                {"world_name", {{"type", "string"}}},
                {"world_path", {{"type", "string"}}},
                {"expected_world", {{"type", "string"}}},
                {"controller", {{"type", "string"}}},
                {"controller_id", {{"type", "string"}}},
                {"player_controller", {{"type", "string"}}},
                {"player_controller_id", {{"type", "string"}}},
                {"pawn", {{"type", "string"}}},
                {"pawn_id", {{"type", "string"}}},
                {"expected_pawn", {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Log + crash forensics (Phase 4.6 round 3 batch 3) -------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.search_log",
        .description = "Substring search across the active editor log with "
                       "Windows shared-read fallback while the editor is "
                       "appending. case_sensitive=false by default. "
                       "Returns {hits: [{line, text}], count, total_lines, "
                       "capped, read_diagnostics} where text is line-truncated "
                       "to 500 chars. max_lines clamped 1..5000 (default 100).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query",          {{"type", "string"}}},
                {"max_lines",      {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}},
                {"case_sensitive", {{"type", "boolean"}}},
                {"path",           {{"type", "string"}}},
                {"log_path",       {{"type", "string"}}},
                {"max_bytes",      {{"type", "integer"}, {"minimum", 1}}},
            }},
            {"required", nlohmann::json::array({"query"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.list_crashes",
        .description = "Enumerate UE crash report directories under "
                       "~/Library/.../Epic/UnrealEngine/<Version>/Saved/"
                       "Crashes/ (Mac) / %LOCALAPPDATA%/.../ (Win), sorted "
                       "newest-first. max_results clamped 1..200 "
                       "(default 25). Each entry: {crash_dir, name, "
                       "timestamp, has_log, has_dump, has_context}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.check_for_crashes",
        .description = "Quick boolean: did any crash report drop within "
                       "the last `within_hours` hours? Returns "
                       "{recent_count, has_recent, latest_timestamp?}. "
                       "within_hours clamped 1..8760 (default 24).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"within_hours", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8760}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_crash_info",
        .description = "Read a specific crash dir's CrashContext.runtime-"
                       "xml (extracts <ErrorMessage> + <CallStack>) plus "
                       "a tail of UnrealEditor.log / <Project>.log inside "
                       "the dir. crash_dir must live under the per-user "
                       "crashes root (path-safety guard, -32602 otherwise). "
                       "Optional `lines` (default 50, max 10000) and "
                       "`offset_from_end` (default 0) control the log tail "
                       "window.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"crash_dir",       {{"type", "string"}}},
                {"lines",           {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                {"offset_from_end", {{"type", "integer"}, {"minimum", 0}}},
            }},
            {"required", nlohmann::json::array({"crash_dir"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Level building (Phase 4.6 round 3 batch 4) --------------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.build_all",
        .description = "Trigger a full level build: MAP REBUILD (BSP) + "
                       "BUILD LIGHTING + RebuildNavigation. Fire-and-"
                       "forget — async; poll editor.get_build_status. "
                       "Long-running destructive op (lighting can take "
                       "hours). Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.build_geometry",
        .description = "Rebuild BSP geometry only via 'MAP REBUILD' "
                       "console command. Useful before lighting build "
                       "after volume/CSG edits. Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.build_lighting",
        .description = "Build lighting via 'BUILD LIGHTING <quality>' "
                       "console command. quality ∈ {Preview (default), "
                       "Medium, High, Production}. Async — agent should "
                       "poll editor.get_build_status until lighting_"
                       "running becomes false. Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"quality", {{"type", "string"},
                             {"enum", nlohmann::json::array({
                                 "Preview","Medium","High","Production"})}}},
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.build_hlod",
        .description = "Trigger HLOD build via 'BuildHLODs' console "
                       "command. Async. Pass `confirmed:true` to proceed.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"confirmed", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"confirmed"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_build_status",
        .description = "Report editor's current build state. Returns "
                       "{status: 'idle' | 'lighting_running' | "
                       "'lighting_exporting', lighting_running, "
                       "lighting_exporting}. UE 5.7 only exposes the "
                       "lighting build flags — geometry/HLOD/navigation "
                       "build progress isn't queryable per-call.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Python scripting (Phase 4.6 round 3 batch 5) ------------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.run_python",
        .description = "Run a Python snippet inside the editor's "
                       "PythonScriptPlugin host. Returns {success, "
                       "result (last expression), log_output[] (entries: "
                       "type ∈ Info/Warning/Error, output), _security_warning}. "
                       "-32603 if the project hasn't enabled the "
                       "PythonScriptPlugin (IsPythonAvailable=false). "
                       "Known crash-prone Enhanced Input IMC Mappings array "
                       "mutations are rejected unless "
                       "allow_unsafe_asset_mutation=true; use the typed "
                       "gameplay.set_imc_mapping_* tools instead. "
                       "Returns `_security_warning`; this tool exposes "
                       "arbitrary Python with full UE editor access and is "
                       "expected to be auth-gated server-side before public "
                       "release.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"code", {{"type", "string"}}},
                {"allow_unsafe_asset_mutation", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"code"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.cleanup_python_refs",
        .description = "Purge Python-held UObject wrappers for PIE/editor "
                       "world roots via PrepareToCleanseEditorObject, "
                       "optionally clear public __main__ globals, then run "
                       "Python and UE garbage collection. Use before map "
                       "loads or PIE teardown when Python inspected runtime "
                       "objects.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"clear_main_globals", {{"type", "boolean"}}},
                {"clear_python_main_globals", {{"type", "boolean"}}},
                {"include_pie_worlds", {{"type", "boolean"}}},
                {"include_editor_world", {{"type", "boolean"}}},
                {"collect_unreal_garbage", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Dialog policy (Phase 4.6 round 2) -----------------------------
    // Hooks FCoreDelegates::ModalMessageDialog so unattended agent flows
    // don't stall on Save?/Reload?/Confirm Delete? modals. Lazy install
    // on first set_dialog_policy call; hook stays bound for the editor
    // session lifetime and is unbound on plugin module shutdown.
    registerRemote(sage::mcp::Tool{
        .name = "editor.set_dialog_policy",
        .description = "Auto-respond to any modal whose title or message "
                       "contains 'pattern' (substring). response ∈ "
                       "{yes, no, ok, cancel, retry, continue, yesall, "
                       "noall}. Replaces an existing policy with the same "
                       "pattern. First call lazy-installs the dialog hook.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"pattern",  {{"type", "string"}}},
                {"response", {{"type", "string"},
                              {"enum", nlohmann::json::array({
                                  "yes", "no", "ok", "cancel", "retry",
                                  "continue", "yesall", "noall"})}}},
            }},
            {"required", nlohmann::json::array({"pattern", "response"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.clear_dialog_policy",
        .description = "Remove a policy by exact-pattern match, or all "
                       "policies when 'pattern' is omitted. Returns "
                       "{removed, policy_count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"pattern", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_dialog_policy",
        .description = "List active dialog policies and the hook install "
                       "status. Returns {policies[], count, hook_installed}.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.list_dialogs",
        .description = "Walk Slate to describe the currently-active modal "
                       "(UE shows at most one). Returns {dialogs: [{title, "
                       "message, buttons[]}], count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.respond_to_dialog",
        .description = "Click a button on the active modal. Provide "
                       "button_index (0-based), button_label (substring), "
                       "or action='escape' to dismiss. -32004 if no modal "
                       "is active; -32602 with available_buttons[] if no "
                       "match.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"button_index", {{"type", "integer"}, {"minimum", 0}}},
                {"button_label", {{"type", "string"}}},
                {"action",       {{"type", "string"},
                                  {"enum", nlohmann::json::array({"escape"})}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Material parameter (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "modify_material_parameter",
        .description = "Set a scalar (number) or vector (3/4-element array → "
                       "FLinearColor) parameter on a UMaterialInstanceConstant. "
                       "Backed by UMaterialEditingLibrary. FScopedTransaction. "
                       "Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"},
                                {"description", "Material instance constant path"}}},
                {"parameter",  {{"type", "string"}}},
                {"value",      {{"description", "Number for scalar; 3-4 element array for vector (RGBA)"}}},
            }},
            {"required", nlohmann::json::array({"asset_path", "parameter", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // ---- Knowledge layer MCP tools (Milestone 2.2 — T1 indexing) -------
    sage::mcp::Tool indexSlotTool{
        .name        = "index_slot",
        .description = "Reindex the slot's knowledge graph from the editor's "
                       "AssetRegistry. Server asks the connected plugin for a "
                       "full asset scan, then full-replaces the Asset table. "
                       "Duplicate or empty primary-key rows from transient "
                       "asset move/delete states are skipped with counts/examples "
                       "instead of failing the full reindex. "
                       "If 'slot_id' omitted, uses the active editor (or the "
                       "single connected editor when exactly one is present). "
                       "Supports async:true through the same jobs.* surface as "
                       "remote long-running tools. "
                       "Returns {slot_id, asset_count, last_indexed_at_ms, "
                       "scan_ms?}. -32001 EditorNotConnected if no editor "
                       "available, -32603 InternalError on plugin or DB failure.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"slot_id", {{"type", "string"}}},
                {"async", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [&bridge, graphMgr, resolveSlotId, jobMgr](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            nlohmann::json cleanParams = params.is_object() ? params : nlohmann::json::object();
            cleanParams.erase("async");

            auto runIndex = [&bridge, graphMgr, resolveSlotId, cleanParams]() -> sage::mcp::ToolResult {
                auto slot = resolveSlotId(cleanParams);
                if (!slot.has_value()) return std::unexpected(slot.error());

                // Plugin scans AssetRegistry; we receive {assets, scan_ms}.
                auto scan = bridge.dispatchTool(
                    "_scan_asset_registry", nlohmann::json::object());
                if (!scan.has_value()) return std::unexpected(scan.error());

                const auto& payload = scan.value();
                if (!payload.contains("assets") || !payload["assets"].is_array()) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        "plugin response missing 'assets' array"));
                }

                try {
                    auto& store = graphMgr->acquireSlot(*slot);
                    // payload already has the {assets, dependencies?} shape that
                    // ingestSnapshot expects.
                    auto ingest = sage::graph::ingestSnapshot(store, payload);
                    if (sage::graph::is_error(ingest)) {
                        return std::unexpected(sage::mcp::ErrorObject::fromCode(
                            sage::mcp::ErrorCode::InternalError,
                            "ingest failed: " + sage::graph::error_of(ingest).message));
                    }
                    nlohmann::json out = sage::graph::value_of(ingest);
                    out["slot_id"] = *slot;
                    if (payload.contains("scan_ms")) out["scan_ms"] = payload["scan_ms"];
                    return out;
                } catch (const std::exception& ex) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        std::string{"graph slot acquire failed: "} + ex.what()));
                }
            };

            if (params.is_object() && params.value("async", false)) {
                return jobMgr->startLocal("index_slot", cleanParams, std::move(runIndex));
            }
            return runIndex();
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(indexSlotTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'index_slot'");
    }

    sage::mcp::Tool indexStatusTool{
        .name        = "index_status",
        .description = "Read the slot's last index pass: {slot_id, asset_count, "
                       "last_indexed_at_ms}. last_indexed_at_ms is null if the "
                       "slot has never been indexed. Read-only — does not open "
                       "an editor connection.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"slot_id", {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());
            try {
                auto& store = graphMgr->acquireSlot(*slot);
                auto st = sage::graph::getIndexStatus(store);
                if (sage::graph::is_error(st)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(st).message));
                }
                nlohmann::json out = sage::graph::value_of(st);
                out["slot_id"] = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"graph slot acquire failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(indexStatusTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'index_status'");
    }

    // ---- Knowledge layer query tools (Milestone 2.4) -------------------
    // Cypher escape helper (kept inline to avoid pulling sage-graph internals
    // into the public surface; refactored into a util header in 2.5).
    auto escCypher = [](std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\\')      out.append("\\\\");
            else if (c == '\'') out.append("\\'");
            else                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    };

    sage::mcp::Tool impactOfTool{
        .name        = "impact_of",
        .description = "Reverse-traversal of DEPENDS_ON. Returns assets that "
                       "would be affected if `asset_path` changed — i.e. "
                       "transitive referencers up to `max_depth` hops "
                       "(default 3, range 1..10). Result is a deduplicated "
                       "list ordered by path; `truncated:true` indicates the "
                       "limit was hit. Use this before edits as the safety "
                       "check for delete/rename/refactor.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path",  {{"type", "string"},
                                 {"description", "/Game/.../Asset.Asset (SoftObjectPath form)"}}},
                {"max_depth",   {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 10}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("asset_path")
                || !params["asset_path"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'asset_path'"));
            }
            const auto path = params["asset_path"].get<std::string>();
            const int  maxDepth   = std::clamp(params.value("max_depth",   3), 1, 10);
            const int  maxResults = std::clamp(params.value("max_results", 100), 1, 500);

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                q << "MATCH (target:Asset {path: " << escCypher(path)
                  << "})<-[:DEPENDS_ON*1.." << maxDepth << "]-(impacted:Asset) "
                  << "RETURN DISTINCT impacted.path AS path, impacted.kind AS kind "
                  << "ORDER BY path LIMIT " << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["target"]    = path;
                out["max_depth"] = maxDepth;
                out["impacted"]  = env["rows"];
                out["count"]     = env["row_count"];
                out["truncated"] = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]   = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"impact_of failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(impactOfTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'impact_of'");
    }

    sage::mcp::Tool referencesToTool{
        .name        = "references_to",
        .description = "Direct (1-hop) inbound references. Returns the assets "
                       "that explicitly DEPENDS_ON `asset_path` — equivalent "
                       "to impact_of with max_depth=1 but cheaper. Use for "
                       "'who imports this' surveys; use impact_of for safety "
                       "before destructive edits.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path",  {{"type", "string"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("asset_path")
                || !params["asset_path"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'asset_path'"));
            }
            const auto path = params["asset_path"].get<std::string>();
            const int  maxResults = std::clamp(params.value("max_results", 100), 1, 500);

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                q << "MATCH (target:Asset {path: " << escCypher(path)
                  << "})<-[:DEPENDS_ON]-(ref:Asset) "
                  << "RETURN ref.path AS path, ref.kind AS kind "
                  << "ORDER BY path LIMIT " << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["target"]     = path;
                out["references"] = env["rows"];
                out["count"]      = env["row_count"];
                out["truncated"]  = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]    = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"references_to failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(referencesToTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'references_to'");
    }

    sage::mcp::Tool findUnusedTool{
        .name        = "find_unused",
        .description = "Assets with no incoming DEPENDS_ON edge — candidates "
                       "for cleanup. CAVEAT: only catches asset-to-asset "
                       "references tracked by AssetRegistry. C++ code "
                       "references (e.g. UClass::FindObject('/Game/...')), "
                       "config files, and runtime-string lookups are NOT "
                       "in the graph; never auto-delete from this list — "
                       "treat it as a triage view, not a kill list.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"kind",        {{"type", "string"},
                                 {"description", "Optional: filter by asset kind (e.g. 'Texture2D')"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            const int maxResults = std::clamp(params.value("max_results", 100), 1, 500);
            std::optional<std::string> kindFilter;
            if (params.is_object() && params.contains("kind") && params["kind"].is_string()) {
                kindFilter = params["kind"].get<std::string>();
            }

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                q << "MATCH (a:Asset) WHERE NOT EXISTS { MATCH (a)<-[:DEPENDS_ON]-() }";
                if (kindFilter) q << " AND a.kind = " << escCypher(*kindFilter);
                q << " RETURN a.path AS path, a.kind AS kind ORDER BY path LIMIT "
                  << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["unused"]    = env["rows"];
                out["count"]     = env["row_count"];
                out["truncated"] = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]   = *slot;
                if (kindFilter) out["kind"] = *kindFilter;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"find_unused failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(findUnusedTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'find_unused'");
    }

    sage::mcp::Tool classHierarchyTool{
        .name        = "class_hierarchy",
        .description = "Walk INHERITS_FROM edges from a UClass. "
                       "direction='ancestors' returns parent chain (Pawn → "
                       "Actor → Object); 'descendants' returns subclasses "
                       "(Pawn → all pawn types). max_depth 1..10 (default 10 "
                       "covers UE's typical inheritance depth). Returns "
                       "[{name, module, is_native, depth}] ordered by depth. "
                       "Class table is populated by index_slot from UE's "
                       "reflected UClass registry.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class_name",  {{"type", "string"}}},
                {"direction",   {{"type", "string"},
                                 {"enum", nlohmann::json::array({"ancestors","descendants"})}}},
                {"max_depth",   {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 10}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"class_name"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("class_name")
                || !params["class_name"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'class_name'"));
            }
            const auto cls = params["class_name"].get<std::string>();
            const auto dir = params.value("direction", std::string{"ancestors"});
            if (dir != "ancestors" && dir != "descendants") {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams,
                    "direction must be 'ancestors' or 'descendants'"));
            }
            const int  maxDepth   = std::clamp(params.value("max_depth",   10), 1, 10);
            const int  maxResults = std::clamp(params.value("max_results", 200), 1, 500);

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                if (dir == "ancestors") {
                    q << "MATCH (c:Class {name: " << escCypher(cls) << "})"
                      << "-[r:INHERITS_FROM*1.." << maxDepth << "]->(a:Class) ";
                } else {
                    q << "MATCH (c:Class {name: " << escCypher(cls) << "})"
                      << "<-[r:INHERITS_FROM*1.." << maxDepth << "]-(a:Class) ";
                }
                q << "RETURN DISTINCT a.name AS name, a.module AS module, "
                  << "       a.is_native AS is_native "
                  << "ORDER BY name LIMIT " << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["class_name"] = cls;
                out["direction"]  = dir;
                out["max_depth"]  = maxDepth;
                out["classes"]    = env["rows"];
                out["count"]      = env["row_count"];
                out["truncated"]  = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]    = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"class_hierarchy failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(classHierarchyTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'class_hierarchy'");
    }

    // ---- Cypher subset escape hatch (Milestone 2.5) --------------------
    sage::mcp::Tool queryGraphTool{
        .name        = "query_graph",
        .description = "Read-only Cypher against the slot's knowledge graph. "
                       "Use when impact_of / references_to / find_unused don't "
                       "fit the question. Banned: CREATE, MERGE, SET, DELETE, "
                       "DETACH, REMOVE, DROP, ALTER, COPY, LOAD, INSERT, CALL "
                       "(read-only — for writes use ingest tools / direct MCP "
                       "tools). Variable-length traversals must be bounded "
                       "*N..M with M ≤ 10. LIMIT is auto-injected if missing "
                       "(default 200, max 1000).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"cypher",      {{"type", "string"},
                                 {"description", "Cypher MATCH/RETURN query (max 8KB)"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 1000}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"cypher"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("cypher")
                || !params["cypher"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'cypher' string"));
            }
            const auto cypher = params["cypher"].get<std::string>();
            const int maxResults = std::clamp(params.value("max_results",
                sage::graph::kDefaultRowLimit), 1, sage::graph::kMaxRowLimit);

            // Whitelist + bound check.
            const auto v = sage::graph::validateReadOnlySubset(cypher);
            if (!v.ok) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams,
                    "cypher rejected: " + v.error));
            }

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            const auto bounded = sage::graph::ensureLimit(cypher, maxResults);
            try {
                auto& store = graphMgr->acquireSlot(*slot);
                auto r = store.execute(bounded);
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InvalidParams,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["rows"]      = env["rows"];
                out["schema"]    = env["schema"];
                out["row_count"] = env["row_count"];
                out["truncated"] = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]   = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"query_graph failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(queryGraphTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'query_graph'");
    }

    // ---- Editor restart orchestrator (Milestone 1.6b) ------------------
    sage::tools::RestartConfig restartCfg{
        .repoRoot = envOr("SAGE_REPO_ROOT", std::filesystem::current_path().string()),
        .ueRoot   = envOr("SAGE_UE_ROOT",   ""),
    };
    spdlog::info("Restart orchestrator: repo_root={} ue_root={}",
                 restartCfg.repoRoot.string(),
                 restartCfg.ueRoot.empty() ? "<from-script-default>" : restartCfg.ueRoot.string());
    if (auto r = registry->registerTool(
            sage::tools::buildRestartEditorTool(bridge, std::move(restartCfg)));
        !r.has_value()) {
        spdlog::warn("Failed to register 'restart_editor'");
    }

    // ---- wait_for_editor (Milestone 1.6c) ------------------------------
    // Replaces the polling+fixed-wait pattern callers used after restart_editor:
    // block until the plugin handshake completes (or matching slot_id arrives),
    // up to a timeout. Returns immediately when a matching session is already
    // present. Backed by BridgeServer::waitForSession (cv signaled on hello).
    {
        sage::mcp::Tool waitForEditorTool{
            .name        = "wait_for_editor",
            .description = "Block until an editor's plugin handshake completes (or the named "
                           "slot_id arrives) and return its session info. Replaces "
                           "list_editors polling + fixed sleeps after restart_editor. "
                           "If a matching editor is already connected, returns immediately "
                           "with already_connected=true. Timeout returns "
                           "EditorNotConnected (-32001).",
            .inputSchema = nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"slot_id",    {{"type", "string"},
                                    {"description", "Optional: only count editors with this slot_id"}}},
                    {"timeout_ms", {{"type", "integer"},
                                    {"minimum", 100}, {"maximum", 600000},
                                    {"description", "Default 120000 (2 min); max 600000 (10 min)"}}},
                }},
                {"additionalProperties", false},
            },
            .handler = [&bridge](const nlohmann::json& params) -> sage::mcp::ToolResult {
                std::optional<std::string> slotFilter;
                if (params.is_object() && params.contains("slot_id")
                    && params["slot_id"].is_string()) {
                    auto s = params["slot_id"].get<std::string>();
                    if (!s.empty()) slotFilter = std::move(s);
                }
                const int timeoutMs = std::clamp(
                    params.value("timeout_ms", 120000), 100, 600000);

                auto sessionToJson = [](const sage::bridge::EditorSession& s,
                                        bool already) -> nlohmann::json {
                    return {
                        {"already_connected", already},
                        {"session_id",        s.session_id},
                        {"slot_id",           s.slot_id},
                        {"instance_id",       s.instance_id},
                        {"label",             s.label},
                        {"project_id",        s.project_id},
                        {"project_path",      s.project_path},
                        {"engine_version",    s.engine_version},
                        {"pid",               s.pid},
                    };
                };

                // Fast path: matching editor already in sessions_.
                for (const auto& s : bridge.snapshotSessions()) {
                    if (!slotFilter || s.slot_id == *slotFilter) {
                        return sessionToJson(s, /*already=*/true);
                    }
                }

                // Slow path: park on the cv until handleHello signals or timeout.
                auto session = bridge.waitForSession(
                    slotFilter, std::chrono::milliseconds(timeoutMs));
                if (!session) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::EditorNotConnected,
                        slotFilter
                            ? "no editor with matching slot_id connected within timeout"
                            : "no editor connected within timeout"));
                }
                return sessionToJson(*session, /*already=*/false);
            },
            .remote = false,
        };
        if (auto r = registry->registerTool(std::move(waitForEditorTool));
            !r.has_value()) {
            spdlog::warn("Failed to register 'wait_for_editor'");
        }
    }

    // ---- Editor lifecycle notifications (Milestone 1.6c) ---------------
    // Bridge fires connected/disconnected callbacks; we relay them as MCP
    // `notifications/message` envelopes (spec-canonical, so MCP clients
    // with default logging UIs surface them without custom handlers).
    bridge.setSessionEventCallback(
        [&mcpServer](std::string_view kind,
                     const sage::bridge::EditorSession& s) {
            nlohmann::json data = {
                {"event",          std::string{kind}},  // "connected" / "disconnected"
                {"session_id",     s.session_id},
                {"slot_id",        s.slot_id},
                {"instance_id",    s.instance_id},
                {"label",          s.label},
                {"project_path",   s.project_path},
                {"project_id",     s.project_id},
                {"engine_version", s.engine_version},
                {"pid",            s.pid},
            };
            mcpServer.publishNotification("notifications/message", nlohmann::json{
                {"level",  "info"},
                {"logger", "sage.editor"},
                {"data",   std::move(data)},
            });
        });

    // ---- Real-time delta (Milestone 2.3b) -------------------------------
    // Plugin emits AssetRegistry deltas as `event` envelopes; we patch the
    // per-slot graph in place so the snapshot stays current without a full
    // re-index. Failures are logged but never rethrown — async stream.
    bridge.setEventHandler(
        [graphMgr, escCypher](std::string_view slot_id,
                              const sage::bridge::EventMessage& ev) {
            try {
                auto& store = graphMgr->acquireSlot(slot_id);
                const auto& p = ev.payload;

                if (ev.kind == "asset_added") {
                    if (!p.contains("path") || !p.contains("kind")) return;
                    std::ostringstream q;
                    q << "MERGE (a:Asset {path: "
                      << escCypher(p["path"].get<std::string>())
                      << "}) SET a.kind = "
                      << escCypher(p["kind"].get<std::string>()) << ";";
                    auto r = store.execute(q.str());
                    if (sage::graph::is_error(r)) {
                        spdlog::warn("delta asset_added failed: {}",
                                     sage::graph::error_of(r).message);
                    }
                }
                else if (ev.kind == "asset_removed") {
                    if (!p.contains("path")) return;
                    std::ostringstream q;
                    q << "MATCH (a:Asset {path: "
                      << escCypher(p["path"].get<std::string>())
                      << "}) DETACH DELETE a;";
                    auto r = store.execute(q.str());
                    if (sage::graph::is_error(r)) {
                        spdlog::warn("delta asset_removed failed: {}",
                                     sage::graph::error_of(r).message);
                    }
                }
                else if (ev.kind == "asset_renamed") {
                    if (!p.contains("old_path") || !p.contains("new_path")) return;
                    std::ostringstream del;
                    del << "MATCH (a:Asset {path: "
                        << escCypher(p["old_path"].get<std::string>())
                        << "}) DETACH DELETE a;";
                    auto delRes = store.execute(del.str());
                    if (sage::graph::is_error(delRes)) {
                        spdlog::warn("delta asset_renamed failed: {}",
                                     sage::graph::error_of(delRes).message);
                        return;
                    }

                    std::ostringstream add;
                    add << "MERGE (a:Asset {path: "
                        << escCypher(p["new_path"].get<std::string>())
                        << "})";
                    if (p.contains("kind") && p["kind"].is_string()) {
                        add << " SET a.kind = "
                            << escCypher(p["kind"].get<std::string>());
                    }
                    add << ";";
                    auto addRes = store.execute(add.str());
                    if (sage::graph::is_error(addRes)) {
                        spdlog::warn("delta asset_renamed failed: {}",
                                     sage::graph::error_of(addRes).message);
                    }
                }
                else {
                    spdlog::debug("delta: unhandled event kind='{}'", ev.kind);
                }
            } catch (const std::exception& ex) {
                spdlog::warn("delta handler threw on kind='{}' slot='{}': {}",
                             ev.kind, slot_id, ex.what());
            }
        });

    // ---- MCP Transport ---------------------------------------------------
    if (useHttp) {
        sage::transport::HttpSseConfig httpCfg{
            .host            = envOr("SAGE_HTTP_HOST", "127.0.0.1"),
            .port            = envIntOr("SAGE_HTTP_PORT", 7777),
            .mcpEndpoint     = "/mcp",
            .readTimeoutSec  = 30,
            .writeTimeoutSec = 30,
        };
        sage::transport::HttpSseServer transport(mcpServer, httpCfg);
        g_runningHttp.store(&transport, std::memory_order_release);

        std::signal(SIGINT,  &signalHandler);
        std::signal(SIGTERM, &signalHandler);

        const bool ok = transport.listen();

        g_runningHttp.store(nullptr, std::memory_order_release);
        bridge.stop();
        g_runningBridge.store(nullptr, std::memory_order_release);

        if (!ok) {
            spdlog::error("HTTP server failed to bind {}:{}", httpCfg.host, httpCfg.port);
            return 1;
        }
    } else {
        // stdio: parent (Claude Code) closes stdin to end the session.
        // Minimal SIGINT handler so Ctrl+C from a manual shell still cleans
        // the bridge before terminating.
        std::signal(SIGINT, [](int) {
            if (auto* b = g_runningBridge.load(std::memory_order_acquire)) b->stop();
        });

        sage::transport::StdioMcp stdio(mcpServer);

        // Wire server-pushed notifications (editor connect/disconnect) to the
        // stdio writer. The writer is mutex-serialized internally so concurrent
        // emissions from the bridge worker thread can't tear a JSON-RPC line.
        mcpServer.setNotificationSink(
            [&stdio](const nlohmann::json& envelope) {
                stdio.writeJson(envelope);
            });

        stdio.run();

        // Detach the sink before stdio leaves scope — late notifications from
        // the bridge worker would dereference a dangling reference.
        mcpServer.setNotificationSink(nullptr);

        bridge.stop();
        g_runningBridge.store(nullptr, std::memory_order_release);
    }

    spdlog::info("Bye");
    return 0;
}
