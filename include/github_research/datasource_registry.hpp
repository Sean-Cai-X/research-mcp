#pragma once

#include "github_research/datasource.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace github_research {

// =============================================================
// DataSourceRegistry: 9-source registration + priority-based dispatch
//
// Core principle: priority 1 (kiwix_local) hit terminates search;
// other sources fall through in priority order, supporting partial success.
// =============================================================
class DataSourceRegistry {
public:
    // Register a source (takes ownership)
    void register_source(std::unique_ptr<IDataSource> source);

    // Get a source by source_id (non-owning)
    IDataSource* get(const std::string& source_id) const;

    // Get all registered sources sorted by priority (1=highest)
    std::vector<IDataSource*> getAll() const;

    // Get available sources (healthCheck passed), sorted by priority
    // Caches health check results for health_cache_ttl_ms
    std::vector<IDataSource*> getAvailableSorted();

    // Multi-source search: query multiple sources in priority order
    // stopOnFirstHit=true: Kiwix (priority 1) hit terminates search
    // maxSources=0: use all available sources
    struct MultiSearchOptions {
        int max_sources = 0;        // 0 = all available
        bool stop_on_first_hit = true;
    };
    std::vector<SearchResult> multiSearch(const SearchQuery& query,
                                           const MultiSearchOptions& options = {});

    // Fetch with priority-based fallback
    // 1. Parse canonical_uri scheme to find the owning source
    // 2. Try that source first
    // 3. If fails, try remaining sources in priority order
    // Returns nullopt if all sources fail
    std::optional<FetchResult> fetchWithFallback(const std::string& canonical_uri);

    // Check if any source is available
    bool hasAvailable() const;

    // Get source count
    size_t size() const { return sources_.size(); }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<IDataSource>> sources_;

    // Health check cache: source_id -> (result, timestamp_ms)
    mutable std::map<std::string, std::pair<bool, int64_t>> health_cache_;
    static constexpr int64_t HEALTH_CACHE_TTL_MS = 30000; // 30s
};

} // namespace github_research
