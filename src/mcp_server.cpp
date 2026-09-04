#include "github_research/mcp_server.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/errors.hpp"
#include "github_research/http_server.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/focus_web_search.hpp"
#include "github_research/arxiv_tools.hpp"
#include "github_research/hackernews_tools.hpp"
#include "github_research/research_deep_dive.hpp"
#include "github_research/package_tools.hpp"
#include "github_research/paperswithcode_tools.hpp"
#include "github_research/huggingface_tools.hpp"
#include "github_research/focus_tools.hpp"
#include "github_research/semanticscholar_tools.hpp"
#include "github_research/stackoverflow_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <chrono>
#include <iomanip>

namespace github_research {

// ============ 调试日志辅助(带毫秒时间戳) ============
// 用法: DBG_LOG("hn") << "msg"; 会在 stderr 输出 [12:34:56.789][hn] msg
struct DbgLog {
    DbgLog(const char* tag) {
        auto now = std::chrono::system_clock::now();
        auto t  = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::cerr << "[" << std::put_time(&tmv, "%H:%M:%S") << "."
                  << std::setfill('0') << std::setw(3) << ms.count() << "]["
                  << tag << "] ";
    }
    ~DbgLog() { std::cerr << std::endl; }

    template <typename T>
    DbgLog& operator<<(const T& v) { std::cerr << v; return *this; }
};
#define DBG_LOG(tag) DbgLog(tag)

// tools.cpp 中实现的 GitHub 工具分发函数
json dispatch_tool_call(GitHubClient& client, const json& params);

McpServer::McpServer(std::optional<std::string> token, int timeout_seconds)
    : client_(token, timeout_seconds) {
    // 注册 GitHub 数据源 + source_fetch 回调
    // entity_key = "owner/repo",调用 get_repo_info 抓取并提取 fields
    CacheManager& cm = CacheManager::instance();
    cm.register_source("github_api", "api", "https://api.github.com",
                        0.9, 200, 5000, 1.0, "{}");
    GitHubClient* client_ptr = &client_;
    cm.register_source_fetch("github_api",
        [client_ptr](const std::string& entity_key) -> std::map<std::string, json> {
            // 解析 entity_key = "owner/repo"
            size_t slash = entity_key.find('/');
            if (slash == std::string::npos || slash == 0 || slash == entity_key.size() - 1) {
                throw std::runtime_error("invalid github entity_key, expected owner/repo");
            }
            std::string owner = entity_key.substr(0, slash);
            std::string repo = entity_key.substr(slash + 1);
            json repo_info = client_ptr->get_repo_info(owner, repo);
            if (!repo_info.is_object()) {
                throw std::runtime_error("github fetch: empty repo_info");
            }
            CleanerPipeline cleaner;
            return cleaner.clean(repo_info, "github_api");
        });
}

McpServer::~McpServer() {
    shutdown_arxiv();
    shutdown_hackernews();
    shutdown_package();
    shutdown_paperswithcode();
    shutdown_huggingface();
    shutdown_semanticscholar();
    shutdown_stackoverflow();
}

void McpServer::set_proxy(const std::string& proxy_url) {
    proxy_url_ = proxy_url;
    client_.set_proxy(proxy_url);
}

// ============ 通用 init/shutdown 辅助 ============
bool McpServer::init_session(std::unique_ptr<WebViewSession>& session,
                              const std::wstring& userDataDir,
                              const std::string& proxy_url,
                              const char* logName) {
    if (session) return true;  // 已初始化
    std::string effective_proxy = proxy_url.empty() ? proxy_url_ : proxy_url;

    DBG_LOG(logName) << "init_session: creating WebViewSession, proxy=" << effective_proxy;
    session = std::make_unique<WebViewSession>();
    DBG_LOG(logName) << "init_session: calling session->Init ...";
    HRESULT hr = session->Init(userDataDir, L"", effective_proxy);
    if (FAILED(hr)) {
        DBG_LOG(logName) << "init_session: Init FAILED: 0x" << std::hex << hr;
        session.reset();
        return false;
    }
    DBG_LOG(logName) << "init_session: Init OK, session ready";
    log(std::string(logName) + " session ready");
    return true;
}

void McpServer::shutdown_session(std::unique_ptr<WebViewSession>& session,
                                  const char* logName) {
    if (session) {
        log(std::string("shutting down ") + logName + " session");
        session->Destroy();
        session.reset();
    }
}

// ============ 各源 init/shutdown ============
bool McpServer::init_arxiv(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(arxiv_session_, userDataDir, proxy_url, "arXiv");
    if (ok) {
        // 注册 arXiv 数据源 + source_fetch 回调
        CacheManager& cm = CacheManager::instance();
        cm.register_source("arxiv_web", "web_scrape", "https://arxiv.org",
                            0.85, 1500, 100, 0.9, "{}");
        // 回调:entity_key = arxiv_id,调用 ToolArxivFetchPaperDetail 抓取并提取 fields
        // 注意:session 生命周期与 McpServer 一致,捕获裸指针安全
        WebViewSession* session_ptr = arxiv_session_.get();
        cm.register_source_fetch("arxiv_web",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("arxiv session not initialized");
                json args = {{"arxiv_id", entity_key}, {"fetch_full_text", false}};
                json result = ToolArxivFetchPaperDetail(*session_ptr, args);
                // 从 MCP 包装结果中提取 payload
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            // 转换为 fields map(用 CleanerPipeline 的字段映射)
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "arxiv_web");
                        } catch (...) {
                            throw std::runtime_error("arxiv fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("arxiv fetch: empty result");
            });
        log("arxiv source_fetch callback registered");
    }
    return ok;
}
void McpServer::shutdown_arxiv() { shutdown_session(arxiv_session_, "arXiv"); }

bool McpServer::init_hackernews(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(hn_session_, userDataDir, proxy_url, "HackerNews");
    if (ok) {
        // 注册 HN 数据源 + source_fetch 回调
        CacheManager& cm = CacheManager::instance();
        cm.register_source("hn_web", "web_scrape", "https://news.ycombinator.com",
                            0.8, 800, 200, 0.8, "{}");
        // 回调:entity_key = hn_id,调用 ToolHnFetchDetailedStory 抓取并提取 fields
        WebViewSession* session_ptr = hn_session_.get();
        cm.register_source_fetch("hn_web",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("hn session not initialized");
                json args = {{"hn_id", entity_key},
                             {"fetch_external_article", false},
                             {"fetch_comments", true}};
                json result = ToolHnFetchDetailedStory(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "hn_web");
                        } catch (...) {
                            throw std::runtime_error("hn fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("hn fetch: empty result");
            });
        log("hn source_fetch callback registered");
    }
    return ok;
}
void McpServer::shutdown_hackernews() { shutdown_session(hn_session_, "HackerNews"); }

bool McpServer::init_package(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(pkg_session_, userDataDir, proxy_url, "Package");
    if (ok) {
        // 注册 npm + pypi 两个数据源 + source_fetch 回调
        CacheManager& cm = CacheManager::instance();
        cm.register_source("npm_registry", "api", "https://registry.npmjs.org",
                            0.85, 300, 100000, 0.8, "{}");
        cm.register_source("pypi_registry", "api", "https://pypi.org",
                            0.85, 300, 100000, 0.8, "{}");
        WebViewSession* session_ptr = pkg_session_.get();
        // npm 回调:entity_key = 包名(不含 registry 前缀)
        cm.register_source_fetch("npm_registry",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("pkg session not initialized");
                json args = {{"registry", "npm"}, {"name", entity_key}};
                json result = ToolPkgFetchDetail(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "npm_registry");
                        } catch (...) {
                            throw std::runtime_error("npm fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("npm fetch: empty result");
            });
        // pypi 回调
        cm.register_source_fetch("pypi_registry",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("pkg session not initialized");
                json args = {{"registry", "pypi"}, {"name", entity_key}};
                json result = ToolPkgFetchDetail(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "pypi_registry");
                        } catch (...) {
                            throw std::runtime_error("pypi fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("pypi fetch: empty result");
            });
        log("pkg source_fetch callback registered (npm + pypi)");
    }
    return ok;
}
void McpServer::shutdown_package() { shutdown_session(pkg_session_, "Package"); }

bool McpServer::init_paperswithcode(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(pwc_session_, userDataDir, proxy_url, "PapersWithCode");
    if (ok) {
        CacheManager& cm = CacheManager::instance();
        cm.register_source("pwc_web", "web_scrape", "https://paperswithcode.com",
                            0.85, 1500, 100, 0.9, "{}");
        WebViewSession* session_ptr = pwc_session_.get();
        cm.register_source_fetch("pwc_web",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("pwc session not initialized");
                json args = {{"paper_id", entity_key}};
                json result = ToolPwcFetchPaperDetail(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "pwc_web");
                        } catch (...) {
                            throw std::runtime_error("pwc fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("pwc fetch: empty result");
            });
        log("pwc source_fetch callback registered");
    }
    return ok;
}
void McpServer::shutdown_paperswithcode() { shutdown_session(pwc_session_, "PapersWithCode"); }

bool McpServer::init_huggingface(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(hf_session_, userDataDir, proxy_url, "HuggingFace");
    if (ok) {
        CacheManager& cm = CacheManager::instance();
        cm.register_source("hf_web", "web_scrape", "https://huggingface.co",
                            0.85, 1200, 200, 0.9, "{}");
        WebViewSession* session_ptr = hf_session_.get();
        // model 回调:entity_key = model_id(如 "bert-base-uncased")
        cm.register_source_fetch("hf_web",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("hf session not initialized");
                // 若 entity_key 以 "dataset:" 开头,走 dataset 回调,否则走 model
                json args;
                if (entity_key.rfind("dataset:", 0) == 0) {
                    args = {{"dataset_id", entity_key.substr(8)}};
                    json result = ToolHfFetchDatasetDetail(*session_ptr, args);
                    if (result.contains("content") && result["content"].is_array() &&
                        !result["content"].empty()) {
                        const json& content = result["content"][0];
                        if (content.contains("text") && content["text"].is_string()) {
                            try {
                                json payload = json::parse(content["text"].get<std::string>());
                                CleanerPipeline cleaner;
                                return cleaner.clean(payload, "hf_web");
                            } catch (...) {
                                throw std::runtime_error("hf dataset fetch: payload parse failed");
                            }
                        }
                    }
                    throw std::runtime_error("hf dataset fetch: empty result");
                }
                args = {{"model_id", entity_key}};
                json result = ToolHfFetchModelDetail(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "hf_web");
                        } catch (...) {
                            throw std::runtime_error("hf model fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("hf model fetch: empty result");
            });
        log("hf source_fetch callback registered (model + dataset)");
    }
    return ok;
}
void McpServer::shutdown_huggingface() { shutdown_session(hf_session_, "HuggingFace"); }

bool McpServer::init_semanticscholar(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(s2_session_, userDataDir, proxy_url, "SemanticScholar");
    if (ok) {
        CacheManager& cm = CacheManager::instance();
        cm.register_source("s2_web", "web_scrape", "https://www.semanticscholar.org",
                            0.9, 1500, 100, 1.0, "{}");
        WebViewSession* session_ptr = s2_session_.get();
        cm.register_source_fetch("s2_web",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("s2 session not initialized");
                json args = {{"paper_id", entity_key}};
                json result = ToolS2FetchPaperDetail(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "s2_web");
                        } catch (...) {
                            throw std::runtime_error("s2 fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("s2 fetch: empty result");
            });
        log("s2 source_fetch callback registered");
    }
    return ok;
}
void McpServer::shutdown_semanticscholar() { shutdown_session(s2_session_, "SemanticScholar"); }

bool McpServer::init_stackoverflow(const std::wstring& userDataDir, const std::string& proxy_url) {
    bool ok = init_session(so_session_, userDataDir, proxy_url, "StackOverflow");
    if (ok) {
        CacheManager& cm = CacheManager::instance();
        cm.register_source("so_web", "web_scrape", "https://stackoverflow.com",
                            0.8, 800, 200, 0.8, "{}");
        WebViewSession* session_ptr = so_session_.get();
        cm.register_source_fetch("so_web",
            [session_ptr](const std::string& entity_key) -> std::map<std::string, json> {
                if (!session_ptr) throw std::runtime_error("so session not initialized");
                json args = {{"question_id", entity_key}};
                json result = ToolSoFetchQuestionDetail(*session_ptr, args);
                if (result.contains("content") && result["content"].is_array() &&
                    !result["content"].empty()) {
                    const json& content = result["content"][0];
                    if (content.contains("text") && content["text"].is_string()) {
                        try {
                            json payload = json::parse(content["text"].get<std::string>());
                            CleanerPipeline cleaner;
                            return cleaner.clean(payload, "so_web");
                        } catch (...) {
                            throw std::runtime_error("so fetch: payload parse failed");
                        }
                    }
                }
                throw std::runtime_error("so fetch: empty result");
            });
        log("so source_fetch callback registered");
    }
    return ok;
}
void McpServer::shutdown_stackoverflow() { shutdown_session(so_session_, "StackOverflow"); }

// ============ 懒加载:首次 tool 调用时按 profile 路径初始化对应会话 ============
namespace {
// UTF-8 string -> wstring(profile 路径)
inline std::wstring profile_to_wstr(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}
}  // namespace

bool McpServer::ensure_arxiv_session() {
    if (arxiv_session_) return true;
    if (profile_paths_.arxiv.empty()) return false;
    return init_arxiv(profile_to_wstr(profile_paths_.arxiv), proxy_url_);
}
bool McpServer::ensure_hn_session() {
    if (hn_session_) {
        DBG_LOG("hn") << "ensure_hn_session: already initialized";
        return true;
    }
    if (profile_paths_.hn.empty()) {
        DBG_LOG("hn") << "ensure_hn_session: FAIL, profile_paths_.hn is empty";
        return false;
    }
    DBG_LOG("hn") << "ensure_hn_session: first call, invoking init_hackernews ...";
    bool ok = init_hackernews(profile_to_wstr(profile_paths_.hn), proxy_url_);
    DBG_LOG("hn") << "ensure_hn_session: init_hackernews returned " << (ok ? "true" : "false");
    return ok;
}
bool McpServer::ensure_pkg_session() {
    if (pkg_session_) return true;
    if (profile_paths_.pkg.empty()) return false;
    return init_package(profile_to_wstr(profile_paths_.pkg), proxy_url_);
}
bool McpServer::ensure_pwc_session() {
    if (pwc_session_) return true;
    if (profile_paths_.pwc.empty()) return false;
    return init_paperswithcode(profile_to_wstr(profile_paths_.pwc), proxy_url_);
}
bool McpServer::ensure_hf_session() {
    if (hf_session_) return true;
    if (profile_paths_.hf.empty()) return false;
    return init_huggingface(profile_to_wstr(profile_paths_.hf), proxy_url_);
}
bool McpServer::ensure_s2_session() {
    if (s2_session_) return true;
    if (profile_paths_.s2.empty()) return false;
    return init_semanticscholar(profile_to_wstr(profile_paths_.s2), proxy_url_);
}
bool McpServer::ensure_so_session() {
    if (so_session_) return true;
    if (profile_paths_.so.empty()) return false;
    return init_stackoverflow(profile_to_wstr(profile_paths_.so), proxy_url_);
}

// ============ arXiv 工具分发(6 个工具: 4 原始 + 2 分层) ============
json McpServer::dispatch_arxiv_tool(const std::string& tool_name, const json& args) {
    // arxiv_get_pdf_link 是纯 ID 规则拼接,零网络请求,允许在无会话时使用
    if (tool_name == "arxiv_get_pdf_link") {
        try {
            return ToolArxivGetPdfLink(args);
        } catch (const std::exception& e) {
            return {
                {"content", json::array({{{"type", "text"}, {"text", std::string("ERROR: arxiv_get_pdf_link exception: ") + e.what()}}})},
                {"isError", true}
            };
        }
    }
    // 其余 5 个工具(搜索/详情/连通性/索引/深挖)需要浏览器会话
    if (!ensure_arxiv_session()) {
        return {
            {"content", json::array({{{"type", "text"}, {"text", "ERROR: arXiv session not initialized. Start server with --arxiv-profile <DIR>."}}})},
            {"isError", true}
        };
    }
    try {
        if (tool_name == "arxiv_search_papers")
            return ToolArxivSearchPapers(*arxiv_session_, args);
        if (tool_name == "arxiv_get_paper_detail")
            return ToolArxivGetPaperDetail(*arxiv_session_, args);
        if (tool_name == "arxiv_check_available")
            return ToolArxivCheckAvailable(*arxiv_session_, args);
        if (tool_name == "arxiv_search_index")
            return ToolArxivSearchIndex(*arxiv_session_, args);
        if (tool_name == "arxiv_fetch_paper_detail")
            return ToolArxivFetchPaperDetail(*arxiv_session_, args);
    } catch (const std::exception& e) {
        return {
            {"content", json::array({{{"type", "text"}, {"text", std::string("ERROR: arxiv tool exception: ") + e.what()}}})},
            {"isError", true}
        };
    }
    return {
        {"content", json::array({{{"type", "text"}, {"text", "ERROR: unknown arxiv tool: " + tool_name}}})},
        {"isError", true}
    };
}

// ============ Hacker News 工具分发(7 个 hn_* 工具) ============
// 索引类工具(top/new/best/latest_index): 纯 Firebase REST,不需要 WebView2
// 深度类工具(item/search/fetch_detailed): 需要 WebView2(导航 HN 页面/外部文章)
json McpServer::dispatch_hn_tool(const std::string& tool_name, const json& args) {
    DBG_LOG("hn") << "dispatch_hn_tool: tool=" << tool_name << " args=" << args.dump().substr(0, 200);

    // ---- 索引类: 纯 Firebase,不需要 hn_session_ ----
    // fetch_stories_with_fallback 内部:
    //   1. Firebase API 优先(纯 curl,不需要 WebView2)
    //   2. WebView2 兜底(仅当 session.IsReady() 时才尝试)
    // 所以即使没有 --hn-profile,索引工具也能正常工作
    if (tool_name == "hn_get_top_stories" ||
        tool_name == "hn_get_new_stories" ||
        tool_name == "hn_get_best_stories" ||
        tool_name == "hn_get_latest_index") {
        try {
            static WebViewSession dummy_session;  // 未 Init,IsReady()=false
            WebViewSession& sess = hn_session_ ? *hn_session_ : dummy_session;
            if (tool_name == "hn_get_top_stories")      return ToolHnGetTopStories(sess, args);
            if (tool_name == "hn_get_new_stories")      return ToolHnGetNewStories(sess, args);
            if (tool_name == "hn_get_best_stories")     return ToolHnGetBestStories(sess, args);
            if (tool_name == "hn_get_latest_index")     return ToolHnGetLatestIndex(sess, args);
        } catch (const std::exception& e) {
            DBG_LOG("hn") << "dispatch_hn_tool (index): exception: " << e.what();
            return McpError(std::string("ERROR: hn tool exception: ") + e.what());
        }
    }

    // ---- 深度类: 必须有 WebView2 ----
    if (!ensure_hn_session()) {
        DBG_LOG("hn") << "dispatch_hn_tool: ensure_hn_session failed for deep tool";
        return McpError("ERROR: HackerNews session not initialized. Start with --hn-profile <DIR>.");
    }
    try {
        if (tool_name == "hn_get_item")              return ToolHnGetItem(*hn_session_, args);
        if (tool_name == "hn_search_by_keyword")     return ToolHnSearchByKeyword(*hn_session_, args);
        if (tool_name == "hn_fetch_detailed_story")  return ToolHnFetchDetailedStory(*hn_session_, args);
    } catch (const std::exception& e) {
        DBG_LOG("hn") << "dispatch_hn_tool (deep): exception: " << e.what();
        return McpError(std::string("ERROR: hn tool exception: ") + e.what());
    }
    DBG_LOG("hn") << "dispatch_hn_tool: unknown tool: " << tool_name;
    return McpError("ERROR: unknown hn tool: " + tool_name);
}

// ============ 跨源 DeepDive 工具 (research_* 前缀, 可选 HN session) ============
json McpServer::dispatch_research_tool(const std::string& tool_name, const json& args) {
    DBG_LOG("dd") << "dispatch_research_tool: tool=" << tool_name;

    // 读取 mode 参数: auto(默认) | hn | general
    std::string mode = "auto";
    if (args.contains("mode") && args["mode"].is_string()) {
        mode = args["mode"].get<std::string>();
    }

    // 根据 mode 决定是否需要 HN session
    WebViewSession* usable_session = nullptr;
    if (mode == "general") {
        // general 模式: 不依赖 HN,可以没有 WebView session
        // 如果有其他 WebView session(比如 arxiv),也可以传过来做页面抓取
        // 但为了简化,general 模式默认不使用 WebView,让 deep_dive 内部跳过网页抓取
        DBG_LOG("dd") << "mode=general: no HN session required";
    } else {
        // auto 或 hn: 尝试 HN session
        if (ensure_hn_session()) {
            usable_session = hn_session_.get();
        } else {
            if (mode == "hn") {
                return McpError(
                    "ERROR: research_deep_dive mode='hn' requires --hn-profile <DIR>. "
                    "Use mode='general' for cross-discipline academic topics without HN dependency.");
            }
            // auto 模式下没有 HN session → 降级 general
            DBG_LOG("dd") << "mode=auto but no HN session, degrading to general mode";
        }
    }

    try {
        if (tool_name == "research_deep_dive") {
            DBG_LOG("dd") << "calling ToolResearchDeepDive, usable_session="
                         << (usable_session ? "yes" : "null")
                         << ", mode=" << mode;
            auto r = ToolResearchDeepDive(usable_session, args);
            DBG_LOG("dd") << "ToolResearchDeepDive done, result size=" << r.dump().size();
            return r;
        }
    } catch (const std::exception& e) {
        DBG_LOG("dd") << "dispatch_research_tool exception: " << e.what();
        return McpError(std::string("ERROR: research tool exception: ") + e.what());
    }
    DBG_LOG("dd") << "dispatch_research_tool: unknown tool: " << tool_name;
    return McpError("ERROR: unknown research tool: " + tool_name);
}

// ============ Wiki tools (9-source unified search) ============
void McpServer::init_datasource_registry() {
    if (datasource_initialized_) return;
    datasource_initialized_ = true;

    DBG_LOG("wiki") << "init_datasource_registry: creating 9 sources";

    // Create shared HTTP client for HTTP-based sources
    if (!shared_http_client_) {
        shared_http_client_ = std::make_unique<CurlHttpClient>("research-mcp/9source", 30);
        if (!shared_http_client_->initialize()) {
            DBG_LOG("wiki") << "shared_http_client_ init failed, HTTP sources will be unavailable";
            shared_http_client_.reset();
        }
    }

    datasource_registry_ = std::make_unique<DataSourceRegistry>();

    // Source 1: Kiwix (priority 1, offline)
    if (!kiwix_url_.empty()) {
        datasource_registry_->register_source(
            std::make_unique<KiwixSource>(kiwix_url_, shared_http_client_.get()));
        DBG_LOG("wiki") << "registered kiwix_local: " << kiwix_url_;
    } else {
        DBG_LOG("wiki") << "skipped kiwix_local: no --kiwix-url / KIWIX_SERVER_URL configured";
    }

    // Source 2: GitRaw (priority 2)
    datasource_registry_->register_source(
        std::make_unique<GitRawSource>(shared_http_client_.get()));

    // Source 3: GithubWiki (priority 3)
    datasource_registry_->register_source(
        std::make_unique<GithubWikiSource>(shared_http_client_.get()));

    // Source 4: WebCrawler (priority 4) — uses hn_session_ if available
    WebViewSession* crawl_session = hn_session_ ? hn_session_.get() : nullptr;
    datasource_registry_->register_source(
        std::make_unique<WebCrawlerSource>(crawl_session));

    // Source 5: GithubApi (priority 5)
    datasource_registry_->register_source(
        std::make_unique<GithubApiSource>(&client_));

    // Source 6: Arxiv (priority 6) — uses arxiv_session_ if available
    WebViewSession* arxiv_session = arxiv_session_ ? arxiv_session_.get() : nullptr;
    datasource_registry_->register_source(
        std::make_unique<ArxivSource>(arxiv_session));

    // Source 7: WebSearch (priority 7) — uses hn_session_ if available
    datasource_registry_->register_source(
        std::make_unique<WebSearchSource>(crawl_session));

    // Source 8: LocalFs (priority 8)
    datasource_registry_->register_source(
        std::make_unique<LocalFsSource>());

    // Source 9: GitClone (priority 9)
    datasource_registry_->register_source(
        std::make_unique<GitCloneSource>());

    // Create WikiExplorer
    wiki_explorer_ = std::make_unique<WikiExplorer>(*datasource_registry_,
                                                       CacheManager::instance());

    DBG_LOG("wiki") << "datasource registry ready: " << datasource_registry_->size() << " sources";
}

json McpServer::dispatch_wiki_tool(const std::string& tool_name, const json& args) {
    init_datasource_registry();

    if (!wiki_explorer_) {
        return McpError("ERROR: Wiki explorer not initialized.");
    }

    DBG_LOG("wiki") << "dispatch_wiki_tool: " << tool_name;
    try {
        if (tool_name == "wiki_discover") {
            auto res = wiki_explorer_->discover(args);

            // 🔧 修复:本地源全部为空且有 query 时,自动 fallback 到 web_search
            if (res["resources"].empty() &&
                res.value("query", "").size() > 0) {
                DBG_LOG("wiki") << "wiki_discover empty, falling back to web_search for: "
                                << res["query"].get<std::string>();
                json search_args = json::object();
                search_args["query"] = res["query"];
                search_args["max_results"] = 5;
                json search_res = dispatch_focus_tool("web_search", search_args);

                if (!search_res.value("isError", false) &&
                    search_res.contains("content") && !search_res["content"].empty() &&
                    search_res["content"][0].contains("text")) {
                    try {
                        auto search_text = json::parse(
                            search_res["content"][0]["text"].get<std::string>());
                        for (const auto& r : search_text.value("results", json::array())) {
                            res["resources"].push_back({
                                {"canonical_uri", r.value("url", "")},
                                {"title", r.value("title", "")},
                                {"resource_kind", "web_search"},
                                {"source_id", r.value("source_engine", "web_search")},
                                {"is_local", false},
                                {"snippet", r.value("snippet", "")}
                            });
                        }
                        res["warnings"].push_back(
                            "wiki sources returned empty, auto-fell back to web_search");
                    } catch (...) {
                        DBG_LOG("wiki") << "web_search fallback parse failed";
                    }
                }
            }

            return McpSuccess(res);
        }
        if (tool_name == "wiki_read") {
            auto res = wiki_explorer_->read(args);
            return McpSuccess(res);
        }
        if (tool_name == "wiki_scan") {
            auto res = wiki_explorer_->scan(args);
            return McpSuccess(res);
        }
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: wiki tool exception: ") + e.what());
    }
    return McpError("ERROR: unknown wiki tool: " + tool_name);
}

// ============ Package Registry 工具分发(5 个 pkg_* 工具: 4 原始 + 1 分层) ============
json McpServer::dispatch_pkg_tool(const std::string& tool_name, const json& args) {
    if (!ensure_pkg_session()) return McpError("ERROR: Package session not initialized. Start with --pkg-profile <DIR>.");
    try {
        if (tool_name == "pkg_search_npm")      return ToolPkgSearchNpm(*pkg_session_, args);
        if (tool_name == "pkg_get_npm_detail")   return ToolPkgGetNpmDetail(*pkg_session_, args);
        if (tool_name == "pkg_search_pypi")     return ToolPkgSearchPypi(*pkg_session_, args);
        if (tool_name == "pkg_get_pypi_detail")  return ToolPkgGetPypiDetail(*pkg_session_, args);
        if (tool_name == "pkg_fetch_detail")     return ToolPkgFetchDetail(*pkg_session_, args);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: pkg tool exception: ") + e.what());
    }
    return McpError("ERROR: unknown pkg tool: " + tool_name);
}

// ============ Papers with Code 工具分发(6 个 pwc_* 工具: 5 原始 + 1 分层) ============
json McpServer::dispatch_pwc_tool(const std::string& tool_name, const json& args) {
    if (!ensure_pwc_session()) return McpError("ERROR: PapersWithCode session not initialized. Start with --pwc-profile <DIR>.");
    try {
        if (tool_name == "pwc_search_papers")    return ToolPwcSearchPapers(*pwc_session_, args);
        if (tool_name == "pwc_get_paper_detail") return ToolPwcGetPaperDetail(*pwc_session_, args);
        if (tool_name == "pwc_get_sota")         return ToolPwcGetSota(*pwc_session_, args);
        if (tool_name == "pwc_search_tasks")    return ToolPwcSearchTasks(*pwc_session_, args);
        if (tool_name == "pwc_search_datasets")  return ToolPwcSearchDatasets(*pwc_session_, args);
        if (tool_name == "pwc_fetch_paper_detail") return ToolPwcFetchPaperDetail(*pwc_session_, args);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: pwc tool exception: ") + e.what());
    }
    return McpError("ERROR: unknown pwc tool: " + tool_name);
}

// ============ Hugging Face 工具分发(9 个 hf_* 工具: 7 原始 + 2 分层) ============
json McpServer::dispatch_hf_tool(const std::string& tool_name, const json& args) {
    if (!ensure_hf_session()) return McpError("ERROR: HuggingFace session not initialized. Start with --hf-profile <DIR>.");
    try {
        if (tool_name == "hf_search_models")    return ToolHfSearchModels(*hf_session_, args);
        if (tool_name == "hf_get_model_info")   return ToolHfGetModelInfo(*hf_session_, args);
        if (tool_name == "hf_get_model_readme") return ToolHfGetModelReadme(*hf_session_, args);
        if (tool_name == "hf_search_datasets")  return ToolHfSearchDatasets(*hf_session_, args);
        if (tool_name == "hf_get_dataset_info") return ToolHfGetDatasetInfo(*hf_session_, args);
        if (tool_name == "hf_get_trending_models") return ToolHfGetTrendingModels(*hf_session_, args);
        if (tool_name == "hf_search_spaces")    return ToolHfSearchSpaces(*hf_session_, args);
        if (tool_name == "hf_fetch_model_detail")   return ToolHfFetchModelDetail(*hf_session_, args);
        if (tool_name == "hf_fetch_dataset_detail") return ToolHfFetchDatasetDetail(*hf_session_, args);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: hf tool exception: ") + e.what());
    }
    return McpError("ERROR: unknown hf tool: " + tool_name);
}

// ============ Semantic Scholar 工具分发(7 个 s2_* 工具: 6 原始 + 1 分层) ============
json McpServer::dispatch_s2_tool(const std::string& tool_name, const json& args) {
    if (!ensure_s2_session()) return McpError("ERROR: SemanticScholar session not initialized. Start with --s2-profile <DIR>.");
    try {
        if (tool_name == "s2_search_papers")     return ToolS2SearchPapers(*s2_session_, args);
        if (tool_name == "s2_get_paper_detail")   return ToolS2GetPaperDetail(*s2_session_, args);
        if (tool_name == "s2_get_citations")     return ToolS2GetCitations(*s2_session_, args);
        if (tool_name == "s2_get_references")     return ToolS2GetReferences(*s2_session_, args);
        if (tool_name == "s2_get_author_papers")  return ToolS2GetAuthorPapers(*s2_session_, args);
        if (tool_name == "s2_search_author")     return ToolS2SearchAuthor(*s2_session_, args);
        if (tool_name == "s2_fetch_paper_detail") return ToolS2FetchPaperDetail(*s2_session_, args);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: s2 tool exception: ") + e.what());
    }
    return McpError("ERROR: unknown s2 tool: " + tool_name);
}

// ============ Stack Overflow 工具分发(6 个 so_* 工具: 5 原始 + 1 分层) ============
json McpServer::dispatch_so_tool(const std::string& tool_name, const json& args) {
    if (!ensure_so_session()) return McpError("ERROR: StackOverflow session not initialized. Start with --so-profile <DIR>.");
    try {
        if (tool_name == "so_search_questions")  return ToolSoSearchQuestions(*so_session_, args);
        if (tool_name == "so_get_question_detail") return ToolSoGetQuestionDetail(*so_session_, args);
        if (tool_name == "so_get_top_answers")   return ToolSoGetTopAnswers(*so_session_, args);
        if (tool_name == "so_search_by_tags")    return ToolSoSearchByTags(*so_session_, args);
        if (tool_name == "so_get_similar")       return ToolSoGetSimilar(*so_session_, args);
        if (tool_name == "so_fetch_question_detail") return ToolSoFetchQuestionDetail(*so_session_, args);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: so tool exception: ") + e.what());
    }
    return McpError("ERROR: unknown so tool: " + tool_name);
}

bool McpServer::read_line(std::string& line) {
    std::getline(std::cin, line);
    return !std::cin.eof();
}

void McpServer::write_line(const std::string& line) {
    std::cout << line << "\n";
    std::cout.flush();
}

void McpServer::log(const std::string& msg) {
    std::cerr << "[mcp] " << msg << std::endl;
}

int McpServer::run() {
    log("server starting");

    // 初始化 WebView2(一次)
    if (!client_.is_ready()) {
        // WebView2 在首次 HTTP 请求时延迟初始化,这里不阻塞
        log("webview2 will be initialized on first request");
    }

    std::string line;
    while (read_line(line)) {
        if (line.empty()) continue;

        json request;
        try {
            request = json::parse(line);
        } catch (const std::exception& e) {
            json err = {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}}
            };
            write_line(err.dump());
            continue;
        }

        std::string response = handle_request(request);
        if (!response.empty()) {
            write_line(response);
        }
    }

    log("server shutting down");
    return 0;
}

std::string McpServer::handle_request(const json& request) {
    // 校验 JSON-RPC 2.0
    {
        std::string m = request.value("method", std::string());
        DBG_LOG("rpc") << "handle_request: method=" << m << " has_id=" << request.contains("id");
    }
    if (!request.is_object() || !request.contains("jsonrpc")) {
        json err = {
            {"jsonrpc", "2.0"},
            {"id", request.contains("id") ? request["id"] : json(nullptr)},
            {"error", {{"code", -32600}, {"message", "Invalid Request"}}}
        };
        return err.dump();
    }

    json id = request.contains("id") ? request["id"] : json(nullptr);
    std::string method = request.value("method", std::string());

    json result;
    try {
        if (method == "initialize") {
            result = handle_initialize(request.value("params", json::object()));
            initialized_ = true;
        } else if (method == "initialized" || method == "notifications/initialized") {
            // 通知,无响应
            return "";
        } else if (method == "tools/list") {
            result = handle_tools_list();
        } else if (method == "tools/call") {
            result = handle_tools_call(request.value("params", json::object()));
        } else if (method == "shutdown") {
            json err = {{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
            return err.dump();
        } else if (method == "ping") {
            result = {{"pong", true}};
        } else {
            json err = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}
            };
            return err.dump();
        }
    } catch (const std::exception& e) {
        json err = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", -32603}, {"message", std::string("Internal error: ") + e.what()}}}
        };
        return err.dump();
    }

    json response = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
    return response.dump();
}

json McpServer::handle_initialize(const json& params) {
    // instructions:仅提供简短描述(不注入完整 system_prompt.md)
    // 完整 prompt 曾通过此字段注入,但大体积 instructions(>9KB)会导致
    // 部分 MCP 客户端(如 llama.cpp b10333 HTTP 客户端)栈缓冲区溢出崩溃。
    // MCP 协议规定 instructions 是简短描述,不应承载完整 system prompt。
    std::string instructions =
        "GitHub deep research assistant. 8 sources: GitHub API + arXiv/HN/Pkg/PWC/HF/S2/SO (WebView2). "
        "CRITICAL: always call github_get_branches before github_get_commits, "
        "then call github_get_commits per-branch with branch=<name>. "
        "Default branch commits may be stale; active development is often on non-default branches.";

    return {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", json::object({
            {"tools", json::object()},
            {"logging", json::object()}
        })},
        {"serverInfo", {
            {"name", "research-mcp"},
            {"version", "0.2.0"}
        }},
        {"instructions", instructions}
    };
}

json McpServer::handle_tools_list() {
    // 13 个 tool 的 schema,严格对齐规范
    return {
        {"tools", json::array({
            {
                {"name", "github_get_repo_info"},
                {"description", "Get basic repository information (name, description, stars, forks, etc.)."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner (user or org)"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_readme"},
                {"description", "Get repository README content as markdown text."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_tree"},
                {"description", "Get repository directory tree as formatted text."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"branch", {{"type", "string"}, {"default", "main"}, {"description", "Branch name (auto-fallback to master if main fails)"}}},
                        {"max_depth", {{"type", "integer"}, {"default", 3}, {"minimum", 1}, {"maximum", 10}}},
                        {"recursive", {{"type", "boolean"}, {"default", true}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_languages"},
                {"description", "Get repository languages and their byte counts."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_contributors"},
                {"description", "Get repository contributors list."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"limit", {{"type", "integer"}, {"default", 30}, {"minimum", 1}, {"maximum", 100}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_commits"},
                {"description", "Get recent commits of a GitHub repository. Use for timeline reconstruction and activity analysis. Use branch/sha to query non-default branches. For large repos (linux/chromium), MUST pass 'path' to filter by directory/file to avoid timeout."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"limit", {{"type", "integer"}, {"default", 50}, {"minimum", 1}, {"maximum", 100}}},
                        {"since", {{"type", "string"}, {"format", "date-time"}, {"description", "ISO 8601 datetime, only commits after this"}}},
                        {"branch", {{"type", "string"}, {"description", "Branch name to query (e.g. codex/cxcore-integration)"}}},
                        {"sha", {{"type", "string"}, {"description", "Override: branch name, tag name, or commit SHA (takes priority over branch)"}}},
                        {"path", {{"type", "string"}, {"description", "Filter commits by file/directory path (e.g. drivers/net/ethernet). CRITICAL for large repos to avoid full-scan timeout."}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_branches"},
                {"description", "List all branches of a repository. Use this before per-branch commit aggregation to discover branch names including those with slashes."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"limit", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 100}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_issues"},
                {"description", "Get repository issues (excludes PRs unless state filter includes them)."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"state", {{"type", "string"}, {"default", "all"}, {"enum", json::array({"open", "closed", "all"})}}},
                        {"limit", {{"type", "integer"}, {"default", 30}, {"minimum", 1}, {"maximum", 100}}},
                        {"labels", {{"type", "string"}, {"description", "Comma-separated label names"}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_pull_requests"},
                {"description", "Get repository pull requests list."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"state", {{"type", "string"}, {"default", "all"}, {"enum", json::array({"open", "closed", "all"})}}},
                        {"limit", {{"type", "integer"}, {"default", 30}, {"minimum", 1}, {"maximum", 100}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_get_releases"},
                {"description", "Get repository releases list."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}},
                        {"limit", {{"type", "integer"}, {"default", 10}, {"minimum", 1}, {"maximum", 100}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_summarize_repo"},
                {"description", "Get comprehensive repository summary (info, languages, contributor_count, latest_release)."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"owner", {{"type", "string"}, {"description", "Repository owner"}}},
                        {"repo", {{"type", "string"}, {"description", "Repository name"}}}
                    }},
                    {"required", json::array({"owner", "repo"})}
                }}
            },
            {
                {"name", "github_search_repositories"},
                {"description", "GitHub trending / discovery API. Use this when user asks for 'hot projects', 'trending repos', 'popular X', or gives a topic/language without a specific owner/repo. Query examples: 'stars:>1000 pushed:>2026-07-01' (recent hot), 'language:C++ stars:>500' (popular C++), 'topic:computer-vision' (by topic). Always use this instead of saying 'no trending API'."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"q", json::object({
                            {"type", "string"},
                            {"description", "GitHub search query. Supports qualifiers: language:C++, stars:>1000, topic:cv, user:octocat, pushed:>2025-01-01, fork:true, license:MIT, size:>1000"}
                        })},
                        {"sort", json::object({
                            {"type", "string"},
                            {"enum", json::array({"stars", "forks", "updated"})},
                            {"description", "Sort field (omit for best-match)"}
                        })},
                        {"order", json::object({
                            {"type", "string"},
                            {"default", "desc"},
                            {"enum", json::array({"asc", "desc"})}
                        })},
                        {"limit", json::object({
                            {"type", "integer"},
                            {"default", 30},
                            {"minimum", 1},
                            {"maximum", 100}
                        })},
                        {"page", json::object({
                            {"type", "integer"},
                            {"default", 1},
                            {"minimum", 1},
                            {"maximum", 10}
                        })}
                    })},
                    {"required", json::array({"q"})}
                })}
            },
            {
                {"name", "github_search_users"},
                {"description", "Find GitHub authors/orgs by name, location, language, followers. Use when user asks 'who is working on X', 'find orgs in field Y', or mentions an author name. Query examples: 'type:org followers:>100' (top orgs), 'language:C++ location:China' (Chinese C++ devs), 'cxvisionai' (by name)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"q", json::object({
                            {"type", "string"},
                            {"description", "GitHub user search query. Supports: type:user, type:org, followers:>100, location:China, language:C++, repos:>50, created:<2015-01-01"}
                        })},
                        {"sort", json::object({
                            {"type", "string"},
                            {"enum", json::array({"followers", "repositories", "joined"})},
                            {"description", "Sort field (omit for best-match)"}
                        })},
                        {"order", json::object({
                            {"type", "string"},
                            {"default", "desc"},
                            {"enum", json::array({"asc", "desc"})}
                        })},
                        {"limit", json::object({
                            {"type", "integer"},
                            {"default", 30},
                            {"minimum", 1},
                            {"maximum", 100}
                        })},
                        {"page", json::object({
                            {"type", "integer"},
                            {"default", 1},
                            {"minimum", 1},
                            {"maximum", 10}
                        })}
                    })},
                    {"required", json::array({"q"})}
                })}
            },
            // ========== GitHub 分层渐进挖掘工具(新增,与 HN/arXiv 对称) ==========
            {
                {"name", "github_search_index"},
                {"description", "Layer 1 - Lightweight GitHub repo search index: returns structured list "
                                "(repo_id, full_name, description, language, stars, topics, html_url). "
                                "No deep parsing, no contributor fetch. Use this first, then call "
                                "github_fetch_repo_detail with selected owner/repo for deep mining."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"},{"description","Search keyword (supports language:, stars:, topic: qualifiers)"}})},
                        {"language", json::object({{"type","string"},{"default",""},{"description","Language filter (e.g. python, cpp)"}})},
                        {"sort", json::object({{"type","string"},{"default","stars"},{"enum", json::array({"stars","forks","updated"})}})},
                        {"max_results", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "github_fetch_repo_detail"},
                {"description", "Layer 2 - Deep mine a single GitHub repo: returns tech_stack "
                                "(runtime/framework/database/devops/testing parsed from requirements.txt/package.json/Cargo.toml/go.mod/pom.xml), "
                                "tech_blocks (core module dirs: src/api/cli/models/utils), "
                                "top_contributors (login + contributions), direct_dependencies list. "
                                "Use after github_search_index to mine selected repos."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"fetch_tech_stack", json::object({{"type","boolean"},{"default",true}})},
                        {"fetch_code_structure", json::object({{"type","boolean"},{"default",true}})},
                        {"fetch_top_contributors", json::object({{"type","boolean"},{"default",true}})},
                        {"fetch_dependencies", json::object({{"type","boolean"},{"default",true}})},
                        {"max_contributors", json::object({{"type","integer"},{"default",15},{"minimum",1},{"maximum",30}})}
                    })},
                    {"required", json::array({"owner","repo"})}
                })}
            },
            {
                {"name", "github_fetch_relation_network"},
                {"description", "Layer 3 - Two-hop relation network mining: "
                                "similar_repos (via topic + language Jaccard similarity), "
                                "developer_related_repos (BFS from top contributors, level 1 + optional level 2 hop). "
                                "Use after github_fetch_repo_detail when ecosystem/relation analysis is needed. "
                                "Note: developer_depth=2 may issue many API calls (capped at ~30 related repos)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"find_similar_repos", json::object({{"type","boolean"},{"default",true}})},
                        {"similar_by_tech_stack", json::object({{"type","boolean"},{"default",true}})},
                        {"similar_by_topic", json::object({{"type","boolean"},{"default",true}})},
                        {"max_similar", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",20}})},
                        {"explore_developer_links", json::object({{"type","boolean"},{"default",true}})},
                        {"developer_depth", json::object({{"type","integer"},{"default",2},{"minimum",1},{"maximum",2}})}
                    })},
                    {"required", json::array({"owner","repo"})}
                })}
            },
            // === 局部对象连续动态分析索引 (next2) ===
            {
                {"name", "github_ingest_commit_timeline"},
                {"description", "Ingest a single commit's file-level changes into file_timeline + file_cooccurrence tables. "
                                "Fetches commit detail (with files array), parses each file's additions/deletions, "
                                "and batch-inserts into SQLite. Idempotent via UNIQUE(repo, file_path, commit_hash). "
                                "Returns records_inserted count. Use before github_module_timeline_analysis to populate local index."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"commit_hash", json::object({{"type","string"},{"description","Single commit SHA"}})}
                    })},
                    {"required", json::array({"owner","repo","commit_hash"})}
                })}
            },
            {
                {"name", "github_ingest_recent_commits_timeline"},
                {"description", "Batch-ingest recent commits (since_days) into file_timeline + file_cooccurrence. "
                                "Paginates commits list (per_page=100, max 10 pages), then calls per-commit detail API. "
                                "Already-ingested commits are skipped via UNIQUE constraint (still costs 1 API call per commit). "
                                "Returns records_inserted total. Use to populate local index before module_timeline_analysis. "
                                "For large repos (linux/chromium), MUST pass 'path' to filter by directory to avoid timeout."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"since_days", json::object({{"type","integer"},{"default",365},{"minimum",1},{"maximum",365}})},
                        {"branch", json::object({{"type","string"},{"default",""},{"description","Branch name (empty = default branch)"}})},
                        {"max_commits", json::object({{"type","integer"},{"default",100},{"minimum",1},{"maximum",500}})},
                        {"path", json::object({{"type","string"},{"default",""},{"description","Filter commits by file/directory path (e.g. drivers/net). CRITICAL for large repos to avoid full-scan timeout."}})}
                    })},
                    {"required", json::array({"owner","repo"})}
                })}
            },
            {
                {"name", "github_module_timeline_analysis"},
                {"description", "Local object continuous dynamic analysis - unified 3-layer entry point. "
                                "Layer 1 (lightweight index): returns candidate list only. "
                                "Layer 2 (deep dig): returns full timeline + contributor_rank + related_files + change_density. "
                                "Layer 3 (two-hop): layer 2 + developer_modules (other modules each top contributor touches) + coupled_clusters. "
                                "target_type: file/module/signature. time_range: 1y/180d/90d/30d. "
                                "If ingest_first=true, auto-fetches recent commits + auto-clusters modules + triggers pre-aggregation before query."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"target_type", json::object({{"type","string"},
                            {"enum", json::array({"file","module","signature"})},
                            {"description","Analysis target granularity"}})},
                        {"target_path", json::object({{"type","string"},{"default",""},{"description","Required when target_type=file"}})},
                        {"module_name", json::object({{"type","string"},{"default",""},{"description","Required when target_type=module. NOTE: name is 'module_name', NOT 'target_module'."}})},
                        {"signature_regex", json::object({{"type","string"},{"default",""},{"description","Required when target_type=signature (substring match on file_path/commit_message)"}})},
                        {"time_range", json::object({{"type","string"},{"default","1y"},{"enum", json::array({"1y","180d","90d","30d"})}})},
                        {"layer", json::object({{"type","integer"},{"default",2},{"minimum",1},{"maximum",3}})},
                        {"ingest_first", json::object({{"type","boolean"},{"default",true},{"description","If true, auto-ingest recent commits + auto-cluster modules + pre-aggregate before query"}})},
                        {"branch", json::object({{"type","string"},{"default",""},{"description","Branch name for non-default branch (e.g. codex/cxcore-integration). Empty = default branch"}})}
                    })},
                    {"required", json::array({"owner","repo","target_type"})}
                })}
            },
            // === 原语A:子模块拆分时序切片 ===
            {
                {"name", "github_subdir_timeline_slice"},
                {"description", "Sub-module timeline slicing: group by first-level subdirectory under root_path, "
                                "generate independent change-density curves per subdir to observe hotspot migration. "
                                "Example: root_path=drivers/usb -> subdirs: typec/gadget/host/serial/core/..., "
                                "each with monthly change counts, author counts, lines added/deleted. "
                                "Reveals trend shifts (e.g. host controllers in early years -> typec/gadget recently). "
                                "Requires prior ingest (set ingest_first=true for auto-ingest with path filter)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"root_path", json::object({{"type","string"},{"description","Root directory path to slice (e.g. drivers/usb). Accepts target_path as alias."}})},
                        {"time_range", json::object({{"type","string"},{"default","1y"},{"enum", json::array({"1y","180d","90d","30d"})}})},
                        {"ingest_first", json::object({{"type","boolean"},{"default",true},{"description","Auto-ingest recent commits with path filter before query"}})},
                        {"branch", json::object({{"type","string"},{"default",""},{"description","Branch name (empty = default branch)"}})}
                    })},
                    {"required", json::array({"owner","repo","root_path"})}
                })}
            },
            // === 原语B:维护链路归因分析 ===
            {
                {"name", "github_maintenance_attribution"},
                {"description", "Maintenance chain attribution: distinguish merge commits vs development commits, "
                                "restore kernel maintenance pipeline (developer -> subsystem maintainer -> patch tag -> mainline merge). "
                                "Merge commit detection: commit_message starts with 'Merge'. "
                                "Returns: contributors with role(maintainer/developer), dev/merge commit counts, merge_ratio, "
                                "plus merge_samples showing actual merge commit messages (e.g. 'Merge tag usb-7.2-rc3'). "
                                "Identifies key maintainers (e.g. torvalds, gregkh) vs active developers."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"owner", json::object({{"type","string"},{"description","Repo owner"}})},
                        {"repo", json::object({{"type","string"},{"description","Repo name"}})},
                        {"target_path", json::object({{"type","string"},{"description","Directory path to analyze (e.g. drivers/usb). Accepts path as alias."}})},
                        {"time_range", json::object({{"type","string"},{"default","1y"},{"enum", json::array({"1y","180d","90d","30d"})}})},
                        {"ingest_first", json::object({{"type","boolean"},{"default",true},{"description","Auto-ingest recent commits with path filter before query"}})},
                        {"branch", json::object({{"type","string"},{"default",""},{"description","Branch name (empty = default branch)"}})}
                    })},
                    {"required", json::array({"owner","repo","target_path"})}
                })}
            },
            // ========== arXiv 工具集(4 个 arxiv_*) ==========
            {
                {"name", "arxiv_search_papers"},
                {"description", "Search papers on arXiv.org. Supports native arXiv search syntax: 'cat:cs.AI' for category, 'abs:keyword' for abstract match, 'au:author_name' for author search, 'ti:title_word' for title match. Example query: 'cat:cs.LG diffusion large model' returns recent ML diffusion papers."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({
                            {"type", "string"},
                            {"description", "Search query using arXiv syntax. Supports cat:<category>, au:<author>, abs:<keyword>, ti:<word>, AND/OR operators. e.g. 'cat:cs.CV AND abs:video generation'"}
                        })},
                        {"max_results", json::object({
                            {"type", "integer"},
                            {"default", 10},
                            {"minimum", 1},
                            {"maximum", 50},
                            {"description", "Max number of results (page default is 25 per request)"}
                        })}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "arxiv_get_paper_detail"},
                {"description", "Get full details of an arXiv paper by ID: complete abstract, full author list, subject categories, PDF link, submission history. Use after arxiv_search_papers to read papers of interest."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"arxiv_id", json::object({
                            {"type", "string"},
                            {"description", "arXiv paper ID. New format: '2501.01234' or '2501.01234v2'. Old format: 'cs.AI/0309001'"}
                        })}
                    })},
                    {"required", json::array({"arxiv_id"})}
                })}
            },
            {
                {"name", "arxiv_get_pdf_link"},
                {"description", "Quickly get the direct PDF download link and abstract page link for an arXiv paper (zero-latency, no web request, ID-rule based)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"arxiv_id", json::object({
                            {"type", "string"},
                            {"description", "arXiv paper ID e.g. '2501.01234'"}
                        })}
                    })},
                    {"required", json::array({"arxiv_id"})}
                })}
            },
            {
                {"name", "arxiv_check_available"},
                {"description", "Check if arXiv.org website is reachable and loaded correctly (CDN / network connectivity probe). No parameters."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object()},
                    {"required", json::array()}
                })}
            },
            // === 分层渐进挖掘工具(新增,与 HN 对称) ===
            {
                {"name", "arxiv_search_index"},
                {"description", "Lightweight arXiv search index: returns structured list "
                                "(arxiv_id, title, authors, primary_category, abstract_short, pdf_url, submitted_date). "
                                "No PDF download, no full-text fetch. Use this first, then call "
                                "arxiv_fetch_paper_detail with selected arxiv_id for deep mining."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"},{"description","Search keyword"}})},
                        {"max_results", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",50}})},
                        {"searchtype", json::object({{"type","string"},{"default","all"},
                                                    {"enum", json::array({"all","title","abstract","author"})}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "arxiv_fetch_paper_detail"},
                {"description", "Deep-fetch a single arXiv paper by arxiv_id: returns title, authors, primary_category, "
                                "abstract_full, submitted_date, pdf_url, full_text (from ar5iv.org HTML rendering, "
                                "no PDF parsing required), references list. "
                                "Use after arxiv_search_index to mine selected papers on demand. "
                                "full_text_status: ok|skipped|fetch_failed|no_text. "
                                "references_status: ok|skipped|no_refs_section|no_refs_found."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"arxiv_id", json::object({{"type","string"},{"description","arXiv paper id (e.g. 2401.12345)"}})},
                        {"fetch_full_text", json::object({{"type","boolean"},{"default",true}})},
                        {"fetch_references", json::object({{"type","boolean"},{"default",true}})},
                        {"text_limit_chars", json::object({{"type","integer"},{"default",20000},{"minimum",1000},{"maximum",50000}})}
                    })},
                    {"required", json::array({"arxiv_id"})}
                })}
            },
            // ========== Hacker News 工具集(5 个 hn_*) ==========
            {
                {"name", "hn_get_top_stories"},
                {"description", "Get top stories from Hacker News front page. Returns structured JSON with stories array: each item has hn_id, rank, title, external_url, score, author, comment_count. Use hn_fetch_detailed_story with hn_id for secondary analysis (comments, article text)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",100}})}
                    })}
                })}
            },
            {
                {"name", "hn_get_new_stories"},
                {"description", "Get newest stories from Hacker News. Returns structured JSON with stories array (same format as hn_get_top_stories). Use hn_fetch_detailed_story with hn_id for secondary analysis."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",100}})}
                    })}
                })}
            },
            {
                {"name", "hn_get_best_stories"},
                {"description", "Get best stories from Hacker News. Returns structured JSON with stories array (same format as hn_get_top_stories). Use hn_fetch_detailed_story with hn_id for secondary analysis."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",100}})}
                    })}
                })}
            },
            {
                {"name", "hn_get_item"},
                {"description", "Get full detail of a Hacker News post including all comments, by item ID."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"id", json::object({{"type","integer"},{"description","HN item ID"}})}
                    })},
                    {"required", json::array({"id"})}
                })}
            },
            {
                {"name", "hn_search_by_keyword"},
                {"description", "Search Hacker News stories and comments by keyword via Algolia search."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"},{"description","Search keyword"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            // === 分层渐进挖掘工具(新增) ===
            {
                {"name", "hn_get_latest_index"},
                {"description", "Lightweight HN index: fetch front/newest/best page and return structured list "
                                "(hn_id, rank, title, external_url, score, created_min_ago, has_discussion). "
                                "No deep fetches, no external page loads. Use this first, then call "
                                "hn_fetch_detailed_story with selected hn_id for deep mining."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"limit", json::object({{"type","integer"},{"default",30},{"minimum",1},{"maximum",100}})},
                        {"source", json::object({{"type","string"},{"default","front"},
                                                {"enum", json::array({"front","newest","best"})}})}
                    })}
                })}
            },
            {
                {"name", "hn_fetch_detailed_story"},
                {"description", "Deep-fetch a single HN story by hn_id: returns title, source_url, "
                                "article_plaintext (from external_url via kJsExtractRawPage) and "
                                "discussion_comments tree (author, text, reply_level). "
                                "Use after hn_get_latest_index to mine selected stories on demand. "
                                "article_fetch_status: ok|skipped|disabled|no_external_url|fetch_failed|no_text."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"hn_id", json::object({{"type","string"},{"description","Numeric HN story id"}})},
                        {"fetch_external_article", json::object({{"type","boolean"},{"default",true}})},
                        {"fetch_comments", json::object({{"type","boolean"},{"default",true}})},
                        {"comment_max_depth", json::object({{"type","integer"},{"default",2},{"minimum",1},{"maximum",5}})},
                        {"max_comment_count", json::object({{"type","integer"},{"default",80},{"minimum",1},{"maximum",200}})},
                        {"text_max_chars", json::object({{"type","integer"},{"default",20000},{"minimum",1000},{"maximum",50000}})}
                    })},
                    {"required", json::array({"hn_id"})}
                })}
            },
            // ========== Research DeepDive (跨源次级链接发现 + 批量抓取) ==========
            {
                {"name", "research_deep_dive"},
                {"description",
                 "Comprehensive secondary web analysis driven by graph traversal. "
                 "STEP 1 (SEED DISCOVERY): Resolve seed_query into starting points — "
                 "prefix 'hn:XXXX' or pure numeric id resolves to HN story (title + comments + article body via hn_fetch_detailed_story); "
                 "otherwise lookup entities by name or fuzzy search in the entity index. "
                 "STEP 2 (GRAPH TRAVERSE): 2-hop BFS on entity graph to collect all related entities. "
                 "STEP 3 (URL AGGREGATION): Extract URLs from HN comments/article, entity metadata fields, "
                 "graph-traversed entities — normalize, deduplicate, filter low-value (login pages, "
                 "trackers, binary assets, short URLs), rank by (citation_count × relation_weight). "
                 "STEP 4 (SECONDARY FETCH): Navigate and scrape top-N secondary URLs (TTL=72h cache). "
                 "Use this tool when the initial story mentions multiple related articles, papers, "
                 "repos, or blog posts that you want to read in full before drawing conclusions."
                },
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"seed_query", json::object({
                            {"type", "string"},
                            {"description",
                             "Research starting point. Options: (1) 'hn:42000' or numeric '42000' = HN story id; "
                             "(2) exact entity canonical_name (e.g. 'hn:42000'); "
                             "(3) keyword / topic name for entity search (falls back to fuzzy match)."}
                        })},
                        {"max_secondary_links", json::object({
                            {"type", "integer"}, {"default", 5}, {"minimum", 1}, {"maximum", 15},
                            {"description", "Maximum number of secondary web pages to fetch after ranking."}
                        })},
                        {"page_text_max_chars", json::object({
                            {"type", "integer"}, {"default", 15000}, {"minimum", 1000}, {"maximum", 50000},
                            {"description", "Plaintext character cap per secondary web page."}
                        })},
                        {"graph_max_depth", json::object({
                            {"type", "integer"}, {"default", 2}, {"minimum", 1}, {"maximum", 3},
                            {"description", "BFS traversal depth from seed entities (higher = wider coverage, slower)."}
                        })},
                        {"force_refresh", json::object({
                            {"type", "boolean"}, {"default", false},
                            {"description", "If true, ignore all 72h caches and re-fetch every page."}
                        })}
                    })},
                    {"required", json::array({"seed_query"})}
                })}
            },
            // ========== Package Registry 工具集(4 个 pkg_*) ==========
            {
                {"name", "pkg_search_npm"},
                {"description", "Search npm packages by keyword. Returns package names, descriptions, versions, authors, download counts."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "pkg_get_npm_detail"},
                {"description", "Get detailed info for an npm package: version, license, dependencies, README preview, weekly downloads."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"name", json::object({{"type","string"},{"description","npm package name"}})}
                    })},
                    {"required", json::array({"name"})}
                })}
            },
            {
                {"name", "pkg_search_pypi"},
                {"description", "Search PyPI packages by keyword. Returns package names, versions, descriptions."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "pkg_get_pypi_detail"},
                {"description", "Get detailed info for a PyPI package: version, author, license, dependencies, project URLs."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"name", json::object({{"type","string"},{"description","PyPI package name"}})}
                    })},
                    {"required", json::array({"name"})}
                })}
            },
            {
                {"name", "pkg_fetch_detail"},
                {"description", "Fetch package detail with cache (TTL=24h) + entity_mapper. Supports npm and pypi registries. Returns cache_hit=true on second call."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"registry", json::object({{"type","string"},{"enum",json::array({"npm","pypi"})},{"default","npm"}})},
                        {"name", json::object({{"type","string"},{"description","Package name"}})}
                    })},
                    {"required", json::array({"name"})}
                })}
            },
            // ========== Papers with Code 工具集(5 个 pwc_*) ==========
            {
                {"name", "pwc_search_papers"},
                {"description", "Search papers on Papers with Code. Returns titles, authors, abstracts, code repository links if available."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "pwc_get_paper_detail"},
                {"description", "Get paper detail with official and community code implementations from Papers with Code."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"paper_id", json::object({{"type","string"},{"description","Papers with Code paper ID or slug"}})}
                    })},
                    {"required", json::array({"paper_id"})}
                })}
            },
            {
                {"name", "pwc_get_sota"},
                {"description", "Get State-of-the-Art (SOTA) leaderboard for a specific task from Papers with Code."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"task", json::object({{"type","string"},{"description","Task name e.g. 'Image Classification'"}})},
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",100}})}
                    })},
                    {"required", json::array({"task"})}
                })}
            },
            {
                {"name", "pwc_search_tasks"},
                {"description", "Search task categories on Papers with Code."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "pwc_search_datasets"},
                {"description", "Search datasets on Papers with Code."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "pwc_fetch_paper_detail"},
                {"description", "Fetch PwC paper detail with cache (TTL=72h) + entity_mapper. Auto-registers paper entity + pwc_stars metric."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"paper_id", json::object({{"type","string"},{"description","Papers with Code paper slug/id"}})}
                    })},
                    {"required", json::array({"paper_id"})}
                })}
            },
            // ========== Hugging Face 工具集(7 个 hf_*) ==========
            {
                {"name", "hf_search_models"},
                {"description", "Search Hugging Face models by keyword and optional task filter. Returns model IDs, downloads, likes, tags."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"task", json::object({{"type","string"},{"description","Filter by task e.g. 'text-classification'"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "hf_get_model_info"},
                {"description", "Get detailed info for a Hugging Face model: tags, downloads, likes, pipeline tag, framework, last modified."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"model_id", json::object({
                            {"type", "string"},
                            {"description", "e.g. bert-base-uncased"}
                        })}
                    })},
                    {"required", json::array({"model_id"})}
                })}
            },
            {
                {"name", "hf_get_model_readme"},
                {"description", "Get the full README/model card content for a Hugging Face model (truncated to 5000 chars)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"model_id", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"model_id"})}
                })}
            },
            {
                {"name", "hf_search_datasets"},
                {"description", "Search Hugging Face datasets by keyword. Returns dataset IDs, downloads, likes."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "hf_get_dataset_info"},
                {"description", "Get detailed info for a Hugging Face dataset."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"dataset_id", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"dataset_id"})}
                })}
            },
            {
                {"name", "hf_get_trending_models"},
                {"description", "Get trending Hugging Face models sorted by recent popularity."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })}
                })}
            },
            {
                {"name", "hf_search_spaces"},
                {"description", "Search Hugging Face Spaces (online demos) by keyword."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "hf_fetch_model_detail"},
                {"description", "Fetch HF model detail with cache (TTL=12h) + entity_mapper. Auto-registers model entity + hf_observed metric."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"model_id", json::object({{"type","string"},{"description","HF model id, e.g. 'bert-base-uncased'"}})}
                    })},
                    {"required", json::array({"model_id"})}
                })}
            },
            {
                {"name", "hf_fetch_dataset_detail"},
                {"description", "Fetch HF dataset detail with cache (TTL=24h) + entity_mapper. Auto-registers dataset entity."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"dataset_id", json::object({{"type","string"},{"description","HF dataset id"}})}
                    })},
                    {"required", json::array({"dataset_id"})}
                })}
            },
            // ========== Semantic Scholar 工具集(6 个 s2_*) ==========
            {
                {"name", "s2_search_papers"},
                {"description", "Search papers on Semantic Scholar. Supports year range filter. Returns titles, authors, citation counts, abstracts."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})},
                        {"year", json::object({{"type","string"},{"description","Year range e.g. 2020-2024"}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "s2_get_paper_detail"},
                {"description", "Get paper detail from Semantic Scholar: abstract, authors, venue, citation count, fields of study, external IDs."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"paper_id", json::object({{"type","string"},{"description","Semantic Scholar paper ID, DOI, or arXiv ID"}})}
                    })},
                    {"required", json::array({"paper_id"})}
                })}
            },
            {
                {"name", "s2_get_citations"},
                {"description", "Get papers that cite a given paper (citation network)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"paper_id", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"paper_id"})}
                })}
            },
            {
                {"name", "s2_get_references"},
                {"description", "Get papers referenced by a given paper (reference list)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"paper_id", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"paper_id"})}
                })}
            },
            {
                {"name", "s2_get_author_papers"},
                {"description", "Get an author's paper list from Semantic Scholar. Includes author name, total citations, h-index."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"author_id", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",20},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"author_id"})}
                })}
            },
            {
                {"name", "s2_search_author"},
                {"description", "Search for authors on Semantic Scholar by name."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"name", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",5},{"minimum",1},{"maximum",20}})}
                    })},
                    {"required", json::array({"name"})}
                })}
            },
            {
                {"name", "s2_fetch_paper_detail"},
                {"description", "Fetch Semantic Scholar paper detail with cache (TTL=72h) + entity_mapper. Supports DOI/corpus ID/arXiv ID."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"paper_id", json::object({{"type","string"},{"description","Paper id: DOI / corpus ID / arXiv ID"}})}
                    })},
                    {"required", json::array({"paper_id"})}
                })}
            },
            // ========== Stack Overflow 工具集(5 个 so_*) ==========
            {
                {"name", "so_search_questions"},
                {"description", "Search Stack Overflow questions by keyword, with optional tag filter and sort order."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"tag", json::object({{"type","string"},{"description","Optional tag filter"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})},
                        {"sort", json::object({{"type","string"},{"default","relevance"},{"enum",json::array({"relevance","newest","active","votes"})}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "so_get_question_detail"},
                {"description", "Get full question detail with all answers from Stack Overflow."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"question_id", json::object({{"type","integer"}})}
                    })},
                    {"required", json::array({"question_id"})}
                })}
            },
            {
                {"name", "so_get_top_answers"},
                {"description", "Get top-voted answers for a Stack Overflow question."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"question_id", json::object({{"type","integer"}})},
                        {"count", json::object({{"type","integer"},{"default",3},{"minimum",1},{"maximum",20}})}
                    })},
                    {"required", json::array({"question_id"})}
                })}
            },
            {
                {"name", "so_search_by_tags"},
                {"description", "Search Stack Overflow questions by tags (semicolon-separated, e.g. 'python;pandas')."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"tags", json::object({{"type","string"},{"description","Semicolon-separated tags e.g. python;django"}})},
                        {"count", json::object({{"type","integer"},{"default",10},{"minimum",1},{"maximum",50}})}
                    })},
                    {"required", json::array({"tags"})}
                })}
            },
            {
                {"name", "so_get_similar"},
                {"description", "Find similar Stack Overflow questions by title."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"title", json::object({{"type","string"}})},
                        {"count", json::object({{"type","integer"},{"default",5},{"minimum",1},{"maximum",30}})}
                    })},
                    {"required", json::array({"title"})}
                })}
            },
            {
                {"name", "so_fetch_question_detail"},
                {"description", "Fetch SO question detail with cache (TTL=24h) + entity_mapper. Auto-registers question entity + so_observed metric."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"question_id", json::object({{"type","integer"},{"description","Stack Overflow question id"}})}
                    })},
                    {"required", json::array({"question_id"})}
                })}
            },
            // ============ wiki_* tools (9-source unified search) ============
            {
                {"name", "wiki_discover"},
                {"description", "Probe documentation resources: local Kiwix first, then repo docs/Wiki/external sites. Returns lightweight resource list with is_local flag."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"repo", json::object({{"type","string"},{"description","Repository identifier owner/repo"}})},
                        {"query", json::object({{"type","string"},{"description","Keywords or article title, prioritizes local Kiwix"}})},
                        {"repo_branch", json::object({{"type","string"},{"default","main"}})}
                    })}
                })}
            },
            {
                {"name", "wiki_read"},
                {"description", "Read a single document precisely. Auto-uses global cache, local Kiwix first, falls back to online if missing."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"target_uri", json::object({{"type","string"},{"description","canonical_uri from discover or scan"}})},
                        {"force_refresh", json::object({{"type","boolean"},{"default",false}})}
                    })},
                    {"required", json::array({"target_uri"})}
                })}
            },
            {
                {"name", "wiki_scan"},
                {"description", "Expand from a root resource + sub_path, batch-read related pages. Returns page summaries with content_preview only (first 500 chars)."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"root_canonical_uri", json::object({{"type","string"},{"description","Root resource URI to expand from"}})},
                        {"sub_path", json::object({{"type","string"},{"description","Sub-path or related keyword for expansion"}})},
                        {"max_depth", json::object({{"type","integer"},{"minimum",1},{"maximum",5},{"default",2}})},
                        {"max_pages", json::object({{"type","integer"},{"minimum",1},{"maximum",40},{"default",15}})},
                        {"force_refresh", json::object({{"type","boolean"},{"default",false}})}
                    })},
                    {"required", json::array({"root_canonical_uri","sub_path"})}
                })}
            },
            // ═══════════════════════════════════════════════════════════
            //  定向知识雷达 — Focus 工具 (next.txt)
            // ═══════════════════════════════════════════════════════════
            {
                {"name", "focus_create"},
                {"description", "Create a focus domain (定向知识雷达): define what to track, seed entities to start from, and let the system automatically discover and expand related entities over time."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"name", json::object({{"type","string"},{"description","Focus name (e.g. 'Transformer 架构演进')"}})},
                        {"description", json::object({{"type","string"},{"description","Natural language description of what to track"}})},
                        {"seed_entity_ids", json::object({{"type","array"},{"items",{{"type","string"}}},{"description","Existing entity IDs as seeds"}})},
                        {"seed_queries", json::object({{"type","array"},{"items",{{"type","string"}}},{"description","Search terms to find seed entities"}})},
                        {"keywords", json::object({{"type","array"},{"items",{{"type","string"}}},{"description","Positive keywords for relevance scoring"}})},
                        {"exclude_words", json::object({{"type","array"},{"items",{{"type","string"}}},{"description","Words that reduce relevance score"}})},
                        {"max_depth", json::object({{"type","integer"},{"minimum",1},{"maximum",5},{"default",3}})},
                        {"relevance_threshold", json::object({{"type","number"},{"minimum",0},{"maximum",1},{"default",0.55}})},
                        {"max_nodes", json::object({{"type","integer"},{"minimum",10},{"maximum",2000},{"default",500}})}
                    })},
                    {"required", json::array({"name"})}
                })}
            },
            {
                {"name", "focus_list"},
                {"description", "List all focus domains with node counts and status."},
                {"inputSchema", json::object({{"type","object"},{"properties",json::object()}})}
            },
            {
                {"name", "focus_get"},
                {"description", "Get focus domain details including keywords, seeds, thresholds, and sprawl stats."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"},{"description","Focus ID (e.g. f_3fa1b2)"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_delete"},
                {"description", "Delete a focus domain. Entity data can be preserved for other focuses."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"keep_entities", json::object({{"type","boolean"},{"default",true}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_members"},
                {"description", "List entities belonging to a focus domain, filtered by sprawl_status."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"sprawl_status", json::object({{"type","string"},{"enum",json::array({"seed","active","boundary","pruned","exhausted"})}})},
                        {"limit", json::object({{"type","integer"},{"default",100}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_gaps"},
                {"description", "List unresolved gaps (missing attributes) for a focus domain, sorted by priority."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"min_priority", json::object({{"type","number"},{"default",0.0}})},
                        {"limit", json::object({{"type","integer"},{"default",20}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_stats"},
                {"description", "Get sprawl progress: entity count, attribute count, relation count, open gaps, status breakdown."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"},{"description","Optional: if empty, returns global stats"}})}
                    })}
                })}
            },
            {
                {"name", "entity_attrs"},
                {"description", "Query all attributes of an entity, optionally filter by attr_key. Returns multi-source values and merged result."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"entity_id", json::object({{"type","string"}})},
                        {"attr_key", json::object({{"type","string"},{"description","Optional: filter by key (e.g. 'authors', 'repo_url')"}})}
                    })},
                    {"required", json::array({"entity_id"})}
                })}
            },
            {
                {"name", "focus_prune"},
                {"description", "Manually prune (set sprawl_status=pruned) a focus member entity to stop further expansion."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"entity_id", json::object({{"type","string"}})},
                        {"reason", json::object({{"type","string"},{"default","manual_prune"}})}
                    })},
                    {"required", json::array({"focus_id","entity_id"})}
                })}
            },
            {
                {"name", "focus_promote"},
                {"description", "Manually promote (set sprawl_status=active) a boundary/pruned entity to enable expansion."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"entity_id", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"focus_id","entity_id"})}
                })}
            },
            {
                {"name", "focus_sprawl_tick"},
                {"description", "Execute one sprawl tick for a focus."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"max_nodes_per_tick", json::object({{"type","integer"}})},
                        {"dry_run", json::object({{"type","boolean"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_estimate_relevance"},
                {"description", "Debug: estimate relevance of candidate entity against focus."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"entity_name", json::object({{"type","string"}})},
                        {"entity_description", json::object({{"type","string"}})},
                        {"relation_type", json::object({{"type","string"}})},
                        {"depth", json::object({{"type","integer"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_extract_tick"},
                {"description", "Run attribute extraction tick for focus entities."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"max_entities_per_tick", json::object({{"type","integer"}})},
                        {"dry_run", json::object({{"type","boolean"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_gaps_detect"},
                {"description", "Detect attribute gaps in a focus."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_track_tick"},
                {"description", "Run adaptive tracking tick for exhausted/seed nodes."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"max_schedules_per_tick", json::object({{"type","integer"}})},
                        {"dry_run", json::object({{"type","boolean"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_updates_since"},
                {"description", "RSS-style incremental query for a focus."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"since", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "web_search"},
                {"description", "Multi-engine web search (Bing main + Tavily fallback) with caching."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({{"type","string"}})},
                        {"focus_id", json::object({{"type","string"}})},
                        {"max_results", json::object({{"type","integer"}})},
                        {"freshness", json::object({{"type","string"}})},
                        {"mkt", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"query"})}
                })}
            },
            {
                {"name", "focus_cross_gain"},
                {"description", "Find entities referenced by multiple focuses."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "focus_export"},
                {"description", "Export a focus domain as structured JSON."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"focus_id", json::object({{"type","string"}})},
                        {"format", json::object({{"type","string"}})}
                    })},
                    {"required", json::array({"focus_id"})}
                })}
            },
            {
                {"name", "system_diagnostics"},
                {"description", "Unified diagnostic snapshot: engines, cache summary, entities, focus."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"include_cache_keys", json::object({{"type","boolean"}})}
                    })}
                })}
            },
            {
                {"name", "system_list_cache"},
                {"description", "List recent cache entries filtered by source_type."},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"source_type", json::object({{"type","string"}})},
                        {"limit", json::object({{"type","integer"}})}
                    })}
                })}
            }
        })}
    };
}

// tools/call 分发:按工具名前缀路由到各源独立 WebView 会话
json McpServer::handle_tools_call(const json& params) {
    if (!params.is_object()) {
        return McpError("invalid params");
    }
    std::string name = params.value("name", std::string());
    if (name.empty()) {
        return McpError("missing 'name'");
    }
    json args = params.value("arguments", json::object());
    if (!args.is_object()) args = json::object();

    DBG_LOG("rpc") << "handle_tools_call: name=" << name;
    // 按前缀路由到各源 dispatcher
    if (name.rfind("arxiv_", 0) == 0)  return dispatch_arxiv_tool(name, args);
    if (name.rfind("hn_", 0) == 0)     return dispatch_hn_tool(name, args);
    if (name.rfind("pkg_", 0) == 0)    return dispatch_pkg_tool(name, args);
    if (name.rfind("pwc_", 0) == 0)    return dispatch_pwc_tool(name, args);
    if (name.rfind("hf_", 0) == 0)     return dispatch_hf_tool(name, args);
    if (name.rfind("s2_", 0) == 0)     return dispatch_s2_tool(name, args);
    if (name.rfind("so_", 0) == 0)     return dispatch_so_tool(name, args);
    if (name.rfind("research_", 0) == 0) return dispatch_research_tool(name, args);
    if (name.rfind("wiki_", 0) == 0)     return dispatch_wiki_tool(name, args);
    if (name == "web_search")           return dispatch_focus_tool(name, args);
    if (name.rfind("focus_", 0) == 0)    return dispatch_focus_tool(name, args);
    if (name.rfind("entity_", 0) == 0)   return dispatch_focus_tool(name, args);

    // system_* 诊断工具 (内联实现, 直接调 CacheManager)
    if (name.rfind("system_", 0) == 0) {
        CacheManager& cm = CacheManager::instance();
        if (name == "system_diagnostics") {
            bool include_keys = args.value("include_cache_keys", true);
            int sample_limit = include_keys ? 5 : 0;
            json result = json::object();
            result["cache_manager"]     = cm.stats();
            result["cache_summary"]     = cm.get_cache_summary(sample_limit);
            result["source_health"]     = cm.get_source_health();
            result["engines"]           = web_search_engine_status();
            try { result["focus_overview"] = cm.get_sprawl_stats(""); } catch (...) {}
            return McpSuccess(result);
        }
        if (name == "system_list_cache") {
            std::string st = args.value("source_type", std::string(""));
            int lim = args.value("limit", 20);
            if (lim < 1) lim = 1;
            if (lim > 200) lim = 200;
            json arr = cm.list_cache(st, lim);
            return McpSuccess({{"count", arr.size()}, {"entries", arr}});
        }
        return McpError("ERROR: unknown system tool: " + name);
    }

    // GitHub 工具走原路径
    return dispatch_tool_call(client_, params);
}

// === HTTP 模式 ===

McpServer::HttpResult McpServer::handle_http_request(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) {
    HttpResult result;
    DBG_LOG("http") << "handle_http_request: " << method << " " << path << " body_len=" << body.size();

    // GET / -> 服务状态
    if (method == "GET" && (path == "/" || path == "/health")) {
        result.body = json{
            {"service", "research-mcp"},
            {"version", "0.2.0"},
            {"mode", "http"},
            {"status", "ok"},
            {"sources", {
                {"github", true},
                {"arxiv", has_arxiv()},
                {"hackernews", has_hackernews()},
                {"package", has_package()},
                {"paperswithcode", has_paperswithcode()},
                {"huggingface", has_huggingface()},
                {"semanticscholar", has_semanticscholar()},
                {"stackoverflow", has_stackoverflow()}
            }},
            {"endpoints", {
                {"POST /mcp", "JSON-RPC 2.0 request"},
                {"GET /", "service status"},
                {"GET /tools", "list available tools"}
            }}
        }.dump();
        result.content_type = "application/json";
        return result;
    }

    // GET /tools -> 工具列表(便于调试)
    if (method == "GET" && path == "/tools") {
        json list = handle_tools_list();
        result.body = list.dump();
        result.content_type = "application/json";
        return result;
    }

    // GET /mcp -> Streamable HTTP MCP 的 SSE 探测
    // research-mcp 只支持旧版 HTTP JSON-RPC (MCP 2024-11-05),不支持 SSE
    // 返回 405 Method Not Allowed 明确告知客户端不支持 SSE,避免客户端无限重试崩溃
    if (method == "GET" && path == "/mcp") {
        DBG_LOG("http") << "GET /mcp: returning 405 (SSE not supported)";
        result.status = 405;
        result.content_type = "application/json";
        result.body = json{
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", -32000}, {"message", "SSE/streamable HTTP not supported. Use POST /mcp for JSON-RPC 2.0."}}}
        }.dump();
        // 通过自定义 header 告知客户端只支持 POST
        result.extra_headers = "Allow: POST\r\n";
        return result;
    }

    // POST /mcp -> JSON-RPC 请求
    if (method == "POST" && (path == "/mcp" || path == "/")) {
        json request;
        try {
            request = json::parse(body);
        } catch (const std::exception& e) {
            result.status = 400;
            result.body = json{
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}}
            }.dump();
            return result;
        }

        // 支持批量请求(JSON 数组)
        if (request.is_array()) {
            json responses = json::array();
            for (auto& single : request) {
                std::string resp = handle_request(single);
                if (!resp.empty()) {
                    try {
                        responses.push_back(json::parse(resp));
                    } catch (...) {
                        // skip unparseable
                    }
                }
            }
            result.body = responses.dump();
            return result;
        }

        std::string resp = handle_request(request);
        if (resp.empty()) {
            // 通知类请求,无响应
            result.status = 202;
            result.body = "";
            return result;
        }
        result.body = resp;
        return result;
    }

    // 其它路径
    result.status = 404;
    result.body = json{{"error", "not found"}, {"path", path}}.dump();
    return result;
}

int McpServer::run_http(int port) {
    this->log("server starting in HTTP mode on port " + std::to_string(port));

    if (!client_.is_ready()) {
        this->log("webview2 will be initialized on first request");
    }

    // 用指针以便 lambda 能引用 server 并触发 stop()
    HttpServer* server_ptr = nullptr;
    HttpServer server(port, [this, &server_ptr](const HttpRequest& req) -> HttpServerResponse {
        HttpServerResponse http_resp;
        McpServer::HttpResult mcp_resp = handle_http_request(req.method, req.path, req.body);
        http_resp.status = mcp_resp.status;
        http_resp.body = mcp_resp.body;
        http_resp.content_type = mcp_resp.content_type;
        http_resp.extra_headers = mcp_resp.extra_headers;

        // 检测 shutdown 请求 -> 触发 server 停止
        if (req.method == "POST" && req.path == "/mcp") {
            try {
                json j = json::parse(req.body);
                if (j.is_object() && j.value("method", std::string()) == "shutdown") {
                    if (server_ptr) server_ptr->stop();
                }
            } catch (...) {
                // ignore
            }
        }
        return http_resp;
    });
    server_ptr = &server;

    int rc = server.run();
    log("http server shutting down");
    return rc;
}

// ═════════════════════════════════════════════════════════════
//  定向知识雷达 — Focus 工具分发
// ═════════════════════════════════════════════════════════════
json McpServer::dispatch_focus_tool(const std::string& tool_name, const json& args) {
    DBG_LOG("focus") << "dispatch_focus_tool: " << tool_name;
    try {
        // Focus 管理
        if (tool_name == "focus_create") {
            json result = ToolFocusCreate(args);

            // 🔧 修复:如果种子实体找不到,且 seed_queries 里有 GitHub URL,
            //    自动调 GitHub API 注册实体后重试
            if (result.value("isError", false) &&
                args.contains("seed_queries") && args["seed_queries"].is_array()) {
                std::string err_text;
                if (result.contains("content") && !result["content"].empty() &&
                    result["content"][0].contains("text")) {
                    err_text = result["content"][0]["text"].get<std::string>();
                }
                if (err_text.find("no seed entities found") != std::string::npos) {
                    CacheManager& cm = CacheManager::instance();
                    std::vector<std::string> auto_registered;

                    for (const auto& q : args["seed_queries"]) {
                        if (!q.is_string()) continue;
                        std::string url = q.get<std::string>();

                        // 解析 https://github.com/owner/repo[/...] 格式
                        auto idx = url.find("github.com/");
                        if (idx == std::string::npos) continue;
                        auto rest = url.substr(idx + 11);
                        auto slash1 = rest.find('/');
                        if (slash1 == std::string::npos || slash1 == 0) continue;
                        auto owner = rest.substr(0, slash1);
                        auto slash2 = rest.find('/', slash1 + 1);
                        auto repo = (slash2 == std::string::npos) ?
                                    rest.substr(slash1 + 1) :
                                    rest.substr(slash1 + 1, slash2 - slash1 - 1);
                        if (owner.empty() || repo.empty()) continue;

                        // 跳过 owner 里的 query/hash
                        repo = repo.substr(0, repo.find('?'));
                        repo = repo.substr(0, repo.find('#'));
                        if (repo.empty()) continue;

                        DBG_LOG("focus") << "auto-registering seed: github.com/"
                                         << owner << "/" << repo;
                        std::string canonical = owner + "/" + repo;
                        json repo_info;
                        std::string desc;
                        try {
                            repo_info = client_.get_repo_info(owner, repo);
                            desc = repo_info.value("description", "");
                        } catch (const std::exception& e) {
                            // GitHub API 可能 403(无 token 或 rate limit),
                            // 降级为仅注册基本实体,metadata 里标记 API 不可用
                            DBG_LOG("focus") << "get_repo_info failed for "
                                             << canonical << ": " << e.what()
                                             << " (fallback to basic register)";
                            repo_info = {
                                {"owner", owner}, {"repo", repo},
                                {"fallback", true}, {"source", "github_url_auto"}
                            };
                            desc = "(basic auto-registered from GitHub URL)";
                        }
                        try {
                            std::string eid = cm.register_entity(
                                "project", canonical, {}, {}, repo_info, desc);
                            auto_registered.push_back(eid);
                            DBG_LOG("focus") << "auto-registered entity " << eid;
                        } catch (const std::exception& e) {
                            DBG_LOG("focus") << "register_entity failed for "
                                             << canonical << ": " << e.what();
                        }
                    }

                    if (!auto_registered.empty()) {
                        // 带 seed_entity_ids 重调
                        json new_args = args;
                        new_args["seed_entity_ids"] = auto_registered;
                        result = ToolFocusCreate(new_args);
                    }
                }
            }
            return result;
        }
        if (tool_name == "focus_list")    return ToolFocusList(args);
        if (tool_name == "focus_get")     return ToolFocusGet(args);
        if (tool_name == "focus_delete")  return ToolFocusDelete(args);
        // 成员 & 缺口 & 统计
        if (tool_name == "focus_members") return ToolFocusMembers(args);
        if (tool_name == "focus_gaps")    return ToolFocusGaps(args);
        if (tool_name == "focus_stats")   return ToolFocusStats(args);
        // 实体属性查询
        if (tool_name == "entity_attrs")  return ToolEntityAttrs(args);
        // 人工干预
        if (tool_name == "focus_prune")   return ToolFocusPrune(args);
        if (tool_name == "focus_promote") return ToolFocusPromote(args);
        if (tool_name == "focus_sprawl_tick") return ToolFocusSprawlTick(args);
        if (tool_name == "focus_estimate_relevance") return ToolFocusEstimateRelevance(args);
        if (tool_name == "focus_extract_tick") return ToolFocusExtractTick(args);
        if (tool_name == "focus_gaps_detect") return ToolFocusGapsDetect(args);
        if (tool_name == "focus_track_tick") return ToolFocusTrackTick(args);
        if (tool_name == "focus_updates_since") return ToolFocusUpdatesSince(args);
        if (tool_name == "web_search") return ToolWebSearch(args);
        if (tool_name == "focus_cross_gain") return ToolFocusCrossGain(args);
        if (tool_name == "focus_export") return ToolFocusExport(args);
    } catch (const std::exception& e) {
        DBG_LOG("focus") << "dispatch_focus_tool exception: " << e.what();
        return McpError(std::string("ERROR: focus tool exception: ") + e.what());
    }
    DBG_LOG("focus") << "dispatch_focus_tool: unknown tool " << tool_name;
    return McpError("ERROR: unknown focus tool: " + tool_name);
}

} // namespace github_research
