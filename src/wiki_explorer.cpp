#include "github_research/wiki_explorer.hpp"
#include "github_research/webview_helpers.hpp"
#include <chrono>
#include <algorithm>

namespace github_research {

WikiExplorer::WikiExplorer(DataSourceRegistry& registry, CacheManager& cache)
    : registry_(registry), cache_(cache) {}

bool WikiExplorer::isLocalSource(const std::string& source_id) {
    return source_id == "kiwix_local" || source_id == "local_fs" || source_id == "git_clone";
}

IDataSource* WikiExplorer::findSourceForUri(const std::string& canonical_uri) {
    std::string scheme = uri::scheme(canonical_uri);
    static const std::map<std::string, std::string> scheme_map = {
        {"kiwix", "kiwix_local"}, {"github-raw", "git_raw"},
        {"github-wiki", "github_wiki"}, {"web", "web_crawler"},
        {"github-api", "github_api"}, {"arxiv", "arxiv"},
        {"search", "web_search"}, {"file", "local_fs"},
        {"git-clone", "git_clone"},
    };
    auto it = scheme_map.find(scheme);
    if (it == scheme_map.end()) return nullptr;
    return registry_.get(it->second);
}

std::optional<WikiExplorer::CachedFetch>
WikiExplorer::fetchWithCache(const std::string& canonical_uri, bool force_refresh) {
    // 1. Check cache (unless force_refresh)
    if (!force_refresh) {
        auto cached = cache_.get(CACHE_SOURCE_TYPE, canonical_uri);
        if (cached.has_value()) {
            bool fresh = cache_.is_fresh(CACHE_SOURCE_TYPE, canonical_uri);
            try {
                auto payload = json::parse(cached->payload);
                FetchResult fr;
                fr.canonical_uri = payload.value("canonical_uri", canonical_uri);
                fr.title = payload.value("title", "");
                fr.content_markdown = payload.value("content_markdown", "");
                fr.resource_kind = payload.value("resource_kind", "");
                fr.source_id = payload.value("source_id", "");
                return CachedFetch{fr, true, !fresh};
            } catch (...) {}
        }
    }

    // 2. Fetch from registry (with fallback)
    auto result = registry_.fetchWithFallback(canonical_uri);
    if (!result) return std::nullopt;

    // 3. Store in cache
    json payload = {
        {"canonical_uri", result->canonical_uri},
        {"title", result->title},
        {"content_markdown", result->content_markdown},
        {"resource_kind", result->resource_kind},
        {"source_id", result->source_id}
    };
    // TTL by resource kind
    int ttl = 24; // default 24h
    if (result->resource_kind == ResourceKind::KIWIX_ARTICLE) ttl = 720; // 30d
    else if (result->resource_kind == ResourceKind::ARXIV_PAPER) ttl = 720;
    else if (result->resource_kind == ResourceKind::GIT_MARKDOWN) ttl = 336; // 14d
    else if (result->resource_kind == ResourceKind::WIKI_DOC) ttl = 336;
    else if (result->resource_kind == ResourceKind::WEB_PAGE) ttl = 168; // 7d
    else if (result->resource_kind == ResourceKind::SEARCH_RESULT) ttl = 24; // 1d

    cache_.put(CACHE_SOURCE_TYPE, canonical_uri, payload.dump(), "json", ttl, "", "ok", "");

    return CachedFetch{result.value(), false, false};
}

// =============================================================
// Tool 1: wiki_discover
// =============================================================
json WikiExplorer::discover(const json& args) {
    std::string repo = args.value("repo", "");
    std::string query = args.value("query", "");
    std::string branch = args.value("repo_branch", "main");

    json result;
    result["repo"] = repo;
    result["query"] = query;
    result["resources"] = json::array();
    result["warnings"] = json::array();

    if (query.empty() && repo.empty()) {
        result["warnings"].push_back("At least one of 'query' or 'repo' is required");
        return result;
    }

    // 1. If query provided: multi-source search (Kiwix priority)
    if (!query.empty()) {
        SearchQuery sq;
        sq.query = query;
        sq.max_results = 20;
        auto search_results = registry_.multiSearch(sq, {0, true});

        for (auto& sr : search_results) {
            result["resources"].push_back({
                {"canonical_uri", sr.canonical_uri},
                {"title", sr.title},
                {"resource_kind", sr.resource_kind},
                {"source_id", sr.source_id},
                {"is_local", isLocalSource(sr.source_id)},
                {"snippet", sr.snippet}
            });
        }
    }

    // 2. If repo provided: probe via github_api + git_raw
    if (!repo.empty()) {
        // Try git_raw: list docs/*.md, README.md
        auto* git_raw = registry_.get("git_raw");
        if (git_raw && git_raw->healthCheck()) {
            SearchQuery sq;
            sq.query = "README";
            sq.repo = repo;
            sq.repo_branch = branch;
            auto results = git_raw->search(sq);
            for (auto& sr : results) {
                result["resources"].push_back({
                    {"canonical_uri", sr.canonical_uri},
                    {"title", sr.title},
                    {"resource_kind", sr.resource_kind},
                    {"source_id", sr.source_id},
                    {"is_local", false},
                    {"snippet", sr.snippet}
                });
            }
        }

        // Try github_wiki
        auto* wiki = registry_.get("github_wiki");
        if (wiki && wiki->healthCheck()) {
            SearchQuery sq;
            sq.query = repo;
            sq.repo = repo;
            auto results = wiki->search(sq);
            for (auto& sr : results) {
                result["resources"].push_back({
                    {"canonical_uri", sr.canonical_uri},
                    {"title", sr.title},
                    {"resource_kind", sr.resource_kind},
                    {"source_id", sr.source_id},
                    {"is_local", false},
                    {"snippet", sr.snippet}
                });
            }
        }
    }

    // Sort: is_local=true first
    auto& resources = result["resources"];
    if (resources.is_array()) {
        std::sort(resources.begin(), resources.end(), [](const json& a, const json& b) {
            bool a_local = a.value("is_local", false);
            bool b_local = b.value("is_local", false);
            if (a_local != b_local) return a_local;
            return a.value("title", "") < b.value("title", "");
        });
    }

    return result;
}

// =============================================================
// Tool 2: wiki_read
// =============================================================
json WikiExplorer::read(const json& args) {
    std::string target_uri = args.value("target_uri", "");
    bool force_refresh = args.value("force_refresh", false);

    json result;

    if (target_uri.empty()) {
        return {{"error", "target_uri is required"}, {"canonical_uri", ""}};
    }

    auto cached = fetchWithCache(target_uri, force_refresh);
    if (!cached) {
        // Try stale cache as last resort
        auto stale = cache_.get(CACHE_SOURCE_TYPE, target_uri);
        if (stale.has_value()) {
            try {
                auto payload = json::parse(stale->payload);
                result["canonical_uri"] = payload.value("canonical_uri", target_uri);
                result["title"] = payload.value("title", "");
                result["content_markdown"] = payload.value("content_markdown", "");
                result["meta"] = {
                    {"from_cache", true},
                    {"source_id", payload.value("source_id", "")},
                    {"fetched_at", stale->fetched_at},
                    {"is_stale", true}
                };
                result["warning"] = "All sources failed; returning stale cache";
                return result;
            } catch (...) {}
        }
        return {{"error", "Failed to fetch resource and no cache available"},
                {"canonical_uri", target_uri}};
    }

    result["canonical_uri"] = cached->result.canonical_uri;
    result["title"] = cached->result.title;
    result["content_markdown"] = cached->result.content_markdown;
    result["meta"] = {
        {"from_cache", cached->from_cache},
        {"source_id", cached->result.source_id},
        {"fetched_at", std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
        {"is_stale", cached->is_stale}
    };
    if (cached->is_stale) {
        result["warning"] = "Cache entry is stale; content may be outdated";
    }
    return result;
}

// =============================================================
// Tool 3: wiki_scan
// =============================================================
json WikiExplorer::scan(const json& args) {
    std::string root_uri = args.value("root_canonical_uri", "");
    std::string sub_path = args.value("sub_path", "");
    int max_depth = args.value("max_depth", 2);
    int max_pages = args.value("max_pages", 15);
    bool force_refresh = args.value("force_refresh", false);

    json result;
    result["root_uri"] = root_uri;
    result["sub_path"] = sub_path;
    result["scanned_items"] = json::array();
    result["skipped_count"] = 0;
    result["errors"] = json::array();

    if (root_uri.empty()) {
        result["errors"].push_back({{"uri", root_uri}, {"message", "root_canonical_uri is required"}});
        return result;
    }

    // Find the source for the root URI
    auto* source = findSourceForUri(root_uri);
    if (!source) {
        result["errors"].push_back({{"uri", root_uri}, {"message", "No source found for URI scheme"}});
        return result;
    }

    // Expand from root
    std::vector<std::string> expanded_uris;
    try {
        expanded_uris = source->expand(root_uri, sub_path, max_depth);
    } catch (const std::exception& e) {
        result["errors"].push_back({{"uri", root_uri}, {"message", std::string("expand failed: ") + e.what()}});
        return result;
    }

    // Read each expanded URI (up to max_pages)
    int scanned = 0;
    int skipped = 0;
    for (const auto& uri : expanded_uris) {
        if (scanned >= max_pages) {
            skipped = (int)expanded_uris.size() - scanned;
            break;
        }

        auto cached = fetchWithCache(uri, force_refresh);
        if (!cached) {
            result["errors"].push_back({{"uri", uri}, {"message", "fetch failed"}});
            continue;
        }

        // Only return preview (first 500 chars)
        std::string preview = cached->result.content_markdown;
        if (preview.size() > 500) preview = preview.substr(0, 500) + "...";

        result["scanned_items"].push_back({
            {"canonical_uri", cached->result.canonical_uri},
            {"title", cached->result.title},
            {"resource_kind", cached->result.resource_kind},
            {"source_id", cached->result.source_id},
            {"is_local", isLocalSource(cached->result.source_id)},
            {"content_preview", preview}
        });
        ++scanned;
    }

    result["skipped_count"] = skipped;
    return result;
}

} // namespace github_research
