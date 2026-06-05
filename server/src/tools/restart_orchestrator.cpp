#include "tools/restart_orchestrator.h"

#include "mcp/error_codes.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <optional>
#include <thread>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#  define popen  _popen
#  define pclose _pclose
using pid_t = int;
#else
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
#endif

namespace sage::tools {

namespace {

namespace fs = std::filesystem;

// Host platform binary suffix mirrors UAT BuildPlugin layout.
#if defined(__APPLE__)
constexpr const char* kPlatformDir = "Mac";
constexpr const char* kUbtBuildScript = "Engine/Build/BatchFiles/Mac/Build.sh";
#elif defined(__linux__)
constexpr const char* kPlatformDir = "Linux";
constexpr const char* kUbtBuildScript = "Engine/Build/BatchFiles/Linux/Build.sh";
#elif defined(_WIN32)
constexpr const char* kPlatformDir = "Win64";
constexpr const char* kUbtBuildScript = "Engine/Build/BatchFiles/Build.bat";
#else
#  error "unsupported platform for restart_orchestrator"
#endif

struct ScriptResult {
    int         exitCode = -1;
    std::string lastOutput;  // tail of stdout+stderr for error context
};

struct EditorTargetResolution {
    std::string target;
    std::string source;
};

std::string quoteShellArg(const std::string& value) {
#if defined(_WIN32)
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += "\\\"";
        else         out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
#endif
}

std::string quoteShellArg(const fs::path& value) {
    return quoteShellArg(value.string());
}

std::string errnoMessage(int errorCode) {
#if defined(_WIN32)
    char buffer[256]{};
    if (strerror_s(buffer, sizeof(buffer), errorCode) == 0 && buffer[0] != '\0') {
        return std::string{buffer};
    }
    return "errno " + std::to_string(errorCode);
#else
    const char* message = std::strerror(errorCode);
    return message != nullptr ? std::string{message}
                              : "errno " + std::to_string(errorCode);
#endif
}

fs::path normalizeRepoRoot(fs::path root) {
    root = root.lexically_normal();
    // Some launch configs accidentally pass .../sage-unreal-mcp/scripts as
    // SAGE_REPO_ROOT. Accept that shape so restart_editor does not construct
    // scripts/scripts/build-plugin.*.
    if (root.filename() == fs::path{"scripts"}
        && fs::exists(root.parent_path() / "server")
        && fs::exists(root.parent_path() / "scripts")) {
        return root.parent_path();
    }
    return root;
}

std::optional<fs::path> findBuildPluginScript(const fs::path& repoRoot) {
#if defined(_WIN32)
    constexpr const char* kBuildPluginScript = "build-plugin.ps1";
#else
    constexpr const char* kBuildPluginScript = "build-plugin.sh";
#endif
    const fs::path candidates[] = {
        repoRoot / "scripts" / kBuildPluginScript,
        repoRoot / kBuildPluginScript,
    };
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return std::nullopt;
}

std::string buildPluginCommand(const fs::path& script, const fs::path& ueRoot) {
#if defined(_WIN32)
    std::string cmd;
    if (!ueRoot.empty()) {
        cmd += "set \"SAGE_UE_ROOT=" + ueRoot.string() + "\" && ";
    }
    cmd += "powershell.exe -NoProfile -ExecutionPolicy Bypass -File "
        + quoteShellArg(script);
    return cmd;
#else
    std::string cmd;
    if (!ueRoot.empty()) {
        cmd = "SAGE_UE_ROOT=" + quoteShellArg(ueRoot) + " ";
    }
    cmd += quoteShellArg(script);
    return cmd;
#endif
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size()
        && value.substr(value.size() - suffix.size()) == suffix;
}

std::string stripTargetCsSuffix(const fs::path& path) {
    std::string name = path.filename().string();
    constexpr std::string_view suffix = ".Target.cs";
    if (endsWith(name, suffix)) {
        name.resize(name.size() - suffix.size());
    }
    return name;
}

std::optional<EditorTargetResolution> resolveEditorTargetFromSource(
    const fs::path& projectPath) {
    const fs::path sourceDir = projectPath.parent_path() / "Source";
    if (!fs::is_directory(sourceDir)) {
        return std::nullopt;
    }

    const std::string preferred = projectPath.stem().string() + "Editor";
    std::vector<fs::path> editorTargets;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(sourceDir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string target = stripTargetCsSuffix(entry.path());
        if (target == entry.path().filename().string()) continue;
        if (!endsWith(target, "Editor")) continue;
        if (target == preferred) {
            return EditorTargetResolution{
                target,
                "Source target file: " + entry.path().string(),
            };
        }
        editorTargets.push_back(entry.path());
    }

    if (editorTargets.size() == 1) {
        return EditorTargetResolution{
            stripTargetCsSuffix(editorTargets.front()),
            "Source target file: " + editorTargets.front().string(),
        };
    }
    return std::nullopt;
}

std::optional<EditorTargetResolution> resolveEditorTargetFromUProject(
    const fs::path& projectPath) {
    std::ifstream in(projectPath);
    if (!in) {
        return std::nullopt;
    }

    nlohmann::json project = nlohmann::json::parse(in, nullptr, false);
    if (project.is_discarded() || !project.is_object()
        || !project.contains("Modules") || !project["Modules"].is_array()) {
        return std::nullopt;
    }

    const std::string preferred = projectPath.stem().string() + "Editor";
    std::vector<std::string> editorModules;
    for (const auto& module : project["Modules"]) {
        if (!module.is_object()
            || !module.contains("Name") || !module["Name"].is_string()
            || !module.contains("Type") || !module["Type"].is_string()) {
            continue;
        }
        if (module["Type"].get<std::string>() != "Editor") {
            continue;
        }

        const std::string name = module["Name"].get<std::string>();
        if (name == preferred) {
            return EditorTargetResolution{name, ".uproject Modules[] Editor entry"};
        }
        editorModules.push_back(name);
    }

    if (editorModules.size() == 1) {
        return EditorTargetResolution{
            editorModules.front(),
            ".uproject Modules[] Editor entry",
        };
    }
    return std::nullopt;
}

EditorTargetResolution resolveEditorTarget(const fs::path& projectPath) {
    if (auto fromSource = resolveEditorTargetFromSource(projectPath)) {
        return *fromSource;
    }
    if (auto fromProject = resolveEditorTargetFromUProject(projectPath)) {
        return *fromProject;
    }
    return EditorTargetResolution{
        projectPath.stem().string() + "Editor",
        "fallback: <ProjectName>Editor",
    };
}

std::string buildProjectModulesCommand(const fs::path& buildScript,
                                       const std::string& editorTarget,
                                       const fs::path& projectPath) {
#if defined(_WIN32)
    // _popen runs through cmd.exe. A quoted .bat path as the first token can
    // be parsed as a malformed command; `call` handles quoted batch paths.
    std::string cmd = "call " + quoteShellArg(buildScript);
#else
    std::string cmd = quoteShellArg(buildScript);
#endif
    cmd += " " + editorTarget + " " + std::string{kPlatformDir}
         + " Development -Project=" + quoteShellArg(projectPath)
         + " -WaitMutex -NoHotReload";
    return cmd;
}

mcp::ErrorObject restartError(mcp::ErrorCode code,
                              std::string detail,
                              const nlohmann::json& data) {
    mcp::ErrorObject err = mcp::ErrorObject::fromCode(code, std::move(detail));
    err.data = data;
    return err;
}

bool pathIsInsideOrEqual(const fs::path& root, const fs::path& candidate) {
    std::error_code ec;
    const fs::path rootAbs = fs::absolute(root, ec).lexically_normal();
    if (ec) return false;
    const fs::path candAbs = fs::absolute(candidate, ec).lexically_normal();
    if (ec) return false;
    if (candAbs == rootAbs) return true;

    const fs::path rel = candAbs.lexically_relative(rootAbs);
    if (rel.empty() || rel.is_absolute()) return false;
    for (const fs::path& part : rel) {
        if (part == "..") return false;
    }
    return true;
}

std::optional<std::string> copyFileChecked(const fs::path& src,
                                           const fs::path& dst,
                                           const char* label) {
    if (!fs::is_regular_file(src)) {
        return std::string{label} + " source file not found: " + src.string();
    }

    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (ec) {
        return std::string{label} + " create destination directory failed: "
             + dst.parent_path().string() + ": " + ec.message();
    }

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::string{label} + " copy failed " + src.string() + " -> "
             + dst.string() + ": " + ec.message();
    }
    return std::nullopt;
}

std::optional<std::string> replaceDirectoryChecked(const fs::path& src,
                                                   const fs::path& dst,
                                                   const fs::path& allowedRoot,
                                                   const char* label) {
    if (!fs::is_directory(src)) {
        return std::string{label} + " source directory not found: " + src.string();
    }
    if (!pathIsInsideOrEqual(allowedRoot, dst)) {
        return std::string{"refusing to replace "} + label
             + " outside plugin root: " + dst.string();
    }

    std::error_code ec;
    if (fs::exists(dst, ec)) {
        fs::remove_all(dst, ec);
        if (ec) {
            return std::string{label} + " remove old destination failed: "
                 + dst.string() + ": " + ec.message();
        }
    }

    fs::create_directories(dst.parent_path(), ec);
    if (ec) {
        return std::string{label} + " create destination parent failed: "
             + dst.parent_path().string() + ": " + ec.message();
    }

    fs::copy(src, dst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        return std::string{label} + " copy failed " + src.string() + " -> "
             + dst.string() + ": " + ec.message();
    }
    return std::nullopt;
}

std::optional<std::string> deployPackagedPlugin(const fs::path& repoRoot,
                                                const fs::path& pluginRoot,
                                                nlohmann::json& result) {
    const fs::path packagedRoot = repoRoot / "build" / "plugin";
    const fs::path descriptorSrc = packagedRoot / "SageBridge.uplugin";
    const fs::path descriptorDst = pluginRoot / "SageBridge.uplugin";
    const fs::path binariesSrc = packagedRoot / "Binaries" / kPlatformDir;
    const fs::path binariesDst = pluginRoot / "Binaries" / kPlatformDir;
    const fs::path sourceSrc = packagedRoot / "Source";
    const fs::path sourceDst = pluginRoot / "Source";

    if (!pathIsInsideOrEqual(pluginRoot.parent_path(), pluginRoot)) {
        return "refusing to deploy SageBridge outside the project Plugins directory: "
             + pluginRoot.string();
    }

    if (auto err = copyFileChecked(descriptorSrc, descriptorDst, "plugin descriptor")) {
        return err;
    }

    if (!fs::is_directory(binariesSrc)) {
        return "packaged plugin binary directory not found: " + binariesSrc.string();
    }

    int binaryFileCount = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(binariesSrc)) {
        if (!entry.is_regular_file()) continue;
        const fs::path dst = binariesDst / entry.path().filename();
        if (auto err = copyFileChecked(entry.path(), dst, "plugin binary")) {
            return err;
        }
        ++binaryFileCount;
    }
    if (binaryFileCount == 0) {
        return "packaged plugin binary directory contained no files: "
             + binariesSrc.string();
    }

    if (auto err = replaceDirectoryChecked(sourceSrc, sourceDst,
                                           pluginRoot, "plugin Source")) {
        return err;
    }

    result["plugin_descriptor_swapped"] = true;
    result["plugin_binary_file_count"]  = binaryFileCount;
    result["plugin_source_swapped"]     = true;
    result["plugin_root"]               = pluginRoot.string();
    result["plugin_binaries_dir"]       = binariesDst.string();
    return std::nullopt;
}

// Run a shell command, capture combined stdout/stderr, return exit code.
// We keep only the last 32KB of output so a chatty UAT log doesn't blow
// the JSON response size.
ScriptResult runShell(const std::string& cmd) {
    constexpr size_t kKeepBytes = 32 * 1024;
    ScriptResult out;
    std::string buf;
    buf.reserve(kKeepBytes + 1024);

    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        out.lastOutput = "popen failed: " + errnoMessage(errno);
        return out;
    }
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) {
        buf.append(chunk);
        if (buf.size() > kKeepBytes * 2) {
            buf.erase(0, buf.size() - kKeepBytes);
        }
    }
    const int rc = ::pclose(pipe);
    out.lastOutput = std::move(buf);
#if defined(_WIN32)
    // _pclose returns the spawned program's exit code directly.
    out.exitCode = rc;
#else
    if (rc == -1) {
        out.exitCode = -1;
    } else if (WIFEXITED(rc)) {
        out.exitCode = WEXITSTATUS(rc);
    } else {
        out.exitCode = -1;
    }
#endif
    return out;
}

bool processAlive(pid_t pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    // OpenProcess + GetExitCodeProcess. STILL_ACTIVE (259) means the process
    // is running; any other value is its exit code.
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(pid));
    if (h == nullptr) return false;
    DWORD code = 0;
    bool alive = false;
    if (::GetExitCodeProcess(h, &code)) {
        alive = (code == STILL_ACTIVE);
    }
    ::CloseHandle(h);
    return alive;
#else
    // ::kill(pid, 0) returns 0 if signalable; ESRCH means gone, EPERM means
    // it's there but not ours (still 'alive' for our purposes).
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
#endif
}

bool killEditor(pid_t pid, std::chrono::seconds graceWindow) {
    if (!processAlive(pid)) return true;

#if defined(_WIN32)
    // Windows has no clean SIGTERM analog for a Slate window. Live Coding is
    // available on Windows so restart_editor is rarely the right tool here;
    // when it IS invoked the caller already passed confirmed=true and
    // save_dirty has run. Go straight to TerminateProcess and wait for the
    // OS to release the kernel object before the next-step plugin .dll swap.
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                             static_cast<DWORD>(pid));
    if (h == nullptr) {
        spdlog::warn("restart_editor: OpenProcess failed pid={} err={}",
                     pid, ::GetLastError());
        return false;
    }
    if (!::TerminateProcess(h, 1)) {
        spdlog::error("restart_editor: TerminateProcess failed pid={} err={}",
                      pid, ::GetLastError());
        ::CloseHandle(h);
        return false;
    }
    ::WaitForSingleObject(h, static_cast<DWORD>(graceWindow.count() * 1000));
    ::CloseHandle(h);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return !processAlive(pid);
#else
    if (::kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        spdlog::warn("restart_editor: SIGTERM failed pid={}: {}", pid, errnoMessage(errno));
    }

    const auto deadline = std::chrono::steady_clock::now() + graceWindow;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!processAlive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    spdlog::warn("restart_editor: pid={} still alive after {}s, sending SIGKILL",
                 pid, graceWindow.count());
    if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        spdlog::error("restart_editor: SIGKILL failed pid={}: {}", pid, errnoMessage(errno));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return !processAlive(pid);
#endif
}

bool relaunchEditor(const std::string& projectPath) {
#if defined(__APPLE__)
    const auto cmd = "open '" + projectPath + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#elif defined(__linux__)
    // UnrealEditor binary path is conventional under UE_ROOT but we don't
    // wire that here yet — Linux relaunch arrives in Phase 3.
    spdlog::warn("relaunchEditor: Linux not yet implemented");
    (void)projectPath;
    return false;
#elif defined(_WIN32)
    const auto cmd = "start \"\" \"" + projectPath + "\"";
    return std::system(cmd.c_str()) == 0;
#endif
}

// Wait for a handshake whose slot_id matches `expectedSlot` AND whose
// session_id differs from the old one (so we know it's the new instance,
// not the about-to-die old one). Returns the new session_id, or empty on
// timeout.
std::string waitForNewSession(bridge::BridgeServer& bridge,
                               std::string_view expectedSlot,
                               std::string_view oldSessionId,
                               std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& s : bridge.snapshotSessions()) {
            if (s.slot_id == expectedSlot && s.session_id != oldSessionId) {
                return s.session_id;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return {};
}

}  // namespace

mcp::Tool buildRestartEditorTool(bridge::BridgeServer& bridge, RestartConfig cfg) {
    const auto schema = nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"confirmed",                {{"type", "boolean"},
                                          {"description", "MUST be true. Safety guard."}}},
            {"build_plugin",             {{"type", "boolean"}}},
            {"save_dirty",               {{"type", "boolean"}}},
            {"rebuild_project_modules",  {{"type", "boolean"},
                                          {"description",
                                              "Full restart substitute for Live Coding: "
                                              "between editor kill and relaunch, run "
                                              "UBT to rebuild the resolved Editor target. "
                                              "Required after editing project C++ "
                                              "sources (added classes, modified UCLASS "
                                              "members, etc) — UE editor relaunch "
                                              "alone does NOT recompile project modules."}}},
            {"wait_handshake_sec",       {{"type", "integer"},
                                          {"minimum", 0}, {"maximum", 300}}},
            {"slot_id",                  {{"type", "string"}}},
        }},
        {"required", nlohmann::json::array({"confirmed"})},
        {"additionalProperties", false},
    };

    auto handler = [&bridge, cfg](const nlohmann::json& params) -> mcp::ToolResult {
        if (!params.value("confirmed", false)) {
            return std::unexpected(mcp::ErrorObject::fromCode(
                mcp::ErrorCode::InvalidParams,
                "restart_editor requires confirmed=true (terminates the editor)"));
        }
        const bool buildPlugin     = params.value("build_plugin", true);
        const bool saveDirty       = params.value("save_dirty",   true);
        const bool rebuildProject  = params.value("rebuild_project_modules", false);
        const int  waitHandshakeS  = params.value("wait_handshake_sec", 90);
        const fs::path repoRoot = normalizeRepoRoot(cfg.repoRoot);

        // ---- Resolve target session ------------------------------------
        std::optional<bridge::EditorSession> session;
        if (params.contains("slot_id") && params["slot_id"].is_string()) {
            const auto slotId = params["slot_id"].get<std::string>();
            for (const auto& s : bridge.snapshotSessions()) {
                if (s.slot_id == slotId) { session = s; break; }
            }
        } else {
            const auto activeId = bridge.activeSession();
            if (!activeId.empty()) session = bridge.snapshotSession(activeId);
            if (!session) {
                const auto all = bridge.snapshotSessions();
                if (all.size() == 1) session = all.front();
            }
        }
        if (!session) {
            return std::unexpected(mcp::ErrorObject::fromCode(
                mcp::ErrorCode::EditorNotConnected,
                "no editor session to restart"));
        }
        if (session->pid <= 0) {
            return std::unexpected(mcp::ErrorObject::fromCode(
                mcp::ErrorCode::InternalError,
                "session has no pid; cannot terminate (handshake may be stale)"));
        }
        if (session->project_path.empty()) {
            return std::unexpected(mcp::ErrorObject::fromCode(
                mcp::ErrorCode::InternalError,
                "session has no project_path; cannot relaunch"));
        }

        const auto projectPath  = session->project_path;
        const auto projectDir   = fs::path{projectPath}.parent_path();
        const auto pluginRoot    = projectDir / "Plugins" / "SageBridge";
        const auto editorPid    = static_cast<pid_t>(session->pid);
        const auto oldSessionId = session->session_id;
        const auto slotId       = session->slot_id;

        nlohmann::json result = {
            {"slot_id",        slotId},
            {"old_session_id", oldSessionId},
            {"build_plugin",   buildPlugin},
            {"rebuild_project_modules", rebuildProject},
            {"editor_terminated", false},
        };

        fs::path projectBuildScript;
        EditorTargetResolution editorTarget;
        if (rebuildProject) {
            if (cfg.ueRoot.empty()) {
                return std::unexpected(mcp::ErrorObject::fromCode(
                    mcp::ErrorCode::InternalError,
                    "rebuild_project_modules requires SAGE_UE_ROOT or "
                    "RestartConfig::ueRoot to be set"));
            }
            projectBuildScript = cfg.ueRoot / kUbtBuildScript;
            if (!fs::exists(projectBuildScript)) {
                return std::unexpected(mcp::ErrorObject::fromCode(
                    mcp::ErrorCode::InternalError,
                    "UBT build script not found: " + projectBuildScript.string()));
            }
            editorTarget = resolveEditorTarget(fs::path{projectPath});
            result["rebuild_project_target"] = editorTarget.target;
            result["rebuild_project_target_source"] = editorTarget.source;
        }

        // ---- Step 1: save_dirty (best-effort) --------------------------
        if (saveDirty) {
            const auto t0 = std::chrono::steady_clock::now();
            auto sr = bridge.dispatchTool("save_assets", nlohmann::json::object(),
                                           std::chrono::seconds(20));
            const auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            result["save_ms"] = saveMs;
            if (!sr.has_value()) {
                spdlog::warn("restart_editor: save_assets failed: {}",
                             sr.error().message);
                result["save_warning"] = sr.error().message;
            }
        }

        // ---- Step 2: build plugin (BEFORE killing — abort on failure) --
        if (buildPlugin) {
            const std::optional<fs::path> script = findBuildPluginScript(repoRoot);
            if (!script) {
                return std::unexpected(mcp::ErrorObject::fromCode(
                    mcp::ErrorCode::InternalError,
                    "build script not found under repo root: " + repoRoot.string()));
            }
            const std::string cmd = buildPluginCommand(*script, cfg.ueRoot);

            spdlog::info("restart_editor: running {}", cmd);
            const auto t0 = std::chrono::steady_clock::now();
            const auto br = runShell(cmd);
            const auto buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            result["build_ms"]   = buildMs;
            result["build_exit"] = br.exitCode;
            if (br.exitCode != 0) {
                // Tail kept short — full log lives at LocalBuildLogs/.
                const auto tail = br.lastOutput.size() > 2000
                    ? br.lastOutput.substr(br.lastOutput.size() - 2000)
                    : br.lastOutput;
                return std::unexpected(mcp::ErrorObject::fromCode(
                    mcp::ErrorCode::InternalError,
                    "plugin build failed (exit=" + std::to_string(br.exitCode)
                    + "); tail:\n" + tail));
            }
        }

        // ---- Step 3: terminate editor ---------------------------------
        const auto t0 = std::chrono::steady_clock::now();
        if (!killEditor(editorPid, std::chrono::seconds(8))) {
            return std::unexpected(mcp::ErrorObject::fromCode(
                mcp::ErrorCode::InternalError,
                "failed to terminate editor pid=" + std::to_string(editorPid)));
        }
        result["kill_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        result["editor_terminated"] = true;

        // ---- Step 4: deploy packaged plugin ---------------------------
        if (buildPlugin) {
            if (const auto deployError = deployPackagedPlugin(repoRoot, pluginRoot, result)) {
                return std::unexpected(restartError(
                    mcp::ErrorCode::InternalError,
                    *deployError,
                    result));
            }
            result["plugin_deployed"] = true;
        }

        // ---- Step 4b: rebuild project modules (UBT) -------------------
        // Mac/Linux substitute for Live Coding. Editor MUST be killed first
        // (Step 3) so module .dylib's aren't memory-mapped. UE 5.7 macOS
        // doesn't ship Live Coding; without this step a freshly-added
        // project C++ class never registers in reflection and bp_reparent /
        // bp_get_cdo_properties on /Script/<Project>.<Class> fail with
        // "class not found".
        if (rebuildProject) {
            const std::string cmd = buildProjectModulesCommand(
                projectBuildScript, editorTarget.target, fs::path{projectPath});

            spdlog::info("restart_editor: rebuilding project module: {}", cmd);
            const auto t1 = std::chrono::steady_clock::now();
            const auto pr = runShell(cmd);
            const auto rebuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t1).count();
            result["rebuild_project_ms"]   = rebuildMs;
            result["rebuild_project_exit"] = pr.exitCode;
            result["rebuild_project_target"] = editorTarget.target;
            result["rebuild_project_target_source"] = editorTarget.source;
            if (pr.exitCode != 0) {
                const auto tail = pr.lastOutput.size() > 3000
                    ? pr.lastOutput.substr(pr.lastOutput.size() - 3000)
                    : pr.lastOutput;
                return std::unexpected(restartError(
                    mcp::ErrorCode::InternalError,
                    "project rebuild failed (exit=" + std::to_string(pr.exitCode)
                    + " target=" + editorTarget.target
                    + " editor_terminated=true); tail:\n" + tail,
                    result));
            }
            result["rebuild_project"] = true;
        }

        // ---- Step 5: relaunch ----------------------------------------
        if (!relaunchEditor(projectPath)) {
            return std::unexpected(restartError(
                mcp::ErrorCode::InternalError,
                "editor relaunch failed for " + projectPath,
                result));
        }

        // ---- Step 6: wait for new handshake ---------------------------
        if (waitHandshakeS > 0) {
            const auto t1 = std::chrono::steady_clock::now();
            auto newSessionId = waitForNewSession(
                bridge, slotId, oldSessionId,
                std::chrono::seconds(waitHandshakeS));
            result["handshake_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t1).count();
            if (newSessionId.empty()) {
                return std::unexpected(restartError(
                    mcp::ErrorCode::InternalError,
                    "editor relaunched but did not handshake within "
                    + std::to_string(waitHandshakeS) + "s",
                    result));
            }
            result["new_session_id"] = newSessionId;
        }
        result["restarted"] = true;
        return result;
    };

    return mcp::Tool{
        .name        = "restart_editor",
        .description = "Save dirty assets → (optionally) build plugin via UAT "
                       "→ terminate editor → swap plugin dylib → (optionally) "
                       "rebuild project modules via UBT → relaunch → wait for "
                       "handshake. Full-restart stand-in for Live Coding. "
                       "REQUIRES confirmed=true. Aborts before kill if plugin "
                       "build fails. Pass rebuild_project_modules=true after "
                       "editing project C++ sources (added classes / modified "
                       "UCLASS) — UE editor relaunch alone does NOT recompile "
                       "project modules. Resolves the real editor target from "
                       "Source/*.Target.cs or .uproject Modules instead of "
                       "assuming <ProjectName>Editor. Returns the new "
                       "session_id on success along with build/rebuild durations.",
        .inputSchema = schema,
        .handler     = std::move(handler),
        .remote      = false,
    };
}

}  // namespace sage::tools
