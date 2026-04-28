#include "graph/cypher_subset.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>

namespace sage::graph {

namespace {

// Strip Cypher single-quoted, double-quoted, and backticked literals from
// the query so the keyword scan can't be tricked by strings like
// `'/Game/CREATE_Asset'`. We replace the literal body with spaces so byte
// offsets line up if we ever surface them in errors.
std::string stripLiteralsAndComments(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];

        // Cypher line comment: //
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') { out.push_back(' '); ++i; }
            continue;
        }
        // Cypher block comment: /* ... */
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            out.append("  ");
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) {
                out.push_back(' ');
                ++i;
            }
            if (i + 1 < in.size()) { out.append("  "); i += 2; }
            continue;
        }
        // String/identifier literals (', ", `): drop body, preserve quotes
        // so a downstream regex still sees `'  '` as a single token.
        if (c == '\'' || c == '"' || c == '`') {
            const char q = c;
            out.push_back(q);
            ++i;
            while (i < in.size() && in[i] != q) {
                // Backslash escape inside string
                if (in[i] == '\\' && i + 1 < in.size()) {
                    out.push_back(' ');
                    out.push_back(' ');
                    i += 2;
                    continue;
                }
                out.push_back(' ');
                ++i;
            }
            if (i < in.size()) { out.push_back(q); ++i; }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

bool containsKeyword(const std::string& haystack, const std::string& kw) {
    // Word boundaries: keyword surrounded by non-alphanumeric/underscore.
    const std::regex re("(^|[^A-Za-z0-9_])" + kw + "([^A-Za-z0-9_]|$)",
                        std::regex_constants::icase);
    return std::regex_search(haystack, re);
}

}  // namespace

CypherValidation validateReadOnlySubset(std::string_view cypher) {
    if (cypher.empty()) {
        return {false, "empty cypher"};
    }
    if (cypher.size() > 8192) {
        return {false, "cypher too long (>8192 bytes)"};
    }

    const std::string scanned = stripLiteralsAndComments(cypher);

    // Banned write/DDL keywords. CALL is banned to keep admin procs off
    // the agent surface (show_tables etc. live behind index_status).
    static const std::array<const char*, 12> kBanned{
        "CREATE", "DELETE", "DETACH", "SET", "REMOVE", "MERGE",
        "DROP",   "ALTER",  "COPY",   "LOAD", "INSERT", "CALL",
    };
    for (const char* kw : kBanned) {
        if (containsKeyword(scanned, kw)) {
            return {false, std::string{"banned keyword '"} + kw
                          + "'; query_graph is read-only"};
        }
    }

    // At least one MATCH or RETURN — purely defensive (a query with
    // neither is either malformed or a CALL we already rejected).
    if (!containsKeyword(scanned, "MATCH") && !containsKeyword(scanned, "RETURN")) {
        return {false, "query must contain MATCH and/or RETURN"};
    }

    // Variable-length traversal bounds. Accept only `*N..M` with both
    // bounds present and M <= kMaxVariableLengthHops. Reject `*`,
    // `*N..`, `*..M`, `*N` (single digit means exactly N hops — fine,
    // but we still cap N), and `*N..*`-style malformed.
    static const std::regex varLen(R"(\*\s*([0-9]*)\s*(?:\.\.\s*([0-9]*))?)",
                                   std::regex::ECMAScript);
    auto begin = std::sregex_iterator(scanned.begin(), scanned.end(), varLen);
    auto end   = std::sregex_iterator{};
    for (auto it = begin; it != end; ++it) {
        const auto lo = (*it)[1].str();
        const auto hi = (*it)[2].str();
        const bool hasRange = (*it)[2].matched;
        if (lo.empty() && !hasRange) {
            return {false, "unbounded `*` traversal forbidden; use *1..N with N<=10"};
        }
        if (hasRange && (lo.empty() || hi.empty())) {
            return {false, "open-ended traversal range forbidden; both bounds required"};
        }
        try {
            const int loI = lo.empty() ? 0 : std::stoi(lo);
            const int hiI = hi.empty() ? loI : std::stoi(hi);
            if (hiI > kMaxVariableLengthHops) {
                return {false, "variable-length traversal upper bound exceeds "
                              + std::to_string(kMaxVariableLengthHops)};
            }
            if (loI < 0 || hiI < loI) {
                return {false, "invalid variable-length traversal range"};
            }
        } catch (const std::exception&) {
            return {false, "could not parse variable-length traversal bounds"};
        }
    }

    return {true, ""};
}

std::string ensureLimit(std::string_view cypher, int limit) {
    // Strip trailing whitespace + optional `;` so we can append cleanly.
    std::string body{cypher};
    while (!body.empty()
           && (body.back() == ' '  || body.back() == '\n'
            || body.back() == '\t' || body.back() == ';')) {
        body.pop_back();
    }

    // Top-level LIMIT detection on a literal-stripped copy (so a string
    // containing 'LIMIT' doesn't fool us).
    const auto scanned = stripLiteralsAndComments(body);
    if (containsKeyword(scanned, "LIMIT")) {
        return body + ";";
    }
    return body + " LIMIT " + std::to_string(limit) + ";";
}

}  // namespace sage::graph
