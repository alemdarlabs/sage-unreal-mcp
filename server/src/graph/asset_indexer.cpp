#include "graph/asset_indexer.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>

namespace sage::graph {

namespace {

// Cypher single-quoted string literal escape for kuzu. kuzu uses C-style
// backslash escapes inside string literals (verified by smoke). We escape
// `\` first, then `'`. Newlines/tabs are passed through — UE asset paths
// don't contain them, but if they ever do, kuzu's lexer accepts raw `\n`
// inside the string body.
std::string escapeCypherStr(std::string_view s) {
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
}

int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Best-effort upsert (kuzu 0.11 lacks generic MERGE). On a fresh slot
// `_IndexState` may have no row — we MATCH first to detect, then INSERT or
// SET as appropriate.
GraphResult upsertIndexState(GraphStore& store, int64_t assetCount, int64_t indexedAtMs) {
    auto exists = store.execute(
        "MATCH (s:_IndexState {id: 1}) RETURN s.id AS id;");
    if (is_error(exists)) return error_of(exists);

    std::ostringstream oss;
    if (value_of(exists)["row_count"].get<int64_t>() == 0) {
        oss << "CREATE (:_IndexState {id: 1, "
            << "last_indexed_at_ms: " << indexedAtMs << ", "
            << "asset_count: "        << assetCount << "});";
    } else {
        oss << "MATCH (s:_IndexState {id: 1}) "
            << "SET s.last_indexed_at_ms = " << indexedAtMs
            << ", s.asset_count = "          << assetCount << ";";
    }
    auto r = store.execute(oss.str());
    if (is_error(r)) return error_of(r);
    return Json(true);
}

}  // namespace

GraphResult ingestAssets(GraphStore& store, const Json& assets) {
    if (!assets.is_array()) {
        return GraphError{"ingestAssets: assets must be a JSON array", 0};
    }

    // Pre-validate every record so we don't half-wipe and fail.
    for (size_t i = 0; i < assets.size(); ++i) {
        const auto& a = assets[i];
        if (!a.is_object()
            || !a.contains("path") || !a["path"].is_string()
            || !a.contains("kind") || !a["kind"].is_string()) {
            return GraphError{"ingestAssets: row " + std::to_string(i)
                              + " missing string 'path' or 'kind'", 0};
        }
    }

    auto wipe = store.execute("MATCH (a:Asset) DETACH DELETE a;");
    if (is_error(wipe)) return error_of(wipe);

    constexpr size_t kBatchSize = 200;
    for (size_t i = 0; i < assets.size(); i += kBatchSize) {
        std::ostringstream q;
        q << "CREATE ";
        const size_t end = std::min(i + kBatchSize, assets.size());
        for (size_t j = i; j < end; ++j) {
            if (j > i) q << ", ";
            q << "(:Asset {path: " << escapeCypherStr(assets[j]["path"].get<std::string>())
              << ", kind: "         << escapeCypherStr(assets[j]["kind"].get<std::string>())
              << "})";
        }
        q << ";";
        auto r = store.execute(q.str());
        if (is_error(r)) {
            spdlog::error("ingestAssets: batch [{}, {}) failed: {}",
                          i, end, error_of(r).message);
            return error_of(r);
        }
    }

    const int64_t at = nowEpochMs();
    auto upsert = upsertIndexState(
        store, static_cast<int64_t>(assets.size()), at);
    if (is_error(upsert)) return error_of(upsert);

    Json out = Json::object();
    out["asset_count"]        = static_cast<int64_t>(assets.size());
    out["last_indexed_at_ms"] = at;
    return out;
}

GraphResult getIndexStatus(GraphStore& store) {
    auto r = store.execute(
        "MATCH (s:_IndexState {id: 1}) "
        "RETURN s.last_indexed_at_ms AS last_indexed_at_ms, "
        "       s.asset_count        AS asset_count;");
    if (is_error(r)) return error_of(r);

    const auto& env = value_of(r);
    Json out = Json::object();
    if (env["row_count"].get<int64_t>() == 0) {
        out["asset_count"]        = 0;
        out["last_indexed_at_ms"] = nullptr;
    } else {
        const auto& row = env["rows"][0];
        out["asset_count"]        = row["asset_count"];
        out["last_indexed_at_ms"] = row["last_indexed_at_ms"];
    }
    return out;
}

}  // namespace sage::graph
