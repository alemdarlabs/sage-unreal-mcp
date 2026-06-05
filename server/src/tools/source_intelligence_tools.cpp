#include "tools/source_intelligence_tools.h"

#include "mcp/error_codes.h"
#include "mcp/tool.h"
#include "mcp/tool_registry.h"
#include "util/env.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Tool = sage::mcp::Tool;
using ToolResult = sage::mcp::ToolResult;

constexpr std::uintmax_t kMaxReadBytes = 1024 * 1024;
constexpr int kDefaultMaxFiles = 6000;

std::optional<fs::path> g_enginePathOverride;
std::optional<fs::path> g_projectPathOverride;

Json obj(Json props, std::initializer_list<const char*> required = {}) {
    Json s{{"type", "object"}, {"additionalProperties", false}};
    s["properties"] = props.is_null() ? Json::object() : std::move(props);
    if (!required.size()) return s;
    Json req = Json::array();
    for (const char* key : required) req.push_back(key);
    s["required"] = std::move(req);
    return s;
}

Json str() { return {{"type", "string"}}; }
Json i32() { return {{"type", "integer"}}; }
Json bln() { return {{"type", "boolean"}}; }
Json strArray() { return {{"type", "array"}, {"items", {{"type", "string"}}}}; }

ToolResult invalid(std::string detail) {
    return std::unexpected(sage::mcp::ErrorObject::fromCode(
        sage::mcp::ErrorCode::InvalidParams, std::move(detail)));
}

std::string trim(std::string_view v) {
    std::size_t a = 0;
    while (a < v.size() && std::isspace(static_cast<unsigned char>(v[a])) != 0) ++a;
    std::size_t b = v.size();
    while (b > a && std::isspace(static_cast<unsigned char>(v[b - 1])) != 0) --b;
    return std::string{v.substr(a, b - a)};
}

std::string lower(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool containsI(std::string_view haystack, std::string_view needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string regexEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (char c : text) {
        switch (c) {
            case '.': case '^': case '$': case '|': case '(': case ')':
            case '[': case ']': case '{': case '}': case '*': case '+':
            case '?': case '\\':
                out.push_back('\\');
                break;
            default:
                break;
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> split(std::string_view text, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(delim, start);
        const auto part = text.substr(start, pos == std::string_view::npos ? text.size() - start : pos - start);
        std::string t = trim(part);
        if (!t.empty()) out.push_back(std::move(t));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> linesOf(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('\n', start);
        std::string line{text.substr(start, pos == std::string_view::npos ? text.size() - start : pos - start)};
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return lines;
}

int lineNumberAt(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

std::optional<std::string> stringArg(const Json& params,
                                     std::initializer_list<const char*> keys,
                                     std::string fallback = {}) {
    if (!params.is_object()) return fallback;
    for (const char* key : keys) {
        if (params.contains(key) && params[key].is_string()) {
            return params[key].get<std::string>();
        }
    }
    return fallback;
}

int intArg(const Json& params, const char* key, int fallback, int minValue, int maxValue) {
    if (!params.is_object() || !params.contains(key) || !params[key].is_number_integer()) {
        return fallback;
    }
    return std::clamp(params[key].get<int>(), minValue, maxValue);
}

bool boolArg(const Json& params, const char* key, bool fallback) {
    if (!params.is_object() || !params.contains(key) || !params[key].is_boolean()) return fallback;
    return params[key].get<bool>();
}

std::vector<std::string> stringArrayArg(const Json& params, const char* key) {
    std::vector<std::string> out;
    if (!params.is_object() || !params.contains(key) || !params[key].is_array()) return out;
    for (const auto& v : params[key]) {
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (!s.empty()) out.push_back(std::move(s));
        }
    }
    return out;
}

fs::path weakAbs(const fs::path& p) {
    std::error_code ec;
    fs::path abs = p.is_absolute() ? p : fs::absolute(p, ec);
    if (ec) abs = p;
    fs::path norm = fs::weakly_canonical(abs, ec);
    return ec ? abs.lexically_normal() : norm;
}

fs::path envPath(const char* name) {
    const std::string value = sage::util::envValue(name);
    if (value.empty()) return {};
    return weakAbs(fs::path{value});
}

fs::path defaultRepoRoot() {
    fs::path root = envPath("SAGE_REPO_ROOT");
    if (!root.empty()) return root;
    std::error_code ec;
    return weakAbs(fs::current_path(ec));
}

fs::path rootArg(const Json& params) {
    for (const char* key : {"root", "project_root", "repo_root", "source_root"}) {
        if (auto s = stringArg(params, {key}); s && !s->empty()) return weakAbs(*s);
    }
    return defaultRepoRoot();
}

fs::path engineRootArg(const Json& params) {
    if (auto s = stringArg(params, {"engine_root"}); s && !s->empty()) return weakAbs(*s);
    return envPath("SAGE_UE_ROOT");
}

void addIfDir(std::vector<fs::path>& out, const fs::path& p) {
    if (p.empty()) return;
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
        fs::path n = weakAbs(p);
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(std::move(n));
    }
}

void addPluginSourceRoots(std::vector<fs::path>& out, const fs::path& root) {
    const fs::path plugins = root / "Plugins";
    std::error_code ec;
    if (!fs::exists(plugins, ec) || !fs::is_directory(plugins, ec)) return;
    for (const fs::directory_entry& plugin : fs::directory_iterator(plugins, fs::directory_options::skip_permission_denied, ec)) {
        if (plugin.is_directory(ec)) addIfDir(out, plugin.path() / "Source");
    }
}

std::vector<fs::path> collectSourceRoots(const Json& params, bool includeEngineByDefault) {
    std::vector<fs::path> roots;
    for (const std::string& s : stringArrayArg(params, "source_roots")) addIfDir(roots, s);

    const fs::path base = rootArg(params);
    addIfDir(roots, base / "Source");
    addIfDir(roots, base / "server" / "src");
    addIfDir(roots, base / "plugin" / "Source");
    addPluginSourceRoots(roots, base);
    if (roots.empty()) addIfDir(roots, base);

    const bool includeEngine = boolArg(params, "include_engine", includeEngineByDefault);
    if (includeEngine) {
        const fs::path engine = engineRootArg(params);
        const std::string category = stringArg(params, {"category"}, "").value_or("");
        if (!engine.empty()) {
            if (!category.empty()) addIfDir(roots, engine / "Engine" / "Source" / category);
            else addIfDir(roots, engine / "Engine" / "Source");
        }
    }
    return roots;
}

std::vector<fs::path> collectDocRoots(const Json& params) {
    std::vector<fs::path> roots;
    for (const std::string& s : stringArrayArg(params, "docs_roots")) addIfDir(roots, s);
    const fs::path base = rootArg(params);
    addIfDir(roots, base / "docs");
    const fs::path engine = engineRootArg(params);
    addIfDir(roots, engine / "Engine" / "Documentation" / "Source");
    if (roots.empty()) addIfDir(roots, base);
    return roots;
}

bool skipDirName(std::string_view name) {
    static const std::unordered_set<std::string> kSkip{
        ".git", ".vs", ".idea", ".vscode", "binaries", "intermediate", "saved",
        "deriveddatacache", "build", "cmake-build-debug", "cmake-build-release",
        "node_modules", "vcpkg_installed", "__pycache__", ".cache"
    };
    return kSkip.contains(lower(name));
}

bool sourceExt(const fs::path& p) {
    const std::string name = lower(p.filename().string());
    return endsWith(name, ".h") || endsWith(name, ".hpp") || endsWith(name, ".hh")
        || endsWith(name, ".cpp") || endsWith(name, ".cc") || endsWith(name, ".cxx")
        || endsWith(name, ".inl") || endsWith(name, ".cs") || endsWith(name, ".build.cs")
        || endsWith(name, ".target.cs");
}

bool headerExt(const fs::path& p) {
    const std::string name = lower(p.filename().string());
    return endsWith(name, ".h") || endsWith(name, ".hpp") || endsWith(name, ".hh");
}

bool docExt(const fs::path& p) {
    const std::string name = lower(p.filename().string());
    return endsWith(name, ".md") || endsWith(name, ".rst") || endsWith(name, ".txt")
        || endsWith(name, ".html") || endsWith(name, ".adoc");
}

std::vector<fs::path> walkFiles(const std::vector<fs::path>& roots,
                                bool docs,
                                int maxFiles = kDefaultMaxFiles) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const fs::path& root : roots) {
        if (files.size() >= static_cast<std::size_t>(maxFiles)) break;
        if (!fs::exists(root, ec)) continue;
        if (fs::is_regular_file(root, ec)) {
            if ((docs ? docExt(root) : sourceExt(root))) files.push_back(weakAbs(root));
            continue;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; it != end && files.size() < static_cast<std::size_t>(maxFiles); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            const fs::path p = it->path();
            if (it->is_directory(ec)) {
                if (skipDirName(p.filename().string())) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if ((docs ? docExt(p) : sourceExt(p))) files.push_back(weakAbs(p));
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::optional<std::string> readText(const fs::path& path, std::uintmax_t maxBytes = kMaxReadBytes) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (!ec && size > maxBytes) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() > maxBytes) text.resize(static_cast<std::size_t>(maxBytes));
    return text;
}

std::string relPath(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    fs::path rel = fs::relative(path, root, ec);
    return (ec ? path : rel).generic_string();
}

Json rootsJson(const std::vector<fs::path>& roots) {
    Json arr = Json::array();
    for (const fs::path& r : roots) arr.push_back(r.generic_string());
    return arr;
}

Json searchTextFiles(const std::vector<fs::path>& roots,
                     bool docs,
                     std::string_view query,
                     int maxResults) {
    const std::string qLower = lower(query);
    Json matches = Json::array();
    int scanned = 0;
    for (const fs::path& file : walkFiles(roots, docs)) {
        if (matches.size() >= static_cast<std::size_t>(maxResults)) break;
        auto textOpt = readText(file);
        if (!textOpt) continue;
        ++scanned;
        const auto lines = linesOf(*textOpt);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lower(lines[i]).find(qLower) == std::string::npos) continue;
            matches.push_back({
                {"path", file.generic_string()},
                {"line", i + 1},
                {"snippet", trim(lines[i])},
            });
            if (matches.size() >= static_cast<std::size_t>(maxResults)) break;
        }
    }
    return {{"matches", matches}, {"count", matches.size()}, {"scanned_files", scanned}, {"roots", rootsJson(roots)}};
}

std::string shortNameFromFqn(std::string fqn) {
    if (const std::size_t dot = fqn.rfind('.'); dot != std::string::npos) fqn = fqn.substr(dot + 1);
    if (const std::size_t colon = fqn.rfind("::"); colon != std::string::npos) fqn = fqn.substr(colon + 2);
    if (const std::size_t slash = fqn.rfind('/'); slash != std::string::npos) fqn = fqn.substr(slash + 1);
    return fqn;
}

std::string lastIdentifierBeforeParen(std::string_view signature) {
    const std::size_t paren = signature.find('(');
    if (paren == std::string_view::npos) return {};
    std::string left = std::string{signature.substr(0, paren)};
    std::regex idRe(R"(([A-Za-z_~][A-Za-z0-9_]*)\s*$)");
    std::smatch m;
    if (std::regex_search(left, m, idRe)) {
        std::string name = m[1].str();
        if (!name.empty() && name.front() == '~') name.erase(name.begin());
        return name;
    }
    return {};
}

std::size_t matchingBrace(std::string_view text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

struct ClassRecord {
    std::string name;
    std::string specifierText;
    std::vector<std::string> specifiers;
    std::string inheritance;
    fs::path file;
    int line = 0;
    Json properties = Json::array();
    Json functions = Json::array();
};

Json specifierArray(std::string_view text) {
    Json arr = Json::array();
    for (const std::string& s : split(text, ',')) arr.push_back(s);
    return arr;
}

std::vector<ClassRecord> parseClassesInFile(const fs::path& file, std::string_view text) {
    std::vector<ClassRecord> out;
    const std::regex classRe(
        R"(UCLASS\s*(?:\(([\s\S]*?)\))?\s*(?:[A-Z0-9_]+_API\s+)?class\s+(?:[A-Z0-9_]+_API\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*(?:final\s*)?(?::\s*([^{]+))?\{)");
    const std::string source{text};
    for (std::sregex_iterator it(source.begin(), source.end(), classRe), end; it != end; ++it) {
        const std::smatch& m = *it;
        const std::size_t brace = static_cast<std::size_t>(m.position(0) + m.length(0) - 1);
        const std::size_t close = matchingBrace(source, brace);
        if (close == std::string_view::npos || close <= brace) continue;

        ClassRecord rec;
        rec.specifierText = trim(m[1].str());
        rec.specifiers = split(rec.specifierText, ',');
        rec.name = m[2].str();
        rec.inheritance = trim(m[3].str());
        rec.file = file;
        rec.line = lineNumberAt(source, static_cast<std::size_t>(m.position(0)));

        const std::string body = source.substr(brace + 1, close - brace - 1);
        const std::size_t bodyOffset = brace + 1;

        const std::regex propRe(R"(UPROPERTY\s*(?:\(([\s\S]*?)\))?\s*([^;]+;))");
        for (std::sregex_iterator pit(body.begin(), body.end(), propRe), pend; pit != pend; ++pit) {
            const std::string spec = trim((*pit)[1].str());
            const std::string decl = trim((*pit)[2].str());
            std::string name;
            std::smatch nameMatch;
            if (std::regex_search(decl, nameMatch, std::regex(R"(([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]+\])?\s*;)"))) {
                name = nameMatch[1].str();
            }
            rec.properties.push_back({
                {"name", name},
                {"specifiers", specifierArray(spec)},
                {"specifier_text", spec},
                {"declaration", decl},
                {"line", lineNumberAt(source, bodyOffset + static_cast<std::size_t>((*pit).position(0)))},
            });
        }

        const std::regex funcRe(R"(UFUNCTION\s*(?:\(([\s\S]*?)\))?\s*([^;{]+[;)])?)");
        for (std::sregex_iterator fit(body.begin(), body.end(), funcRe), fend; fit != fend; ++fit) {
            const std::string spec = trim((*fit)[1].str());
            std::string sig = trim((*fit)[2].str());
            if (sig.empty()) continue;
            if (sig.back() == '{') sig.pop_back();
            rec.functions.push_back({
                {"name", lastIdentifierBeforeParen(sig)},
                {"specifiers", specifierArray(spec)},
                {"specifier_text", spec},
                {"signature", trim(sig)},
                {"line", lineNumberAt(source, bodyOffset + static_cast<std::size_t>((*fit).position(0)))},
            });
        }

        out.push_back(std::move(rec));
    }
    return out;
}

std::vector<ClassRecord> buildReflectionIndex(const Json& params) {
    const auto roots = collectSourceRoots(params, false);
    const int maxFiles = intArg(params, "max_files", kDefaultMaxFiles, 1, 50000);
    std::vector<ClassRecord> classes;
    for (const fs::path& file : walkFiles(roots, false, maxFiles)) {
        if (!headerExt(file)) continue;
        auto text = readText(file);
        if (!text) continue;
        auto parsed = parseClassesInFile(file, *text);
        classes.insert(classes.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    }
    return classes;
}

Json classRecordJson(const ClassRecord& c, bool includeMembers) {
    Json j{
        {"name", c.name},
        {"path", c.file.generic_string()},
        {"line", c.line},
        {"specifier_text", c.specifierText},
        {"specifiers", c.specifiers},
        {"inheritance", c.inheritance},
    };
    if (includeMembers) {
        j["uproperties"] = c.properties;
        j["ufunctions"] = c.functions;
        j["property_count"] = c.properties.size();
        j["function_count"] = c.functions.size();
    }
    return j;
}

std::optional<ClassRecord> findClassRecord(const Json& params, std::string name) {
    name = shortNameFromFqn(std::move(name));
    auto classes = buildReflectionIndex(params);
    const std::string target = lower(name);
    for (const ClassRecord& c : classes) {
        if (lower(c.name) == target) return c;
    }
    return std::nullopt;
}

Json searchFunctionSignatures(const Json& params, std::string_view name, int maxResults) {
    Json matches = Json::array();
    const std::string pattern = std::string{R"(\b)"} + regexEscape(name) + R"(\s*\()";
    std::regex re(pattern);
    for (const fs::path& file : walkFiles(collectSourceRoots(params, true), false)) {
        if (matches.size() >= static_cast<std::size_t>(maxResults)) break;
        auto text = readText(file);
        if (!text) continue;
        const auto lines = linesOf(*text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!std::regex_search(lines[i], re)) continue;
            matches.push_back({{"path", file.generic_string()}, {"line", i + 1}, {"signature", trim(lines[i])}});
            if (matches.size() >= static_cast<std::size_t>(maxResults)) break;
        }
    }
    return {{"functions", matches}, {"count", matches.size()}};
}

std::optional<std::string> includePathForHeader(const fs::path& header) {
    std::vector<std::string> parts;
    for (const auto& part : header) parts.push_back(part.string());
    for (std::size_t i = 0; i + 2 < parts.size(); ++i) {
        if (parts[i] == "Source") {
            for (std::size_t j = i + 2; j < parts.size(); ++j) {
                const std::string pLower = lower(parts[j]);
                if (pLower == "public" || pLower == "classes") {
                    fs::path rel;
                    for (std::size_t k = j + 1; k < parts.size(); ++k) rel /= parts[k];
                    if (!rel.empty()) return rel.generic_string();
                }
            }
            fs::path rel;
            for (std::size_t k = i + 2; k < parts.size(); ++k) rel /= parts[k];
            if (!rel.empty()) return rel.generic_string();
        }
    }
    return header.filename().generic_string();
}

std::optional<std::string> shellQuote(const fs::path& p) {
    std::string s = p.string();
    if (s.find_first_of("\"\r\n") != std::string::npos) return std::nullopt;
    return "\"" + s + "\"";
}

std::optional<std::string> runCommand(const std::string& cmd, std::size_t maxBytes = 2 * 1024 * 1024) {
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe == nullptr) return std::nullopt;
    std::string out;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
        if (out.size() >= maxBytes) break;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

std::optional<std::string> gitOutput(const fs::path& root, const std::string& args) {
    auto quoted = shellQuote(root);
    if (!quoted) return std::nullopt;
    return runCommand("git -C " + *quoted + " " + args + " 2>&1");
}

Json gitChurn(const fs::path& root, int days, int maxResults) {
    const std::string args = "log --name-only --format= --since=\"" + std::to_string(days)
        + " days ago\" --max-count=1000 -- .";
    auto out = gitOutput(root, args);
    if (!out) return {{"git_available", false}, {"reason", "git command could not be launched"}};
    std::map<std::string, int> counts;
    std::istringstream in(*out);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with("fatal:")) continue;
        ++counts[line];
    }
    std::vector<std::pair<std::string, int>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    Json files = Json::array();
    for (const auto& [file, count] : rows) {
        if (files.size() >= static_cast<std::size_t>(maxResults)) break;
        files.push_back({{"path", file}, {"changes", count}});
    }
    return {{"git_available", true}, {"days", days}, {"files", files}, {"count", files.size()}};
}

std::vector<std::string> extractIncludedModules(std::string_view text) {
    std::set<std::string> mods;
    std::regex incRe(R"include((?:^|\n)\s*#\s*include\s+[<"]([^/">]+)[/">])include");
    const std::string source{text};
    for (std::sregex_iterator it(source.begin(), source.end(), incRe), end; it != end; ++it) {
        std::string first = (*it)[1].str();
        if (!first.empty() && std::isupper(static_cast<unsigned char>(first[0])) != 0) mods.insert(std::move(first));
    }
    return {mods.begin(), mods.end()};
}

std::vector<std::string> parseBuildDeps(std::string_view text) {
    std::set<std::string> deps;
    std::regex depRe("\"([^\"]+)\"");
    const std::string source{text};
    for (std::sregex_iterator it(source.begin(), source.end(), depRe), end; it != end; ++it) {
        deps.insert((*it)[1].str());
    }
    return {deps.begin(), deps.end()};
}

std::vector<fs::path> decisionFiles(const fs::path& root) {
    std::vector<fs::path> roots;
    addIfDir(roots, root / "docs" / "adr");
    addIfDir(roots, root / "decisions");
    return walkFiles(roots, true, 2000);
}

Json parseDecision(const fs::path& file, const fs::path& root) {
    Json j = {{"id", file.stem().string()}, {"path", file.generic_string()}, {"relative_path", relPath(file, root)}};
    auto text = readText(file);
    if (!text) {
        j["readable"] = false;
        return j;
    }
    j["readable"] = true;
    const auto lines = linesOf(*text);
    for (const std::string& line : lines) {
        if (line.starts_with("# ")) {
            j["title"] = trim(std::string_view{line}.substr(2));
            break;
        }
    }
    const std::string source = *text;
    std::regex statusRe(R"(^\s*status\s*[:|-]\s*([A-Za-z0-9 _-]+)\s*$)", std::regex_constants::icase);
    std::regex refRe(R"(^\s*(supersedes|replaces|superseded_by|replaced_by|depends_on|references)\s*[:|-]\s*(.+)$)", std::regex_constants::icase);
    Json refs = Json::array();
    for (const std::string& line : lines) {
        std::smatch lineMatch;
        if (!j.contains("status") && std::regex_match(line, lineMatch, statusRe)) {
            j["status"] = trim(lineMatch[1].str());
        }
        if (std::regex_match(line, lineMatch, refRe)) {
            refs.push_back({{"kind", lower(lineMatch[1].str())}, {"value", trim(lineMatch[2].str())}});
        }
    }
    if (!j.contains("status") && containsI(source, "superseded")) {
        j["status"] = "superseded";
    } else if (!j.contains("status")) {
        j["status"] = "unknown";
    }
    std::smatch m;
    if (std::regex_search(source, m, std::regex(R"((20[0-9]{2}-[0-9]{2}-[0-9]{2}))"))) {
        j["date"] = m[1].str();
    } else {
        const std::string filename = file.filename().string();
        if (std::regex_search(filename, m, std::regex(R"((20[0-9]{2}-[0-9]{2}-[0-9]{2}))"))) {
        j["date"] = m[1].str();
        }
    }
    j["references"] = refs;
    return j;
}

Json listDecisionRecords(const Json& params) {
    const fs::path root = rootArg(params);
    Json arr = Json::array();
    for (const fs::path& file : decisionFiles(root)) arr.push_back(parseDecision(file, root));
    return {{"decisions", arr}, {"count", arr.size()}, {"root", root.generic_string()}};
}

ToolResult SearchUnrealApi(const Json& params) {
    const std::string query = stringArg(params, {"query", "symbol", "name"}, "").value_or("");
    if (query.empty()) return invalid("missing query");
    const int maxResults = intArg(params, "max_results", 50, 1, 500);
    return searchTextFiles(collectSourceRoots(params, true), false, query, maxResults);
}

ToolResult GetByFqn(const Json& params) {
    const std::string symbol = stringArg(params, {"fqn", "symbol", "name"}, "").value_or("");
    if (symbol.empty()) return invalid("missing fqn/symbol");
    const std::string name = shortNameFromFqn(symbol);
    Json out{{"query", symbol}, {"short_name", name}};
    if (auto cls = findClassRecord(params, name)) {
        out["kind"] = "class";
        out["class"] = classRecordJson(*cls, true);
        return out;
    }
    out["kind"] = "source_matches";
    out["matches"] = searchTextFiles(collectSourceRoots(params, true), false, name, intArg(params, "max_results", 25, 1, 200))["matches"];
    return out;
}

ToolResult GetClassMembers(const Json& params) {
    const std::string cls = stringArg(params, {"class", "class_name", "name", "fqn"}, "").value_or("");
    if (cls.empty()) return invalid("missing class");
    auto rec = findClassRecord(params, cls);
    if (!rec) return invalid("class not found in scanned source roots: " + cls);
    return Json{{"class", rec->name}, {"path", rec->file.generic_string()}, {"uproperties", rec->properties},
                {"ufunctions", rec->functions}, {"property_count", rec->properties.size()},
                {"function_count", rec->functions.size()}};
}

ToolResult GetClassReference(const Json& params) {
    const std::string cls = stringArg(params, {"class", "class_name", "name", "fqn"}, "").value_or("");
    if (cls.empty()) return invalid("missing class");
    auto rec = findClassRecord(params, cls);
    if (!rec) return invalid("class not found in scanned source roots: " + cls);
    Json out = classRecordJson(*rec, true);
    out["include_path"] = includePathForHeader(rec->file).value_or(rec->file.filename().generic_string());
    return out;
}

ToolResult GetFunctionSignature(const Json& params) {
    const std::string fn = stringArg(params, {"function", "name", "symbol"}, "").value_or("");
    if (fn.empty()) return invalid("missing function");
    return searchFunctionSignatures(params, shortNameFromFqn(fn), intArg(params, "max_results", 25, 1, 200));
}

ToolResult GetIncludePath(const Json& params) {
    const std::string symbol = stringArg(params, {"symbol", "class", "class_name", "name", "fqn"}, "").value_or("");
    if (symbol.empty()) return invalid("missing symbol");
    auto rec = findClassRecord(params, symbol);
    if (!rec) return invalid("symbol header not found in scanned source roots: " + symbol);
    return Json{{"symbol", symbol}, {"path", rec->file.generic_string()},
                {"include_path", includePathForHeader(rec->file).value_or(rec->file.filename().generic_string())}};
}

ToolResult SearchDeprecated(const Json& params) {
    const int maxResults = intArg(params, "max_results", 100, 1, 500);
    const std::string query = stringArg(params, {"query", "symbol", "name"}, "").value_or("");
    Json out = searchTextFiles(collectSourceRoots(params, true), false, "DEPRECATED", maxResults);
    if (!query.empty()) {
        Json filtered = Json::array();
        for (const auto& m : out["matches"]) {
            if (containsI(m.value("snippet", ""), query) || containsI(m.value("path", ""), query)) filtered.push_back(m);
        }
        out["matches"] = filtered;
        out["count"] = filtered.size();
    }
    return out;
}

ToolResult GetDeprecationWarnings(const Json& params) {
    const std::string symbol = stringArg(params, {"symbol", "name", "fqn", "query"}, "").value_or("");
    if (symbol.empty()) return invalid("missing symbol");
    Json out = searchTextFiles(collectSourceRoots(params, true), false, shortNameFromFqn(symbol), intArg(params, "max_results", 50, 1, 200));
    Json warnings = Json::array();
    for (const auto& m : out["matches"]) {
        const std::string snip = m.value("snippet", "");
        if (containsI(snip, "deprecated") || containsI(snip, "UE_DEPRECATED")) warnings.push_back(m);
    }
    return Json{{"symbol", symbol}, {"warnings", warnings}, {"count", warnings.size()}};
}

ToolResult LookupDocs(const Json& params) {
    const std::string query = stringArg(params, {"query", "symbol", "name"}, "").value_or("");
    if (query.empty()) return invalid("missing query");
    return searchTextFiles(collectDocRoots(params), true, query, intArg(params, "max_results", 50, 1, 300));
}

ToolResult LookupClass(const Json& params) {
    const std::string cls = stringArg(params, {"class", "class_name", "name", "query"}, "").value_or("");
    if (cls.empty()) return invalid("missing class");
    Json out{{"class", cls}};
    if (auto rec = findClassRecord(params, cls)) {
        out["source"] = classRecordJson(*rec, true);
        out["include_path"] = includePathForHeader(rec->file).value_or(rec->file.filename().generic_string());
    }
    out["docs"] = searchTextFiles(collectDocRoots(params), true, shortNameFromFqn(cls), intArg(params, "max_results", 25, 1, 200))["matches"];
    return out;
}

ToolResult FindCallers(const Json& params) {
    const std::string symbol = stringArg(params, {"symbol", "function", "name", "query"}, "").value_or("");
    if (symbol.empty()) return invalid("missing symbol");
    Json out = searchTextFiles(collectSourceRoots(params, false), false, shortNameFromFqn(symbol), intArg(params, "max_results", 100, 1, 500));
    out["symbol"] = symbol;
    return out;
}

ToolResult FindCallees(const Json& params) {
    const std::string pathArg = stringArg(params, {"path", "file"}, "").value_or("");
    const std::string function = stringArg(params, {"function", "name", "symbol"}, "").value_or("");
    if (pathArg.empty() && function.empty()) return invalid("missing file/path or function");

    std::vector<fs::path> candidates;
    if (!pathArg.empty()) candidates.push_back(weakAbs(pathArg));
    else {
        Json callers = searchTextFiles(collectSourceRoots(params, false), false, function, 10);
        for (const auto& m : callers["matches"]) candidates.emplace_back(m.value("path", ""));
    }

    static const std::unordered_set<std::string> kIgnore{
        "if", "for", "while", "switch", "return", "sizeof", "alignof", "catch", "static_cast",
        "reinterpret_cast", "const_cast", "dynamic_cast", "TEXT", "UE_LOG", "check", "ensure"
    };

    std::map<std::string, int> counts;
    for (const fs::path& file : candidates) {
        auto text = readText(file);
        if (!text) continue;
        std::regex callRe(R"(\b([A-Za-z_][A-Za-z0-9_:]*)\s*\()");
        for (std::sregex_iterator it(text->begin(), text->end(), callRe), end; it != end; ++it) {
            std::string name = (*it)[1].str();
            std::string leaf = shortNameFromFqn(name);
            if (kIgnore.contains(leaf)) continue;
            ++counts[name];
        }
    }
    Json calls = Json::array();
    for (const auto& [name, count] : counts) calls.push_back({{"name", name}, {"count", count}});
    return Json{{"function", function}, {"files_scanned", candidates.size()}, {"callees", calls}, {"count", calls.size()}};
}

ToolResult AuditModuleDepReality(const Json& params) {
    const fs::path root = rootArg(params);
    Json modules = Json::array();
    for (const fs::path& file : walkFiles({root}, false, 20000)) {
        if (!endsWith(lower(file.filename().string()), ".build.cs")) continue;
        auto buildText = readText(file);
        if (!buildText) continue;
        const std::vector<std::string> declared = parseBuildDeps(*buildText);
        std::set<std::string> observed;
        const fs::path moduleDir = file.parent_path();
        for (const fs::path& src : walkFiles({moduleDir}, false, 5000)) {
            if (endsWith(lower(src.filename().string()), ".build.cs")) continue;
            auto text = readText(src);
            if (!text) continue;
            for (const std::string& m : extractIncludedModules(*text)) observed.insert(m);
        }
        Json missing = Json::array();
        for (const std::string& m : observed) {
            if (std::find(declared.begin(), declared.end(), m) == declared.end()) missing.push_back(m);
        }
        modules.push_back({{"module", file.stem().string()}, {"build_cs", file.generic_string()},
                           {"declared_dependencies", declared}, {"observed_include_roots", observed},
                           {"missing_candidates", missing}, {"missing_count", missing.size()}});
    }
    return Json{{"root", root.generic_string()}, {"modules", modules}, {"count", modules.size()}};
}

ToolResult DecisionList(const Json& params) {
    Json out = listDecisionRecords(params);
    const std::string query = stringArg(params, {"query"}, "").value_or("");
    const std::string status = stringArg(params, {"status"}, "").value_or("");
    if (query.empty() && status.empty()) return out;
    Json filtered = Json::array();
    for (const auto& d : out["decisions"]) {
        const bool qOk = query.empty() || containsI(d.dump(), query);
        const bool sOk = status.empty() || containsI(d.value("status", ""), status);
        if (qOk && sOk) filtered.push_back(d);
    }
    out["decisions"] = filtered;
    out["count"] = filtered.size();
    return out;
}

ToolResult DecisionGet(const Json& params) {
    const std::string id = stringArg(params, {"id", "decision", "path"}, "").value_or("");
    if (id.empty()) return invalid("missing id/decision/path");
    const fs::path root = rootArg(params);
    for (const fs::path& file : decisionFiles(root)) {
        Json d = parseDecision(file, root);
        if (containsI(d.value("id", ""), id) || containsI(d.value("relative_path", ""), id)
            || containsI(d.value("title", ""), id)) {
            d["content"] = readText(file, 256 * 1024).value_or("");
            return d;
        }
    }
    return invalid("decision not found: " + id);
}

ToolResult DecisionListStale(const Json& params) {
    const int olderThanDays = intArg(params, "older_than_days", 180, 1, 5000);
    Json out = listDecisionRecords(params);
    Json stale = Json::array();
    for (const auto& d : out["decisions"]) {
        const std::string status = lower(d.value("status", ""));
        if (status.find("stale") != std::string::npos || status.find("deprecated") != std::string::npos
            || status.find("superseded") != std::string::npos) {
            stale.push_back(d);
        }
    }
    out["decisions"] = stale;
    out["count"] = stale.size();
    out["older_than_days"] = olderThanDays;
    return out;
}

ToolResult DecisionSupersessionChain(const Json& params) {
    const std::string id = stringArg(params, {"id", "decision", "query"}, "").value_or("");
    if (id.empty()) return invalid("missing id/decision");
    Json all = listDecisionRecords(params);
    Json chain = Json::array();
    for (const auto& d : all["decisions"]) {
        if (!containsI(d.dump(), id)) continue;
        chain.push_back(d);
        for (const auto& ref : d.value("references", Json::array())) {
            const std::string kind = ref.value("kind", "");
            if (kind == "supersedes" || kind == "replaces" || kind == "superseded_by" || kind == "replaced_by") {
                chain.push_back({{"reference", ref}});
            }
        }
    }
    return Json{{"query", id}, {"chain", chain}, {"count", chain.size()}};
}

ToolResult DecisionReferents(const Json& params) {
    const std::string query = stringArg(params, {"query", "id", "decision", "path"}, "").value_or("");
    if (query.empty()) return invalid("missing query");
    Json all = listDecisionRecords(params);
    Json hits = Json::array();
    for (const auto& d : all["decisions"]) {
        if (containsI(d.dump(), query)) hits.push_back(d);
    }
    return Json{{"query", query}, {"decisions", hits}, {"count", hits.size()}};
}

ToolResult RiskFileChurn(const Json& params) {
    return gitChurn(rootArg(params), intArg(params, "days", 90, 1, 5000), intArg(params, "max_results", 50, 1, 500));
}

ToolResult RiskHotspotScore(const Json& params) {
    const fs::path root = rootArg(params);
    Json churn = gitChurn(root, intArg(params, "days", 90, 1, 5000), intArg(params, "max_results", 50, 1, 500));
    for (auto& f : churn["files"]) {
        const int changes = f.value("changes", 0);
        f["hotspot_score"] = changes * 10;
    }
    churn["scoring"] = "changes_in_window * 10";
    return churn;
}

ToolResult RiskCochangePairs(const Json& params) {
    const fs::path root = rootArg(params);
    auto out = gitOutput(root, "log --name-only --format=__SAGE_COMMIT__ --max-count=300 -- .");
    if (!out) return Json{{"git_available", false}};
    std::map<std::pair<std::string, std::string>, int> pairs;
    std::vector<std::string> current;
    auto flush = [&]() {
        std::sort(current.begin(), current.end());
        current.erase(std::unique(current.begin(), current.end()), current.end());
        for (std::size_t i = 0; i < current.size(); ++i) {
            for (std::size_t j = i + 1; j < current.size(); ++j) ++pairs[{current[i], current[j]}];
        }
        current.clear();
    };
    std::istringstream in(*out);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line == "__SAGE_COMMIT__") {
            flush();
        } else if (!line.empty() && !line.starts_with("fatal:")) {
            current.push_back(line);
        }
    }
    flush();
    std::vector<std::pair<std::pair<std::string, std::string>, int>> rows(pairs.begin(), pairs.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    Json arr = Json::array();
    const int maxResults = intArg(params, "max_results", 50, 1, 500);
    for (const auto& [pair, count] : rows) {
        if (arr.size() >= static_cast<std::size_t>(maxResults)) break;
        arr.push_back({{"a", pair.first}, {"b", pair.second}, {"cochanges", count}});
    }
    return Json{{"pairs", arr}, {"count", arr.size()}};
}

ToolResult RiskReleaseWindowHotspots(const Json& params) {
    Json churn = gitChurn(rootArg(params), intArg(params, "days", 30, 1, 365), intArg(params, "max_results", 50, 1, 500));
    churn["window"] = "release";
    return churn;
}

ToolResult RiskConditionalGates(const Json& params) {
    const auto roots = collectSourceRoots(params, false);
    Json matches = searchTextFiles(roots, false, "confirmed", intArg(params, "max_results", 100, 1, 500));
    Json dryRun = searchTextFiles(roots, false, "dry_run", intArg(params, "max_results", 100, 1, 500));
    return Json{{"confirmed_gates", matches["matches"]}, {"dry_run_gates", dryRun["matches"]},
                {"confirmed_count", matches["count"]}, {"dry_run_count", dryRun["count"]}};
}

ToolResult CppReflectGetUClass(const Json& params) {
    const std::string cls = stringArg(params, {"class", "class_name", "name", "fqn"}, "").value_or("");
    if (cls.empty()) return invalid("missing class");
    auto rec = findClassRecord(params, cls);
    if (!rec) return invalid("UCLASS not found: " + cls);
    return classRecordJson(*rec, true);
}

ToolResult CppReflectListUProperties(const Json& params) {
    const std::string cls = stringArg(params, {"class", "class_name", "name", "fqn"}, "").value_or("");
    if (cls.empty()) return invalid("missing class");
    auto rec = findClassRecord(params, cls);
    if (!rec) return invalid("UCLASS not found: " + cls);
    return Json{{"class", rec->name}, {"properties", rec->properties}, {"count", rec->properties.size()}};
}

ToolResult CppReflectListUFunctions(const Json& params) {
    const std::string cls = stringArg(params, {"class", "class_name", "name", "fqn"}, "").value_or("");
    if (cls.empty()) return invalid("missing class");
    auto rec = findClassRecord(params, cls);
    if (!rec) return invalid("UCLASS not found: " + cls);
    return Json{{"class", rec->name}, {"functions", rec->functions}, {"count", rec->functions.size()}};
}

ToolResult CppReflectFindInterfaceImpls(const Json& params) {
    const std::string iface = stringArg(params, {"interface", "name", "class"}, "").value_or("");
    if (iface.empty()) return invalid("missing interface");
    Json impls = Json::array();
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        if (containsI(c.inheritance, iface) || containsI(c.specifierText, iface)) impls.push_back(classRecordJson(c, false));
    }
    return Json{{"interface", iface}, {"implementers", impls}, {"count", impls.size()}};
}

ToolResult CppReflectFindClassSpecifier(const Json& params) {
    const std::string spec = stringArg(params, {"specifier", "query", "name"}, "").value_or("");
    if (spec.empty()) return invalid("missing specifier");
    Json hits = Json::array();
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        if (containsI(c.specifierText, spec)) hits.push_back(classRecordJson(c, false));
    }
    return Json{{"specifier", spec}, {"classes", hits}, {"count", hits.size()}};
}

ToolResult CppReflectListClassSpecifiers(const Json& params) {
    std::map<std::string, int> counts;
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        for (const std::string& s : c.specifiers) ++counts[s];
    }
    Json arr = Json::array();
    for (const auto& [spec, count] : counts) arr.push_back({{"specifier", spec}, {"count", count}});
    return Json{{"specifiers", arr}, {"count", arr.size()}};
}

ToolResult ReflectRebuildReflectionIndex(const Json& params) {
    auto classes = buildReflectionIndex(params);
    Json arr = Json::array();
    for (const ClassRecord& c : classes) arr.push_back(classRecordJson(c, false));
    return Json{{"rebuilt", true}, {"classes", arr}, {"class_count", arr.size()}};
}

ToolResult NetworkListReplicatedClasses(const Json& params) {
    Json arr = Json::array();
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        bool replicated = false;
        for (const auto& p : c.properties) replicated = replicated || containsI(p.value("specifier_text", ""), "Replicated");
        for (const auto& f : c.functions) replicated = replicated || containsI(f.value("specifier_text", ""), "Server")
            || containsI(f.value("specifier_text", ""), "Client") || containsI(f.value("specifier_text", ""), "NetMulticast");
        if (replicated) arr.push_back(classRecordJson(c, false));
    }
    return Json{{"classes", arr}, {"count", arr.size()}};
}

ToolResult NetworkListRpcFunctions(const Json& params) {
    Json arr = Json::array();
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        for (const auto& f : c.functions) {
            const std::string spec = f.value("specifier_text", "");
            if (containsI(spec, "Server") || containsI(spec, "Client") || containsI(spec, "NetMulticast")) {
                Json row = f;
                row["class"] = c.name;
                row["path"] = c.file.generic_string();
                arr.push_back(row);
            }
        }
    }
    return Json{{"rpc_functions", arr}, {"count", arr.size()}};
}

ToolResult NetworkListOnRepHandlers(const Json& params) {
    Json arr = Json::array();
    std::regex onrepRe(R"(ReplicatedUsing\s*=\s*([A-Za-z_][A-Za-z0-9_]*))");
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        for (const auto& p : c.properties) {
            std::smatch m;
            const std::string spec = p.value("specifier_text", "");
            if (std::regex_search(spec, m, onrepRe)) {
                arr.push_back({{"class", c.name}, {"property", p.value("name", "")},
                               {"handler", m[1].str()}, {"path", c.file.generic_string()},
                               {"line", p.value("line", 0)}});
            }
        }
    }
    return Json{{"onrep_handlers", arr}, {"count", arr.size()}};
}

ToolResult NetworkAuditUnbalancedOnReps(const Json& params) {
    Json issues = Json::array();
    std::regex onrepRe(R"(ReplicatedUsing\s*=\s*([A-Za-z_][A-Za-z0-9_]*))");
    for (const ClassRecord& c : buildReflectionIndex(params)) {
        std::set<std::string> declared;
        std::map<std::string, std::string> propForHandler;
        for (const auto& f : c.functions) declared.insert(f.value("name", ""));
        for (const auto& p : c.properties) {
            std::smatch m;
            const std::string spec = p.value("specifier_text", "");
            if (!std::regex_search(spec, m, onrepRe)) continue;
            const std::string handler = m[1].str();
            propForHandler[handler] = p.value("name", "");
            if (!declared.contains(handler)) {
                issues.push_back({{"class", c.name}, {"property", p.value("name", "")},
                                  {"handler", handler}, {"issue", "missing_handler_declaration"},
                                  {"path", c.file.generic_string()}});
            }
        }
        for (const std::string& fn : declared) {
            if (fn.starts_with("OnRep_") && !propForHandler.contains(fn)) {
                issues.push_back({{"class", c.name}, {"handler", fn}, {"issue", "handler_without_replicated_using_property"},
                                  {"path", c.file.generic_string()}});
            }
        }
    }
    return Json{{"issues", issues}, {"count", issues.size()}};
}

ToolResult PipelinePrReview(const Json& params) {
    const fs::path root = rootArg(params);
    Json out{{"root", root.generic_string()}};
    out["status"] = gitOutput(root, "status --short").value_or("");
    out["churn"] = gitChurn(root, intArg(params, "days", 30, 1, 365), 25);
    out["conditional_gates"] = *RiskConditionalGates(params);
    out["decisions"] = listDecisionRecords(params)["count"];
    return out;
}

ToolResult PipelineReleaseReadiness(const Json& params) {
    const fs::path root = rootArg(params);
    Json out{{"root", root.generic_string()}};
    const std::string status = gitOutput(root, "status --short").value_or("");
    out["dirty"] = !trim(status).empty();
    out["status"] = status;
    out["hotspots"] = gitChurn(root, intArg(params, "days", 30, 1, 365), 25)["files"];
    out["stale_decisions"] = (*DecisionListStale(params))["decisions"];
    out["conditional_gates"] = *RiskConditionalGates(params);
    return out;
}

fs::path configuredEngineRoot() {
    if (g_enginePathOverride && !g_enginePathOverride->empty()) return *g_enginePathOverride;
    return envPath("SAGE_UE_ROOT");
}

fs::path configuredProjectPath() {
    if (g_projectPathOverride && !g_projectPathOverride->empty()) return *g_projectPathOverride;
    fs::path fromEnv = envPath("SAGE_PROJECT_ROOT");
    if (!fromEnv.empty()) return fromEnv;
    return {};
}

Json pathStatusJson(const fs::path& path) {
    std::error_code ec;
    Json out{{"path", path.empty() ? "" : path.generic_string()}};
    out["exists"] = !path.empty() && fs::exists(path, ec);
    out["is_directory"] = !path.empty() && fs::is_directory(path, ec);
    out["is_regular_file"] = !path.empty() && fs::is_regular_file(path, ec);
    return out;
}

bool looksLikeEngineRoot(const fs::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::exists(path / "Engine" / "Build", ec)
        || fs::exists(path / "Build" / "BatchFiles", ec);
}

bool looksLikeProjectPath(const fs::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (endsWith(lower(path.extension().string()), ".uproject")) return fs::exists(path, ec);
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) return false;
    for (const fs::directory_entry& entry : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec) && endsWith(lower(entry.path().extension().string()), ".uproject")) return true;
    }
    return false;
}

ToolResult SetUnrealEnginePath(const Json& params) {
    const std::string path = stringArg(params, {"path", "engine_path", "engine_root"}, "").value_or("");
    if (path.empty()) return invalid("missing path/engine_path");
    g_enginePathOverride = weakAbs(path);
    Json out = pathStatusJson(*g_enginePathOverride);
    out["configured"] = true;
    out["source"] = "process_override";
    out["looks_like_unreal_engine"] = looksLikeEngineRoot(*g_enginePathOverride);
    return out;
}

ToolResult GetUnrealEnginePath(const Json&) {
    fs::path path = configuredEngineRoot();
    Json out = pathStatusJson(path);
    out["source"] = g_enginePathOverride ? "process_override" : "SAGE_UE_ROOT";
    out["looks_like_unreal_engine"] = looksLikeEngineRoot(path);
    return out;
}

ToolResult SetUnrealProjectPath(const Json& params) {
    const std::string path = stringArg(params, {"path", "project_path", "uproject"}, "").value_or("");
    if (path.empty()) return invalid("missing path/project_path");
    g_projectPathOverride = weakAbs(path);
    Json out = pathStatusJson(*g_projectPathOverride);
    out["configured"] = true;
    out["source"] = "process_override";
    out["looks_like_unreal_project"] = looksLikeProjectPath(*g_projectPathOverride);
    return out;
}

ToolResult GetUnrealProjectPath(const Json&) {
    fs::path path = configuredProjectPath();
    Json out = pathStatusJson(path);
    out["source"] = g_projectPathOverride ? "process_override" : "SAGE_PROJECT_ROOT";
    out["looks_like_unreal_project"] = looksLikeProjectPath(path);
    return out;
}

ToolResult UnifiedStatus(const Json& params) {
    const fs::path root = rootArg(params);
    const fs::path engine = configuredEngineRoot();
    const fs::path project = configuredProjectPath();

    Json out;
    out["repo_root"] = pathStatusJson(root);
    out["engine"] = pathStatusJson(engine);
    out["engine"]["looks_like_unreal_engine"] = looksLikeEngineRoot(engine);
    out["project"] = pathStatusJson(project);
    out["project"]["looks_like_unreal_project"] = looksLikeProjectPath(project);
    out["env"] = {
        {"SAGE_REPO_ROOT", sage::util::envValue("SAGE_REPO_ROOT")},
        {"SAGE_UE_ROOT", sage::util::envValue("SAGE_UE_ROOT")},
        {"SAGE_PROJECT_ROOT", sage::util::envValue("SAGE_PROJECT_ROOT")},
        {"SAGE_HTTP_PORT", sage::util::envValue("SAGE_HTTP_PORT")},
        {"SAGE_WS_PORT", sage::util::envValue("SAGE_WS_PORT")}
    };

    Json git;
    auto statusText = gitOutput(root, "status --short");
    git["available"] = statusText.has_value();
    git["dirty"] = statusText.has_value() && !trim(*statusText).empty();
    git["status_short"] = statusText.value_or("");
    auto branchText = gitOutput(root, "rev-parse --abbrev-ref HEAD");
    git["branch"] = branchText ? trim(*branchText) : "";
    auto headText = gitOutput(root, "rev-parse --short HEAD");
    git["head"] = headText ? trim(*headText) : "";
    out["git"] = git;

    out["server"] = {
        {"cwd", weakAbs(fs::current_path()).generic_string()},
        {"data_dir", sage::util::envValue("SAGE_DATA_DIR")},
        {"log_level", sage::util::envValue("SAGE_LOG_LEVEL")}
    };
    out["ok"] = out["repo_root"].value("exists", false);
    return out;
}

void regLocal(sage::mcp::ToolRegistry& registry,
              std::string name,
              std::string description,
              Json schema,
              sage::mcp::ToolHandler handler) {
    Tool tool{.name = std::move(name),
              .description = std::move(description),
              .inputSchema = std::move(schema),
              .handler = std::move(handler),
              .remote = false};
    const std::string toolName = tool.name;
    if (auto r = registry.registerTool(std::move(tool)); !r.has_value()) {
        spdlog::warn("source-intelligence: failed to register '{}'", toolName);
    }
}

Json commonProps() {
    return {
        {"query", str()},
        {"symbol", str()},
        {"name", str()},
        {"fqn", str()},
        {"class", str()},
        {"class_name", str()},
        {"function", str()},
        {"interface", str()},
        {"specifier", str()},
        {"path", str()},
        {"file", str()},
        {"id", str()},
        {"decision", str()},
        {"status", str()},
        {"root", str()},
        {"project_root", str()},
        {"repo_root", str()},
        {"source_root", str()},
        {"engine_path", str()},
        {"engine_root", str()},
        {"project_path", str()},
        {"uproject", str()},
        {"category", str()},
        {"source_roots", strArray()},
        {"docs_roots", strArray()},
        {"include_engine", bln()},
        {"max_results", i32()},
        {"max_files", i32()},
        {"days", i32()},
        {"older_than_days", i32()},
    };
}

}  // namespace

namespace sage::tools {

void registerSourceIntelligenceTools(mcp::ToolRegistry& registry) {
    const Json props = commonProps();
    auto schema = [&](std::initializer_list<const char*> required = {}) {
        return obj(props, required);
    };

    regLocal(registry, "search_unreal_api",
             "Search Unreal/project C++ API source by symbol or text. Scans SAGE_UE_ROOT Engine/Source when available plus project roots.",
             schema({"query"}), SearchUnrealApi);
    regLocal(registry, "get_by_fqn",
             "Resolve a fully-qualified Unreal/C++ symbol to class metadata or source matches.",
             schema({"fqn"}), GetByFqn);
    regLocal(registry, "get_class_members",
             "Parse a UCLASS header and return UPROPERTY/UFUNCTION members.",
             schema({"class"}), GetClassMembers);
    regLocal(registry, "get_class_reference",
             "Return UCLASS declaration reference, members, inheritance, and include path.",
             schema({"class"}), GetClassReference);
    regLocal(registry, "get_function_signature",
             "Find C++ function signatures by name across source roots.",
             schema({"function"}), GetFunctionSignature);
    regLocal(registry, "get_include_path",
             "Infer the include path for a UCLASS/header symbol.",
             schema({"symbol"}), GetIncludePath);
    regLocal(registry, "search_deprecated",
             "Search source for UE_DEPRECATED/deprecated API declarations and optional symbol filters.",
             schema(), SearchDeprecated);
    regLocal(registry, "get_deprecation_warnings",
             "Return deprecation-related source lines for a symbol.",
             schema({"symbol"}), GetDeprecationWarnings);
    regLocal(registry, "lookup_docs",
             "Search project, Sage, and engine documentation roots for a query.",
             schema({"query"}), LookupDocs);
    regLocal(registry, "lookup_class",
             "Lookup a class in source plus nearby docs.",
             schema({"class"}), LookupClass);
    regLocal(registry, "find_callers",
             "Find source call sites/usages for a symbol.",
             schema({"symbol"}), FindCallers);
    regLocal(registry, "find_callees",
             "Extract called identifiers from a source file or from files matching a function name.",
             schema(), FindCallees);
    regLocal(registry, "source.audit_module_dep_reality",
             "Compare Build.cs declared dependencies with observed include-root evidence.",
             schema(), AuditModuleDepReality);

    regLocal(registry, "decision.list_decisions",
             "List ADR/decision documents under repo decision directories.",
             schema(), DecisionList);
    regLocal(registry, "decision.get_decision",
             "Read a decision document by id, path, or title fragment.",
             schema({"id"}), DecisionGet);
    regLocal(registry, "decision.list_stale",
             "List stale, deprecated, or superseded decision documents.",
             schema(), DecisionListStale);
    regLocal(registry, "decision.find_supersession_chain",
             "Find supersedes/replaces references around a decision query.",
             schema({"id"}), DecisionSupersessionChain);
    regLocal(registry, "decision.find_referent_decisions",
             "Find decisions that mention a query, file, or decision id.",
             schema({"query"}), DecisionReferents);

    regLocal(registry, "risk.get_hotspot_score",
             "Score git file hotspots from recent churn.",
             schema(), RiskHotspotScore);
    regLocal(registry, "risk.get_cochange_pairs",
             "Find file pairs that changed in the same recent commits.",
             schema(), RiskCochangePairs);
    regLocal(registry, "risk.get_file_churn",
             "Count git file churn in a time window.",
             schema(), RiskFileChurn);
    regLocal(registry, "risk.get_release_window_hotspots",
             "List recent churn hotspots for release-readiness triage.",
             schema(), RiskReleaseWindowHotspots);
    regLocal(registry, "risk.list_conditional_gates",
             "Find confirmed/dry_run style safety gates in source roots.",
             schema(), RiskConditionalGates);

    regLocal(registry, "cppreflect.get_uclass",
             "Parse and return one UCLASS declaration and reflected members.",
             schema({"class"}), CppReflectGetUClass);
    regLocal(registry, "cppreflect.list_uproperties",
             "List UPROPERTY declarations for a parsed UCLASS.",
             schema({"class"}), CppReflectListUProperties);
    regLocal(registry, "cppreflect.list_ufunctions",
             "List UFUNCTION declarations for a parsed UCLASS.",
             schema({"class"}), CppReflectListUFunctions);
    regLocal(registry, "cppreflect.find_interface_impls",
             "Find parsed UCLASS declarations that inherit or mention an interface.",
             schema({"interface"}), CppReflectFindInterfaceImpls);
    regLocal(registry, "cppreflect.find_class_specifier",
             "Find parsed UCLASS declarations with a specifier substring.",
             schema({"specifier"}), CppReflectFindClassSpecifier);
    regLocal(registry, "cppreflect.list_class_specifiers",
             "List UCLASS specifier usage counts across source roots.",
             schema(), CppReflectListClassSpecifiers);
    regLocal(registry, "reflect.rebuild_reflection_index",
             "Rescan source roots and return the parsed C++ reflection index snapshot.",
             schema(), ReflectRebuildReflectionIndex);

    regLocal(registry, "network.list_replicated_classes",
             "List UCLASS declarations with replicated properties or RPC functions.",
             schema(), NetworkListReplicatedClasses);
    regLocal(registry, "network.list_rpc_functions",
             "List UFUNCTION Server/Client/NetMulticast RPC declarations.",
             schema(), NetworkListRpcFunctions);
    regLocal(registry, "network.list_onrep_handlers",
             "List UPROPERTY ReplicatedUsing handlers.",
             schema(), NetworkListOnRepHandlers);
    regLocal(registry, "network.audit_unbalanced_onreps",
             "Find ReplicatedUsing properties without handlers and orphan OnRep_ handlers.",
             schema(), NetworkAuditUnbalancedOnReps);

    regLocal(registry, "pipeline.pr_review",
             "Compose git status, churn, safety gates, and decision counts for PR review triage.",
             schema(), PipelinePrReview);
    regLocal(registry, "pipeline.release_readiness",
             "Compose dirty-state, churn hotspots, stale decisions, and safety gates for release readiness.",
             schema(), PipelineReleaseReadiness);

    regLocal(registry, "set_unreal_engine_path",
             "Set the process-local Unreal Engine root override used by Sage server-side path/status helpers.",
             schema({"path"}), SetUnrealEnginePath);
    regLocal(registry, "get_unreal_engine_path",
             "Return the process-local or SAGE_UE_ROOT Unreal Engine path plus validation flags.",
             schema(), GetUnrealEnginePath);
    regLocal(registry, "set_unreal_project_path",
             "Set the process-local Unreal project path override used by Sage server-side path/status helpers.",
             schema({"path"}), SetUnrealProjectPath);
    regLocal(registry, "get_unreal_project_path",
             "Return the process-local or SAGE_PROJECT_ROOT Unreal project path plus validation flags.",
             schema(), GetUnrealProjectPath);
    regLocal(registry, "status",
             "Return unified Sage server health, repo/git state, configured engine/project paths, and relevant environment values.",
             schema(), UnifiedStatus);
}

}  // namespace sage::tools
