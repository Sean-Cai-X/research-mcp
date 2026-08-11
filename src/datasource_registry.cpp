#include "github_research/datasource_registry.hpp"
#include <algorithm>
#include <chrono>

namespace github_research {

void DataSourceRegistry::register_source(std::unique_ptr<IDataSource> source) {
    if (!source) return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = source->sourceId();
    sources_[id] = std::move(source);
}

IDataSource* DataSourceRegistry::get(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(source_id);
    return it != sources_.end() ? it->second.get() : nullptr;
}

std::vector<IDataSource*> DataSourceRegistry::getAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IDataSource*> result;
    result.reserve(sources_.size());
    for (auto& [id, src] : sources_) {
        result.push_back(src.get());
    }
    // Sort by priority (1=highest)
    std::sort(result.begin(), result.end(),
              [](IDataSource* a, IDataSource* b) { return a->priority() < b->priority(); });
    return result;
}

std::vector<IDataSource*> DataSourceRegistry::getAvailableSorted() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<IDataSource*> result;
    for (auto& [id, src] : sources_) {
        // Check health cache
        auto cache_it = health_cache_.find(id);
        bool healthy = false;
        if (cache_it != health_cache_.end()) {
            auto& [cached_result, cached_time] = cache_it->second;
            if (now_ms - cached_time < HEALTH_CACHE_TTL_MS) {
                healthy = cached_result;
            } else {
                // Cache expired, re-check
                healthy = src->healthCheck();
                health_cache_[id] = {healthy, now_ms};
            }
        } else {
            healthy = src->healthCheck();
            health_cache_[id] = {healthy, now_ms};
        }
        if (healthy) {
            result.push_back(src.get());
        }
    }
    std::sort(result.begin(), result.end(),
              [](IDataSource* a, IDataSource* b) { return a->priority() < b->priority(); });
    return result;
}

std::vector<SearchResult> DataSourceRegistry::multiSearch(
    const SearchQuery& query,
    const MultiSearchOptions& options) {

    std::vector<SearchResult> all_results;
    auto available = getAvailableSorted();

    int sources_used = 0;
    int max = options.max_sources > 0 ? options.max_sources : (int)available.size();

    for (auto* src : available) {
        if (sources_used >= max) break;
        ++sources_used;

        try {
            auto results = src->search(query);
            if (!results.empty()) {
                for (auto& r : results) {
                    r.source_id = src->sourceId();
                    all_results.push_back(std::move(r));
                }
                // Kiwix (priority 1) hit terminates search
                if (options.stop_on_first_hit && src->priority() == 1) {
                    break;
                }
            }
        } catch (...) {
            // Source failed silently, continue to next
        }
    }

    return all_results;
}

std::optional<FetchResult> DataSourceRegistry::fetchWithFallback(
    const std::string& canonical_uri) {

    // Parse scheme to find owning source
    std::string scheme = uri::scheme(canonical_uri);

    // Map scheme to source_id
    static const std::map<std::string, std::string> scheme_to_source = {
        {"kiwix",       "kiwix_local"},
        {"github-raw",  "git_raw"},
        {"github-wiki", "github_wiki"},
        {"web",         "web_crawler"},
        {"github-api",  "github_api"},
        {"arxiv",       "arxiv"},
        {"search",      "web_search"},
        {"file",        "local_fs"},
        {"git-clone",   "git_clone"},
    };

    auto available = getAvailableSorted();

    // Try the owning source first
    auto scheme_it = scheme_to_source.find(scheme);
    if (scheme_it != scheme_to_source.end()) {
        auto* owning = get(scheme_it->second);
        if (owning) {
            try {
                auto result = owning->fetch(canonical_uri);
                if (result) return result;
            } catch (...) {}
        }
    }

    // Fallback: try remaining sources in priority order
    for (auto* src : available) {
        if (scheme_it != scheme_to_source.end() && src->sourceId() == scheme_it->second) {
            continue; // Already tried
        }
        try {
            auto result = src->fetch(canonical_uri);
            if (result) return result;
        } catch (...) {}
    }

    return std::nullopt;
}

bool DataSourceRegistry::hasAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !sources_.empty();
}

} // namespace github_research
