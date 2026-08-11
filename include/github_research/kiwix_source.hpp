#pragma once

#include "github_research/datasource.hpp"
#include "github_research/http_client.hpp"

namespace github_research {

// =============================================================
// KiwixSource (priority 1, offline-first)
//
// Local Kiwix HTTP service serving ZIM encyclopedia articles.
// canonical_uri scheme: kiwix://{zim_id}/{article_path}
// Example: kiwix://wikipedia/en/Cpp_(programming_language)
//
// Environment variable: KIWIX_SERVER_URL=http://127.0.0.1:8080
// =============================================================
class KiwixSource : public IDataSource {
public:
    explicit KiwixSource(std::string base_url, IHttpClient* http_client = nullptr);

    std::string sourceId() const override { return "kiwix_local"; }
    int priority() const override { return 1; }
    bool isOnline() const override { return false; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;
    std::vector<std::string> expand(const std::string& root_uri,
                                     const std::string& sub_path,
                                     int max_depth) override;

private:
    std::string base_url_;
    IHttpClient* http_client_;
    bool health_checked_ = false;
    bool healthy_ = false;

    // Helper: simple HTML to text/markdown conversion
    static std::string htmlToMarkdown(const std::string& html);
    // Helper: extract article links from HTML
    static std::vector<std::string> extractLinks(const std::string& html, const std::string& base_url);
};

} // namespace github_research
