#pragma once

#include <string>
#include <optional>
#include <map>
#include <memory>
#include <iostream>
#include <nlohmann/json.hpp>
#ifdef RESEARCH_MCP_USE_WEBVIEW2
#include "webview_client.hpp"
#endif
#include "curl_http_client.hpp"
#include "errors.hpp"

namespace github_research {

using json = nlohmann::json;

// GitHub REST API 客户端
// 迁移自 DeerFlow skills/public/github-deep-research/scripts/github_api.py
// 保留所有方法语义与返回 JSON 字段名不变,仅改语言与 HTTP 后端
// 混合方案:GitHub API 默认走 libcurl(轻量纯 HTTP),可切换 WebView2(浏览器指纹)
// 网页源(arXiv/HN/PWC 等)仍统一使用 WebView2
class GitHubClient {
public:
    // HTTP 后端选择
    enum class Backend {
        Curl,      // libcurl:纯 HTTP,无浏览器依赖,适用于 GitHub REST API(默认)
#ifdef RESEARCH_MCP_USE_WEBVIEW2
        WebView2   // WebView2:浏览器内核,适用于需要 JS 渲染/浏览器指纹的场景
#endif
    };

    explicit GitHubClient(std::optional<std::string> token = std::nullopt,
                          int timeout_seconds = 30,
                          Backend backend = Backend::Curl);

    // === 10 个 GitHub API 方法(对应 10 个 MCP tool) ===

    // 1. github_get_repo_info
    json get_repo_info(const std::string& owner, const std::string& repo);

    // 2. github_get_readme(返回 markdown 文本)
    std::string get_readme(const std::string& owner, const std::string& repo);

    // 3. github_get_tree(返回格式化文本)
    std::string get_tree(const std::string& owner, const std::string& repo,
                         const std::string& branch = "main",
                         int max_depth = 3,
                         bool recursive = true);

    // 3b. 获取原始 tree JSON(供 fetch_repo_detail 解析目录结构使用)
    // 返回 GitHub Git Trees API 的原始 JSON,包含完整 path 字段
    json get_tree_raw(const std::string& owner, const std::string& repo,
                      const std::string& branch = "main",
                      bool recursive = true);

    // 4. github_get_languages
    json get_languages(const std::string& owner, const std::string& repo);

    // 5. github_get_contributors
    json get_contributors(const std::string& owner, const std::string& repo, int limit = 30);

    // 6. github_get_commits
    // sha: 分支名、tag 或 commit SHA(默认走 HEAD 分支)
    // path: 按文件/目录路径过滤 commits(对大仓库至关重要,避免全量拉取超时)
    json get_recent_commits(const std::string& owner, const std::string& repo,
                            int limit = 50,
                            const std::optional<std::string>& since = std::nullopt,
                            const std::optional<std::string>& sha = std::nullopt,
                            const std::optional<std::string>& path = std::nullopt);

    // 6b. github_get_branches(新增:枚举所有分支,用于按分支汇总提交)
    json get_branches(const std::string& owner, const std::string& repo, int limit = 100);

    // 7. github_get_issues
    json get_issues(const std::string& owner, const std::string& repo,
                    const std::string& state = "all", int limit = 30,
                    const std::optional<std::string>& labels = std::nullopt);

    // 8. github_get_pull_requests
    json get_pull_requests(const std::string& owner, const std::string& repo,
                           const std::string& state = "all", int limit = 30);

    // 9. github_get_releases
    json get_releases(const std::string& owner, const std::string& repo, int limit = 10);

    // 10. github_summarize_repo
    json summarize_repo(const std::string& owner, const std::string& repo);

    // === 辅助方法(非 MCP tool,但保留语义) ===

    // 获取文件内容(返回文本)
    std::string get_file_content(const std::string& owner, const std::string& repo,
                                 const std::string& path);

    // 获取 tags 列表
    json get_tags(const std::string& owner, const std::string& repo, int limit = 20);

    // 搜索 issues
    json search_issues(const std::string& owner, const std::string& repo,
                       const std::string& query, int limit = 30);

    // 11. github_search_repositories
    // q 支持 GitHub Search 语法:language:、stars:>N、topic:、user:、pushed:>YYYY-MM-DD 等
    // sort: stars / forks / updated(空表示按 best-match)
    // order: desc(默认) / asc
    json search_repositories(const std::string& query,
                             const std::string& sort = "",
                             const std::string& order = "desc",
                             int limit = 30,
                             int page = 1);

    // 12. github_search_users
    // q 支持 type:user、type:org、followers:>N、location:、language: 等
    // sort: followers / repositories / joined(空表示按 best-match)
    json search_users(const std::string& query,
                      const std::string& sort = "",
                      const std::string& order = "desc",
                      int limit = 30,
                      int page = 1);

    // === 分层渐进挖掘工具(新增,与 HN/arXiv 对称) ===

    // 13. github_search_index - 轻量索引(结构化,只返回精简元数据)
    // query: 搜索关键词(可含 language:、stars:、topic: 等限定符)
    // language: 语言过滤(空表示不限)
    // sort: stars / forks / updated(空表示 best-match)
    // max_results: 返回上限(1-50)
    // 返回: [{repo_id, full_name, description, language, stars, topics}]
    json search_index(const std::string& query,
                      const std::string& language = "",
                      const std::string& sort = "stars",
                      int max_results = 20);

    // 14. github_fetch_repo_detail - 单仓库深度挖掘
    // owner/repo: 仓库坐标
    // fetch_tech_stack: 读取依赖文件(requirements.txt/package.json/Cargo.toml/go.mod/pom.xml)
    // fetch_code_structure: 解析目录树识别核心模块
    // fetch_top_contributors: 拉取 top N 贡献者
    // fetch_dependencies: 从依赖文件提取直接依赖列表
    // max_contributors: 贡献者上限(1-30)
    // 返回: {repo_full_name, description, stars, language, topics,
    //        tech_stack:{runtime,framework,database,devops,testing},
    //        tech_blocks:[{name,path,purpose,files_count}],
    //        top_contributors:[{login,contributions}],
    //        direct_dependencies:[]}
    json fetch_repo_detail(const std::string& owner,
                           const std::string& repo,
                           bool fetch_tech_stack = true,
                           bool fetch_code_structure = true,
                           bool fetch_top_contributors = true,
                           bool fetch_dependencies = true,
                           int max_contributors = 15);

    // 15. github_fetch_relation_network - 二级递进关联网络挖掘
    // owner/repo: 目标仓库坐标
    // find_similar_repos: 基于技术栈/topics 构造 query 搜索相似项目
    // similar_by_tech_stack / similar_by_topic: 相似度匹配维度
    // max_similar: 相似项目上限(1-20)
    // explore_developer_links: 从贡献者出发查找关联项目
    // developer_depth: 开发者关联扩散深度(1-2,2 表示二级递进)
    // 返回: {repo_full_name,
    //        similar_repos:[{full_name,similarity_score,match_breakdown,shared_dependencies}],
    //        developer_related_repos:[{full_name,shared_developers,relation_level,stars}]}
    json fetch_relation_network(const std::string& owner,
                                const std::string& repo,
                                bool find_similar_repos = true,
                                bool similar_by_tech_stack = true,
                                bool similar_by_topic = true,
                                int max_similar = 10,
                                bool explore_developer_links = true,
                                int developer_depth = 2);

    // === 局部对象连续动态分析索引 (next2) ===

    // 16. github_ingest_commit_timeline - 拉取单 commit 详情,解析所有文件变更,
    //     写入 file_timeline + file_cooccurrence
    // commit_hash: 单个 commit SHA
    // 返回: 写入的 file_timeline 记录数
    int ingest_commit_file_timeline(const std::string& owner,
                                     const std::string& repo,
                                     const std::string& commit_hash);

    // 17. github_ingest_recent_commits_timeline - 批量增量抓取近 N 天 commits,
    //     逐个写入 file_timeline + file_cooccurrence
    // since_days: 拉取近 N 天的 commits
    // branch: 分支名(空表示默认分支)
    // max_commits: 单次最多处理的 commit 数(防止 API 超限)
    // path: 按文件/目录路径过滤 commits(大仓库必传,避免全量拉取超时)
    int ingest_recent_commits_timeline(const std::string& owner,
                                        const std::string& repo,
                                        int since_days = 365,
                                        const std::string& branch = "",
                                        int max_commits = 100,
                                        const std::string& path = "");

    // 18. github_module_timeline_analysis - 三层职责统一入口
    //   target_type: "file" / "module" / "signature"
    //   target_path: 文件路径(file 类型时必填)
    //   module_name: 模块名(module 类型时必填)
    //   signature_regex: 代码特征(signature 类型时必填,简单子串匹配)
    //   time_range: "1y" / "180d" / "90d" / "30d"
    //   layer: 1=轻量索引层(只返回候选) 2=定点深挖(完整时序) 3=二级递进(向外扩散)
    //   ingest_first: 若本地无数据,先增量抓取 commits
    //   branch: 分支名(空表示默认分支;支持 codex/cxcore-integration 等带斜杠分支)
    // 返回结构按 layer 不同:
    //   layer=1: {candidates:[{file_path,module_name,change_count,last_commit_time}]}
    //   layer=2: {timeline:[...], contributor_rank:[...], related_files:[...],
    //             change_density:[...]}
    //   layer=3: layer=2 全部 + {developer_modules:{user:[...]}, coupled_clusters:[...]}
    json module_timeline_analysis(const std::string& owner,
                                   const std::string& repo,
                                   const std::string& target_type,
                                   const std::string& target_path = "",
                                   const std::string& module_name = "",
                                   const std::string& signature_regex = "",
                                   const std::string& time_range = "1y",
                                   int layer = 2,
                                   bool ingest_first = true,
                                   const std::string& branch = "");

    // 19. github_subdir_timeline_slice - 原语A:子模块拆分时序切片
    //   按一级子目录分组,分别生成独立变更密度曲线,实现热点迁徙观测
    //   root_path 如 "drivers/usb",自动扫描 typec/gadget/host/serial/core 等子目录
    //   返回:{subdirs:[{subdir, full_path, total_changes, monthly_density:[...]}], summary}
    json subdir_timeline_slice(const std::string& owner,
                                const std::string& repo,
                                const std::string& root_path,
                                const std::string& time_range = "1y",
                                bool ingest_first = true,
                                const std::string& branch = "");

    // 20. github_maintenance_attribution - 原语B:维护链路归因分析
    //   区分开发提交 vs 合并提交(Merge commit),还原内核维护流水线
    //   社区开发者 → 子系统维护者(gregkh)收集 → 补丁集 tag → 主线合并(torvalds)
    //   返回:{contributors:[{author_login, role, dev_commits, merge_commits, merge_ratio, ...}],
    //         maintainers:[...], developers:[...], merge_samples:[...]}
    json maintenance_attribution(const std::string& owner,
                                  const std::string& repo,
                                  const std::string& target_path,
                                  const std::string& time_range = "1y",
                                  bool ingest_first = true,
                                  const std::string& branch = "");

    // 后端是否就绪
    bool is_ready() const { return http_client_->is_ready(); }

    // 设置代理(必须在首次请求前调用)
    // libcurl:CURLOPT_PROXY;WebView2:Chromium --proxy-server
    void set_proxy(const std::string& proxy_url) {
        proxy_url_ = proxy_url;
        if (!proxy_url.empty()) {
            http_client_->set_proxy(proxy_url);
        }
    }

    // 设置独立 user data dir(仅 WebView2 后端生效,libcurl 后端为 no-op)
    // 用于 8 源会话隔离,避免与其他源共用默认路径导致 0x800700aa
    void set_user_data_dir(const std::string& dir) {
        user_data_dir_ = dir;
#ifdef RESEARCH_MCP_USE_WEBVIEW2
        if (backend_ == Backend::WebView2) {
            static_cast<WebViewClient*>(http_client_.get())->set_user_data_dir(dir);
        }
#endif
    }

    // 当前使用的后端名称("libcurl" 或 "webview2")
    std::string backend_name() const {
        return http_client_->backend_name();
    }

    // 首次请求前确保后端已就绪
    // 返回 false 表示后端不可用(libcurl 链接失败 / WebView2 Edge Runtime 缺失)
    bool ensure_ready() {
        if (http_client_->is_ready()) return true;

        std::cerr << "[github] initializing " << backend_name() << " backend..." << std::endl;
        if (http_client_->initialize()) {
            std::cerr << "[github] " << backend_name() << " backend ready" << std::endl;
            return true;
        }

        std::cerr << "[github] ERROR: " << backend_name() << " init failed (no fallback)"
                  << std::endl;
        return false;
    }

private:
    // 统一 GET 请求封装
    // endpoint: /repos/{owner}/{repo} 等
    // params: query string 参数(未编码)
    // accept: Accept 头(覆盖默认 v3+json),用于 readme/file 走 raw
    // 返回 JSON;若响应是纯文本(readme/file),则抛 GitHubAPIError 由调用方处理
    json http_get(const std::string& endpoint,
                  const std::map<std::string, std::string>& params = {},
                  const std::optional<std::string>& accept = std::nullopt);

    // 统一 GET 请求,返回原始文本(用于 readme/file)
    std::string http_get_text(const std::string& endpoint,
                              const std::optional<std::string>& accept = std::nullopt);

    // 拼接完整 URL
    static std::string build_url(const std::string& endpoint,
                                 const std::map<std::string, std::string>& params = {});

    std::unique_ptr<IHttpClient> http_client_;
    Backend backend_ = Backend::Curl;
    std::string user_data_dir_;  // 仅 WebView2 后端使用
    int timeout_seconds_ = 30;
    std::string proxy_url_;
    std::map<std::string, std::string> headers_;
    std::string base_url_ = "https://api.github.com";
    std::string last_ingest_error_;  // ingest 链路最后一次错误(空=无错误)
};

} // namespace github_research
