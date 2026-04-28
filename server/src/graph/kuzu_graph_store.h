#pragma once

#include "graph/graph_store.h"

#include <filesystem>
#include <memory>
#include <mutex>

// Forward declarations to keep kuzu.hpp out of the public header.
// Including kuzu.hpp pollutes consumers with a 9 kLOC bundle and (under
// libc++ + C++23) a constexpr-destructor static_assert; keeping it
// translation-unit-local lets the rest of Sage stay on C++23.
namespace kuzu::main {
class Database;
class Connection;
}  // namespace kuzu::main

namespace sage::graph {

class KuzuGraphStore : public GraphStore {
public:
    // Opens (or creates) a KuzuDB at the given file path. Creates parent
    // directory if missing. Throws on open failure (constructor cannot
    // return std::expected; callers may wrap in try/catch for graceful
    // bootstrap).
    explicit KuzuGraphStore(const std::filesystem::path& dbPath);
    ~KuzuGraphStore() override;

    [[nodiscard]] GraphResult execute(std::string_view cypher) override;
    [[nodiscard]] GraphResult execute(std::string_view cypher, const Json& params) override;
    [[nodiscard]] bool        isOpen() const override;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dbPath_; }

private:
    std::filesystem::path                   dbPath_;
    std::unique_ptr<kuzu::main::Database>   db_;
    std::unique_ptr<kuzu::main::Connection> conn_;
    mutable std::mutex                      mu_;  // kuzu Connection is not thread-safe; serialize
};

}  // namespace sage::graph
