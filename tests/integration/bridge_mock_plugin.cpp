// Sage bridge mock plugin (server-side end-to-end smoke harness).
//
// Connects to sage-server's bridge endpoint, performs the handshake, then
// waits for a single `tool_call` and replies with `tool_result` (echoing
// args back with an `echoed_by:"mock"` marker). Exits 0 on success, 1 on
// timeout/error.
//
// Lets us run end-to-end tool dispatch tests (Claude → server → mock plugin
// → server → response) without a real UE editor in the loop.

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr std::chrono::milliseconds TIMEOUT{30'000};

}  // namespace

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "ws://127.0.0.1:7778/bridge";

    ix::initNetSystem();
    ix::WebSocket ws;
    ws.setUrl(url);
    ws.disableAutomaticReconnection();

    std::atomic<bool> done{false};
    std::atomic<bool> errored{false};

    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            std::cout << "[mock] connected; sending hello" << std::endl;
            const nlohmann::json hello = {
                {"type",    "hello"},
                {"version", "0.1.0"},
                {"slot_id", "mock-slot"},
                {"editor", {
                    {"id",             "mock@1"},
                    {"label",          "mock"},
                    {"project_id",     ""},
                    {"project_path",   "/tmp/mock"},
                    {"engine_version", "5.7.4"},
                    {"session_id",     "mock-1"},
                    {"pid",            1},
                }},
            };
            ws.send(hello.dump());
            break;
        }
        case ix::WebSocketMessageType::Message: {
            try {
                const auto j = nlohmann::json::parse(msg->str);
                const auto type = j.value("type", std::string{});
                if (type == "welcome") {
                    std::cout << "[mock] welcome received; awaiting tool_call" << std::endl;
                } else if (type == "tool_call") {
                    const auto txId = j.value("tx_id", std::string{});
                    const auto tool = j.value("tool",  std::string{});
                    const auto args = j.value("args",  nlohmann::json::object());
                    std::cout << "[mock] tool_call: " << tool
                              << " tx=" << txId
                              << " args=" << args.dump() << std::endl;

                    nlohmann::json result = args;
                    result["echoed_by"] = "mock";
                    result["tool"]      = tool;

                    const nlohmann::json reply = {
                        {"type",    "tool_result"},
                        {"tx_id",   txId},
                        {"success", true},
                        {"result",  result},
                    };
                    ws.send(reply.dump());
                    std::cout << "[mock] tool_result sent" << std::endl;
                    done.store(true);
                }
            } catch (const std::exception& ex) {
                std::cerr << "[mock] parse error: " << ex.what() << std::endl;
                errored.store(true);
            }
            break;
        }
        case ix::WebSocketMessageType::Error:
            std::cerr << "[mock] connection error: " << msg->errorInfo.reason << std::endl;
            errored.store(true);
            break;
        default:
            break;
        }
    });

    ws.start();

    const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done.load() || errored.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ws.stop();
    ix::uninitNetSystem();

    if (done.load()) {
        std::cout << "[mock] OK — handshake + tool_call echoed" << std::endl;
        return 0;
    }
    std::cerr << "[mock] FAIL (timeout=" << !done.load()
              << ", err=" << errored.load() << ")" << std::endl;
    return 1;
}
