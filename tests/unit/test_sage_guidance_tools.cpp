#include "mcp/tool_registry.h"
#include "tools/sage_guidance_tools.h"
#include "version.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

using namespace sage::mcp;

namespace {

ToolRegistry makeGuidanceRegistry() {
    ToolRegistry reg;
    sage::tools::SageGuidanceContext context{
        .serverVersion = std::string{sage::kServerVersion},
        .protocolVersion = std::string{sage::kProtocolVersion},
        .transport = "stdio",
        .httpEndpoint = "http://127.0.0.1:7777/mcp",
        .bridgeEndpoint = "ws://127.0.0.1:7778/bridge",
        .editorSessions = [] {
            return nlohmann::json{{"count", 0}, {"editors", nlohmann::json::array()}};
        },
        .registrySummary = [] {
            return nlohmann::json{
                {"total_tools", 8},
                {"remote_tools", 0},
                {"local_tools", 8},
                {"prefix_counts", nlohmann::json{{"sage", 8}}},
            };
        },
    };
    sage::tools::registerSageGuidanceTools(reg, std::move(context));
    return reg;
}

std::filesystem::path uniqueTempProjectRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("sage-guidance-test-" + std::to_string(stamp));
}

}  // namespace

TEST_CASE("Sage guidance tools expose agent onboarding", "[tools][guidance]") {
    ToolRegistry reg = makeGuidanceRegistry();
    REQUIRE(reg.has("sage.about"));
    REQUIRE(reg.has("sage.status"));
    REQUIRE(reg.has("sage.doctor"));
    REQUIRE(reg.has("sage.project.discover"));
    REQUIRE(reg.has("sage.capabilities"));
    REQUIRE(reg.has("sage.workflow.suggest"));
    REQUIRE(reg.has("sage.help"));
    REQUIRE(reg.has("sage.guide"));

    auto about = reg.dispatch("sage.about", nlohmann::json::object());
    REQUIRE(about.has_value());
    REQUIRE((*about)["server_version"] == std::string{sage::kServerVersion});
    REQUIRE((*about)["what_to_do_first"].is_array());

    auto workflow = reg.dispatch("sage.workflow.suggest",
                                 nlohmann::json{{"intent", "edit a Blueprint safely"}});
    REQUIRE(workflow.has_value());
    REQUIRE((*workflow)["workflow_id"] == "blueprint_edit");
    REQUIRE((*workflow)["steps"].dump().find("bp.full_dump") != std::string::npos);

    auto guide = reg.dispatch("sage.guide", nlohmann::json{{"topic", "niagara"}});
    REQUIRE(guide.has_value());
    REQUIRE((*guide)["topic"] == "niagara");
    REQUIRE((*guide)["recommended_sequence"].dump().find("niagara.validate_system") != std::string::npos);
}

TEST_CASE("Sage project discovery reports SageBridge version", "[tools][guidance]") {
    ToolRegistry reg = makeGuidanceRegistry();
    const std::filesystem::path root = uniqueTempProjectRoot();
    const std::filesystem::path nested = root / "Content" / "Maps";
    const std::filesystem::path plugin = root / "Plugins" / "SageBridge";
    std::filesystem::create_directories(nested);
    std::filesystem::create_directories(plugin / "Source");

    {
        std::ofstream project(root / "GuidanceSmoke.uproject");
        project << "{}\n";
    }
    {
        std::ofstream descriptor(plugin / "SageBridge.uplugin");
        descriptor << "{\n  \"VersionName\": \"" << sage::kServerVersion << "\"\n}\n";
    }

    auto discovered = reg.dispatch("sage.project.discover",
                                   nlohmann::json{{"start_path", nested.string()}});
    REQUIRE(discovered.has_value());
    REQUIRE((*discovered)["ok"] == true);
    REQUIRE((*discovered)["sagebridge"]["descriptor_exists"] == true);
    REQUIRE((*discovered)["sagebridge"]["plugin_version"] == std::string{sage::kServerVersion});
    REQUIRE((*discovered)["sagebridge"]["version_status"] == "current");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
