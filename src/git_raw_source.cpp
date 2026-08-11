#include "github_research/git_raw_source.hpp"
#include "github_research/string_utils.hpp"

#include <optional>
#include <string>
#include <vector>

namespace github_research {

namespace {

const std::string GIT_RAW_SCHEME_PREFIX = "github-raw://";

// URL-encode each segment of a slash-separated path while preserving the
// '/' separators. This keeps "docs/intro.md" -> "docs/intro.md" but
// properly escapes any unsafe characters within individual segments.
std::string encode_path_segments(const std::string& path) {
    if (path.empty()) return path;
    std::string out;
    out.reserve(path.size());
    size_t seg_start = 0;
    while (true) {
        size_t slash = path.find('/', seg_start);
        std::string seg = (slash == std::string::npos)
            ? path.substr(seg_start)
            : path.substr(seg_start, slash - seg_start);
        out += url_encode(seg);
        if (slash == std::string::npos) break;
        out.push_back('/');
        seg_start = slash + 1;
    }
    return out;
}

} // namespace

// =============================================================
// GitRawSource::GitRawSource
// =============================================================
GitRawSource::GitRawSource(IHttpClient* http_client)
    : http_client_(http_client) {}

// =============================================================
// GitRawSource::healthCheck
// Source is healthy only when an HTTP client is wired up and ready.
// =============================================================
bool GitRawSource::healthCheck() {
    return (http_client_ != nullptr) && http_client_->is_ready();
}

// =============================================================
// GitRawSource::search
// If query.repo is "owner/repo", probe well-known markdown entry points
// (README.md, docs/*.md, ...) via raw.githubusercontent.com. Each found
// .md file becomes a SearchResult with canonical_uri of the form
// github-raw://{owner}/{repo}/{branch}/{path}.
// =============================================================
std::vector<SearchResult> GitRawSource::search(const SearchQuery& query) {
    std::vector<SearchResult> results;
    if (http_client_ == nullptr) return results;
    if (!http_client_->is_ready()) return results;
    if (query.repo.empty()) return results;

    // Parse "owner/repo" — LLM clients may pass "owner/repo/extra", so we
    // extract only the first two segments and ignore trailing path parts.
    const size_t slash1 = query.repo.find('/');
    if (slash1 == std::string::npos || slash1 == 0) return results;
    const std::string owner  = query.repo.substr(0, slash1);
    const size_t slash2 = query.repo.find('/', slash1 + 1);
    const std::string repo   = (slash2 == std::string::npos)
        ? query.repo.substr(slash1 + 1)
        : query.repo.substr(slash1 + 1, slash2 - slash1 - 1);
    if (owner.empty() || repo.empty()) return results;
    const std::string branch = query.repo_branch.empty()
        ? std::string("main")
        : query.repo_branch;

    static const char* const kCommonPaths[] = {
        "README.md",
        "readme.md",
        "README.MD",
        "docs/index.md",
        "docs/README.md",
        "doc/README.md",
        "DOCS.md",
        "CHANGELOG.md",
        "CONTRIBUTING.md",
        "docs/getting-started.md",
        "docs/overview.md"
    };

    auto try_fetch = [&](const std::string& path) {
        if (query.max_results > 0 &&
            static_cast<int>(results.size()) >= query.max_results) {
            return;
        }
        const std::string url = std::string(RAW_BASE) + "/" +
                                url_encode(owner) + "/" +
                                url_encode(repo) + "/" +
                                url_encode(branch) + "/" +
                                encode_path_segments(path);
        HttpResponse resp = http_client_->get(url);
        if (resp.status_code != 200 || resp.body.empty()) return;

        SearchResult r;
        r.canonical_uri = GIT_RAW_SCHEME_PREFIX + owner + "/" + repo + "/" +
                          branch + "/" + path;
        const size_t last_slash = path.find_last_of('/');
        r.title = (last_slash == std::string::npos)
            ? path
            : path.substr(last_slash + 1);
        r.resource_kind = ResourceKind::GIT_MARKDOWN;
        r.source_id = "git_raw";
        r.snippet = (resp.body.size() > 200)
            ? resp.body.substr(0, 200)
            : resp.body;
        results.push_back(std::move(r));
    };

    for (const char* p : kCommonPaths) {
        try_fetch(p);
    }
    return results;
}

// =============================================================
// GitRawSource::fetch
// Parse "github-raw://{owner}/{repo}/{branch}/{path}", build the
// raw.githubusercontent.com URL, and HTTP GET it.
// =============================================================
std::optional<FetchResult> GitRawSource::fetch(const std::string& canonical_uri) {
    if (http_client_ == nullptr) return std::nullopt;
    if (!http_client_->is_ready()) return std::nullopt;
    if (canonical_uri.size() < GIT_RAW_SCHEME_PREFIX.size()) return std::nullopt;
    if (canonical_uri.compare(0, GIT_RAW_SCHEME_PREFIX.size(),
                              GIT_RAW_SCHEME_PREFIX) != 0) {
        return std::nullopt;
    }

    const std::string rest = canonical_uri.substr(GIT_RAW_SCHEME_PREFIX.size());
    // rest = {owner}/{repo}/{branch}/{path}
    const size_t s1 = rest.find('/');
    if (s1 == std::string::npos || s1 == 0) return std::nullopt;
    const size_t s2 = rest.find('/', s1 + 1);
    if (s2 == std::string::npos) return std::nullopt;
    const size_t s3 = rest.find('/', s2 + 1);
    if (s3 == std::string::npos) return std::nullopt;

    const std::string owner  = rest.substr(0, s1);
    const std::string repo   = rest.substr(s1 + 1, s2 - s1 - 1);
    const std::string branch = rest.substr(s2 + 1, s3 - s2 - 1);
    const std::string path   = rest.substr(s3 + 1);
    if (owner.empty() || repo.empty() || branch.empty() || path.empty()) {
        return std::nullopt;
    }

    const std::string url = std::string(RAW_BASE) + "/" +
                            url_encode(owner) + "/" +
                            url_encode(repo) + "/" +
                            url_encode(branch) + "/" +
                            encode_path_segments(path);
    HttpResponse resp = http_client_->get(url);
    if (resp.status_code != 200) return std::nullopt;

    FetchResult fr;
    fr.canonical_uri = canonical_uri;
    const size_t last_slash = path.find_last_of('/');
    fr.title = (last_slash == std::string::npos)
        ? path
        : path.substr(last_slash + 1);
    fr.content_markdown = resp.body;
    fr.resource_kind = ResourceKind::GIT_MARKDOWN;
    fr.source_id = "git_raw";
    return fr;
}

} // namespace github_research
