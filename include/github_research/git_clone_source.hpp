#pragma once

#include "github_research/datasource.hpp"

#include <filesystem>
#include <string>

namespace github_research {

// =============================================================
// GitCloneSource (priority 9, local/online)
//
// Full repository clone + local file index.
// Heavy operation: triggered on demand, not by default search.
// canonical_uri scheme: git-clone://{owner}/{repo}/{path}
// Example: git-clone://octocat/Hello-World/README.md
// =============================================================
class GitCloneSource : public IDataSource {
public:
    explicit GitCloneSource(std::string clone_root = "./data/git_clones");

    std::string sourceId() const override { return "git_clone"; }
    int priority() const override { return 9; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;
    std::vector<std::string> expand(const std::string& root_uri,
                                     const std::string& sub_path,
                                     int max_depth) override;

private:
    std::string clone_root_;

    // Clone a repo if not already cloned; returns local path
    std::string ensureCloned(const std::string& owner, const std::string& repo);

    // Get local clone path for owner/repo
    std::string clonePath(const std::string& owner, const std::string& repo) const;

    // Execute system git command
    static bool runGitClone(const std::string& url, const std::string& dest);
};

} // namespace github_research
