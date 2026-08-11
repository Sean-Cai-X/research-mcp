#pragma once

#include "github_research/datasource.hpp"
#include "github_research/http_client.hpp"

namespace github_research {

// =============================================================
// GitRawSource (priority 2, online)
//
// Fetch raw files from GitHub/GitLab repositories.
// canonical_uri scheme: github-raw://{owner}/{repo}/{branch}/{path}
// Example: github-raw://octocat/Hello-World/main/README.md
// =============================================================
class GitRawSource : public IDataSource {
public:
    explicit GitRawSource(IHttpClient* http_client = nullptr);

    std::string sourceId() const override { return "git_raw"; }
    int priority() const override { return 2; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;

private:
    IHttpClient* http_client_;
    static constexpr const char* RAW_BASE = "https://raw.githubusercontent.com";
};

} // namespace github_research
