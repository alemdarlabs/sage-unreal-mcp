// KuzuDB integration smoke. Opens an embedded DB, creates a tiny node table,
// inserts rows, runs a Cypher query, prints results. Sanity check that the
// kuzu prebuilt links and runs on this host before we layer the GraphStore
// abstraction on top.
//
// Returns 0 on success; non-zero on any failure path.

#include "kuzu.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    // Kuzu v0.11 expects a file path; create the parent dir, drop any prior file.
    const fs::path parent = fs::temp_directory_path() / "sage_kuzu_smoke";
    const fs::path dbPath = parent / "graph.kuzu";
    std::error_code ec;
    fs::remove_all(parent, ec);
    fs::create_directories(parent, ec);

    try {
        std::cout << "[kuzu-smoke] opening db at " << dbPath << std::endl;
        kuzu::main::Database db(dbPath.string());
        kuzu::main::Connection conn(&db);

        const auto exec = [&](const std::string& cypher) {
            std::cout << "[kuzu-smoke] query: " << cypher << std::endl;
            auto result = conn.query(cypher);
            if (!result->isSuccess()) {
                throw std::runtime_error("query failed: " + result->getErrorMessage());
            }
            return result;
        };

        exec("CREATE NODE TABLE Asset(path STRING PRIMARY KEY, kind STRING);");
        exec("CREATE (:Asset {path: '/Game/BP_Foo', kind: 'Blueprint'});");
        exec("CREATE (:Asset {path: '/Game/MI_Hero', kind: 'MaterialInstance'});");

        auto out = exec("MATCH (a:Asset) RETURN a.path AS path, a.kind AS kind ORDER BY path;");
        int rows = 0;
        while (out->hasNext()) {
            auto tuple = out->getNext();
            const std::string p = tuple->getValue(0)->getValue<std::string>();
            const std::string k = tuple->getValue(1)->getValue<std::string>();
            std::cout << "  row[" << rows << "] path=" << p << " kind=" << k << std::endl;
            ++rows;
        }
        if (rows != 2) {
            std::cerr << "[kuzu-smoke] FAIL — expected 2 rows, got " << rows << std::endl;
            return 1;
        }

        std::cout << "[kuzu-smoke] OK — kuzu " << kuzu::main::Version::getVersion()
                  << " linked + queried" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[kuzu-smoke] FAIL — " << ex.what() << std::endl;
        return 1;
    }
}
