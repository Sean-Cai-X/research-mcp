#include "github_research/github_client.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/formatters.hpp"
#include "github_research/cache_manager.hpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <set>
#include <vector>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <thread>
#include <chrono>

namespace github_research {

// ISO8601 时间解析: "2024-01-15T08:30:45Z" -> unix timestamp (UTC)
// 失败返回 0
static int64_t parse_iso8601(const std::string& date_str) {
    if (date_str.empty()) return 0;
    // 至少需要 "YYYY-MM-DDTHH:MM:SS"
    if (date_str.size() < 19) {
        // 退化:只解析 "YYYY-MM-DD"
        if (date_str.size() >= 10) {
            std::tm tm{};
            tm.tm_year = std::atoi(date_str.substr(0, 4).c_str()) - 1900;
            tm.tm_mon  = std::atoi(date_str.substr(5, 2).c_str()) - 1;
            tm.tm_mday = std::atoi(date_str.substr(8, 2).c_str());
            tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_isdst = 0;
            // 使用 _mkgmtime (Windows) 将 tm 视为 UTC
            return (int64_t)_mkgmtime(&tm);
        }
        return 0;
    }
    std::tm tm{};
    tm.tm_year = std::atoi(date_str.substr(0, 4).c_str()) - 1900;
    tm.tm_mon  = std::atoi(date_str.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(date_str.substr(8, 2).c_str());
    tm.tm_hour = std::atoi(date_str.substr(11, 2).c_str());
    tm.tm_min  = std::atoi(date_str.substr(14, 2).c_str());
    tm.tm_sec  = std::atoi(date_str.substr(17, 2).c_str());
    tm.tm_isdst = 0;
    // _mkgmtime 将 tm 视为 UTC 时间,返回 time_t
    return (int64_t)_mkgmtime(&tm);
}

// unix timestamp -> ISO8601 字符串 (UTC, "YYYY-MM-DDTHH:MM:SSZ")
static std::string format_iso8601(int64_t ts) {
    if (ts <= 0) return "1970-01-01T00:00:00Z";
    std::time_t t = (std::time_t)ts;
    std::tm gm{};
    gmtime_s(&gm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm);
    return std::string(buf);
}

// 根据 endpoint 决定 TTL(小时)
static int cache_ttl_for_endpoint(const std::string& endpoint) {
    // search: 结果变化较快,TTL 短
    if (endpoint.rfind("/search/", 0) == 0) return 1;
    // file content / readme: 变化较慢
    if (endpoint.find("/contents/") != std::string::npos) return 72;
    if (endpoint.rfind("/readme", 0) == 0 || endpoint.find("/readme") != std::string::npos) return 72;
    // tree / git objects
    if (endpoint.find("/git/trees/") != std::string::npos) return 48;
    if (endpoint.find("/git/blobs/") != std::string::npos) return 72;
    // tags / releases / branches: 变化慢
    if (endpoint.find("/tags") != std::string::npos) return 12;
    if (endpoint.find("/releases") != std::string::npos) return 12;
    if (endpoint.find("/branches") != std::string::npos) return 12;
    // issues / PRs: 中等变化
    if (endpoint.find("/issues") != std::string::npos) return 4;
    if (endpoint.find("/pulls")  != std::string::npos) return 4;
    if (endpoint.find("/commits")!= std::string::npos) return 6;
    // contributors
    if (endpoint.find("/contributors") != std::string::npos) return 24;
    // languages: 基本不变
    if (endpoint.find("/languages") != std::string::npos) return 24 * 30;
    // repo info: 缓存 24h
    if (endpoint.rfind("/repos/", 0) == 0 &&
        std::count(endpoint.begin(), endpoint.end(), '/') == 2) return 24;
    return 6;  // 默认:6h
}


// === 构造 ===

GitHubClient::GitHubClient(std::optional<std::string> token, int timeout_seconds, Backend backend)
    : timeout_seconds_(timeout_seconds), backend_(backend) {
    if (backend_ == Backend::Curl) {
        http_client_ = std::make_unique<CurlHttpClient>("Deep-Research-Bot/1.0", timeout_seconds);
    } else {
        http_client_ = std::make_unique<WebViewClient>("Deep-Research-Bot/1.0", timeout_seconds, true);
    }
    headers_["Accept"] = "application/vnd.github.v3+json";
    headers_["User-Agent"] = "Deep-Research-Bot/1.0";
    if (token && !token->empty()) {
        headers_["Authorization"] = "token " + *token;
    }
}

// === URL 拼接 ===

std::string GitHubClient::build_url(const std::string& endpoint,
                                    const std::map<std::string, std::string>& params) {
    std::string url = "https://api.github.com" + endpoint;
    if (!params.empty()) {
        url += "?" + build_query(params);
    }
    return url;
}

// === 统一 GET(返回 JSON) ===

json GitHubClient::http_get(const std::string& endpoint,
                            const std::map<std::string, std::string>& params,
                            const std::optional<std::string>& accept) {
    std::string url = build_url(endpoint, params);

    // ── 缓存查询:零侵入上层工具 ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "gh:GET:" + url;
    if (cm.is_ready()) {
        auto cached = cm.get("github", cache_key);
        if (cached && cached->fetch_status == "ok") {
            try {
                return json::parse(cached->payload);
            } catch (...) {
                // 缓存损坏,失效后继续
                cm.invalidate("github", cache_key);
            }
        }
    }

    std::map<std::string, std::string> hdrs = headers_;
    if (accept) {
        hdrs["Accept"] = *accept;
    }

    // 首次请求时延迟初始化后端(无 fallback)
    if (!ensure_ready()) {
        GitHubAPIError err("backend initialization failed (no fallback)", 0, url, "");
        if (cm.is_ready()) cm.put("github", cache_key, "", "json", 1, "", "failed", err.what());
        throw err;
    }

    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> resp_headers;

    // 后端网络层(libcurl 或 WebView2)
    HttpResponse resp = http_client_->get(url, hdrs);
    status_code = resp.status_code;
    body = resp.body;
    resp_headers = resp.headers;

    // 网络层错误(status_code == 0)
    if (status_code == 0) {
        GitHubAPIError err(body, 0, url, "");
        if (cm.is_ready()) cm.put("github", cache_key, "", "json", 1, "", "failed", err.what());
        throw err;
    }

    // 限流检查:403/429 + X-RateLimit-Remaining: 0
    if (status_code == 403 || status_code == 429) {
        auto it = resp_headers.find("x-ratelimit-remaining");
        if (it != resp_headers.end() && it->second == "0") {
            auto reset_it = resp_headers.find("x-ratelimit-reset");
            std::string reset_at = (reset_it != resp_headers.end()) ? reset_it->second : "";
            GitHubRateLimitError err("rate limit exceeded", reset_at);
            if (cm.is_ready()) cm.put("github", cache_key, "", "json", 1, "", "rate_limited", err.what());
            throw err;
        }
    }

    // HTTP 错误
    if (status_code >= 400) {
        std::string body_preview = body.substr(0, std::min<size_t>(body.size(), 500));
        std::string msg = "HTTP " + std::to_string(status_code);
        // 尝试从 GitHub 响应体解析真实错误信息
        // GitHub 404 可能是: 仓库不存在 / 资源(分支/tree/commit)不存在 / 私有仓库无权限
        // 之前统一写 "repository not found" 会掩盖分支名错误等真实原因
        if (status_code == 404) {
            msg = "not found";
            try {
                json err_body = json::parse(body);
                std::string gh_msg = err_body.value("message", "");
                if (!gh_msg.empty()) msg = gh_msg;  // "Branch not found" / "Not Found" 等
            } catch (...) {
                // body 不是 JSON,保持 "not found"
            }
        } else if (status_code == 401) {
            msg = "authentication required (no valid token)";
        } else if (status_code == 403) {
            msg = "forbidden (rate limit or access denied)";
        }
        GitHubAPIError err(msg, status_code, url, body_preview);
        if (cm.is_ready()) cm.put("github", cache_key, body, "json", 2, "", "failed", err.what());
        throw err;
    }

    // 解析 JSON
    try {
        json parsed = json::parse(body);
        // 成功写入缓存(差异化 TTL)
        if (cm.is_ready()) {
            int ttl = cache_ttl_for_endpoint(endpoint);
            cm.put("github", cache_key, body, "json", ttl, "", "ok", "");
        }
        return parsed;
    } catch (const std::exception& e) {
        GitHubAPIError err(std::string("invalid JSON: ") + e.what(),
                             status_code, url,
                             body.substr(0, std::min<size_t>(body.size(), 500)));
        if (cm.is_ready()) cm.put("github", cache_key, body, "json", 1, "", "failed", err.what());
        throw err;
    }
}

// === 统一 GET(返回文本,用于 readme/file) ===

std::string GitHubClient::http_get_text(const std::string& endpoint,
                                        const std::optional<std::string>& accept) {
    std::string url = build_url(endpoint);

    // ── 缓存查询 ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "gh:TXT:" + url;
    if (cm.is_ready()) {
        auto cached = cm.get("github", cache_key);
        if (cached && cached->fetch_status == "ok") {
            return cached->payload;
        }
    }

    std::map<std::string, std::string> hdrs = headers_;
    if (accept) {
        hdrs["Accept"] = *accept;
    }

    // 首次请求时延迟初始化后端
    if (!ensure_ready()) {
        GitHubAPIError err("backend initialization failed", 0, url, "");
        if (cm.is_ready()) cm.put("github", cache_key, "", "text", 1, "", "failed", err.what());
        throw err;
    }

    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> resp_headers;

    // 后端网络层(libcurl 或 WebView2)
    HttpResponse resp = http_client_->get(url, hdrs);
    status_code = resp.status_code;
    body = resp.body;
    resp_headers = resp.headers;

    if (status_code == 0) {
        GitHubAPIError err(body, 0, url, "");
        if (cm.is_ready()) cm.put("github", cache_key, "", "text", 1, "", "failed", err.what());
        throw err;
    }

    if (status_code == 403 || status_code == 429) {
        auto it = resp_headers.find("x-ratelimit-remaining");
        if (it != resp_headers.end() && it->second == "0") {
            auto reset_it = resp_headers.find("x-ratelimit-reset");
            std::string reset_at = (reset_it != resp_headers.end()) ? reset_it->second : "";
            GitHubRateLimitError err("rate limit exceeded", reset_at);
            if (cm.is_ready()) cm.put("github", cache_key, "", "text", 1, "", "rate_limited", err.what());
            throw err;
        }
    }

    if (status_code >= 400) {
        std::string body_preview = body.substr(0, std::min<size_t>(body.size(), 500));
        std::string msg = "HTTP " + std::to_string(status_code);
        if (status_code == 404) msg = "not found";
        GitHubAPIError err(msg, status_code, url, body_preview);
        if (cm.is_ready()) cm.put("github", cache_key, body, "text", 2, "", "failed", err.what());
        throw err;
    }

    // 成功写入缓存
    if (cm.is_ready()) {
        int ttl = cache_ttl_for_endpoint(endpoint);
        cm.put("github", cache_key, body, "text", ttl, "", "ok", "");
    }
    return body;
}

// === 10 个 API 方法 ===

json GitHubClient::get_repo_info(const std::string& owner, const std::string& repo) {
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo));
}

std::string GitHubClient::get_readme(const std::string& owner, const std::string& repo) {
    try {
        return http_get_text("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/readme",
                             "application/vnd.github.raw");
    } catch (const GitHubAPIError& e) {
        return "[README not found: " + std::string(e.what()) + "]";
    }
}

std::string GitHubClient::get_tree(const std::string& owner, const std::string& repo,
                                   const std::string& branch, int max_depth, bool recursive) {
    json tree_data = get_tree_raw(owner, repo, branch, recursive);
    return format_tree(tree_data, max_depth);
}

json GitHubClient::get_tree_raw(const std::string& owner, const std::string& repo,
                                const std::string& branch, bool recursive) {
    std::map<std::string, std::string> params;
    if (recursive) params["recursive"] = "1";

    // 判断 branch 是否看起来像 40 字符 hex SHA
    auto is_hex_sha = [](const std::string& s) -> bool {
        if (s.size() != 40) return false;
        for (char c : s) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };

    std::string tree_ref = branch;

    // 非 SHA 输入(即分支名)需要先解析为 commit SHA
    if (!is_hex_sha(branch)) {
        // 尝试 1: 直接用分支名查 branches API
        try {
            json branch_info = http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                                        "/branches/" + url_encode(branch));
            if (branch_info.is_object() && branch_info.contains("commit")) {
                tree_ref = branch_info["commit"].value("sha", branch);
            }
        } catch (const GitHubAPIError&) {
            // 尝试 2: 模糊后缀匹配分支列表
            // 典型场景:用户传 "cxcore-integration",实际分支名是 "codex/cxcore-integration"
            try {
                json branches = http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                                         "/branches", {{"per_page", "100"}});
                if (branches.is_array()) {
                    for (const auto& b : branches) {
                        std::string bname = b.value("name", "");
                        // 精确匹配优先
                        if (bname == branch && b.contains("commit")) {
                            tree_ref = b["commit"].value("sha", branch);
                            break;
                        }
                    }
                    // 精确匹配失败,尝试后缀匹配
                    if (tree_ref == branch) {
                        for (const auto& b : branches) {
                            std::string bname = b.value("name", "");
                            if (bname.size() >= branch.size() &&
                                bname.compare(bname.size() - branch.size(), branch.size(), branch) == 0 &&
                                b.contains("commit")) {
                                tree_ref = b["commit"].value("sha", branch);
                                break;
                            }
                        }
                    }
                }
            } catch (const GitHubAPIError&) {
                // 分支列表也拉不到,保持 tree_ref = branch 原样传入(让 trees 端点给 404)
            }
        }
    }

    try {
        return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                        "/git/trees/" + url_encode(tree_ref), params);
    } catch (const GitHubAPIError&) {
        // main 失败时回退 master(仅当 branch == "main")
        if (branch == "main") {
            return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                            "/git/trees/master", params);
        }
        throw;
    }
}

std::string GitHubClient::get_file_content(const std::string& owner, const std::string& repo,
                                           const std::string& path) {
    // 内部解析契约: 文件不存在/读取失败时返回空串,由调用方跳过。
    // 不返回错误描述字符串,避免被依赖解析器误当作包名。
    // 使用默认 JSON accept(Contents API 返回 {content: base64, encoding: "base64"}),
    // 不使用 application/vnd.github.raw —— WebView2 无法将 raw 文本响应作为导航处理。
    try {
        json resp = http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                             "/contents/" + url_encode(path));
        if (!resp.is_object()) return "";
        std::string encoding = resp.value("encoding", "");
        std::string content = resp.value("content", "");
        if (encoding == "base64" && !content.empty()) {
            return base64_decode(content);
        }
        return content;
    } catch (const GitHubAPIError&) {
        return "";
    }
}

json GitHubClient::get_languages(const std::string& owner, const std::string& repo) {
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/languages");
}

json GitHubClient::get_contributors(const std::string& owner, const std::string& repo, int limit) {
    int per_page = std::min(limit, 100);
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/contributors",
                    {{"per_page", std::to_string(per_page)}});
}

json GitHubClient::get_recent_commits(const std::string& owner, const std::string& repo,
                                      int limit, const std::optional<std::string>& since,
                                      const std::optional<std::string>& sha,
                                      const std::optional<std::string>& path) {
    std::map<std::string, std::string> params;
    params["per_page"] = std::to_string(std::min(limit, 100));
    if (since) params["since"] = *since;
    if (sha && !sha->empty()) params["sha"] = *sha;  // 分支名 / tag / commit SHA
    if (path && !path->empty()) params["path"] = *path;  // 目录/文件路径过滤,避免大仓库全量拉取超时
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/commits", params);
}

json GitHubClient::get_branches(const std::string& owner, const std::string& repo, int limit) {
    std::map<std::string, std::string> params;
    params["per_page"] = std::to_string(std::min(limit, 100));
    // GET /repos/{owner}/{repo}/branches
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/branches", params);
}

json GitHubClient::get_issues(const std::string& owner, const std::string& repo,
                              const std::string& state, int limit,
                              const std::optional<std::string>& labels) {
    std::map<std::string, std::string> params;
    params["state"] = state;
    params["per_page"] = std::to_string(std::min(limit, 100));
    if (labels) params["labels"] = *labels;
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/issues", params);
}

json GitHubClient::get_pull_requests(const std::string& owner, const std::string& repo,
                                     const std::string& state, int limit) {
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/pulls",
                    {{"state", state}, {"per_page", std::to_string(std::min(limit, 100))}});
}

json GitHubClient::get_releases(const std::string& owner, const std::string& repo, int limit) {
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/releases",
                    {{"per_page", std::to_string(std::min(limit, 100))}});
}

json GitHubClient::get_tags(const std::string& owner, const std::string& repo, int limit) {
    return http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) + "/tags",
                    {{"per_page", std::to_string(std::min(limit, 100))}});
}

json GitHubClient::search_issues(const std::string& owner, const std::string& repo,
                                 const std::string& query, int limit) {
    std::string q = "repo:" + owner + "/" + repo + " " + query;
    return http_get("/search/issues",
                    {{"q", q}, {"per_page", std::to_string(std::min(limit, 100))}});
}

json GitHubClient::search_repositories(const std::string& query,
                                       const std::string& sort,
                                       const std::string& order,
                                       int limit, int page) {
    std::map<std::string, std::string> params;
    params["q"] = query;
    if (!sort.empty()) params["sort"] = sort;
    if (!order.empty()) params["order"] = order;
    params["per_page"] = std::to_string(std::min(limit, 100));
    if (page > 1) params["page"] = std::to_string(page);
    return http_get("/search/repositories", params);
}

json GitHubClient::search_users(const std::string& query,
                                const std::string& sort,
                                const std::string& order,
                                int limit, int page) {
    std::map<std::string, std::string> params;
    params["q"] = query;
    if (!sort.empty()) params["sort"] = sort;
    if (!order.empty()) params["order"] = order;
    params["per_page"] = std::to_string(std::min(limit, 100));
    if (page > 1) params["page"] = std::to_string(page);
    return http_get("/search/users", params);
}

// === summarize_repo ===

json GitHubClient::summarize_repo(const std::string& owner, const std::string& repo) {
    json info = get_repo_info(owner, repo);

    // 辅助:取 key 对应的 json 值,不存在则返回 json::value_t::null
    // 避免 info.value(key, nullptr) 在 key 存在且值为 string 时
    // 触发 nlohmann/json type_error.302(get<std::nullptr_t>() 失败)
    auto get_or_null = [](const json& obj, const char* key) -> json {
        if (obj.contains(key)) return obj[key];
        return json(nullptr);
    };

    json summary = json::object();
    summary["name"] = get_or_null(info, "full_name");
    summary["description"] = get_or_null(info, "description");
    summary["url"] = get_or_null(info, "html_url");
    summary["stars"] = info.value("stargazers_count", 0);
    summary["forks"] = info.value("forks_count", 0);
    summary["open_issues"] = info.value("open_issues_count", 0);
    summary["language"] = get_or_null(info, "language");
    if (info.contains("license") && info["license"].is_object()) {
        summary["license"] = get_or_null(info["license"], "spdx_id");
    } else {
        summary["license"] = nullptr;
    }
    summary["created_at"] = get_or_null(info, "created_at");
    summary["updated_at"] = get_or_null(info, "updated_at");
    summary["pushed_at"] = get_or_null(info, "pushed_at");
    summary["default_branch"] = get_or_null(info, "default_branch");
    if (info.contains("topics") && info["topics"].is_array()) {
        summary["topics"] = info["topics"];
    } else {
        summary["topics"] = json::array();
    }

    // languages
    try {
        summary["languages"] = get_languages(owner, repo);
    } catch (...) {
        summary["languages"] = json::object();
    }

    // contributor_count(保留 Python 版语义:调用两次,实际只用第二次的 size)
    try {
        // 第一次调用(limit=1)的结果未被使用,与 Python 版一致
        // 这里省略以减少无效请求,直接调用 limit=100
        json contributors = get_contributors(owner, repo, 100);
        summary["contributor_count"] = contributors.is_array() ? static_cast<int>(contributors.size()) : 0;
    } catch (...) {
        summary["contributor_count"] = "N/A";
    }

    // latest_release
    try {
        json releases = get_releases(owner, repo, 1);
        if (releases.is_array() && !releases.empty()) {
            json r = releases[0];
            summary["latest_release"] = {
                {"tag", get_or_null(r, "tag_name")},
                {"name", get_or_null(r, "name")},
                {"date", get_or_null(r, "published_at")}
            };
        } else {
            summary["latest_release"] = nullptr;
        }
    } catch (...) {
        summary["latest_release"] = nullptr;
    }

    return summary;
}

// ============================================================
// 分层渐进挖掘工具实现(新增,与 HN/arXiv 对称)
// ============================================================

namespace {

// 分割字符串(按分隔符)
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        // trim
        size_t a = item.find_first_not_of(" \t\r\n");
        size_t b = item.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) {
            out.push_back(item.substr(a, b - a + 1));
        }
    }
    return out;
}

// Jaccard 相似度(两个集合的交集/并集)
double jaccard(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty() && b.empty()) return 0.0;
    int inter = 0;
    for (auto& x : a) if (b.count(x)) ++inter;
    int uni = a.size() + b.size() - inter;
    return uni == 0 ? 0.0 : (double)inter / uni;
}

// 从 JSON 数组提取字符串集合
std::set<std::string> json_to_set(const json& arr) {
    std::set<std::string> s;
    if (arr.is_array()) {
        for (auto& v : arr) {
            if (v.is_string()) s.insert(to_lower(v.get<std::string>()));
        }
    }
    return s;
}

} // anonymous namespace

// === 13. github_search_index ===
json GitHubClient::search_index(const std::string& query,
                                const std::string& language,
                                const std::string& sort,
                                int max_results) {
    if (max_results < 1) max_results = 1;
    if (max_results > 50) max_results = 50;

    // 构造搜索 query
    std::string fullQuery = query;
    if (!language.empty()) {
        // 避免重复添加 language: 限定符
        if (fullQuery.find("language:") == std::string::npos) {
            fullQuery += " language:" + language;
        }
    }

    json searchResult = search_repositories(fullQuery, sort, "desc", max_results, 1);
    // search_repositories 返回 {total_count, incomplete_results, items:[...]}
    json items = json::array();
    if (searchResult.is_object() && searchResult.contains("items") && searchResult["items"].is_array()) {
        for (auto& it : searchResult["items"]) {
            json item = {
                {"repo_id", it.value("id", 0)},
                {"full_name", it.value("full_name", "")},
                {"description", it.value("description", "")},
                {"language", it.value("language", "")},
                {"stars", it.value("stargazers_count", 0)},
                {"topics", it.contains("topics") ? it["topics"] : json::array()},
                {"html_url", it.value("html_url", "")}
            };
            items.push_back(item);
        }
    }

    return {
        {"success", true},
        {"query", query},
        {"language", language},
        {"sort", sort},
        {"total_returned", items.size()},
        {"items", items}
    };
}

// === 14. github_fetch_repo_detail ===
json GitHubClient::fetch_repo_detail(const std::string& owner,
                                     const std::string& repo,
                                     bool fetch_tech_stack,
                                     bool fetch_code_structure,
                                     bool fetch_top_contributors,
                                     bool fetch_dependencies,
                                     int max_contributors) {
    if (max_contributors < 1) max_contributors = 1;
    if (max_contributors > 30) max_contributors = 30;

    // 基础元数据
    json info = get_repo_info(owner, repo);
    std::string fullName = info.value("full_name", owner + "/" + repo);
    std::string description = info.value("description", "");
    int stars = info.value("stargazers_count", 0);
    std::string language = info.value("language", "");
    json topics = info.contains("topics") ? info["topics"] : json::array();

    json techStack = {{"runtime", json::array()}, {"framework", json::array()},
                      {"database", json::array()}, {"devops", json::array()},
                      {"testing", json::array()}};
    json techBlocks = json::array();
    json topContributors = json::array();
    json directDeps = json::array();

    // 维度1: 技术栈解析(读取依赖文件)
    if (fetch_tech_stack || fetch_dependencies) {
        // 读取常见依赖文件,解析依赖列表
        std::vector<std::string> depFiles = {
            "requirements.txt", "requirements-dev.txt", "pyproject.toml",
            "package.json", "Cargo.toml", "go.mod", "pom.xml"
        };
        std::set<std::string> allDeps;
        bool pythonDetected = false;
        for (auto& fname : depFiles) {
            try {
                std::string content = get_file_content(owner, repo, fname);
                if (content.empty()) continue;

                if (fname == "requirements.txt" || fname == "requirements-dev.txt") {
                    if (!pythonDetected) { techStack["runtime"].push_back("Python"); pythonDetected = true; }
                    for (auto& line : split(content, '\n')) {
                        if (line.empty() || line[0] == '#') continue;
                        // 取包名(== 或 >= 之前),去掉 -e / -r 等前缀和 git URL
                        size_t pos = line.find_first_of("=<>!");
                        std::string pkg = (pos != std::string::npos) ? line.substr(0, pos) : line;
                        // 去掉行首空格和 -e / -r 前缀
                        size_t p = pkg.find_first_not_of(" \t");
                        if (p != std::string::npos) pkg = pkg.substr(p);
                        if (pkg.substr(0, 2) == "-e" || pkg.substr(0, 2) == "-r") continue;
                        if (pkg.find("git+") != std::string::npos || pkg.find("http") != std::string::npos) continue;
                        pkg = to_lower(pkg);
                        if (!pkg.empty()) allDeps.insert(pkg);
                    }
                } else if (fname == "pyproject.toml") {
                    if (!pythonDetected) { techStack["runtime"].push_back("Python"); pythonDetected = true; }
                    // 精确解析 dependencies = [...] 数组(避免误匹配 classifiers/description)
                    // 支持: dependencies = ["pkg1", "pkg2"] 单行
                    //       dependencies = [  多行
                    //         "pkg1>=1.0",
                    //         "pkg2",
                    //       ]
                    bool inDepsArray = false;
                    for (auto& line : split(content, '\n')) {
                        // 去掉行首空格用于判断 key
                        size_t lead = line.find_first_not_of(" \t");
                        std::string trimmed = (lead != std::string::npos) ? line.substr(lead) : "";

                        if (!inDepsArray) {
                            // 精确匹配以 dependencies 开头的赋值行(避免 optional-dependencies/classifiers)
                            // 合法形式: "dependencies = [" 或 "dependencies = ["
                            if (trimmed.rfind("dependencies", 0) == 0) {
                                size_t eq = trimmed.find('=');
                                if (eq != std::string::npos) {
                                    // 检查 = 前面只有 "dependencies"(不是 optional-dependencies)
                                    std::string key = trimmed.substr(0, eq);
                                    // 去掉 key 两端空格
                                    size_t ks = key.find_first_not_of(" \t");
                                    size_t ke = key.find_last_not_of(" \t");
                                    if (ks != std::string::npos) key = key.substr(ks, ke - ks + 1);
                                    if (key == "dependencies") {
                                        inDepsArray = true;
                                        // 行内可能有完整数组或部分内容
                                        if (trimmed.find(']') != std::string::npos) {
                                            // 单行数组: dependencies = ["pkg1", "pkg2"]
                                            size_t pos = 0;
                                            while ((pos = trimmed.find('"', pos)) != std::string::npos) {
                                                size_t end = trimmed.find('"', pos + 1);
                                                if (end == std::string::npos) break;
                                                std::string pkg = trimmed.substr(pos + 1, end - pos - 1);
                                                size_t vp = pkg.find_first_of("=<>!~ ");
                                                if (vp != std::string::npos) pkg = pkg.substr(0, vp);
                                                pkg = to_lower(pkg);
                                                if (!pkg.empty()) allDeps.insert(pkg);
                                                pos = end + 1;
                                            }
                                            inDepsArray = false;
                                        }
                                        continue;
                                    }
                                }
                            }
                        } else {
                            // 在 dependencies 数组内,遇到 ] 结束
                            if (trimmed.find(']') != std::string::npos) {
                                inDepsArray = false;
                                continue;
                            }
                            // 提取引号内的包名
                            size_t q1 = line.find('"');
                            if (q1 != std::string::npos) {
                                size_t q2 = line.find('"', q1 + 1);
                                if (q2 != std::string::npos) {
                                    std::string pkg = line.substr(q1 + 1, q2 - q1 - 1);
                                    size_t vp = pkg.find_first_of("=<>!~ ");
                                    if (vp != std::string::npos) pkg = pkg.substr(0, vp);
                                    pkg = to_lower(pkg);
                                    if (!pkg.empty()) allDeps.insert(pkg);
                                }
                            }
                        }
                    }
                } else if (fname == "package.json") {
                    techStack["runtime"].push_back("Node.js");
                    try {
                        json pkg = json::parse(content);
                        if (pkg.contains("dependencies")) {
                            for (auto& el : pkg["dependencies"].items()) {
                                allDeps.insert(to_lower(el.key()));
                            }
                        }
                        if (pkg.contains("devDependencies")) {
                            for (auto& el : pkg["devDependencies"].items()) {
                                allDeps.insert(to_lower(el.key()));
                            }
                        }
                    } catch (...) {}
                } else if (fname == "Cargo.toml") {
                    techStack["runtime"].push_back("Rust");
                    // 简单解析 [dependencies] 段
                    bool inDeps = false;
                    for (auto& line : split(content, '\n')) {
                        if (line == "[dependencies]") { inDeps = true; continue; }
                        if (!line.empty() && line[0] == '[') { inDeps = false; continue; }
                        if (inDeps) {
                            size_t eq = line.find('=');
                            if (eq != std::string::npos) {
                                allDeps.insert(to_lower(line.substr(0, eq)));
                            }
                        }
                    }
                } else if (fname == "go.mod") {
                    techStack["runtime"].push_back("Go");
                    for (auto& line : split(content, '\n')) {
                        if (line.find("require") != std::string::npos) continue;
                        if (!line.empty() && line[0] != '\t' && line.find(" ") != std::string::npos) {
                            auto parts = split(line, ' ');
                            if (parts.size() >= 2) allDeps.insert(to_lower(parts[0]));
                        }
                    }
                } else if (fname == "pom.xml") {
                    techStack["runtime"].push_back("Java");
                    // XML 解析复杂,仅标记 Java,依赖提取交给 AI
                }
            } catch (...) {
                // 文件不存在或读取失败,跳过
            }
        }

        // 按依赖推断框架/数据库/测试分类
        for (auto& dep : allDeps) {
            // 直接依赖列表
            directDeps.push_back(dep);
            // 分类推断
            if (dep.find("fastapi") != std::string::npos || dep.find("flask") != std::string::npos ||
                dep.find("django") != std::string::npos || dep.find("express") != std::string::npos ||
                dep.find("react") != std::string::npos || dep.find("vue") != std::string::npos ||
                dep.find("langchain") != std::string::npos || dep.find("pytorch") != std::string::npos ||
                dep.find("tensorflow") != std::string::npos) {
                techStack["framework"].push_back(dep);
            } else if (dep.find("postgres") != std::string::npos || dep.find("redis") != std::string::npos ||
                       dep.find("mysql") != std::string::npos || dep.find("mongo") != std::string::npos ||
                       dep.find("sqlite") != std::string::npos || dep.find("chroma") != std::string::npos) {
                techStack["database"].push_back(dep);
            } else if (dep.find("pytest") != std::string::npos || dep.find("jest") != std::string::npos ||
                       dep.find("mocha") != std::string::npos || dep.find("unittest") != std::string::npos) {
                techStack["testing"].push_back(dep);
            }
        }
    }

    // 维度2: 代码结构分块(解析原始目录树,保留完整 path)
    if (fetch_code_structure) {
        try {
            json treeJson = get_tree_raw(owner, repo, "main", true);
            // 统计每个一级目录下的文件数
            std::map<std::string, int> dirFileCount;
            if (treeJson.contains("tree") && treeJson["tree"].is_array()) {
                for (auto& item : treeJson["tree"]) {
                    if (!item.contains("path") || !item["path"].is_string()) continue;
                    std::string p = item["path"].get<std::string>();
                    // 只统计 blob(文件),忽略 tree(目录)自身
                    std::string type = item.value("type", std::string());
                    if (type != "blob") continue;
                    size_t slash = p.find('/');
                    if (slash != std::string::npos) {
                        dirFileCount[p.substr(0, slash)]++;
                    }
                }
            }
            // 识别核心目录
            std::set<std::string> coreDirs = {
                "src", "core", "api", "cli", "lib", "app", "server", "client",
                "backend", "frontend", "models", "utils", "tests", "docs", "tools",
                "libs", "packages", "modules", "services"
            };
            for (auto& kv : dirFileCount) {
                if (coreDirs.count(kv.first) && kv.second >= 2) {
                    std::string purpose;
                    std::string d = kv.first;
                    if (d == "src" || d == "core") purpose = "core source";
                    else if (d == "api") purpose = "API layer";
                    else if (d == "cli") purpose = "command-line interface";
                    else if (d == "models") purpose = "data models";
                    else if (d == "utils") purpose = "utilities";
                    else if (d == "tests") purpose = "test suite";
                    else if (d == "docs") purpose = "documentation";
                    else if (d == "tools") purpose = "tooling/scripts";
                    else if (d == "backend") purpose = "backend service";
                    else if (d == "frontend") purpose = "frontend UI";
                    else if (d == "libs" || d == "packages") purpose = "bundled libraries/packages";
                    else if (d == "modules" || d == "services") purpose = "modules/services";
                    else purpose = "module";

                    techBlocks.push_back({
                        {"name", d},
                        {"path", d + "/"},
                        {"purpose", purpose},
                        {"files_count", kv.second}
                    });
                }
            }
        } catch (...) {
            // 目录树获取失败,跳过
        }
    }

    // 维度3: 核心贡献者画像
    if (fetch_top_contributors) {
        try {
            json contribs = get_contributors(owner, repo, max_contributors);
            if (contribs.is_array()) {
                for (auto& c : contribs) {
                    topContributors.push_back({
                        {"login", c.value("login", "")},
                        {"contributions", c.value("contributions", 0)},
                        {"html_url", c.value("html_url", "")}
                    });
                }
            }
        } catch (...) {
            // 贡献者获取失败,跳过
        }
    }

    return {
        {"success", true},
        {"repo_full_name", fullName},
        {"description", description},
        {"stars", stars},
        {"language", language},
        {"topics", topics},
        {"tech_stack", techStack},
        {"tech_blocks", techBlocks},
        {"top_contributors", topContributors},
        {"direct_dependencies", directDeps},
        {"dependency_count", directDeps.size()}
    };
}

// === 15. github_fetch_relation_network ===
json GitHubClient::fetch_relation_network(const std::string& owner,
                                          const std::string& repo,
                                          bool find_similar_repos,
                                          bool similar_by_tech_stack,
                                          bool similar_by_topic,
                                          int max_similar,
                                          bool explore_developer_links,
                                          int developer_depth) {
    if (max_similar < 1) max_similar = 1;
    if (max_similar > 20) max_similar = 20;
    if (developer_depth < 1) developer_depth = 1;
    if (developer_depth > 2) developer_depth = 2;

    // 先获取目标仓库详情(复用 fetch_repo_detail 的技术栈/topics 解析)
    json detail = fetch_repo_detail(owner, repo, true, false, true, true, 10);
    std::string fullName = detail.value("repo_full_name", owner + "/" + repo);
    json topics = detail.value("topics", json::array());
    json directDeps = detail.value("direct_dependencies", json::array());
    std::string language = detail.value("language", "");

    json similarRepos = json::array();
    json devRelatedRepos = json::array();

    // 维度1: 相似项目挖掘
    if (find_similar_repos) {
        // 构造检索 query: 策略是宽进严出
        // 1. 用 repo 名 + language 构造宽泛搜索(保证候选集非空)
        // 2. 跳过过长的 topic(>15 字符多为项目专属标签)
        // 3. 相似度排序交给后续 jaccard 计算
        std::string query = repo;
        if (similar_by_topic && topics.is_array()) {
            // 取第一个短 topic 作为补充关键词(避免 AND 过严)
            for (int i = 0; i < (int)topics.size(); ++i) {
                if (topics[i].is_string()) {
                    std::string t = topics[i].get<std::string>();
                    // 跳过 repo 名本身和过长的 topic
                    if (t == repo || t.size() > 15) continue;
                    query += " " + t;
                    break;  // 只取 1 个
                }
            }
        }
        if (similar_by_tech_stack && !language.empty()) {
            query += " language:" + language;
        }

        try {
            json candidates = search_repositories(query, "stars", "desc", max_similar * 3, 1);
            if (candidates.is_object() && candidates.contains("items") && candidates["items"].is_array()) {
                // 目标仓库的依赖集合(用于相似度计算)
                std::set<std::string> targetDeps = json_to_set(directDeps);
                std::set<std::string> targetTopics = json_to_set(topics);

                int added = 0;
                for (auto& cand : candidates["items"]) {
                    if (added >= max_similar) break;
                    std::string candName = cand.value("full_name", "");
                    if (candName == fullName) continue;  // 跳过自身

                    // 计算相似度
                    json candTopics = cand.contains("topics") ? cand["topics"] : json::array();
                    std::set<std::string> candTopicSet = json_to_set(candTopics);

                    double topicSim = similar_by_topic ? jaccard(targetTopics, candTopicSet) : 0.0;
                    // 技术栈相似度需要读依赖文件,成本高,简化为 language 匹配
                    double techSim = 0.0;
                    if (similar_by_tech_stack) {
                        std::string candLang = cand.value("language", "");
                        if (!language.empty() && !candLang.empty() &&
                            to_lower(language) == to_lower(candLang)) {
                            techSim = 0.6;  // 同语言给基础分
                        }
                    }
                    double totalScore = techSim * 0.5 + topicSim * 0.5;

                    similarRepos.push_back({
                        {"full_name", candName},
                        {"description", cand.value("description", "")},
                        {"stars", cand.value("stargazers_count", 0)},
                        {"similarity_score", std::round(totalScore * 1000) / 1000.0},
                        {"match_breakdown", {
                            {"tech_stack_overlap", std::round(techSim * 1000) / 1000.0},
                            {"topic_overlap", std::round(topicSim * 1000) / 1000.0}
                        }},
                        {"html_url", cand.value("html_url", "")}
                    });
                    ++added;
                }
            }
        } catch (...) {
            // 搜索失败,返回空
        }
    }

    // 维度2: 开发者关联网络挖掘
    if (explore_developer_links) {
        try {
            // 一级:目标仓库 top 贡献者
            json topContribs = detail.value("top_contributors", json::array());
            std::set<std::string> targetDevs;
            for (auto& c : topContribs) {
                if (c.is_object()) {
                    std::string login = c.value("login", "");
                    if (!login.empty()) targetDevs.insert(to_lower(login));
                }
            }

            // 对每个贡献者,搜索其参与的其他仓库(user:login 走 search_repositories)
            // 限制:仅取 top 5 贡献者,避免过多请求
            int devLimit = std::min((int)topContribs.size(), 5);
            // 关联项目 -> 共同开发者数
            std::map<std::string, int> repoSharedCount;
            std::map<std::string, json> repoMeta;

            for (int i = 0; i < devLimit; ++i) {
                std::string login = topContribs[i].value("login", "");
                if (login.empty()) continue;
                try {
                    // user:login 限定搜索该开发者的仓库
                    json devRepos = search_repositories("user:" + login, "updated", "desc", 10, 1);
                    if (devRepos.is_object() && devRepos.contains("items") && devRepos["items"].is_array()) {
                        for (auto& dr : devRepos["items"]) {
                            std::string drName = dr.value("full_name", "");
                            if (drName == fullName || drName.empty()) continue;
                            repoSharedCount[drName]++;
                            repoMeta[drName] = {
                                {"stars", dr.value("stargazers_count", 0)},
                                {"description", dr.value("description", "")}
                            };
                        }
                    }
                } catch (...) {
                    // 单个开发者查询失败,继续
                }
            }

            // 转换为数组并按共同开发者数排序
            std::vector<std::pair<std::string, int>> sortedRepos(repoSharedCount.begin(), repoSharedCount.end());
            std::sort(sortedRepos.begin(), sortedRepos.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

            int maxDevRepos = 15;
            int added = 0;
            for (auto& kv : sortedRepos) {
                if (added >= maxDevRepos) break;
                json meta = repoMeta.count(kv.first) ? repoMeta[kv.first] : json::object();
                devRelatedRepos.push_back({
                    {"full_name", kv.first},
                    {"shared_developers", kv.second},
                    {"relation_level", 1},
                    {"stars", meta.value("stars", 0)},
                    {"description", meta.value("description", "")}
                });
                ++added;
            }

            // 二级递进(developer_depth=2):取关联最强的项目,再查其贡献者
            // 为控制请求量,仅对 top 3 一级关联项目做二级扩散
            if (developer_depth >= 2 && !sortedRepos.empty()) {
                int level2Limit = std::min((int)sortedRepos.size(), 3);
                for (int i = 0; i < level2Limit; ++i) {
                    std::string l2Repo = sortedRepos[i].first;
                    // 解析 owner/repo
                    size_t slash = l2Repo.find('/');
                    if (slash == std::string::npos) continue;
                    std::string l2Owner = l2Repo.substr(0, slash);
                    std::string l2RepoName = l2Repo.substr(slash + 1);
                    try {
                        json l2Contribs = get_contributors(l2Owner, l2RepoName, 5);
                        if (!l2Contribs.is_array()) continue;
                        for (auto& c : l2Contribs) {
                            std::string login = c.value("login", "");
                            if (login.empty() || targetDevs.count(to_lower(login))) continue;
                            try {
                                json l2DevRepos = search_repositories("user:" + login, "updated", "desc", 5, 1);
                                if (l2DevRepos.is_object() && l2DevRepos.contains("items") && l2DevRepos["items"].is_array()) {
                                    for (auto& dr : l2DevRepos["items"]) {
                                        std::string drName = dr.value("full_name", "");
                                        if (drName == fullName || drName == l2Repo) continue;
                                        if (repoSharedCount.count(drName)) continue;  // 已在一级
                                        repoSharedCount[drName] = 1;
                                        devRelatedRepos.push_back({
                                            {"full_name", drName},
                                            {"shared_developers", 1},
                                            {"relation_level", 2},
                                            {"stars", dr.value("stargazers_count", 0)},
                                            {"description", dr.value("description", "")}
                                        });
                                        if (devRelatedRepos.size() >= 30) break;
                                    }
                                }
                            } catch (...) {}
                            if (devRelatedRepos.size() >= 30) break;
                        }
                    } catch (...) {}
                    if (devRelatedRepos.size() >= 30) break;
                }
            }
        } catch (...) {
            // 开发者关联挖掘失败,返回空
        }
    }

    return {
        {"success", true},
        {"repo_full_name", fullName},
        {"similar_repos", similarRepos},
        {"similar_count", similarRepos.size()},
        {"developer_related_repos", devRelatedRepos},
        {"developer_related_count", devRelatedRepos.size()}
    };
}

// ═══════════════════════════════════════════════════════════
//  局部对象连续动态分析索引 (next2)
// ═══════════════════════════════════════════════════════════

// 16. ingest_commit_file_timeline
// 拉取单 commit 详情,解析 files 数组,批量写入 file_timeline + file_cooccurrence
int GitHubClient::ingest_commit_file_timeline(const std::string& owner,
                                               const std::string& repo,
                                               const std::string& commit_hash) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready() || owner.empty() || repo.empty() || commit_hash.empty()) return 0;

    std::string repo_full = owner + "/" + repo;

    // GET /repos/{owner}/{repo}/commits/{commit_hash}
    // 返回 commit 详情(含 files 数组,每个元素含 filename/additions/deletions)
    // 403 二级限流(abuse detection)时指数退避重试,避免大量 commit 详情密集调用失败
    json commit_detail;
    const int MAX_RETRIES = 3;
    const int backoff_secs[MAX_RETRIES] = {10, 30, 60};
    bool rate_limited = false;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        try {
            commit_detail = http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                                     "/commits/" + url_encode(commit_hash));
            rate_limited = false;
            break;
        } catch (const GitHubAPIError& e) {
            if (e.status_code() == 403 || e.status_code() == 429) {
                if (attempt < MAX_RETRIES) {
                    rate_limited = true;
                    std::cerr << "[ingest] commit_detail 403/429 for " << commit_hash.substr(0, 8)
                              << ", backing off " << backoff_secs[attempt] << "s (attempt "
                              << (attempt + 1) << "/" << MAX_RETRIES << ")" << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(backoff_secs[attempt]));
                    continue;
                }
                last_ingest_error_ = std::string("commit_detail: HTTP 403 (rate limited after retries)");
                return -1;  // 信号:持续限流,调用方应中断整个 ingest
            }
            last_ingest_error_ = std::string("commit_detail: ") + e.what();
            return 0;
        } catch (const std::exception& e) {
            last_ingest_error_ = std::string("commit_detail: ") + e.what();
            return 0;
        }
    }
    if (!commit_detail.is_object()) return 0;

    // 解析 author_login
    // 优先 commit.author.login(GitHub 用户),退化到 commit.commit.author.name
    std::string author_login;
    if (commit_detail.contains("author") && commit_detail["author"].is_object()) {
        author_login = commit_detail["author"].value("login", "");
    }
    if (author_login.empty() && commit_detail.contains("commit") &&
        commit_detail["commit"].is_object() &&
        commit_detail["commit"].contains("author") &&
        commit_detail["commit"]["author"].is_object()) {
        author_login = commit_detail["commit"]["author"].value("name", "");
    }
    if (author_login.empty()) return 0;

    // 解析 commit_time (ISO8601 -> unix timestamp)
    int64_t commit_time = 0;
    if (commit_detail.contains("commit") && commit_detail["commit"].is_object() &&
        commit_detail["commit"].contains("author") &&
        commit_detail["commit"]["author"].is_object()) {
        std::string date_str = commit_detail["commit"]["author"].value("date", "");
        commit_time = parse_iso8601(date_str);
    }
    if (commit_time == 0) commit_time = (int64_t)std::time(nullptr);

    // 解析 commit_message (截断 500 字符防止 SQLite 过载)
    std::string commit_message;
    if (commit_detail.contains("commit") && commit_detail["commit"].is_object() &&
        commit_detail["commit"].contains("message")) {
        commit_message = commit_detail["commit"].value("message", "");
        if (commit_message.size() > 500) commit_message = commit_message.substr(0, 500);
    }

    // 解析 files 数组
    if (!commit_detail.contains("files") || !commit_detail["files"].is_array()) return 0;

    std::vector<std::tuple<std::string, int, int>> files;
    for (auto& f : commit_detail["files"]) {
        if (!f.is_object()) continue;
        std::string path = f.value("filename", "");
        if (path.empty()) continue;
        int add = f.value("additions", 0);
        int del = f.value("deletions", 0);
        files.emplace_back(path, add, del);
    }
    if (files.empty()) return 0;

    // 批量写入 file_timeline + file_cooccurrence
    // (record_commit_files 内部已实现 UNIQUE 幂等 + 协同配对更新)
    return cm.record_commit_files(repo_full, commit_hash, author_login,
                                   commit_time, files, 0, commit_message);
}

// 17. ingest_recent_commits_timeline
// 批量增量抓取近 N 天 commits,逐个写入 file_timeline + file_cooccurrence
// 增量策略:已入库的 commit 因 UNIQUE 约束会被 INSERT OR IGNORE 跳过,
//           但仍会发起一次 commit 详情请求(轻量,可接受)
int GitHubClient::ingest_recent_commits_timeline(const std::string& owner,
                                                  const std::string& repo,
                                                  int since_days,
                                                  const std::string& branch,
                                                  int max_commits,
                                                  const std::string& path) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready() || owner.empty() || repo.empty()) return 0;
    if (since_days <= 0 || max_commits <= 0) return 0;

    // 计算 since ISO8601
    int64_t now = (int64_t)std::time(nullptr);
    int64_t since_ts = now - (int64_t)since_days * 86400;
    std::string since_iso = format_iso8601(since_ts);

    int total_records = 0;
    int fetched = 0;
    int per_page = 100;  // GitHub 单页最多 100
    int page = 1;
    const int MAX_PAGES = 10;  // 防止无限翻页

    while (fetched < max_commits && page <= MAX_PAGES) {
        int remain = max_commits - fetched;
        int limit = std::min(remain, per_page);

        json commits;
        try {
            std::map<std::string, std::string> params;
            params["per_page"] = std::to_string(limit);
            params["since"] = since_iso;
            if (!branch.empty()) params["sha"] = branch;
            if (!path.empty()) params["path"] = path;  // 大仓库必传:仅拉取该路径相关 commits

            commits = http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                               "/commits", params);
        } catch (const std::exception& e) {
            last_ingest_error_ = std::string("commits_list: ") + e.what();
            break;
        }

        // 大仓库(如 torvalds/linux)精确文件路径过滤可能返回空(GitHub API 限制)
        // 回退到父目录路径重新查询,ingest_commit_file_timeline 会写入 commit 的所有
        // files(含目标文件),后续 query_file_timeline 精确匹配即可命中
        if (page == 1 && (!commits.is_array() || commits.empty()) && !path.empty() &&
            path.find('/') != std::string::npos) {
            std::string fallback_path = path;
            for (int level = 0; level < 3 && fallback_path.find('/') != std::string::npos; ++level) {
                size_t pos = fallback_path.find_last_of('/');
                fallback_path = fallback_path.substr(0, pos);
                if (fallback_path.empty()) break;
                try {
                    std::map<std::string, std::string> params;
                    params["per_page"] = std::to_string(limit);
                    params["since"] = since_iso;
                    if (!branch.empty()) params["sha"] = branch;
                    params["path"] = fallback_path;
                    commits = http_get("/repos/" + url_encode(owner) + "/" + url_encode(repo) +
                                       "/commits", params);
                    if (commits.is_array() && !commits.empty()) {
                        std::cerr << "[ingest] path fallback: \"" << path << "\" -> \""
                                  << fallback_path << "\" (" << commits.size()
                                  << " commits, original path returned empty on large repo)"
                                  << std::endl;
                        break;
                    }
                } catch (const std::exception&) {
                    break;  // 回退查询失败,不再继续
                }
            }
        }

        if (!commits.is_array() || commits.empty()) break;

        int batch_count = 0;
        int consecutive_failures = 0;
        for (auto& c : commits) {
            if (fetched >= max_commits) break;
            if (!c.is_object()) continue;
            std::string sha_hash = c.value("sha", "");
            if (sha_hash.empty()) continue;

            int n = ingest_commit_file_timeline(owner, repo, sha_hash);
            if (n < 0) {
                // 持续限流(403 重试耗尽),中断整个 ingest 避免继续触发 abuse detection
                std::cerr << "[ingest] persistent rate limit detected, aborting ingest at "
                          << fetched << "/" << max_commits << " commits" << std::endl;
                goto ingest_done;
            }
            if (n == 0 && !last_ingest_error_.empty()) {
                ++consecutive_failures;
                if (consecutive_failures >= 5) {
                    std::cerr << "[ingest] 5 consecutive failures, aborting ingest" << std::endl;
                    goto ingest_done;
                }
            } else {
                consecutive_failures = 0;
            }
            total_records += n;
            ++fetched;
            ++batch_count;

            // 节流:每次 commit 详情后 sleep 800ms,避免密集调用触发二级限流
            if (fetched < max_commits) {
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
        }

        // 不足一页,说明已到末尾
        if (batch_count < limit) break;
        ++page;
    }

ingest_done:
    return total_records;
}

// 18. module_timeline_analysis
// 三层职责统一入口: layer=1 轻量索引 / layer=2 定点深挖 / layer=3 二级递进
json GitHubClient::module_timeline_analysis(const std::string& owner,
                                            const std::string& repo,
                                            const std::string& target_type,
                                            const std::string& target_path,
                                            const std::string& module_name,
                                            const std::string& signature_regex,
                                            const std::string& time_range,
                                            int layer,
                                            bool ingest_first,
                                            const std::string& branch) {
    CacheManager& cm = CacheManager::instance();
    std::string repo_full = owner + "/" + repo;

    json result;
    result["repo_full_name"] = repo_full;
    result["target_type"]    = target_type;
    result["time_range"]     = time_range;
    result["layer"]          = layer;
    if (!branch.empty()) result["branch"] = branch;

    if (!cm.is_ready()) {
        result["error"] = "cache manager not ready";
        return result;
    }

    // 校验 target_type
    if (target_type != "file" && target_type != "module" && target_type != "signature") {
        result["error"] = "invalid target_type: must be file/module/signature";
        return result;
    }
    if (target_type == "file" && target_path.empty() && signature_regex.empty()) {
        result["error"] = "target_path or signature_regex required for target_type=file";
        return result;
    }
    if (target_type == "module" && module_name.empty()) {
        result["error"] = "module_name required for target_type=module "
                          "(note: parameter name is 'module_name', not 'target_module'; "
                          "if you passed target_module, retry with module_name instead)";
        return result;
    }
    if (target_type == "signature" && signature_regex.empty()) {
        result["error"] = "signature_regex required for target_type=signature";
        return result;
    }

    // 解析 time_range -> window_days
    int window_days = 365;
    if (time_range == "180d") window_days = 180;
    else if (time_range == "90d") window_days = 90;
    else if (time_range == "30d") window_days = 30;
    int64_t since_ts = (int64_t)std::time(nullptr) - (int64_t)window_days * 86400;

    // 增量抓取(可选):先填充 file_timeline,再做分析
    result["ingest_attempted"] = ingest_first;
    if (ingest_first) {
        last_ingest_error_.clear();
        int ingested = 0;
        try {
            // 限制单次最多 50 commits,避免长时间阻塞
            // 传入路径过滤:大仓库(linux)只拉该路径相关 commits,避免全量拉取超时
            // module 类型:module_name 看起来像路径(包含 /)时,用它作为 path 过滤
            std::string ingest_path = target_path;
            if (target_type == "module" && !module_name.empty() &&
                module_name.find('/') != std::string::npos) {
                ingest_path = module_name;
            }
            ingested = ingest_recent_commits_timeline(owner, repo, window_days, branch, 50, ingest_path);
            result["ingest_records"] = ingested;

            // 若模块表为空且 target_type=module,自动聚类(从 tree API 拉取文件列表)
            // target_type=file/signature 不需要 modules 表,跳过 tree 拉取避免大仓库超时
            // 注意:module_name 为路径时也跳过 tree(用前缀匹配替代,避免大仓库超时)
            if (target_type == "module" &&
                (module_name.empty() || module_name.find('/') == std::string::npos)) {
                auto existing_modules = cm.list_modules(repo_full);
                if (existing_modules.empty()) {
                    try {
                        json tree = get_tree_raw(owner, repo, branch.empty() ? "main" : branch, true);
                        std::vector<std::string> all_files;
                        if (tree.is_object() && tree.contains("tree") && tree["tree"].is_array()) {
                            for (auto& entry : tree["tree"]) {
                                if (entry.is_object() && entry.value("type", "") == "blob") {
                                    std::string p = entry.value("path", "");
                                    if (!p.empty()) all_files.push_back(p);
                                }
                            }
                        }
                        if (!all_files.empty()) cm.auto_cluster_modules(repo_full, all_files);
                    } catch (const std::exception& e) {
                        // tree 拉取失败(大仓库超时)不致命:module 分析降级,不影响 ingest 状态
                        std::cerr << "[ingest] tree auto-cluster skipped: " << e.what() << std::endl;
                        result["tree_warning"] = std::string("auto-cluster skipped: ") + e.what();
                    }
                }
            }
            // 触发预聚合(生成 module_contributor_agg)
            cm.aggregate_module_contributors(repo_full, window_days);
        } catch (const std::exception& e) {
            if (last_ingest_error_.empty())
                last_ingest_error_ = e.what();
        }

        // 报告 ingest 状态
        if (last_ingest_error_.empty() && ingested > 0) {
            result["ingest_status"] = "ok";
        } else if (last_ingest_error_.empty() && ingested == 0) {
            result["ingest_status"] = "no_new_commits";
        } else {
            result["ingest_status"] = "failed";
            result["ingest_error"] = last_ingest_error_;
        }
    }

    // === Layer 1: 轻量索引层(只返回候选列表) ===
    if (layer == 1) {
        json candidates = json::array();

        if (target_type == "file") {
            if (!target_path.empty()) {
                // 单个精确文件路径
                auto tl = cm.query_file_timeline(repo_full, target_path, since_ts, 0, 1000);
                if (!tl.empty()) {
                    json c;
                    c["file_path"]        = target_path;
                    c["change_count"]     = tl.size();
                    // 最后一次提交时间(tl 升序,取末尾)
                    c["last_commit_time"] = tl.back().value("commit_time", 0);
                    candidates.push_back(c);
                } else {
                    // 精确匹配为空时尝试目录前缀匹配(target_path 可能是目录)
                    auto tl_prefix = cm.query_file_timeline(repo_full, target_path,
                                                              since_ts, 0, 1000, true);
                    if (!tl_prefix.empty()) {
                        // 聚合前缀匹配到的所有文件
                        std::map<std::string, std::pair<int, int64_t>> file_summary;
                        for (auto& t : tl_prefix) {
                            std::string fp = t.value("file_path", "");
                            if (fp.empty()) continue;
                            int64_t ct = t.value("commit_time", 0);
                            auto it = file_summary.find(fp);
                            if (it == file_summary.end()) {
                                file_summary[fp] = {1, ct};
                            } else {
                                it->second.first++;
                                if (ct > it->second.second) it->second.second = ct;
                            }
                        }
                        for (auto& kv : file_summary) {
                            json c;
                            c["file_path"]        = kv.first;
                            c["change_count"]     = kv.second.first;
                            c["last_commit_time"] = kv.second.second;
                            c["matched_by"]       = "prefix_match";
                            candidates.push_back(c);
                        }
                    }
                }
            } else if (!signature_regex.empty()) {
                // 用 signature_regex 做 file_path 子串匹配过滤
                auto sigs = cm.search_by_signature(repo_full, signature_regex, since_ts, 1000);
                std::map<std::string, std::pair<int, int64_t>> file_summary;
                for (auto& s : sigs) {
                    std::string fp = s.value("file_path", "");
                    if (fp.empty()) continue;
                    int64_t ct = s.value("commit_time", 0);
                    auto it = file_summary.find(fp);
                    if (it == file_summary.end()) {
                        file_summary[fp] = {1, ct};
                    } else {
                        it->second.first++;
                        if (ct > it->second.second) it->second.second = ct;
                    }
                }
                for (auto& kv : file_summary) {
                    json c;
                    c["file_path"]        = kv.first;
                    c["change_count"]     = kv.second.first;
                    c["last_commit_time"] = kv.second.second;
                    c["matched_by"]       = "signature_regex";
                    candidates.push_back(c);
                }
            }
        } else if (target_type == "module") {
            auto files = cm.query_module_files(repo_full, module_name);
            for (auto& f : files) {
                auto tl = cm.query_file_timeline(repo_full, f, since_ts, 0, 1000);
                if (!tl.empty()) {
                    json c;
                    c["file_path"]        = f;
                    c["module_name"]      = module_name;
                    c["change_count"]     = tl.size();
                    c["last_commit_time"] = tl.back().value("commit_time", 0);
                    candidates.push_back(c);
                }
            }
            // module_def 表为空(大仓库 tree API 超时)且 module_name 像路径时,
            // 用 module_name 作为 path 前缀匹配 file_timeline(与 file 类型回退逻辑一致)
            if (candidates.empty() && !module_name.empty() &&
                module_name.find('/') != std::string::npos) {
                auto tl_prefix = cm.query_file_timeline(repo_full, module_name,
                                                         since_ts, 0, 1000, true);
                std::map<std::string, std::pair<int, int64_t>> file_summary;
                for (auto& t : tl_prefix) {
                    std::string fp = t.value("file_path", "");
                    if (fp.empty()) continue;
                    int64_t ct = t.value("commit_time", 0);
                    auto it = file_summary.find(fp);
                    if (it == file_summary.end()) {
                        file_summary[fp] = {1, ct};
                    } else {
                        it->second.first++;
                        if (ct > it->second.second) it->second.second = ct;
                    }
                }
                for (auto& kv : file_summary) {
                    json c;
                    c["file_path"]        = kv.first;
                    c["module_name"]      = module_name;
                    c["change_count"]     = kv.second.first;
                    c["last_commit_time"] = kv.second.second;
                    c["matched_by"]       = "prefix_match";
                    candidates.push_back(c);
                }
            }
        } else if (target_type == "signature") {
            auto sigs = cm.search_by_signature(repo_full, signature_regex, since_ts, 50);
            std::set<std::string> seen;
            for (auto& s : sigs) {
                std::string fp = s.value("file_path", "");
                if (fp.empty() || seen.count(fp)) continue;
                seen.insert(fp);
                json c;
                c["file_path"]        = fp;
                c["last_commit_time"] = s.value("commit_time", 0);
                candidates.push_back(c);
            }
        }

        result["candidates"]       = candidates;
        result["candidate_count"]  = candidates.size();

        // candidates 为空时补充诊断提示
        if (candidates.empty() && ingest_first) {
            if (result.value("ingest_status", "") == "failed") {
                result["warning"] = "candidates empty: ingest failed - " +
                                    result.value("ingest_error", "unknown");
            } else if (result.value("ingest_status", "") == "no_new_commits") {
                result["warning"] = "candidates empty: no commits found in the last " +
                                    std::to_string(window_days) + " days";
            } else {
                result["warning"] = "candidates empty: target may not match any "
                                    "ingested file (check exact path/type)";
            }
        }
        return result;
    }

    // === Layer 2: 定点深挖(完整时序数据集) ===
    json timeline         = json::array();
    json contributor_rank = json::array();
    json related_files    = json::array();
    json change_density   = json::array();

    if (target_type == "file") {
        if (!target_path.empty()) {
            // 单个精确文件路径
            timeline       = cm.query_file_timeline(repo_full, target_path, since_ts, 0, 500);
            change_density = cm.query_change_density(repo_full, target_path, since_ts, 24);
            related_files  = cm.query_related_files(repo_full, target_path, 2, 20);

            // 精确匹配为空时自动尝试目录前缀匹配
            // 场景1:target_path 是目录(如 "drivers/net"),file_timeline 表存的是具体文件
            //       路径(如 "drivers/net/eth0.c"),精确匹配返回空,用前缀匹配聚合子文件
            // 场景2:target_path 是深层文件(如 "drivers/net/ipv4/tcp_input.c"),GitHub API
            //       大仓库 path 过滤深度有限制,ingest 回退到 drivers/net 拉取 commits,
            //       但 50 个 commits 可能不含该文件,需用父目录前缀匹配找回相关数据
            if (timeline.empty()) {
                auto tl_prefix = cm.query_file_timeline(repo_full, target_path,
                                                         since_ts, 0, 500, true);
                std::string matched_prefix = target_path;

                // 直接前缀匹配也为空时,逐步向上回退 target_path 做前缀匹配
                if (tl_prefix.empty() && target_path.find('/') != std::string::npos) {
                    std::string fallback = target_path;
                    for (int level = 0; level < 3 && fallback.find('/') != std::string::npos; ++level) {
                        size_t pos = fallback.find_last_of('/');
                        fallback = fallback.substr(0, pos);
                        if (fallback.empty()) break;
                        auto tl_fb = cm.query_file_timeline(repo_full, fallback,
                                                             since_ts, 0, 500, true);
                        if (!tl_fb.empty()) {
                            tl_prefix = tl_fb;
                            matched_prefix = fallback;
                            result["prefix_match_fallback_from"] = target_path;
                            break;
                        }
                    }
                }

                if (!tl_prefix.empty()) {
                    timeline = tl_prefix;
                    result["prefix_match"] = true;
                    result["matched_path_prefix"] = matched_prefix + "/";
                    // 前缀匹配时 change_density/related_files 取第一个匹配文件作代表
                    std::string first_file = tl_prefix[0].value("file_path", "");
                    if (!first_file.empty()) {
                        change_density = cm.query_change_density(repo_full, first_file, since_ts, 24);
                        related_files  = cm.query_related_files(repo_full, first_file, 2, 20);
                    }
                }
            }
        } else if (!signature_regex.empty()) {
            // 用 signature_regex 做 file_path + commit_message 子串匹配,聚合所有匹配文件
            auto sigs = cm.search_by_signature(repo_full, signature_regex, since_ts, 2000);
            std::set<std::string> matched_files;
            for (auto& s : sigs) {
                timeline.push_back(s);
                std::string fp = s.value("file_path", "");
                if (!fp.empty()) matched_files.insert(fp);
            }
            // 每个匹配到的文件分别计算 change_density + related_files 合并
            std::map<std::string, int> density_month;
            std::map<std::string, int> related_count;
            for (auto& mf : matched_files) {
                auto cd = cm.query_change_density(repo_full, mf, since_ts, 24);
                for (auto& m : cd) {
                    std::string k = m.value("month", "");
                    int c         = m.value("changes", 0);
                    if (!k.empty()) density_month[k] += c;
                }
                auto rf = cm.query_related_files(repo_full, mf, 2, 20);
                for (auto& r : rf) {
                    std::string p = r.value("file_path", "");
                    int c         = r.value("co_change_count", 0);
                    if (!p.empty()) related_count[p] += c;
                }
            }
            for (auto& kv : density_month) {
                change_density.push_back({
                    {"month",   kv.first},
                    {"changes", kv.second}
                });
            }
            std::vector<std::pair<std::string, int>> rf_sorted(related_count.begin(), related_count.end());
            std::sort(rf_sorted.begin(), rf_sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            size_t rf_limit = rf_sorted.size() > 20 ? 20 : rf_sorted.size();
            for (size_t i = 0; i < rf_limit; ++i) {
                related_files.push_back({
                    {"file_path",       rf_sorted[i].first},
                    {"co_change_count", rf_sorted[i].second}
                });
            }
        }

        // 从 timeline 中聚合 contributor_rank
        std::map<std::string, int> user_changes;
        for (auto& t : timeline) {
            std::string u = t.value("author_login", "");
            if (!u.empty()) user_changes[u]++;
        }
        std::vector<std::pair<std::string, int>> sorted(user_changes.begin(), user_changes.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (auto& kv : sorted) {
            contributor_rank.push_back({
                {"user_login", kv.first},
                {"changes",    kv.second}
            });
        }
    } else if (target_type == "module") {
        auto files = cm.query_module_files(repo_full, module_name);
        // 聚合模块下所有文件的 timeline
        std::map<std::string, int> user_changes;
        for (auto& f : files) {
            auto tl = cm.query_file_timeline(repo_full, f, since_ts, 0, 500);
            for (auto& t : tl) {
                timeline.push_back(t);
                std::string u = t.value("author_login", "");
                if (!u.empty()) user_changes[u]++;
            }
        }

        // module_def 表为空(大仓库 tree API 超时)且 module_name 像路径时,
        // 用 module_name 作为 path 前缀匹配 file_timeline(与 file 类型回退逻辑一致)
        std::string module_matched_prefix;
        if (timeline.empty() && !module_name.empty() &&
            module_name.find('/') != std::string::npos) {
            auto tl_prefix = cm.query_file_timeline(repo_full, module_name,
                                                     since_ts, 0, 500, true);
            if (!tl_prefix.empty()) {
                module_matched_prefix = module_name + "/";
                for (auto& t : tl_prefix) {
                    timeline.push_back(t);
                    std::string u = t.value("author_login", "");
                    if (!u.empty()) user_changes[u]++;
                }
            }
        }

        // contributor_rank 优先从预聚合表读取(已按 total_changes 降序)
        auto mc = cm.query_module_contributors(repo_full, module_name, window_days, 30);
        if (!mc.empty()) {
            contributor_rank = mc;
        } else {
            // 预聚合为空时,用 timeline 聚合结果
            std::vector<std::pair<std::string, int>> sorted(user_changes.begin(), user_changes.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            for (auto& kv : sorted) {
                contributor_rank.push_back({
                    {"user_login",    kv.first},
                    {"total_changes", kv.second}
                });
            }
        }
        // related_files:取模块第一个文件的协同文件作为代表
        std::string first_file;
        if (!files.empty()) {
            first_file = files[0];
        } else if (!timeline.empty()) {
            first_file = timeline[0].value("file_path", "");
        }
        if (!first_file.empty()) {
            related_files = cm.query_related_files(repo_full, first_file, 2, 20);
            change_density = cm.query_change_density(repo_full, first_file, since_ts, 24);
        }
        if (!module_matched_prefix.empty()) {
            result["prefix_match"] = true;
            result["matched_path_prefix"] = module_matched_prefix;
        }
    } else if (target_type == "signature") {
        auto sigs = cm.search_by_signature(repo_full, signature_regex, since_ts, 100);
        timeline = sigs;
        // 聚合 contributor_rank
        std::map<std::string, int> user_changes;
        for (auto& s : sigs) {
            std::string u = s.value("author_login", "");
            if (!u.empty()) user_changes[u]++;
        }
        std::vector<std::pair<std::string, int>> sorted(user_changes.begin(), user_changes.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (auto& kv : sorted) {
            contributor_rank.push_back({
                {"user_login", kv.first},
                {"changes",    kv.second}
            });
        }
    }

    result["timeline"]         = timeline;
    result["timeline_count"]   = timeline.size();
    result["contributor_rank"] = contributor_rank;
    result["related_files"]    = related_files;
    result["change_density"]   = change_density;

    // timeline 为空时补充诊断提示
    if (timeline.empty() && ingest_first) {
        if (result.value("ingest_status", "") == "failed") {
            result["warning"] = "timeline empty: ingest failed - " +
                                result.value("ingest_error", "unknown");
        } else if (result.value("ingest_status", "") == "no_new_commits") {
            result["warning"] = "timeline empty: no commits found in the last " +
                                std::to_string(window_days) + " days";
        } else {
            result["warning"] = "timeline empty: target_path may not match any "
                                "ingested file (check exact path)";
        }
    }

    // === Layer 3: 二级递进(向外扩散) ===
    if (layer >= 3) {
        json developer_modules = json::object();
        json coupled_clusters  = json::array();

        // 对 contributor_rank 中的 top 开发者,查询其在该仓库改动的其他模块
        int dev_limit = std::min((int)contributor_rank.size(), 5);
        for (int i = 0; i < dev_limit; ++i) {
            std::string user = contributor_rank[i].value("user_login", "");
            if (user.empty()) continue;
            auto mods = cm.query_developer_modules(repo_full, user, window_days, 30);
            if (!mods.empty()) developer_modules[user] = mods;
        }

        // coupled_clusters:从 related_files 扩散(简化:直接返回协同文件列表)
        for (auto& rf : related_files) {
            coupled_clusters.push_back(rf);
        }

        result["developer_modules"] = developer_modules;
        result["coupled_clusters"]  = coupled_clusters;
    }

    return result;
}

// ── 原语A:子模块拆分时序切片 ────────────────────────────────
json GitHubClient::subdir_timeline_slice(const std::string& owner,
                                          const std::string& repo,
                                          const std::string& root_path,
                                          const std::string& time_range,
                                          bool ingest_first,
                                          const std::string& branch) {
    CacheManager& cm = CacheManager::instance();
    std::string repo_full = owner + "/" + repo;

    json result;
    result["repo_full_name"] = repo_full;
    result["root_path"]      = root_path;
    result["time_range"]     = time_range;
    if (!branch.empty()) result["branch"] = branch;

    if (!cm.is_ready()) {
        result["error"] = "cache manager not ready";
        return result;
    }
    if (root_path.empty()) {
        result["error"] = "root_path required (e.g. drivers/usb)";
        return result;
    }

    // 解析 time_range
    int window_days = 365;
    if (time_range == "180d") window_days = 180;
    else if (time_range == "90d") window_days = 90;
    else if (time_range == "30d") window_days = 30;
    int64_t since_ts = (int64_t)std::time(nullptr) - (int64_t)window_days * 86400;

    // 增量抓取(复用 module_timeline_analysis 的 path 过滤逻辑)
    result["ingest_attempted"] = ingest_first;
    if (ingest_first) {
        try {
            int ingested = ingest_recent_commits_timeline(owner, repo, window_days,
                                                           branch, 50, root_path);
            result["ingest_records"] = ingested;
        } catch (const std::exception& e) {
            result["ingest_error"] = e.what();
        }
    }

    // 查询子目录变更密度
    auto subdirs = cm.query_subdir_change_density(repo_full, root_path, since_ts, 24);
    result["subdir_count"] = subdirs.size();
    result["subdirs"]      = subdirs;

    // 汇总统计
    int total_changes = 0;
    int total_subdirs = (int)subdirs.size();
    std::string hottest_subdir;
    int hottest_changes = 0;
    for (auto& sd : subdirs) {
        int ch = sd.value("total_changes", 0);
        total_changes += ch;
        if (ch > hottest_changes) {
            hottest_changes = ch;
            hottest_subdir  = sd.value("subdir", "");
        }
    }
    result["summary"] = {
        {"total_subdirs",     total_subdirs},
        {"total_changes",     total_changes},
        {"hottest_subdir",    hottest_subdir},
        {"hottest_changes",   hottest_changes},
        {"window_days",       window_days}
    };

    return result;
}

// ── 原语B:维护链路归因分析 ─────────────────────────────────
json GitHubClient::maintenance_attribution(const std::string& owner,
                                            const std::string& repo,
                                            const std::string& target_path,
                                            const std::string& time_range,
                                            bool ingest_first,
                                            const std::string& branch) {
    CacheManager& cm = CacheManager::instance();
    std::string repo_full = owner + "/" + repo;

    json result;
    result["repo_full_name"] = repo_full;
    result["target_path"]    = target_path;
    result["time_range"]     = time_range;
    if (!branch.empty()) result["branch"] = branch;

    if (!cm.is_ready()) {
        result["error"] = "cache manager not ready";
        return result;
    }
    if (target_path.empty()) {
        result["error"] = "target_path required (e.g. drivers/usb)";
        return result;
    }

    // 解析 time_range
    int window_days = 365;
    if (time_range == "180d") window_days = 180;
    else if (time_range == "90d") window_days = 90;
    else if (time_range == "30d") window_days = 30;
    int64_t since_ts = (int64_t)std::time(nullptr) - (int64_t)window_days * 86400;

    // 增量抓取
    result["ingest_attempted"] = ingest_first;
    if (ingest_first) {
        try {
            int ingested = ingest_recent_commits_timeline(owner, repo, window_days,
                                                           branch, 50, target_path);
            result["ingest_records"] = ingested;
        } catch (const std::exception& e) {
            result["ingest_error"] = e.what();
        }
    }

    // 查询维护链路归因
    auto contributors = cm.query_maintenance_attribution(repo_full, target_path,
                                                          since_ts, 50);
    result["contributor_count"] = contributors.size();
    result["contributors"]      = contributors;

    // 拆分维护者和开发者
    json maintainers = json::array();
    json developers  = json::array();
    int total_dev_commits   = 0;
    int total_merge_commits = 0;
    for (auto& c : contributors) {
        if (c.value("role", "") == "maintainer") {
            maintainers.push_back(c);
        } else {
            developers.push_back(c);
        }
        total_dev_commits   += c.value("dev_commits", 0);
        total_merge_commits += c.value("merge_commits", 0);
    }

    result["maintainers"]        = maintainers;
    result["developers"]         = developers;
    result["maintainer_count"]   = maintainers.size();
    result["developer_count"]    = developers.size();
    result["total_dev_commits"]  = total_dev_commits;
    result["total_merge_commits"] = total_merge_commits;

    // 查询 merge commit 样本(用于展示维护流水线)
    auto timeline = cm.query_file_timeline(repo_full, target_path, since_ts, 0, 200, true);
    json merge_samples = json::array();
    for (auto& t : timeline) {
        std::string msg = t.value("commit_message", "");
        if (msg.rfind("Merge", 0) == 0) {
            json sample;
            sample["commit_hash"]    = t.value("commit_hash", "");
            sample["author_login"]   = t.value("author_login", "");
            sample["commit_time"]    = t.value("commit_time", 0);
            sample["commit_message"] = msg;
            sample["file_path"]      = t.value("file_path", "");
            merge_samples.push_back(sample);
            if (merge_samples.size() >= 10) break;  // 最多 10 个样本
        }
    }
    result["merge_samples"] = merge_samples;

    // 维护流水线摘要
    result["pipeline_summary"] = {
        {"total_commits",        total_dev_commits + total_merge_commits},
        {"dev_commit_ratio",     (total_dev_commits + total_merge_commits) > 0
                                 ? (double)total_dev_commits / (total_dev_commits + total_merge_commits)
                                 : 0.0},
        {"merge_commit_ratio",   (total_dev_commits + total_merge_commits) > 0
                                 ? (double)total_merge_commits / (total_dev_commits + total_merge_commits)
                                 : 0.0},
        {"maintainer_count",     maintainers.size()},
        {"developer_count",      developers.size()},
        {"window_days",          window_days}
    };

    return result;
}

} // namespace github_research
