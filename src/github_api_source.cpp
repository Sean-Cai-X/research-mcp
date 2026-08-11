#include "github_research/github_api_source.hpp"
#include "github_research/string_utils.hpp"

#include <algorithm>
#include <sstream>

namespace github_research {

GithubApiSource::GithubApiSource(GitHubClient* client)
    : client_(client) {}

bool GithubApiSource::healthCheck() {
    return client_ != nullptr;
}

std::vector<SearchResult> GithubApiSource::search(const SearchQuery& query) {
    std::vector<SearchResult> results;
    if (!client_) return results;

    // Repo-scoped search: query.repo is "owner/repo"
    // List .md files in the repo tree
    if (!query.repo.empty()) {
        std::string owner;
        std::string repo;
        auto slash = query.repo.find('/');
        if (slash == std::string::npos) return results;
        owner = query.repo.substr(0, slash);
        repo  = query.repo.substr(slash + 1);
        if (owner.empty() || repo.empty()) return results;

        json tree_json;
        try {
            tree_json = client_->get_tree_raw(owner, repo, query.repo_branch, true);
        } catch (...) {
            return results;
        }
        if (!tree_json.is_object()) return results;
        if (!tree_json.contains("tree") || !tree_json["tree"].is_array()) return results;

        int count = 0;
        for (const auto& entry : tree_json["tree"]) {
            if (count >= query.max_results) break;
            if (!entry.is_object()) continue;
            std::string path = entry.value("path", "");
            std::string type = entry.value("type", "");
            if (path.empty()) continue;
            if (type != "blob") continue;

            std::string lower_path = path;
            std::transform(lower_path.begin(), lower_path.end(),
                           lower_path.begin(), [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (lower_path.size() < 3 ||
                lower_path.compare(lower_path.size() - 3, 3, ".md") != 0) {
                continue;
            }

            SearchResult r;
            r.canonical_uri = "github-api://" + owner + "/" + repo + "/tree/" + path;
            auto last_sep = path.find_last_of("/\\");
            r.title = (last_sep == std::string::npos) ? path : path.substr(last_sep + 1);
            r.resource_kind = ResourceKind::GITHUB_META;
            r.snippet = path;
            r.source_id = sourceId();
            results.push_back(std::move(r));
            ++count;
        }
        return results;
    }

    // Keyword search: search repositories
    if (query.query.empty()) return results;

    json search_json;
    try {
        search_json = client_->search_repositories(query.query, "stars", "desc", 10, 1);
    } catch (...) {
        return results;
    }
    if (!search_json.is_object()) return results;
    if (!search_json.contains("items") || !search_json["items"].is_array()) return results;

    int count = 0;
    for (const auto& item : search_json["items"]) {
        if (count >= query.max_results) break;
        if (!item.is_object()) continue;
        std::string full_name = item.value("full_name", "");
        if (full_name.empty()) continue;

        SearchResult r;
        r.canonical_uri = "github-api://" + full_name + "/repo_info";
        r.title = item.value("name", full_name);
        r.resource_kind = ResourceKind::GITHUB_META;
        r.snippet = item.value("description", "");
        r.source_id = sourceId();
        results.push_back(std::move(r));
        ++count;
    }
    return results;
}

std::optional<FetchResult> GithubApiSource::fetch(const std::string& canonical_uri) {
    if (!client_) return std::nullopt;

    static const std::string scheme = "github-api://";
    if (canonical_uri.compare(0, scheme.size(), scheme) != 0) {
        return std::nullopt;
    }
    std::string rest = canonical_uri.substr(scheme.size());

    auto slash1 = rest.find('/');
    if (slash1 == std::string::npos) return std::nullopt;
    std::string owner = rest.substr(0, slash1);

    auto slash2 = rest.find('/', slash1 + 1);
    if (slash2 == std::string::npos) return std::nullopt;
    std::string repo = rest.substr(slash1 + 1, slash2 - slash1 - 1);

    std::string endpoint = rest.substr(slash2 + 1);
    if (owner.empty() || repo.empty() || endpoint.empty()) {
        return std::nullopt;
    }

    FetchResult fr;
    fr.canonical_uri = canonical_uri;
    fr.source_id = sourceId();
    fr.resource_kind = ResourceKind::GITHUB_META;

    try {
        if (endpoint == "repo_info") {
            json info = client_->get_repo_info(owner, repo);
            fr.title = info.value("full_name", owner + "/" + repo);
            fr.content_markdown = info.dump(2);
            return fr;
        } else if (endpoint == "readme") {
            std::string readme = client_->get_readme(owner, repo);
            fr.title = "README - " + owner + "/" + repo;
            json payload = {
                {"owner", owner},
                {"repo", repo},
                {"readme", readme}
            };
            fr.content_markdown = payload.dump(2);
            return fr;
        } else if (endpoint.rfind("tree/", 0) == 0) {
            std::string file_path = endpoint.substr(5);
            std::string content = client_->get_file_content(owner, repo, file_path);
            fr.title = file_path;
            json payload = {
                {"owner", owner},
                {"repo", repo},
                {"path", file_path},
                {"content", content}
            };
            fr.content_markdown = payload.dump(2);
            return fr;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    return std::nullopt;
}

} // namespace github_research
