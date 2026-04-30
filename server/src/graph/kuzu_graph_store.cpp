#include "graph/kuzu_graph_store.h"

#include "kuzu.hpp"

#include <spdlog/spdlog.h>

#include <stdexcept>

namespace sage::graph {

namespace {

// Convert a kuzu::common::Value to JSON. Phase 2 baseline handles primitive
// scalars; STRUCT / LIST / NODE / REL stringified via toString() for now —
// 2.1c will replace with structured conversion as the query layer needs it.
Json valueToJson(const kuzu::common::Value& v) {
    if (v.isNull()) return Json(nullptr);

    using LID = kuzu::common::LogicalTypeID;
    switch (v.getDataType().getLogicalTypeID()) {
    case LID::BOOL:    return Json(v.getValue<bool>());
    case LID::INT8:    return Json(static_cast<int64_t>(v.getValue<int8_t>()));
    case LID::INT16:   return Json(static_cast<int64_t>(v.getValue<int16_t>()));
    case LID::INT32:   return Json(static_cast<int64_t>(v.getValue<int32_t>()));
    case LID::INT64:   return Json(v.getValue<int64_t>());
    case LID::UINT8:   return Json(static_cast<int64_t>(v.getValue<uint8_t>()));
    case LID::UINT16:  return Json(static_cast<int64_t>(v.getValue<uint16_t>()));
    case LID::UINT32:  return Json(static_cast<int64_t>(v.getValue<uint32_t>()));
    case LID::UINT64:  return Json(static_cast<uint64_t>(v.getValue<uint64_t>()));
    case LID::FLOAT:   return Json(static_cast<double>(v.getValue<float>()));
    case LID::DOUBLE:  return Json(v.getValue<double>());
    case LID::STRING:  return Json(v.getValue<std::string>());
    default:           return Json(v.toString());
    }
}

GraphResult buildResult(kuzu::main::QueryResult& result) {
    if (!result.isSuccess()) {
        return GraphError{result.getErrorMessage(), 0};
    }

    const auto names = result.getColumnNames();
    const auto types = result.getColumnDataTypes();
    const auto numColumns = names.size();

    Json schema = Json::array();
    for (size_t i = 0; i < numColumns; ++i) {
        schema.push_back(Json{
            {"name", names[i]},
            {"type", types[i].toString()},
        });
    }

    Json rows = Json::array();
    while (result.hasNext()) {
        auto tuple = result.getNext();
        Json row = Json::object();
        for (uint32_t i = 0; i < static_cast<uint32_t>(numColumns); ++i) {
            row[names[i]] = valueToJson(*tuple->getValue(i));
        }
        rows.push_back(std::move(row));
    }

    Json envelope = Json::object();
    envelope["rows"]      = std::move(rows);
    envelope["schema"]    = std::move(schema);
    envelope["row_count"] = envelope["rows"].size();
    return envelope;
}

}  // namespace

KuzuGraphStore::KuzuGraphStore(const std::filesystem::path& dbPath)
    : dbPath_(dbPath) {
    std::error_code ec;
    if (dbPath_.has_parent_path()) {
        std::filesystem::create_directories(dbPath_.parent_path(), ec);
    }
    // Stage logs are deliberately verbose. Without them we cannot tell from
    // a post-crash log file whether kuzu's Database ctor (B-tree init / WAL
    // replay / system catalog load) or its Connection ctor was the one
    // that aborted. Both are native dll calls that can raise SEH.
    spdlog::info("KuzuGraphStore: creating Database at {}", dbPath_.string());
    try {
        db_ = std::make_unique<kuzu::main::Database>(dbPath_.string());
    } catch (const std::exception& ex) {
        spdlog::error("KuzuGraphStore: Database ctor threw: {}", ex.what());
        throw;
    } catch (...) {
        spdlog::error("KuzuGraphStore: Database ctor threw unknown exception");
        throw;
    }
    spdlog::info("KuzuGraphStore: Database created");

    try {
        conn_ = std::make_unique<kuzu::main::Connection>(db_.get());
    } catch (const std::exception& ex) {
        spdlog::error("KuzuGraphStore: Connection ctor threw: {}", ex.what());
        throw;
    } catch (...) {
        spdlog::error("KuzuGraphStore: Connection ctor threw unknown exception");
        throw;
    }
    spdlog::info("KuzuGraphStore: Connection created");
    spdlog::info("KuzuGraphStore: opened {}", dbPath_.string());
}

KuzuGraphStore::~KuzuGraphStore() {
    spdlog::info("KuzuGraphStore: closing {}", dbPath_.string());
}

bool KuzuGraphStore::isOpen() const {
    std::lock_guard lk(mu_);
    return db_ != nullptr && conn_ != nullptr;
}

GraphResult KuzuGraphStore::execute(std::string_view cypher) {
    std::lock_guard lk(mu_);
    if (conn_ == nullptr) {
        return GraphError{"store not open", 0};
    }
    // Pre-call log is the breadcrumb that survives a hard crash inside
    // Connection::query (e.g. kuzu binder/parser AV). Truncate to keep
    // the log readable for COPY FROM CSV statements with long paths.
    spdlog::info("KuzuGraphStore::execute: {}",
                 cypher.size() > 200 ? std::string{cypher.substr(0, 200)} + "..." : std::string{cypher});
    // Kuzu's Connection::query is documented to return a valid QueryResult
    // even on failure (caller checks isSuccess()), but defensively guard:
    // (1) nullptr return → would segfault on `*result` and the prior crash
    //     pattern (no error log, abrupt WebSocket 1006 close) matches an
    //     unguarded null deref. (2) Native exception (std::bad_alloc,
    //     kuzu-internal throw) → caller chain has no catch and the process
    //     would std::terminate without flushing logs. Both paths must
    //     surface as a recoverable GraphError.
    try {
        auto result = conn_->query(std::string{cypher});
        if (!result) {
            spdlog::error("KuzuGraphStore::execute: kuzu query returned nullptr; cypher='{}'",
                          cypher);
            return GraphError{"kuzu query returned nullptr", 0};
        }
        return buildResult(*result);
    } catch (const std::exception& ex) {
        spdlog::error("KuzuGraphStore::execute: native exception '{}'; cypher='{}'",
                      ex.what(), cypher);
        return GraphError{std::string{"kuzu query threw: "} + ex.what(), 0};
    } catch (...) {
        spdlog::error("KuzuGraphStore::execute: unknown native exception; cypher='{}'",
                      cypher);
        return GraphError{"kuzu query threw unknown exception", 0};
    }
}

GraphResult KuzuGraphStore::execute(std::string_view cypher, const Json& /*params*/) {
    // Phase 2.1c will introduce kuzu::main::Connection::prepare + execute
    // with parameter binding. For now: forward to the unparameterized path.
    return execute(cypher);
}

}  // namespace sage::graph
