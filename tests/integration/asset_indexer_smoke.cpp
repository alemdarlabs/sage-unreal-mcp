// AssetIndexer smoke. Drives ingestSnapshot / getIndexStatus through a
// GraphStoreManager-acquired slot. Verifies:
//   1) v3 schema applies on a fresh slot (_IndexState.dep_count, DEPENDS_ON).
//   2) Initial getIndexStatus reports counts=0, last_indexed_at_ms=null.
//   3) ingestSnapshot writes assets + DEPENDS_ON edges + _IndexState row.
//   4) Re-running with a smaller set wipes assets AND edges
//      (full-replace semantics across both layers).
//   5) Cypher-quote escaping survives apostrophe in asset path.
//   6) Validation rejects non-array / missing fields without touching DB.
//   7) Edges referencing unknown endpoints are silently skipped.

#include "graph/asset_indexer.h"
#include "graph/graph_store_manager.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace sg = sage::graph;

namespace {

sg::Json mustOk(sg::GraphResult r, const char* what) {
    if (sg::is_error(r))
        throw std::runtime_error(std::string{what} + ": " + sg::error_of(r).message);
    return sg::value_of(r);
}

int64_t countAssets(sg::GraphStore& store) {
    return mustOk(store.execute("MATCH (a:Asset) RETURN count(a) AS c;"),
                  "count assets")["rows"][0]["c"].get<int64_t>();
}

int64_t countDeps(sg::GraphStore& store) {
    return mustOk(store.execute(
        "MATCH ()-[r:DEPENDS_ON]->() RETURN count(r) AS c;"),
        "count deps")["rows"][0]["c"].get<int64_t>();
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "sage_asset_indexer_smoke";
    std::error_code ec;
    fs::remove_all(root, ec);

    try {
        sg::GraphStoreManager mgr(root);
        auto& store = mgr.acquireSlot("test");

        // --- (1) v3 schema present ---------------------------------------
        auto tables = mustOk(store.execute("CALL show_tables() RETURN name;"),
                             "show_tables");
        bool hasIndexState = false, hasDependsOn = false;
        for (const auto& r : tables["rows"]) {
            if (r["name"] == "_IndexState") hasIndexState = true;
            if (r["name"] == "DEPENDS_ON")  hasDependsOn  = true;
        }
        if (!hasIndexState || !hasDependsOn)
            throw std::runtime_error("v3 schema missing tables");

        // dep_count column present (v3 ALTER)
        auto verState = mustOk(store.execute(
            "MATCH (s:_IndexState {id: 1}) RETURN s.dep_count AS d;"),
            "verify dep_count column");
        if (verState["row_count"].get<int64_t>() != 0)
            throw std::runtime_error("_IndexState should be empty before ingest");

        // --- (2) initial status empty ------------------------------------
        auto s0 = mustOk(sg::getIndexStatus(store), "initial status");
        if (s0["asset_count"].get<int64_t>() != 0
            || s0["dep_count"].get<int64_t>() != 0
            || !s0["last_indexed_at_ms"].is_null()) {
            throw std::runtime_error("initial state not empty: " + s0.dump());
        }

        // --- (3) ingest 3 assets + 2 deps --------------------------------
        sg::Json snap1 = {
            {"assets", sg::Json::array({
                {{"path", "/Game/BP_Foo"},   {"kind", "Blueprint"}},
                {{"path", "/Game/SM_Crate"}, {"kind", "StaticMesh"}},
                {{"path", "/Game/MI_Hero"},  {"kind", "MaterialInstance"}},
            })},
            {"dependencies", sg::Json::array({
                {{"from", "/Game/BP_Foo"}, {"to", "/Game/SM_Crate"}},
                {{"from", "/Game/BP_Foo"}, {"to", "/Game/MI_Hero"}},
            })},
        };
        auto in1 = mustOk(sg::ingestSnapshot(store, snap1), "ingest1");
        if (in1["asset_count"].get<int64_t>() != 3
            || in1["dep_count"].get<int64_t>() != 2)
            throw std::runtime_error("ingest1 counts wrong: " + in1.dump());
        if (countAssets(store) != 3 || countDeps(store) != 2)
            throw std::runtime_error("DB row count mismatch after ingest1");

        // round-trip: BP_Foo's outbound deps
        auto outbound = mustOk(store.execute(
            "MATCH (a:Asset {path: '/Game/BP_Foo'})-[:DEPENDS_ON]->(b:Asset) "
            "RETURN b.path AS dep ORDER BY dep;"),
            "outbound deps");
        if (outbound["row_count"].get<int64_t>() != 2)
            throw std::runtime_error("BP_Foo outbound count != 2");

        // --- (4) re-ingest with smaller set wipes edges ------------------
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        sg::Json snap2 = {
            {"assets", sg::Json::array({
                {{"path", "/Game/BP_Foo"}, {"kind", "Blueprint"}},
            })},
            // no dependencies
        };
        auto in2 = mustOk(sg::ingestSnapshot(store, snap2), "ingest2");
        if (in2["asset_count"].get<int64_t>() != 1
            || in2["dep_count"].get<int64_t>() != 0)
            throw std::runtime_error("ingest2 counts wrong: " + in2.dump());
        if (countDeps(store) != 0)
            throw std::runtime_error("re-ingest left stale DEPENDS_ON edges");

        // --- (5) escape-test: apostrophe in path -------------------------
        sg::Json snap3 = {
            {"assets", sg::Json::array({
                {{"path", "/Game/A's_Folder/Asset"}, {"kind", "Texture2D"}},
                {{"path", "/Game/Back\\slash"},      {"kind", "Texture2D"}},
            })},
            {"dependencies", sg::Json::array({
                {{"from", "/Game/A's_Folder/Asset"}, {"to", "/Game/Back\\slash"}},
            })},
        };
        auto in3 = mustOk(sg::ingestSnapshot(store, snap3), "ingest3 (escape)");
        if (in3["dep_count"].get<int64_t>() != 1)
            throw std::runtime_error("escape dep round-trip failed: " + in3.dump());

        // --- (6) bad input rejected --------------------------------------
        auto bad1 = sg::ingestSnapshot(store, sg::Json("not an object"));
        if (!sg::is_error(bad1)) throw std::runtime_error("non-object accepted");

        auto bad2 = sg::ingestSnapshot(store, sg::Json::object({
            {"assets", sg::Json::array({
                {{"path", "/Game/X"}, {"kind", 7}},  // non-string kind
            })},
        }));
        if (!sg::is_error(bad2)) throw std::runtime_error("bad asset row accepted");

        auto bad3 = sg::ingestSnapshot(store, sg::Json::object({
            {"assets", sg::Json::array()},
            {"dependencies", sg::Json::array({
                {{"from", "/Game/X"}, {"to", 99}},  // non-string to
            })},
        }));
        if (!sg::is_error(bad3)) throw std::runtime_error("bad dep row accepted");

        // bad input must not have wiped state — escape rows survive.
        if (countAssets(store) != 2)
            throw std::runtime_error("bad input clobbered DB; pre-validation broken");

        // --- (7) deps to unknown assets silently skipped -----------------
        sg::Json snap4 = {
            {"assets", sg::Json::array({
                {{"path", "/Game/Known1"}, {"kind", "X"}},
                {{"path", "/Game/Known2"}, {"kind", "X"}},
            })},
            {"dependencies", sg::Json::array({
                {{"from", "/Game/Known1"},  {"to",   "/Game/Known2"}},   // OK
                {{"from", "/Game/Known1"},  {"to",   "/Game/Missing"}}, // skip
                {{"from", "/Game/Missing"}, {"to",   "/Game/Known2"}},  // skip
                {{"from", "/Game/Known1"},  {"to",   "/Game/Known1"}},  // skip self
            })},
        };
        auto in4 = mustOk(sg::ingestSnapshot(store, snap4), "ingest4 (skip)");
        if (in4["dep_count"].get<int64_t>() != 1)
            throw std::runtime_error("dep skip filter broken: " + in4.dump());
        if (countDeps(store) != 1)
            throw std::runtime_error("dep skip filter — unexpected edge count");

        std::cout << "[asset-indexer-smoke] OK" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[asset-indexer-smoke] FAIL — " << ex.what() << std::endl;
        return 1;
    }
}
