#include "graph/asset_indexer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_set>

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

GraphResult upsertIndexState(GraphStore& store,
                             int64_t assetCount,
                             int64_t depCount,
                             int64_t indexedAtMs) {
    auto exists = store.execute(
        "MATCH (s:_IndexState {id: 1}) RETURN s.id AS id;");
    if (is_error(exists)) return error_of(exists);

    std::ostringstream oss;
    if (value_of(exists)["row_count"].get<int64_t>() == 0) {
        oss << "CREATE (:_IndexState {id: 1, "
            << "last_indexed_at_ms: " << indexedAtMs << ", "
            << "asset_count: "        << assetCount << ", "
            << "dep_count: "          << depCount   << "});";
    } else {
        oss << "MATCH (s:_IndexState {id: 1}) "
            << "SET s.last_indexed_at_ms = " << indexedAtMs
            << ", s.asset_count = "          << assetCount
            << ", s.dep_count = "            << depCount   << ";";
    }
    auto r = store.execute(oss.str());
    if (is_error(r)) return error_of(r);
    return Json(true);
}

GraphResult insertAssetsBatched(GraphStore& store, const Json& assets) {
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
            spdlog::error("ingestSnapshot: asset batch [{}, {}) failed: {}",
                          i, end, error_of(r).message);
            return error_of(r);
        }
    }
    return Json(true);
}

// Insert DEPENDS_ON edges. kuzu (0.11) does not allow multi-pattern edge
// creation in a single statement when the MATCH is per-row, so we batch
// via UNWIND on a literal list. This brings 8K edges from "thousands of
// queries" down to ~40 (batch=200).
GraphResult insertDepsBatched(GraphStore& store, const Json& deps,
                               const std::unordered_set<std::string>& known) {
    constexpr size_t kBatchSize = 200;
    int64_t inserted = 0;
    int64_t skipped  = 0;

    // Pre-filter to known assets so MATCH never misses (a missing endpoint
    // would silently no-op the CREATE in kuzu, which is fine but we want
    // an accurate count).
    std::vector<std::pair<std::string, std::string>> filtered;
    filtered.reserve(deps.size());
    for (const auto& d : deps) {
        const auto from = d["from"].get<std::string>();
        const auto to   = d["to"].get<std::string>();
        if (known.contains(from) && known.contains(to) && from != to) {
            filtered.emplace_back(std::move(from), std::move(to));
        } else {
            ++skipped;
        }
    }

    for (size_t i = 0; i < filtered.size(); i += kBatchSize) {
        std::ostringstream q;
        q << "UNWIND [";
        const size_t end = std::min(i + kBatchSize, filtered.size());
        for (size_t j = i; j < end; ++j) {
            if (j > i) q << ", ";
            q << "{f: " << escapeCypherStr(filtered[j].first)
              << ", t: " << escapeCypherStr(filtered[j].second) << "}";
        }
        q << "] AS row "
          << "MATCH (a:Asset {path: row.f}), (b:Asset {path: row.t}) "
          << "CREATE (a)-[:DEPENDS_ON]->(b);";

        auto r = store.execute(q.str());
        if (is_error(r)) {
            spdlog::error("ingestSnapshot: dep batch [{}, {}) failed: {}",
                          i, end, error_of(r).message);
            return error_of(r);
        }
        inserted += static_cast<int64_t>(end - i);
    }
    if (skipped > 0) {
        spdlog::debug("ingestSnapshot: {} edges skipped (endpoint outside Asset table)",
                      skipped);
    }
    return Json(inserted);
}

}  // namespace

GraphResult ingestSnapshot(GraphStore& store, const Json& snapshot) {
    if (!snapshot.is_object() || !snapshot.contains("assets")
        || !snapshot["assets"].is_array()) {
        return GraphError{"ingestSnapshot: snapshot must contain 'assets' array", 0};
    }
    const auto& assets = snapshot["assets"];
    for (size_t i = 0; i < assets.size(); ++i) {
        const auto& a = assets[i];
        if (!a.is_object()
            || !a.contains("path") || !a["path"].is_string()
            || !a.contains("kind") || !a["kind"].is_string()) {
            return GraphError{"ingestSnapshot: asset row " + std::to_string(i)
                              + " missing string 'path' or 'kind'", 0};
        }
    }

    const bool hasDeps = snapshot.contains("dependencies")
                      && snapshot["dependencies"].is_array();
    const auto& deps = hasDeps ? snapshot["dependencies"] : Json::array();
    for (size_t i = 0; i < deps.size(); ++i) {
        const auto& d = deps[i];
        if (!d.is_object()
            || !d.contains("from") || !d["from"].is_string()
            || !d.contains("to")   || !d["to"].is_string()) {
            return GraphError{"ingestSnapshot: dep row " + std::to_string(i)
                              + " missing string 'from' or 'to'", 0};
        }
    }

    // Wipe edges first (explicit), then nodes — DETACH on the node would
    // also drop edges but the explicit pass is cheaper to debug.
    auto wipeEdges = store.execute("MATCH ()-[r:DEPENDS_ON]->() DELETE r;");
    if (is_error(wipeEdges)) return error_of(wipeEdges);

    auto wipeNodes = store.execute("MATCH (a:Asset) DETACH DELETE a;");
    if (is_error(wipeNodes)) return error_of(wipeNodes);

    auto insAssets = insertAssetsBatched(store, assets);
    if (is_error(insAssets)) return error_of(insAssets);

    int64_t depCount = 0;
    if (!deps.empty()) {
        // Build the set of known asset paths once; deps reference these.
        std::unordered_set<std::string> known;
        known.reserve(assets.size());
        for (const auto& a : assets) known.insert(a["path"].get<std::string>());

        auto insDeps = insertDepsBatched(store, deps, known);
        if (is_error(insDeps)) return error_of(insDeps);
        depCount = value_of(insDeps).get<int64_t>();
    }

    const int64_t at = nowEpochMs();
    auto upsert = upsertIndexState(
        store, static_cast<int64_t>(assets.size()), depCount, at);
    if (is_error(upsert)) return error_of(upsert);

    Json out = Json::object();
    out["asset_count"]        = static_cast<int64_t>(assets.size());
    out["dep_count"]          = depCount;
    out["last_indexed_at_ms"] = at;
    return out;
}

GraphResult getIndexStatus(GraphStore& store) {
    auto r = store.execute(
        "MATCH (s:_IndexState {id: 1}) "
        "RETURN s.last_indexed_at_ms AS last_indexed_at_ms, "
        "       s.asset_count        AS asset_count, "
        "       s.dep_count          AS dep_count;");
    if (is_error(r)) return error_of(r);

    const auto& env = value_of(r);
    Json out = Json::object();
    if (env["row_count"].get<int64_t>() == 0) {
        out["asset_count"]        = 0;
        out["dep_count"]          = 0;
        out["last_indexed_at_ms"] = nullptr;
    } else {
        const auto& row = env["rows"][0];
        out["asset_count"]        = row["asset_count"];
        out["dep_count"]          = row["dep_count"];
        out["last_indexed_at_ms"] = row["last_indexed_at_ms"];
    }
    return out;
}

}  // namespace sage::graph
