#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

// =============================================================
// 9-source unified search architecture: core types & interface
// =============================================================

// Resource kind tags for cache metadata
struct ResourceKind {
    static constexpr const char* KIWIX_ARTICLE = "kiwix_article";
    static constexpr const char* GIT_MARKDOWN  = "git_markdown";
    static constexpr const char* WIKI_DOC      = "wiki_doc";
    static constexpr const char* WEB_PAGE     = "web_page";
    static constexpr const char* ARXIV_PAPER   = "arxiv_paper";
    static constexpr const char* GITHUB_META   = "github_meta";
    static constexpr const char* SEARCH_RESULT = "search_result";
    static constexpr const char* LOCAL_FILE    = "local_file";
};

// Search query: lightweight, no full content
struct SearchQuery {
    std::string query;                // keywords / article title
    std::string repo;                  // optional: owner/repo for repo-scoped search
    std::string repo_branch = "main";
    int max_results = 20;
};

// Search result: candidate resource reference
struct SearchResult {
    std::string canonical_uri;        // e.g. kiwix://wikipedia/en/Cpp
    std::string title;
    std::string resource_kind;        // ResourceKind::*
    std::string snippet;
    std::string source_id;            // corresponds to IDataSource::sourceId()
};

// Fetch result: full content of a resource
struct FetchResult {
    std::string canonical_uri;
    std::string title;
    std::string content_markdown;
    std::string resource_kind;
    std::string source_id;
};

// Cache metadata extension (stored alongside payload in cache_entries)
struct ResourceMeta {
    std::string resource_kind;
    std::string source_id;
    int64_t fetched_at = 0;
    int ttl_hours = 24;
    std::string title;
    std::string raw_url;

    json to_json() const {
        return {
            {"resource_kind", resource_kind},
            {"source_id", source_id},
            {"fetched_at", fetched_at},
            {"ttl_hours", ttl_hours},
            {"title", title},
            {"raw_url", raw_url}
        };
    }

    static ResourceMeta from_json(const json& j) {
        ResourceMeta m;
        m.resource_kind = j.value("resource_kind", "");
        m.source_id = j.value("source_id", "");
        m.fetched_at = j.value("fetched_at", (int64_t)0);
        m.ttl_hours = j.value("ttl_hours", 24);
        m.title = j.value("title", "");
        m.raw_url = j.value("raw_url", "");
        return m;
    }
};

// =============================================================
// IDataSource: unified interface for all 9 data sources
// Renamed from DataSource to avoid name clash with
// source_fusion.hpp's struct DataSource (data source metadata).
// =============================================================
class IDataSource {
public:
    virtual ~IDataSource() = default;

    // Identity
    virtual std::string sourceId() const = 0;
    virtual int priority() const = 0;          // 1=highest (kiwix), 9=lowest
    virtual bool isOnline() const = 0;         // true if requires network

    // Health check (e.g. Kiwix server alive, GitHub token valid)
    // Returns false if source should be skipped
    virtual bool healthCheck() = 0;

    // Lightweight search: return candidate resource list (no full content)
    virtual std::vector<SearchResult> search(const SearchQuery& query) = 0;

    // Full fetch: retrieve complete content by canonical_uri
    // Returns nullopt on failure (source will be skipped, fallback to next)
    virtual std::optional<FetchResult> fetch(const std::string& canonical_uri) = 0;

    // Optional: expand from a root resource to discover related pages
    // Default: no expansion (sources that don't support it return empty)
    virtual std::vector<std::string> expand(const std::string& root_uri,
                                             const std::string& sub_path,
                                             int max_depth) {
        (void)root_uri; (void)sub_path; (void)max_depth;
        return {};
    }
};

// =============================================================
// Canonical URI helpers
// =============================================================
namespace uri {

// Extract the scheme prefix (e.g. "kiwix" from "kiwix://wikipedia/en/Cpp")
inline std::string scheme(const std::string& canonical_uri) {
    auto pos = canonical_uri.find("://");
    if (pos == std::string::npos) return "";
    return canonical_uri.substr(0, pos);
}

// Extract the path after the scheme (e.g. "wikipedia/en/Cpp" from "kiwix://wikipedia/en/Cpp")
inline std::string path(const std::string& canonical_uri) {
    auto pos = canonical_uri.find("://");
    if (pos == std::string::npos) return canonical_uri;
    return canonical_uri.substr(pos + 3);
}

// Build a canonical_uri from scheme and path
inline std::string build(const std::string& scheme, const std::string& path) {
    return scheme + "://" + path;
}

// Check if a URI belongs to a given source by scheme prefix
inline bool belongsTo(const std::string& canonical_uri, const std::string& source_scheme) {
    return scheme(canonical_uri) == source_scheme;
}

} // namespace uri

} // namespace github_research
