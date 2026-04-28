// AssetIndexer smoke. Drives ingestAssets / getIndexStatus through a
// GraphStoreManager-acquired slot. Verifies:
//   1) v2 schema applies on a fresh slot (_IndexState exists).
//   2) Initial getIndexStatus reports asset_count=0, last_indexed_at_ms=null.
//   3) ingestAssets writes the rows + updates _IndexState.
//   4) Re-running ingestAssets with a smaller set wipes the previous snapshot
//      (full-replace semantics).
//   5) Cypher-quote escaping survives apostrophe in asset path.
//   6) Validation rejects non-array / missing fields without touching DB.

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
    auto r = mustOk(store.execute("MATCH (a:Asset) RETURN count(a) AS c;"),
                    "count assets");
    return r["rows"][0]["c"].get<int64_t>();
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "sage_asset_indexer_smoke";
    std::error_code ec;
    fs::remove_all(root, ec);

    try {
        sg::GraphStoreManager mgr(root);
        auto& store = mgr.acquireSlot("test");

        // --- (1) v2 schema present ---------------------------------------
        auto tables = mustOk(store.execute("CALL show_tables() RETURN name;"),
                             "show_tables");
        bool hasIndexState = false;
        for (const auto& r : tables["rows"]) {
            if (r["name"] == "_IndexState") { hasIndexState = true; break; }
        }
        if (!hasIndexState)
            throw std::runtime_error("v2 _IndexState missing");

        // --- (2) initial status empty ------------------------------------
        auto s0 = mustOk(sg::getIndexStatus(store), "initial status");
        if (s0["asset_count"].get<int64_t>() != 0
            || !s0["last_indexed_at_ms"].is_null()) {
            throw std::runtime_error("initial _IndexState not empty: " + s0.dump());
        }

        // --- (3) ingest 3 assets -----------------------------------------
        sg::Json batch1 = sg::Json::array({
            {{"path", "/Game/BP_Foo"},   {"kind", "Blueprint"}},
            {{"path", "/Game/SM_Crate"}, {"kind", "StaticMesh"}},
            {{"path", "/Game/MI_Hero"},  {"kind", "MaterialInstance"}},
        });
        auto in1 = mustOk(sg::ingestAssets(store, batch1), "ingest1");
        if (in1["asset_count"].get<int64_t>() != 3)
            throw std::runtime_error("ingest1 asset_count != 3: " + in1.dump());
        if (countAssets(store) != 3)
            throw std::runtime_error("Asset row count != 3 after ingest1");

        auto s1 = mustOk(sg::getIndexStatus(store), "status1");
        if (s1["asset_count"].get<int64_t>() != 3)
            throw std::runtime_error("status asset_count != 3: " + s1.dump());
        if (s1["last_indexed_at_ms"].is_null())
            throw std::runtime_error("status last_indexed_at_ms still null");

        // --- (4) re-ingest with 1 asset wipes the rest -------------------
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // ensure ts moves
        sg::Json batch2 = sg::Json::array({
            {{"path", "/Game/BP_Foo"}, {"kind", "Blueprint"}},
        });
        auto in2 = mustOk(sg::ingestAssets(store, batch2), "ingest2");
        if (in2["asset_count"].get<int64_t>() != 1)
            throw std::runtime_error("ingest2 asset_count != 1");
        if (countAssets(store) != 1)
            throw std::runtime_error("re-ingest did not wipe — leaked rows");

        auto s2 = mustOk(sg::getIndexStatus(store), "status2");
        if (s2["last_indexed_at_ms"].get<int64_t>()
            <= s1["last_indexed_at_ms"].get<int64_t>()) {
            throw std::runtime_error("status timestamp did not advance");
        }

        // --- (5) escape-test: apostrophe in path -------------------------
        sg::Json batch3 = sg::Json::array({
            {{"path", "/Game/A's_Folder/Asset"}, {"kind", "Texture2D"}},
            {{"path", "/Game/Back\\slash"},      {"kind", "Texture2D"}},
        });
        auto in3 = mustOk(sg::ingestAssets(store, batch3), "ingest3 (escape)");
        if (in3["asset_count"].get<int64_t>() != 2)
            throw std::runtime_error("escape test asset_count != 2");
        // Round-trip the funny path through SELECT.
        auto sel = mustOk(store.execute(
            "MATCH (a:Asset) RETURN a.path AS p ORDER BY p;"),
            "escape select");
        const auto& rows = sel["rows"];
        if (rows.size() != 2
            || rows[0]["p"].get<std::string>() != "/Game/A's_Folder/Asset"
            || rows[1]["p"].get<std::string>() != "/Game/Back\\slash") {
            throw std::runtime_error("escape round-trip failed: " + sel.dump());
        }

        // --- (6) bad input rejected --------------------------------------
        auto bad1 = sg::ingestAssets(store, sg::Json("not an array"));
        if (!sg::is_error(bad1))
            throw std::runtime_error("non-array input was accepted");

        auto bad2 = sg::ingestAssets(store, sg::Json::array({
            {{"path", "/Game/X"}, {"kind", 7}},  // non-string kind
        }));
        if (!sg::is_error(bad2))
            throw std::runtime_error("missing-string row was accepted");

        // bad input must not have wiped state — the 2 escape rows survive.
        if (countAssets(store) != 2)
            throw std::runtime_error("bad input clobbered DB; pre-validation broken");

        std::cout << "[asset-indexer-smoke] OK" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[asset-indexer-smoke] FAIL — " << ex.what() << std::endl;
        return 1;
    }
}
