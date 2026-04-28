// GraphStore abstraction smoke. Drives KuzuGraphStore through the abstract
// GraphStore interface — proves the impl handles DDL, DML, and SELECT on a
// fresh embedded DB and that the JSON envelope shape is correct.

#include "graph/kuzu_graph_store.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    const fs::path parent = fs::temp_directory_path() / "sage_graph_store_smoke";
    const fs::path dbPath = parent / "graph.kuzu";
    std::error_code ec;
    fs::remove_all(parent, ec);

    try {
        sage::graph::KuzuGraphStore store(dbPath);
        if (!store.isOpen()) {
            std::cerr << "[graph-smoke] FAIL — store not open" << std::endl;
            return 1;
        }

        const auto exec = [&](std::string_view cypher) {
            std::cout << "[graph-smoke] exec: " << cypher << std::endl;
            auto r = store.execute(cypher);
            if (sage::graph::is_error(r)) {
                throw std::runtime_error("execute failed: " + sage::graph::error_of(r).message);
            }
            return sage::graph::value_of(r);
        };

        exec("CREATE NODE TABLE Asset(path STRING PRIMARY KEY, kind STRING, dirty BOOLEAN);");
        exec("CREATE (:Asset {path: '/Game/BP_Foo',    kind: 'Blueprint',         dirty: false});");
        exec("CREATE (:Asset {path: '/Game/MI_Hero',   kind: 'MaterialInstance',  dirty: true });");
        exec("CREATE (:Asset {path: '/Game/SM_Crate',  kind: 'StaticMesh',        dirty: false});");

        const auto select = exec(
            "MATCH (a:Asset) "
            "RETURN a.path AS path, a.kind AS kind, a.dirty AS dirty "
            "ORDER BY path;");

        std::cout << "[graph-smoke] envelope: " << select.dump(2) << std::endl;

        if (!select.contains("rows") || !select.contains("schema") || !select.contains("row_count")) {
            std::cerr << "[graph-smoke] FAIL — envelope missing required keys" << std::endl;
            return 1;
        }
        if (select["row_count"].get<int>() != 3) {
            std::cerr << "[graph-smoke] FAIL — expected 3 rows, got " << select["row_count"]
                      << std::endl;
            return 1;
        }

        // Type round-trip checks: bool stays bool, string stays string.
        const auto& row0 = select["rows"][0];
        if (!row0["dirty"].is_boolean() || !row0["path"].is_string()) {
            std::cerr << "[graph-smoke] FAIL — value type conversion broken: "
                      << row0.dump() << std::endl;
            return 1;
        }

        std::cout << "[graph-smoke] OK" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[graph-smoke] FAIL — " << ex.what() << std::endl;
        return 1;
    }
}
