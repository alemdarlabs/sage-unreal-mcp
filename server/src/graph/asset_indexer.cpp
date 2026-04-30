#include "graph/asset_indexer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
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

[[maybe_unused]] GraphResult insertClassesBatched(GraphStore& store, const Json& classes) {
    constexpr size_t kBatchSize = 2000;
    for (size_t i = 0; i < classes.size(); i += kBatchSize) {
        std::ostringstream q;
        q << "CREATE ";
        const size_t end = std::min(i + kBatchSize, classes.size());
        for (size_t j = i; j < end; ++j) {
            const auto& c = classes[j];
            if (j > i) q << ", ";
            q << "(:Class {name: " << escapeCypherStr(c["name"].get<std::string>())
              << ", parent: "       << escapeCypherStr(c.value("parent", std::string{}))
              << ", module: "       << escapeCypherStr(c.value("module", std::string{}))
              << ", is_native: "    << (c.value("is_native", false) ? "true" : "false")
              << "})";
        }
        q << ";";
        auto r = store.execute(q.str());
        if (is_error(r)) {
            spdlog::error("ingestSnapshot: class batch [{}, {}) failed: {}",
                          i, end, error_of(r).message);
            return error_of(r);
        }
    }
    return Json(true);
}

// INHERITS_FROM edges from Class.parent. Returns the count actually
// inserted (parents missing from the Class table are silently skipped —
// e.g. the topmost UObject often has no recorded parent).
[[maybe_unused]] GraphResult insertInheritsFromEdges(GraphStore& store,
                                     const Json& classes,
                                     const std::unordered_set<std::string>& known) {
    std::vector<std::pair<std::string, std::string>> edges;
    edges.reserve(classes.size());
    for (const auto& c : classes) {
        const auto name   = c["name"].get<std::string>();
        const auto parent = c.value("parent", std::string{});
        if (parent.empty() || parent == name) continue;
        if (!known.contains(parent)) continue;
        edges.emplace_back(name, parent);
    }

    constexpr size_t kBatchSize = 2000;
    int64_t inserted = 0;
    for (size_t i = 0; i < edges.size(); i += kBatchSize) {
        std::ostringstream q;
        q << "UNWIND [";
        const size_t end = std::min(i + kBatchSize, edges.size());
        for (size_t j = i; j < end; ++j) {
            if (j > i) q << ", ";
            q << "{c: " << escapeCypherStr(edges[j].first)
              << ", p: " << escapeCypherStr(edges[j].second) << "}";
        }
        q << "] AS row "
          << "MATCH (child:Class {name: row.c}), (parent:Class {name: row.p}) "
          << "CREATE (child)-[:INHERITS_FROM]->(parent);";
        auto r = store.execute(q.str());
        if (is_error(r)) return error_of(r);
        inserted += static_cast<int64_t>(end - i);
    }
    return Json(inserted);
}

[[maybe_unused]] GraphResult insertAssetsBatched(GraphStore& store, const Json& assets) {
    constexpr size_t kBatchSize = 2000;
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

// CSV escape: double-quote fields, escape internal quotes by doubling.
// UE asset paths don't normally contain quote / comma / newline, but be
// defensive — a single bad row would otherwise corrupt the COPY.
inline void writeCsvField(std::ofstream& out, std::string_view s) {
    out.put('"');
    for (char c : s) {
        if (c == '"') { out.put('"'); out.put('"'); }
        else          { out.put(c); }
    }
    out.put('"');
}

// Picked once per server lifetime so concurrent ingests in tests don't
// stomp each other (slot-scoped mutex elsewhere makes this belt-and-braces).
std::filesystem::path makeTempCsvPath(std::string_view tag) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    return std::filesystem::temp_directory_path() /
        ("sage_" + std::string{tag} + "_" + std::to_string(gen()) + ".csv");
}

// Cypher single-quoted string literals interpret `\` as an escape introducer
// (\n, \t, \uXXXX, …). On Windows std::filesystem::path::string() returns
// `C:\Users\...` which the kuzu parser then chokes on with "Invalid input"
// at the first `\U`/`\A`/`\T`. Forward slashes are accepted everywhere by
// kuzu's COPY FROM, so emit the path with generic_string() (POSIX-style)
// regardless of host. No-op on macOS/Linux.
std::string cypherPathLiteral(const std::filesystem::path& p) {
    return p.generic_string();
}

// Bulk-insert Asset nodes via COPY FROM CSV. ~4x faster than CREATE
// batch (1.9s → ~0.4s on 8K assets).
GraphResult insertAssetsViaCopy(GraphStore& store, const Json& assets) {
    namespace fs = std::filesystem;
    const fs::path csv = makeTempCsvPath("assets");
    {
        std::ofstream out(csv, std::ios::binary);
        if (!out) return GraphError{"insertAssetsViaCopy: cannot open " + csv.string(), 0};
        out << "path,kind\n";
        for (const auto& a : assets) {
            writeCsvField(out, a["path"].get_ref<const std::string&>());
            out.put(',');
            writeCsvField(out, a["kind"].get_ref<const std::string&>());
            out.put('\n');
        }
    }
    const std::string q = "COPY Asset FROM '" + cypherPathLiteral(csv) + "' (HEADER=true);";
    auto r = store.execute(q);
    std::error_code ec;
    fs::remove(csv, ec);
    if (is_error(r)) return error_of(r);
    return Json(true);
}

// Bulk-insert Class nodes via COPY. Schema is (name PRIMARY KEY, parent,
// module, is_native).
GraphResult insertClassesViaCopy(GraphStore& store, const Json& classes) {
    namespace fs = std::filesystem;
    const fs::path csv = makeTempCsvPath("classes");
    {
        std::ofstream out(csv, std::ios::binary);
        if (!out) return GraphError{"insertClassesViaCopy: cannot open " + csv.string(), 0};
        out << "name,parent,module,is_native\n";
        for (const auto& c : classes) {
            writeCsvField(out, c["name"].get_ref<const std::string&>());
            out.put(',');
            writeCsvField(out, c.value("parent", std::string{}));
            out.put(',');
            writeCsvField(out, c.value("module", std::string{}));
            out.put(',');
            out << (c.value("is_native", false) ? "true" : "false");
            out.put('\n');
        }
    }
    const std::string q = "COPY Class FROM '" + cypherPathLiteral(csv) + "' (HEADER=true);";
    auto r = store.execute(q);
    std::error_code ec;
    fs::remove(csv, ec);
    if (is_error(r)) return error_of(r);
    return Json(true);
}

// Bulk-insert INHERITS_FROM edges via COPY.
GraphResult insertInheritsFromViaCopy(GraphStore& store, const Json& classes,
                                       const std::unordered_set<std::string>& known) {
    namespace fs = std::filesystem;
    int64_t written = 0;
    const fs::path csv = makeTempCsvPath("inherits");
    {
        std::ofstream out(csv, std::ios::binary);
        if (!out) return GraphError{"insertInheritsFromViaCopy: cannot open " + csv.string(), 0};
        out << "from,to\n";
        for (const auto& c : classes) {
            const auto& name   = c["name"].get_ref<const std::string&>();
            const auto  parent = c.value("parent", std::string{});
            if (parent.empty() || parent == name || !known.contains(parent)) continue;
            writeCsvField(out, name);
            out.put(',');
            writeCsvField(out, parent);
            out.put('\n');
            ++written;
        }
    }
    if (written == 0) {
        std::error_code ec;
        fs::remove(csv, ec);
        return Json(int64_t{0});
    }
    const std::string q = "COPY INHERITS_FROM FROM '" + cypherPathLiteral(csv) + "' (HEADER=true);";
    auto r = store.execute(q);
    std::error_code ec;
    fs::remove(csv, ec);
    if (is_error(r)) return error_of(r);
    return Json(written);
}

// Bulk-insert DEPENDS_ON via Kuzu's COPY FROM CSV path. ~30x faster than
// UNWIND+MATCH×2+CREATE on 16K-edge SageTest (12.5s → ~0.4s).
GraphResult insertDepsViaCopy(GraphStore& store, const Json& deps,
                               const std::unordered_set<std::string>& known) {
    namespace fs = std::filesystem;
    int64_t skipped = 0;
    int64_t written = 0;

    const fs::path csv = makeTempCsvPath("deps");
    {
        std::ofstream out(csv, std::ios::binary);
        if (!out) return GraphError{"insertDepsViaCopy: cannot open " + csv.string(), 0};
        out << "from,to\n";
        for (const auto& d : deps) {
            const auto& from = d["from"].get_ref<const std::string&>();
            const auto& to   = d["to"].get_ref<const std::string&>();
            if (from == to || !known.contains(from) || !known.contains(to)) {
                ++skipped;
                continue;
            }
            writeCsvField(out, from);
            out.put(',');
            writeCsvField(out, to);
            out.put('\n');
            ++written;
        }
    }
    if (written == 0) {
        fs::remove(csv);
        return Json(int64_t{0});
    }

    const std::string q = "COPY DEPENDS_ON FROM '" + cypherPathLiteral(csv) + "' (HEADER=true);";
    auto r = store.execute(q);
    std::error_code ec;
    fs::remove(csv, ec);  // best-effort
    if (is_error(r)) {
        spdlog::error("insertDepsViaCopy: COPY failed: {}", error_of(r).message);
        return error_of(r);
    }
    if (skipped > 0) {
        spdlog::debug("insertDepsViaCopy: {} edges skipped (endpoint outside Asset table)",
                      skipped);
    }
    return Json(written);
}

// Legacy UNWIND+MATCH path (kept as a reference / fallback if Kuzu COPY
// ever rejects the CSV). Phase 4.4 swapped ingestSnapshot to
// insertDepsViaCopy for the 10x speedup.
[[maybe_unused]] GraphResult insertDepsBatched(GraphStore& store, const Json& deps,
                               const std::unordered_set<std::string>& known) {
    constexpr size_t kBatchSize = 2000;
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

    const bool hasClasses = snapshot.contains("classes")
                         && snapshot["classes"].is_array();
    const auto& classes = hasClasses ? snapshot["classes"] : Json::array();
    for (size_t i = 0; i < classes.size(); ++i) {
        const auto& c = classes[i];
        if (!c.is_object()
            || !c.contains("name") || !c["name"].is_string()) {
            return GraphError{"ingestSnapshot: class row " + std::to_string(i)
                              + " missing string 'name'", 0};
        }
    }

    // Per-stage timing so we can profile bottlenecks (Phase 4.4 work).
    using clk = std::chrono::steady_clock;
    auto stamp = [](auto t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            clk::now() - t0).count();
    };

    auto t = clk::now();
    auto wipeDeps = store.execute("MATCH ()-[r:DEPENDS_ON]->() DELETE r;");
    if (is_error(wipeDeps)) return error_of(wipeDeps);
    spdlog::info("ingestSnapshot stage: wipe DEPENDS_ON {}ms", stamp(t)); t = clk::now();

    auto wipeInherits = store.execute("MATCH ()-[r:INHERITS_FROM]->() DELETE r;");
    if (is_error(wipeInherits)) return error_of(wipeInherits);
    spdlog::info("ingestSnapshot stage: wipe INHERITS_FROM {}ms", stamp(t)); t = clk::now();

    auto wipeClasses = store.execute("MATCH (c:Class) DETACH DELETE c;");
    if (is_error(wipeClasses)) return error_of(wipeClasses);
    spdlog::info("ingestSnapshot stage: wipe Class {}ms", stamp(t)); t = clk::now();

    auto wipeAssets = store.execute("MATCH (a:Asset) DETACH DELETE a;");
    if (is_error(wipeAssets)) return error_of(wipeAssets);
    spdlog::info("ingestSnapshot stage: wipe Asset {}ms", stamp(t)); t = clk::now();

    auto insAssets = insertAssetsViaCopy(store, assets);
    if (is_error(insAssets)) return error_of(insAssets);
    spdlog::info("ingestSnapshot stage: insert {} Assets via COPY {}ms",
                  assets.size(), stamp(t)); t = clk::now();

    int64_t depCount = 0;
    if (!deps.empty()) {
        std::unordered_set<std::string> known;
        known.reserve(assets.size());
        for (const auto& a : assets) known.insert(a["path"].get<std::string>());

        auto insDeps = insertDepsViaCopy(store, deps, known);
        if (is_error(insDeps)) return error_of(insDeps);
        depCount = value_of(insDeps).get<int64_t>();
        spdlog::info("ingestSnapshot stage: insert {} DEPENDS_ON via COPY {}ms",
                      depCount, stamp(t)); t = clk::now();
    }

    int64_t classCount    = 0;
    int64_t classEdgeCount = 0;
    if (!classes.empty()) {
        auto insClasses = insertClassesViaCopy(store, classes);
        if (is_error(insClasses)) return error_of(insClasses);
        classCount = static_cast<int64_t>(classes.size());
        spdlog::info("ingestSnapshot stage: insert {} Classes via COPY {}ms",
                      classCount, stamp(t)); t = clk::now();

        std::unordered_set<std::string> knownClasses;
        knownClasses.reserve(classes.size());
        for (const auto& c : classes) knownClasses.insert(c["name"].get<std::string>());

        auto insInherits = insertInheritsFromViaCopy(store, classes, knownClasses);
        if (is_error(insInherits)) return error_of(insInherits);
        classEdgeCount = value_of(insInherits).get<int64_t>();
        spdlog::info("ingestSnapshot stage: insert {} INHERITS_FROM via COPY {}ms",
                      classEdgeCount, stamp(t)); t = clk::now();
    }

    const int64_t at = nowEpochMs();
    auto upsert = upsertIndexState(
        store, static_cast<int64_t>(assets.size()), depCount, at);
    if (is_error(upsert)) return error_of(upsert);

    Json out = Json::object();
    out["asset_count"]        = static_cast<int64_t>(assets.size());
    out["dep_count"]          = depCount;
    out["class_count"]        = classCount;
    out["class_edge_count"]   = classEdgeCount;
    out["last_indexed_at_ms"] = at;
    return out;
}

GraphResult getIndexStatus(GraphStore& store) {
    // last_indexed_at_ms tracks the last *full* ingest pass; deltas update
    // the graph but not this timestamp (so the agent can tell "since this
    // moment, the snapshot has been patched live"). The counts however
    // must reflect the current graph — `_IndexState.asset_count` would go
    // stale every delta otherwise. So we live-count.
    auto stateR = store.execute(
        "MATCH (s:_IndexState {id: 1}) "
        "RETURN s.last_indexed_at_ms AS last_indexed_at_ms;");
    if (is_error(stateR)) return error_of(stateR);

    Json out = Json::object();
    if (value_of(stateR)["row_count"].get<int64_t>() == 0) {
        out["last_indexed_at_ms"] = nullptr;
    } else {
        out["last_indexed_at_ms"] = value_of(stateR)["rows"][0]["last_indexed_at_ms"];
    }

    auto assetR = store.execute("MATCH (a:Asset) RETURN count(a) AS c;");
    if (is_error(assetR)) return error_of(assetR);
    out["asset_count"] = value_of(assetR)["rows"][0]["c"];

    auto depR = store.execute("MATCH ()-[r:DEPENDS_ON]->() RETURN count(r) AS c;");
    if (is_error(depR)) return error_of(depR);
    out["dep_count"] = value_of(depR)["rows"][0]["c"];

    return out;
}

}  // namespace sage::graph
