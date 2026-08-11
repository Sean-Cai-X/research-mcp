#pragma once

#include "github_research/datasource_registry.hpp"
#include "github_research/cache_manager.hpp"
#include <nlohmann/json.hpp>

namespace github_research {

// =============================================================
// WikiExplorer: Wiki workflow orchestration layer
//
// Provides 3 tools:
//   wiki_discover: lightweight probe (local Kiwix first, then online)
//   wiki_read:     precise single-doc fetch (cache + priority fallback)
//   wiki_scan:     sub-path expand scan (batch read with preview only)
//
// Does NOT do direct IO — delegates to DataSourceRegistry.
// =============================================================
class WikiExplorer {
public:
    WikiExplorer(DataSourceRegistry& registry, CacheManager& cache);

    // Tool 1: discover — lightweight resource probe
    // Input: { repo?, query?, repo_branch? }
    json discover(const json& args);

    // Tool 2: read — precise single-doc fetch with cache
    // Input: { target_uri, force_refresh? }
    json read(const json& args);

    // Tool 3: scan — sub-path expand scan
    // Input: { root_canonical_uri, sub_path, max_depth?, max_pages?, force_refresh? }
    json scan(const json& args);

private:
    DataSourceRegistry& registry_;
    CacheManager& cache_;

    // Cache helpers (source_type="wiki", cache_key=canonical_uri)
    static constexpr const char* CACHE_SOURCE_TYPE = "wiki";

    struct CachedFetch {
        FetchResult result;
        bool from_cache = false;
        bool is_stale = false;
    };

    // Try cache first, then fetch from source
    std::optional<CachedFetch> fetchWithCache(const std::string& canonical_uri,
                                                bool force_refresh);

    // Parse canonical_uri to determine which source owns it
    IDataSource* findSourceForUri(const std::string& canonical_uri);

    // Check if a source is local (kiwix, local_fs, git_clone)
    static bool isLocalSource(const std::string& source_id);
};

} // namespace github_research
