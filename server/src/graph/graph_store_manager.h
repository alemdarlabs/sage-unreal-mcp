#pragma once

#include "graph/graph_store.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sage::graph {

// Per-slot KuzuDB store. One process serves N editor slots concurrently;
// each slot gets its own embedded DB on disk under {dataDir}/{slot}/graph.kuzu
// so neighbouring slots can never see each other's nodes (ADR-014 isolation).
//
// Stores opened here have the current schema applied (migrateToCurrent on
// first acquire). Returned references are stable for the manager's lifetime.
//
// Slot IDs must match: ^[A-Za-z0-9._-]{1,64}$ — anything else is rejected
// because the slot string is used as a directory name.
class GraphStoreManager {
public:
    explicit GraphStoreManager(std::filesystem::path dataDir);
    ~GraphStoreManager();

    GraphStoreManager(const GraphStoreManager&)            = delete;
    GraphStoreManager& operator=(const GraphStoreManager&) = delete;
    GraphStoreManager(GraphStoreManager&&)                 = delete;
    GraphStoreManager& operator=(GraphStoreManager&&)      = delete;

    // Opens (lazily) and migrates the slot's store. Throws std::invalid_argument
    // for malformed slot IDs and std::runtime_error if Kuzu open or schema
    // migration fails.
    [[nodiscard]] GraphStore& acquireSlot(std::string_view slotId);

    // Whether the slot is currently held in memory (does not touch disk).
    [[nodiscard]] bool isOpen(std::string_view slotId) const;

    [[nodiscard]] const std::filesystem::path& dataDir() const noexcept { return dataDir_; }

    // Lifecycle helper — drops the store from memory (closes the Kuzu
    // connection). Caller must ensure no dangling references. Used by tests
    // and the slot prune flow (1.5b/2.x).
    void releaseSlot(std::string_view slotId);

    [[nodiscard]] static bool isValidSlotId(std::string_view slotId);

private:
    [[nodiscard]] std::filesystem::path slotDbPath(std::string_view slotId) const;

    std::filesystem::path                                          dataDir_;
    std::unordered_map<std::string, std::unique_ptr<GraphStore>>   stores_;
    mutable std::mutex                                             mu_;
};

}  // namespace sage::graph
