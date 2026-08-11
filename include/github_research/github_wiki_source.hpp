#pragma once

#include "github_research/datasource.hpp"
#include "github_research/http_client.hpp"

namespace github_research {

// =============================================================
// GithubWikiSource (priority 3, online)
//
// Fetch pages from GitHub repository Wiki.
// canonical_uri scheme: github-wiki://{owner}/{repo}/{page}
// Example: github-wiki://octocat/Hello-World/Home
// =============================================================
class GithubWikiSource : public IDataSource {
public:
    explicit GithubWikiSource(IHttpClient* http_client = nullptr);

    std::string sourceId() const override { return "github_wiki"; }
    int priority() const override { return 3; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;
    std::vector<std::string> expand(const std::string& root_uri,
                                     const std::string& sub_path,
                                     int max_depth) override;

private:
    IHttpClient* http_client_;
    static std::string htmlToMarkdown(const std::string& html);
};

} // namespace github_research
