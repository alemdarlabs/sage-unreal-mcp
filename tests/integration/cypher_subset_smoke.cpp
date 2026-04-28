// Cypher subset validator smoke. Confirms the keyword whitelist, the
// variable-length traversal bound, the literal-strip safety, and the
// LIMIT auto-injection — independent of any kuzu connection.

#include "graph/cypher_subset.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sg = sage::graph;

namespace {

void assertOk(std::string_view cypher, const char* tag) {
    auto v = sg::validateReadOnlySubset(cypher);
    if (!v.ok) {
        throw std::runtime_error(std::string{"["} + tag
            + "] expected OK but got: " + v.error
            + "\n  cypher: " + std::string{cypher});
    }
}

void assertReject(std::string_view cypher, const char* substr, const char* tag) {
    auto v = sg::validateReadOnlySubset(cypher);
    if (v.ok) {
        throw std::runtime_error(std::string{"["} + tag
            + "] expected reject but accepted\n  cypher: " + std::string{cypher});
    }
    if (v.error.find(substr) == std::string::npos) {
        throw std::runtime_error(std::string{"["} + tag
            + "] reason mismatch — wanted '" + substr + "', got '" + v.error + "'");
    }
}

}  // namespace

int main() {
    try {
        // ---- Allowed shapes ----------------------------------------------
        assertOk("MATCH (a:Asset) RETURN a.path LIMIT 50", "basic match");
        assertOk("MATCH (a:Asset)-[:DEPENDS_ON]->(b) RETURN a.path, b.path",
                 "1-hop edge");
        assertOk("MATCH (a:Asset)<-[:DEPENDS_ON*1..5]-(b) RETURN DISTINCT b.path "
                 "ORDER BY b.path LIMIT 100",
                 "bounded *1..5");
        assertOk("MATCH (a:Asset) WHERE a.kind = 'Texture2D' RETURN count(a) AS n",
                 "filter + agg");
        assertOk("WITH 1 AS x MATCH (a:Asset) RETURN a.path", "WITH allowed");

        // ---- Banned write keywords --------------------------------------
        for (const char* kw : {
            "CREATE (a:Asset {path: '/x'})",
            "MATCH (a:Asset) DELETE a",
            "MATCH (a:Asset) DETACH DELETE a",
            "MATCH (a:Asset) SET a.kind = 'X'",
            "MATCH (a:Asset) REMOVE a.kind",
            "MERGE (a:Asset {path: '/x'})",
            "DROP TABLE Asset",
            "ALTER TABLE Asset DROP COLUMN kind",
            "COPY Asset FROM '/tmp/x.json' (file_format='json')",
            "LOAD CSV WITH HEADERS FROM 'x.csv' AS row RETURN row",
            "INSERT INTO Asset VALUES ('/x', 'k')",
            "CALL show_tables() RETURN name",
        }) {
            assertReject(kw, "banned keyword", kw);
        }

        // ---- Banned in literal-stripped scan, NOT in string content -----
        // The string contains 'CREATE' but it's inside a quoted asset path.
        assertOk("MATCH (a:Asset {path: '/Game/CREATE_Sample.CREATE_Sample'}) "
                 "RETURN a.path",
                 "literal CREATE in string");
        assertOk("MATCH (a:Asset {path: '/Game/Foo // DELETE bad'}) RETURN a.path",
                 "literal DELETE in string");

        // ---- Variable-length traversal bounds ---------------------------
        assertReject("MATCH (a)-[*]-(b) RETURN a, b",
                     "unbounded", "*-unbounded");
        assertReject("MATCH (a)-[*1..]-(b) RETURN a", "open-ended", "*1..");
        assertReject("MATCH (a)-[*..5]-(b) RETURN a", "open-ended", "*..5");
        assertReject("MATCH (a)-[*1..20]-(b) RETURN a",
                     "exceeds", "*1..20 too deep");
        assertOk("MATCH (a)-[*1..10]-(b) RETURN a LIMIT 50",
                 "*1..10 boundary");

        // ---- Length cap --------------------------------------------------
        std::string huge(10000, 'x');
        assertReject(huge.c_str(), "too long", "8KB cap");

        // ---- Empty -------------------------------------------------------
        assertReject("", "empty", "empty");

        // ---- LIMIT injection --------------------------------------------
        const std::string injected = sg::ensureLimit("MATCH (a) RETURN a", 50);
        if (injected.find("LIMIT 50") == std::string::npos) {
            throw std::runtime_error("LIMIT not injected: " + injected);
        }
        const std::string preserved = sg::ensureLimit(
            "MATCH (a) RETURN a LIMIT 7", 50);
        if (preserved.find("LIMIT 50") != std::string::npos
            || preserved.find("LIMIT 7") == std::string::npos) {
            throw std::runtime_error("LIMIT 50 wrongly added when 7 already present: "
                                     + preserved);
        }
        // Trailing semicolon stripped before append
        const std::string trimmed = sg::ensureLimit(
            "MATCH (a) RETURN a;\n  ", 25);
        if (trimmed != "MATCH (a) RETURN a LIMIT 25;") {
            throw std::runtime_error("trailing trim failed: '" + trimmed + "'");
        }
        // Literal containing "LIMIT" must not fool the detector
        const std::string literalLimit = sg::ensureLimit(
            "MATCH (a:Asset {kind: 'LIMIT_Hack'}) RETURN a", 99);
        if (literalLimit.find("LIMIT 99") == std::string::npos) {
            throw std::runtime_error("literal-stripped LIMIT detection broken: "
                                     + literalLimit);
        }

        std::cout << "[cypher-subset-smoke] OK" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[cypher-subset-smoke] FAIL — " << ex.what() << std::endl;
        return 1;
    }
}
