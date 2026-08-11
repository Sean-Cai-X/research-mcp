#include "github_research/local_fs_source.hpp"
#include "github_research/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace github_research {

namespace {

const std::string FILE_SCHEME_PREFIX = "file://";

bool has_document_extension(const std::filesystem::path& p) {
    const std::string ext = to_lower(p.extension().string());
    return ext == ".md" || ext == ".txt" || ext == ".rst";
}

// Match query keywords against filename (case-insensitive, any-token match).
bool filename_matches_query(const std::string& filename_lower,
                            const std::string& query_lower) {
    if (query_lower.empty()) return true;
    std::istringstream iss(query_lower);
    std::string token;
    while (iss >> token) {
        if (token.empty()) continue;
        if (filename_lower.find(token) != std::string::npos) return true;
    }
    return false;
}

std::string build_file_uri(const std::filesystem::path& abs_path) {
    return FILE_SCHEME_PREFIX + abs_path.generic_string();
}

// Strip "file://" prefix. Also tolerates RFC 8089 form "file:///D:/..." by
// removing a leading slash that precedes a Windows drive letter.
std::string strip_file_scheme(const std::string& canonical_uri) {
    if (canonical_uri.size() < FILE_SCHEME_PREFIX.size()) return "";
    if (canonical_uri.compare(0, FILE_SCHEME_PREFIX.size(), FILE_SCHEME_PREFIX) != 0) {
        return "";
    }
    std::string path = canonical_uri.substr(FILE_SCHEME_PREFIX.size());
    if (path.size() >= 3 && path[0] == '/' &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) &&
        path[2] == ':') {
        path.erase(0, 1);
    }
    return path;
}

} // namespace

// =============================================================
// LocalFsSource::scanDirectory (private)
// Recursively scan a directory for .md/.txt/.rst files whose filename
// contains any of the query keywords (case-insensitive).
// =============================================================
std::vector<SearchResult> LocalFsSource::scanDirectory(const std::filesystem::path& dir,
                                                       const std::string& query,
                                                       int max_results) {
    std::vector<SearchResult> results;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return results;
    }

    const std::string query_lower = to_lower(query);

    try {
        std::filesystem::recursive_directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied);
        std::filesystem::recursive_directory_iterator end_it;

        for (; it != end_it; ++it) {
            if (max_results > 0 && static_cast<int>(results.size()) >= max_results) {
                break;
            }
            const std::filesystem::directory_entry& entry = *it;

            std::error_code rec_ec;
            if (!entry.is_regular_file(rec_ec)) continue;
            if (!has_document_extension(entry.path())) continue;

            const std::string filename = entry.path().filename().string();
            const std::string filename_lower = to_lower(filename);
            if (!filename_matches_query(filename_lower, query_lower)) continue;

            std::error_code abs_ec;
            std::filesystem::path abs_path = std::filesystem::absolute(entry.path(), abs_ec);
            if (abs_ec) abs_path = entry.path();

            SearchResult r;
            r.canonical_uri = build_file_uri(abs_path);
            r.title = filename;
            r.resource_kind = ResourceKind::LOCAL_FILE;
            r.source_id = "local_fs";

            std::error_code rel_ec;
            std::filesystem::path rel = std::filesystem::relative(entry.path(), dir, rel_ec);
            r.snippet = rel_ec ? entry.path().string() : rel.generic_string();

            results.push_back(std::move(r));
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Best-effort scan: return whatever has been collected so far.
    }
    return results;
}

// =============================================================
// LocalFsSource::search
// If query.repo is set, treat it as the directory to scan; otherwise
// default to ./data/local_docs.
// =============================================================
std::vector<SearchResult> LocalFsSource::search(const SearchQuery& query) {
    std::filesystem::path root_dir;
    if (!query.repo.empty()) {
        root_dir = std::filesystem::path(query.repo);
    } else {
        root_dir = std::filesystem::path("./data/local_docs");
    }
    return scanDirectory(root_dir, query.query, query.max_results);
}

// =============================================================
// LocalFsSource::fetch
// Parse "file://{path}" URI and stream the file content.
// =============================================================
std::optional<FetchResult> LocalFsSource::fetch(const std::string& canonical_uri) {
    std::string path_str = strip_file_scheme(canonical_uri);
    if (path_str.empty()) return std::nullopt;

    std::ifstream ifs(path_str, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;

    std::ostringstream oss;
    oss << ifs.rdbuf();

    FetchResult fr;
    fr.canonical_uri = canonical_uri;
    std::filesystem::path p(path_str);
    fr.title = p.filename().string();
    if (fr.title.empty()) fr.title = path_str;
    fr.content_markdown = oss.str();
    fr.resource_kind = ResourceKind::LOCAL_FILE;
    fr.source_id = "local_fs";
    return fr;
}

// =============================================================
// LocalFsSource::expand
// List immediate subdirectories and .md files under {root_uri}/{sub_path}.
// Returns a canonical_uri (file://...) for each.
// =============================================================
std::vector<std::string> LocalFsSource::expand(const std::string& root_uri,
                                               const std::string& sub_path,
                                               int max_depth) {
    (void)max_depth; // single-level listing by design

    std::vector<std::string> uris;
    std::string root_path_str = strip_file_scheme(root_uri);
    if (root_path_str.empty()) return uris;

    std::filesystem::path root(root_path_str);
    if (!sub_path.empty()) {
        root /= sub_path;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return uris;

    try {
        for (auto it = std::filesystem::directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied);
             it != std::filesystem::directory_iterator(); ++it) {
            const std::filesystem::directory_entry& entry = *it;
            const std::filesystem::path& p = entry.path();

            std::error_code item_ec;
            if (entry.is_directory(item_ec)) {
                std::error_code abs_ec;
                std::filesystem::path abs_path = std::filesystem::absolute(p, abs_ec);
                if (abs_ec) abs_path = p;
                uris.push_back(build_file_uri(abs_path));
            } else if (entry.is_regular_file(item_ec)) {
                if (to_lower(p.extension().string()) == ".md") {
                    std::error_code abs_ec;
                    std::filesystem::path abs_path = std::filesystem::absolute(p, abs_ec);
                    if (abs_ec) abs_path = p;
                    uris.push_back(build_file_uri(abs_path));
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Best-effort: return whatever has been collected.
    }
    return uris;
}

} // namespace github_research
