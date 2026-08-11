#pragma once

#include "github_research/datasource.hpp"
#include "github_research/webview_session.hpp"

namespace github_research {

// =============================================================
// ArxivSource (priority 6, online)
//
// arXiv paper search and full text retrieval.
// canonical_uri scheme: arxiv://{arxiv_id}
// Example: arxiv://2401.12345
// =============================================================
class ArxivSource : public IDataSource {
public:
    explicit ArxivSource(WebViewSession* session = nullptr);

    std::string sourceId() const override { return "arxiv"; }
    int priority() const override { return 6; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;

private:
    WebViewSession* session_;
};

} // namespace github_research
