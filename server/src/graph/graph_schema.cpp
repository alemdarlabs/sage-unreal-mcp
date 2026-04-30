#include "graph/graph_schema.h"

#include <spdlog/spdlog.h>

#include <sstream>

namespace sage::graph {

namespace {

// v1 baseline: knowledge layer skeleton.
// - _SchemaVersion: single-row registry tracking applied version.
// - Asset/Class/Module/Plugin: T1 entity tables (populated by 2.2 indexer).
//
// Underscore prefix on _SchemaVersion is intentional — it segregates the
// meta table from user-facing entity tables in MATCH (n) listings.
//
// IF NOT EXISTS makes each statement idempotent so a crash mid-migration
// replays cleanly.
constexpr const char* kV1Cypher = R"(
CREATE NODE TABLE IF NOT EXISTS _SchemaVersion(id INT64 PRIMARY KEY, value INT64);
CREATE NODE TABLE IF NOT EXISTS Asset(path STRING PRIMARY KEY, kind STRING);
CREATE NODE TABLE IF NOT EXISTS Class(name STRING PRIMARY KEY, module STRING);
CREATE NODE TABLE IF NOT EXISTS Module(name STRING PRIMARY KEY, plugin STRING);
CREATE NODE TABLE IF NOT EXISTS Plugin(name STRING PRIMARY KEY);
)";

// v2: indexer state table. last_indexed_at_ms is a Unix-epoch ms timestamp
// captured server-side at the end of an ingest pass. asset_count is the
// number of Asset rows that were just written (cheaper than COUNT(*) on
// every status query). Single-row table, id always = 1.
constexpr const char* kV2Cypher = R"(
CREATE NODE TABLE IF NOT EXISTS _IndexState(id INT64 PRIMARY KEY, last_indexed_at_ms INT64, asset_count INT64);
)";

// v3: T2 topology. DEPENDS_ON edge between Assets backs `impact_of` /
// `references_to` (Phase 2.4). MANY_MANY because one asset can pull in
// many dependencies, and one shared asset can be referenced from many.
// Phase 2.3 adds dep_count to _IndexState so status queries don't need
// a graph round-trip.
constexpr const char* kV3Cypher = R"(
CREATE REL TABLE IF NOT EXISTS DEPENDS_ON(FROM Asset TO Asset, MANY_MANY);
ALTER TABLE _IndexState ADD dep_count INT64 DEFAULT 0;
)";

// v4: class hierarchy. UClass parent chain backs `class_hierarchy` (Phase
// 3). Class.parent is denormalised so a 1-hop ancestor lookup doesn't
// require the relationship traversal. INHERITS_FROM is the canonical
// edge for *N..M depth queries. is_native discriminates engine/C++
// classes from Blueprint-generated ones.
constexpr const char* kV4Cypher = R"(
ALTER TABLE Class ADD parent STRING DEFAULT '';
ALTER TABLE Class ADD is_native BOOLEAN DEFAULT FALSE;
CREATE REL TABLE IF NOT EXISTS INHERITS_FROM(FROM Class TO Class);
)";

// Split a multi-statement Cypher blob on `;` boundaries, ignoring blanks
// and Cypher line comments. Kuzu's Connection::query takes a single stmt.
std::vector<std::string> splitStatements(std::string_view blob) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(blob.size());

    auto isBlank = [](const std::string& s) {
        for (char c : s) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
        }
        return true;
    };

    for (size_t i = 0; i < blob.size(); ++i) {
        const char c = blob[i];
        // strip `// ...` line comments
        if (c == '/' && i + 1 < blob.size() && blob[i + 1] == '/') {
            while (i < blob.size() && blob[i] != '\n') ++i;
            continue;
        }
        if (c == ';') {
            if (!isBlank(cur)) out.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!isBlank(cur)) out.push_back(std::move(cur));
    return out;
}

GraphResult readCurrentVersion(GraphStore& store) {
    // The original probe used `CALL show_tables() RETURN name;` to detect
    // pre-v1 stores. On Windows kuzu 0.11.x that built-in segfaults inside
    // the dll on a freshly-opened database (verified 2026-05-01: server
    // log shows the cypher reaching execute() then a silent SEH abort with
    // no `applying migration v1` follow-up). The crash happens before any
    // C++ exception is raised, so try/catch can't trap it.
    //
    // The fix sidesteps show_tables entirely: create the _SchemaVersion
    // table with CREATE IF NOT EXISTS (idempotent — v1 migration repeats
    // the same statement, no-op'ing on already-present tables), then read
    // the row. A pre-v1 store now produces "table just created, no rows"
    // → version 0, identical to the old probe semantics. Same shape, no
    // dependency on the broken built-in.
    auto ensure = store.execute(
        "CREATE NODE TABLE IF NOT EXISTS _SchemaVersion(id INT64 PRIMARY KEY, value INT64);");
    if (is_error(ensure)) return error_of(ensure);

    auto r = store.execute(
        "MATCH (v:_SchemaVersion {id: 1}) RETURN v.value AS value;");
    if (is_error(r)) return error_of(r);
    const auto& env = value_of(r);
    if (env["row_count"].get<int64_t>() == 0) {
        return Json(0);
    }
    return Json(env["rows"][0]["value"].get<int64_t>());
}

GraphResult writeVersion(GraphStore& store, int version) {
    // MERGE-ish upsert: try update first; if no rows match, insert.
    // Kuzu 0.11 does not support `MERGE` over arbitrary key sets reliably,
    // so we hand-roll the existence check.
    auto exists = store.execute(
        "MATCH (v:_SchemaVersion {id: 1}) RETURN v.value AS value;");
    if (is_error(exists)) return error_of(exists);

    if (value_of(exists)["row_count"].get<int64_t>() == 0) {
        std::ostringstream oss;
        oss << "CREATE (:_SchemaVersion {id: 1, value: " << version << "});";
        auto ins = store.execute(oss.str());
        if (is_error(ins)) return error_of(ins);
    } else {
        std::ostringstream oss;
        oss << "MATCH (v:_SchemaVersion {id: 1}) "
            << "SET v.value = " << version << ";";
        auto upd = store.execute(oss.str());
        if (is_error(upd)) return error_of(upd);
    }
    return Json(version);
}

}  // namespace

const std::vector<Migration>& schemaMigrations() {
    static const std::vector<Migration> kMigrations = {
        {1, kV1Cypher},
        {2, kV2Cypher},
        {3, kV3Cypher},
        {4, kV4Cypher},
    };
    return kMigrations;
}

GraphResult migrateToCurrent(GraphStore& store) {
    auto verResult = readCurrentVersion(store);
    if (is_error(verResult)) return error_of(verResult);
    const int from = static_cast<int>(value_of(verResult).get<int64_t>());

    int applied = 0;
    int last    = from;

    for (const auto& m : schemaMigrations()) {
        if (m.version <= from) continue;

        spdlog::info("graph schema: applying migration v{}", m.version);
        for (const auto& stmt : splitStatements(m.cypher)) {
            auto r = store.execute(stmt);
            if (is_error(r)) {
                spdlog::error("graph schema: migration v{} failed at stmt '{}': {}",
                              m.version, stmt, error_of(r).message);
                return error_of(r);
            }
        }

        auto wrote = writeVersion(store, m.version);
        if (is_error(wrote)) return error_of(wrote);

        ++applied;
        last = m.version;
    }

    Json result = Json::object();
    result["from"]    = from;
    result["to"]      = last;
    result["applied"] = applied;
    return result;
}

}  // namespace sage::graph
