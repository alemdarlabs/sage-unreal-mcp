// Sage bridge integration smoke test.
//
// Connects to the running sage-server (ws://127.0.0.1:7778/bridge),
// performs the plugin handshake + one heartbeat round-trip, validates
// that the server replies with `welcome` and `heartbeat_ack`.
//
// Usage:
//   sage-bridge-smoke [ws://host:port/bridge]
// Defaults to ws://127.0.0.1:7778/bridge.
//
// Returns 0 on success, 1 on failure / timeout.

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

constexpr int TIMEOUT_MS = 5000;

}  // namespace

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "ws://127.0.0.1:7778/bridge";

    ix::initNetSystem();

    ix::WebSocket ws;
    ws.setUrl(url);
    ws.disableAutomaticReconnection();

    std::atomic<bool> gotWelcome{false};
    std::atomic<bool> gotHbAck{false};
    std::atomic<bool> errored{false};

    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            std::cout << "[smoke] connected; sending hello" << std::endl;
            const nlohmann::json hello = {
                {"type",    "hello"},
                {"version", "0.1.0"},
                {"slot_id", "smoke-slot"},
                {"editor", {
                    {"id",             "smoke@1"},
                    {"label",          "smoke"},
                    {"project_id",     ""},
                    {"project_path",   "/tmp/smoke"},
                    {"engine_version", "5.7.4"},
                    {"session_id",     "smoke-1"},
                    {"pid",            1},
                }},
            };
            ws.send(hello.dump());
            break;
        }
        case ix::WebSocketMessageType::Message: {
            std::cout << "[smoke] recv: " << msg->str << std::endl;
            try {
                const auto j = nlohmann::json::parse(msg->str);
                const auto type = j.value("type", std::string{});
                if (type == "welcome") {
                    gotWelcome.store(true);
                    const nlohmann::json hb = {
                        {"type", "heartbeat"},
                        {"ts",   1234},
                    };
                    ws.send(hb.dump());
                } else if (type == "heartbeat_ack") {
                    gotHbAck.store(true);
                } else if (type == "error") {
                    errored.store(true);
                    std::cerr << "[smoke] server error: " << msg->str << std::endl;
                }
            } catch (const std::exception& ex) {
                std::cerr << "[smoke] parse error: " << ex.what() << std::endl;
                errored.store(true);
            }
            break;
        }
        case ix::WebSocketMessageType::Error:
            std::cerr << "[smoke] connection error: " << msg->errorInfo.reason << std::endl;
            errored.store(true);
            break;
        case ix::WebSocketMessageType::Close:
            std::cout << "[smoke] closed" << std::endl;
            break;
        default:
            break;
        }
    });

    ws.start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        if ((gotWelcome.load() && gotHbAck.load()) || errored.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ws.stop();
    ix::uninitNetSystem();

    const bool ok = gotWelcome.load() && gotHbAck.load() && !errored.load();
    if (ok) {
        std::cout << "[smoke] OK — welcome + heartbeat_ack received" << std::endl;
        return 0;
    }
    std::cerr << "[smoke] FAIL "
              << "(welcome=" << gotWelcome.load()
              << ", hb_ack="  << gotHbAck.load()
              << ", err="     << errored.load()
              << ")" << std::endl;
    return 1;
}
