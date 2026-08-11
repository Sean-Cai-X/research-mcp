#include "github_research/git_clone_source.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace github_research {

namespace fs = std::filesystem;

namespace {

bool containsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

bool endsWithMd(const std::string& s) {
    if (s.size() < 3) return false;
    std::string tail = s.substr(s.size() - 3);
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ".md";
}

std::vector<std::string> splitKeywords(const std::string& query) {
    std::vector<std::string> tokens;
    std::istringstream iss(query);
    std::string tok;
    while (iss >> tok) {
        if (!tok.empty()) tokens.push_back(tok);
    }
    return tokens;
}

} // anonymous namespace

GitCloneSource::GitCloneSource(std::string clone_root)
    : clone_root_(std::move(clone_root)) {}

bool GitCloneSource::healthCheck() {
    std::error_code ec;
    if (!fs::exists(clone_root_, ec)) {
        fs::create_directories(clone_root_, ec);
        if (ec) {
            return false;
        }
    }
    return fs::is_directory(clone_root_, ec);
}

std::string GitCloneSource::clonePath(const std::string& owner,
                                      const std::string& repo) const {
    return clone_root_ + "/" + owner + "_" + repo;
}

bool GitCloneSource::runGitClone(const std::string& url, const std::string& dest) {
    std::string cmd = "git clone --depth 1 \"" + url + "\" \"" + dest + "\"";
    std::cerr << "[git_clone] running: " << cmd << std::endl;
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[git_clone] git clone failed (rc=" << rc << ") for "
                  << url << std::endl;
        return false;
    }
    return true;
}

std::string GitCloneSource::ensureCloned(const std::string& owner,
                                          const std::string& repo) {
    std::string dest = clonePath(owner, repo);
    std::error_code ec;
    if (fs::is_directory(dest, ec)) {
        return dest;
    }
    std::string url = "https://github.com/" + owner + "/" + repo + ".git";
    if (!runGitClone(url, dest)) {
        return "";
    }
    return dest;
}

std::vector<SearchResult> GitCloneSource::search(const SearchQuery& query) {
    std::vector<SearchResult> results;
    std::error_code ec;
    if (!fs::is_directory(clone_root_, ec)) return results;

    auto keywords = splitKeywords(query.query);

    for (auto& entry : fs::directory_iterator(clone_root_, ec)) {
        if (results.size() >= static_cast<size_t>(query.max_results)) break;
        if (!entry.is_directory()) continue;
        std::string dir_name = entry.path().filename().string();

        auto underscore = dir_name.find('_');
        if (underscore == std::string::npos) continue;
        std::string owner = dir_name.substr(0, underscore);
        std::string repo  = dir_name.substr(underscore + 1);
        if (owner.empty() || repo.empty()) continue;

        for (auto& file_it : fs::recursive_directory_iterator(entry.path(), ec)) {
            if (results.size() >= static_cast<size_t>(query.max_results)) break;
            if (!file_it.is_regular_file()) continue;
            std::string path_str = file_it.path().string();
            if (!endsWithMd(path_str)) continue;

            std::string rel = fs::relative(file_it.path(), entry.path()).string();
            std::replace(rel.begin(), rel.end(), '\\', '/');

            bool match = true;
            for (const auto& kw : keywords) {
                if (!containsCI(rel, kw) && !containsCI(dir_name, kw)) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;

            SearchResult r;
            r.canonical_uri = "git-clone://" + owner + "/" + repo + "/" + rel;
            auto last_sep = rel.find_last_of('/');
            r.title = (last_sep == std::string::npos) ? rel : rel.substr(last_sep + 1);
            r.resource_kind = ResourceKind::GIT_MARKDOWN;
            r.snippet = rel;
            r.source_id = sourceId();
            results.push_back(std::move(r));
        }
    }
    return results;
}

std::optional<FetchResult> GitCloneSource::fetch(const std::string& canonical_uri) {
    static const std::string scheme = "git-clone://";
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

    std::string rel_path = rest.substr(slash2 + 1);
    if (owner.empty() || repo.empty() || rel_path.empty()) {
        return std::nullopt;
    }

    std::string local_rel = rel_path;
    std::replace(local_rel.begin(), local_rel.end(), '/', '\\');

    std::string local_path = clonePath(owner, repo) + "/" + local_rel;

    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs.is_open()) {
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();

    FetchResult fr;
    fr.canonical_uri = canonical_uri;
    fr.source_id = sourceId();
    fr.resource_kind = ResourceKind::GIT_MARKDOWN;
    auto last_sep = rel_path.find_last_of('/');
    fr.title = (last_sep == std::string::npos) ? rel_path : rel_path.substr(last_sep + 1);
    fr.content_markdown = oss.str();
    return fr;
}

std::vector<std::string> GitCloneSource::expand(const std::string& root_uri,
                                                  const std::string& sub_path,
                                                  int max_depth) {
    std::vector<std::string> uris;
    static const std::string scheme = "git-clone://";
    if (root_uri.compare(0, scheme.size(), scheme) != 0) {
        return uris;
    }
    std::string rest = root_uri.substr(scheme.size());

    auto slash1 = rest.find('/');
    if (slash1 == std::string::npos) return uris;
    std::string owner = rest.substr(0, slash1);

    auto slash2 = rest.find('/', slash1 + 1);
    if (slash2 == std::string::npos) return uris;
    std::string repo = rest.substr(slash1 + 1, slash2 - slash1 - 1);

    std::string repo_dir = clonePath(owner, repo);
    std::error_code ec;
    if (!fs::is_directory(repo_dir, ec)) return uris;

    fs::path base = repo_dir;
    if (!sub_path.empty()) {
        base /= sub_path;
    }
    if (!fs::is_directory(base, ec)) return uris;

    int depth = 0;
    std::vector<fs::path> current_level = {base};
    while (depth < max_depth && !current_level.empty()) {
        std::vector<fs::path> next_level;
        for (const auto& dir : current_level) {
            for (auto& entry : fs::directory_iterator(dir, ec)) {
                std::string name = entry.path().filename().string();
                if (name == ".git") continue;

                std::string rel = fs::relative(entry.path(), repo_dir).string();
                std::replace(rel.begin(), rel.end(), '\\', '/');

                std::string uri = "git-clone://" + owner + "/" + repo + "/" + rel;
                if (entry.is_directory()) {
                    uris.push_back(uri);
                    next_level.push_back(entry.path());
                } else if (entry.is_regular_file()) {
                    if (endsWithMd(name)) {
                        uris.push_back(uri);
                    }
                }
                if (uris.size() >= 500) {
                    return uris;
                }
            }
        }
        current_level = std::move(next_level);
        ++depth;
    }
    return uris;
}

} // namespace github_research
