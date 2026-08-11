#pragma once

#include "github_research/datasource.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace github_research {

// =============================================================
// LocalFsSource (priority 8, local)
//
// Read documentation from local filesystem.
// canonical_uri scheme: file://{absolute_path}
// Example: file:///D:/docs/readme.md
// =============================================================
class LocalFsSource : public IDataSource {
public:
    LocalFsSource() = default;

    std::string sourceId() const override { return "local_fs"; }
    int priority() const override { return 8; }
    bool isOnline() const override { return false; }

    bool healthCheck() override { return true; } // always available

    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;
    std::vector<std::string> expand(const std::string& root_uri,
                                     const std::string& sub_path,
                                     int max_depth) override;

private:
    // Scan directory for .md/.txt files matching query keywords
    static std::vector<SearchResult> scanDirectory(const std::filesystem::path& dir,
                                                     const std::string& query,
                                                     int max_results);
};

} // namespace github_research
