#include "graph/graph_store_manager.h"

#include "graph/graph_schema.h"
#include "graph/kuzu_graph_store.h"

#include <spdlog/spdlog.h>

#include <stdexcept>

namespace sage::graph {

GraphStoreManager::GraphStoreManager(std::filesystem::path dataDir)
    : dataDir_(std::move(dataDir)) {
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    if (ec) {
        throw std::runtime_error("GraphStoreManager: cannot create data dir '"
                                 + dataDir_.string() + "': " + ec.message());
    }
}

GraphStoreManager::~GraphStoreManager() = default;

bool GraphStoreManager::isValidSlotId(std::string_view slotId) {
    if (slotId.empty() || slotId.size() > 64) return false;
    for (char c : slotId) {
        const bool ok = (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    // Disallow `.` and `..` to keep the slot directory inside dataDir_.
    if (slotId == "." || slotId == "..") return false;
    return true;
}

std::filesystem::path GraphStoreManager::slotDbPath(std::string_view slotId) const {
    return dataDir_ / std::string{slotId} / "graph.kuzu";
}

GraphStore& GraphStoreManager::acquireSlot(std::string_view slotId) {
    if (!isValidSlotId(slotId)) {
        throw std::invalid_argument("GraphStoreManager: invalid slot id '"
                                    + std::string{slotId} + "'");
    }

    std::lock_guard lk(mu_);
    const std::string key{slotId};
    if (auto it = stores_.find(key); it != stores_.end()) {
        return *it->second;
    }

    auto path = slotDbPath(slotId);
    spdlog::info("GraphStoreManager: opening slot '{}' at {}", key, path.string());

    std::unique_ptr<KuzuGraphStore> store;
    try {
        store = std::make_unique<KuzuGraphStore>(path);
    } catch (const std::exception& ex) {
        spdlog::error("GraphStoreManager: KuzuGraphStore ctor for slot '{}' threw: {}",
                      key, ex.what());
        throw;
    } catch (...) {
        spdlog::error("GraphStoreManager: KuzuGraphStore ctor for slot '{}' threw unknown",
                      key);
        throw;
    }
    spdlog::info("GraphStoreManager: KuzuGraphStore for slot '{}' constructed, beginning migration",
                 key);

    auto migrate = migrateToCurrent(*store);
    if (is_error(migrate)) {
        throw std::runtime_error("GraphStoreManager: schema migration for slot '"
                                 + key + "' failed: " + error_of(migrate).message);
    }
    spdlog::info("GraphStoreManager: slot '{}' migrated {}", key, value_of(migrate).dump());

    auto [it, _] = stores_.emplace(key, std::move(store));
    return *it->second;
}

bool GraphStoreManager::isOpen(std::string_view slotId) const {
    std::lock_guard lk(mu_);
    return stores_.find(std::string{slotId}) != stores_.end();
}

void GraphStoreManager::releaseSlot(std::string_view slotId) {
    std::lock_guard lk(mu_);
    stores_.erase(std::string{slotId});
}

}  // namespace sage::graph
