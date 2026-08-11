#pragma once

#include "github_research/datasource.hpp"
#include "github_research/github_client.hpp"

namespace github_research {

// =============================================================
// GithubApiSource (priority 5, online)
//
// GitHub REST API: repo metadata, directory tree, issues, etc.
// canonical_uri scheme: github-api://{owner}/{repo}/{endpoint}
// Example: github-api://octocat/Hello-World/repo_info
// =============================================================
class GithubApiSource : public IDataSource {
public:
    explicit GithubApiSource(GitHubClient* client = nullptr);

    std::string sourceId() const override { return "github_api"; }
    int priority() const override { return 5; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;

private:
    GitHubClient* client_;
};

} // namespace github_research
