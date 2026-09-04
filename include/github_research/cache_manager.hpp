#pragma once
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>
#include "github_research/source_fusion.hpp"

// 前向声明 sqlite3
struct sqlite3;
struct sqlite3_stmt;

namespace github_research {

using json = nlohmann::json;

// 抓取回调: 给定 entity_key (如 repo_full_name), 返回标准化 fields
// 若抓取失败,抛出 std::runtime_error
using FetchEntityCallback = std::function<std::map<std::string, json>(const std::string& entity_key)>;

// =============================================================
// CacheConfig: 缓存配置
// =============================================================
struct CacheConfig {
    bool enabled = true;
    std::string db_path = "./data/research_mcp_cache.db";
    int default_ttl_hours = 24;
    int max_blob_inline_chars = 4000;     // 超过此长度存入 cache_blobs
    int max_cache_size_mb = 500;
    bool auto_record_metrics = true;
    int graph_max_traverse_depth = 3;
    double graph_min_relation_weight = 0.2;
};

// =============================================================
// CacheEntry: 缓存条目返回值(轻量值对象)
// =============================================================
struct CacheEntry {
    int64_t id = 0;
    std::string source_type;      // github / arxiv / hn / npm / ...
    std::string cache_key;        // 唯一键
    std::string entity_id;
    std::string payload_type;     // json / html / pdf_text / markdown
    std::string payload;          // 实际内容(从 inline 或 blobs 合并)
    std::string content_hash;
    int64_t fetched_at = 0;
    int64_t expires_at = 0;
    int64_t hit_count = 0;
    int64_t last_accessed = 0;
    std::string fetch_status;     // ok / failed / partial / rate_limited
    std::string error_message;
};

// =============================================================
// CacheManager: 全局统一 SQLite 缓存层 + 关系索引层
//
//  - 全局单例,所有数据源 Client 共享同一个 db
//  - 接口: get / put / is_fresh / invalidate
//  - 实体索引: register_entity / find_entity
//  - 关系边: add_relation / query_relations / traverse_graph
//  - 时间序列: record_metric / query_timeseries
// =============================================================
class CacheManager {
public:
    // ── 单例 ─────────────────────────────────────────────────
    static CacheManager& instance();

    // 非拷贝
    CacheManager(const CacheManager&) = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    // ── 初始化 / 关闭 ─────────────────────────────────────────
    // 在 main 中调用一次(读取或生成 db_path,建表,开 WAL)
    bool init(const CacheConfig& cfg = CacheConfig{});
    void shutdown();

    // 是否已经 init
    bool is_ready() const { return db_ != nullptr; }
    const CacheConfig& config() const { return cfg_; }

    // ── 基础缓存 ─────────────────────────────────────────────
    // 查询缓存;命中返回 entry,未命中返回 nullopt
    // 命中同时更新 hit_count + last_accessed
    std::optional<CacheEntry> get(const std::string& source_type,
                                  const std::string& cache_key);

    // 写入缓存(自动计算 content_hash,按 TTL 计算 expires_at)
    // entity_id 可选;payload 超过 max_blob_inline_chars 自动走 cache_blobs
    // status: ok / failed / partial / rate_limited
    // 失败调用(网络错误等)也写入缓存(短 TTL),避免重复回源
    void put(const std::string& source_type,
             const std::string& cache_key,
             const std::string& payload,
             const std::string& payload_type = "json",
             int ttl_hours = 24,
             const std::string& entity_id = "",
             const std::string& fetch_status = "ok",
             const std::string& error_message = "");

    // 缓存是否在有效期内(不更新 hit_count)
    bool is_fresh(const std::string& source_type,
                  const std::string& cache_key);

    // 主动失效
    void invalidate(const std::string& source_type,
                    const std::string& cache_key);

    // ── 实体索引 ─────────────────────────────────────────────
    // 注册或查找实体;已存在则更新 last_seen_at + observation_count
    // 返回 entity_id
    std::string register_entity(const std::string& entity_type,
                                const std::string& canonical_name,
                                const std::vector<std::string>& aliases = {},
                                const std::vector<std::string>& tags = {},
                                const json& metadata = json::object(),
                                const std::string& description = "");

    // 按名称模糊查找实体(LIKE %name%),返回 entity_id 列表
    std::vector<std::string> find_entity(const std::string& name,
                                         const std::string& entity_type = "");

    // ── 关系索引 ─────────────────────────────────────────────
    void add_relation(const std::string& source_entity,
                      const std::string& target_entity,
                      const std::string& relation_type,
                      double weight = 1.0,
                      const std::string& evidence_source = "",
                      const std::string& evidence_ref = "");

    // direction: in / out / both
    std::vector<json> query_relations(const std::string& entity_id,
                                       const std::string& direction = "both",
                                       const std::string& relation_type = "",
                                       double min_weight = 0.0,
                                       int limit = 50);

    // BFS 图谱遍历(二级递进挖掘的本地加速)
    json traverse_graph(const std::string& start_entity,
                         int max_depth = 2,
                         double min_weight = 0.3,
                         int limit_per_level = 20);

    // ── 时间序列 ─────────────────────────────────────────────
    void record_metric(const std::string& entity_id,
                       const std::string& metric_name,
                       double metric_value,
                       const std::string& source_type);

    std::vector<json> query_timeseries(const std::string& entity_id,
                                        const std::string& metric_name,
                                        int64_t start_time = 0,
                                        int64_t end_time = 0);

    // ── 诊断/统计 ────────────────────────────────────────────
    json stats();  // 返回 {total_entries, hit_count_sum, entity_count, relation_count}

    // ═══════════════════════════════════════════════════════════
    //  多源融合 + 策略降级层 (next1.txt)
    // ═══════════════════════════════════════════════════════════

    // ── 数据源注册 ───────────────────────────────────────────
    // 注册/更新一个数据源(写入 data_sources 表)
    void register_source(const std::string& source_id,
                         const std::string& source_type,
                         const std::string& base_url = "",
                         double reliability = 0.8,
                         int avg_latency_ms = 0,
                         int rate_limit_per_hour = 0,
                         double priority_weight = 1.0,
                         const std::string& config_json = "");

    // 注册抓取回调(工具层在 init 时调用)
    void register_source_fetch(const std::string& source_id,
                               FetchEntityCallback cb);

    // 获取数据源信息
    std::optional<DataSource> get_source(const std::string& source_id);

    // 记录某源抓取结果(成功/失败),更新熔断器 + data_sources
    void record_source_result(const std::string& source_id,
                               bool success,
                               int latency_ms = 0);

    // ── 实体-数据源映射 ─────────────────────────────────────
    // 注册实体可从某源获取,并记录 source_entity_key
    void register_entity_source(const std::string& entity_id,
                                const std::string& source_id,
                                const std::string& source_entity_key,
                                const std::vector<std::string>& fields_available = {},
                                double quality_score = 0.5);

    // ── 降级策略 ─────────────────────────────────────────────
    // 设置某实体类型的降级链
    void set_fallback_policy(const std::string& entity_type,
                              const std::vector<std::string>& priority_chain,
                              const std::string& field_name = "",
                              double min_quality = 0.3,
                              bool allow_stale = true,
                              int max_stale_hours = 72);

    // 获取降级链
    std::vector<std::string> get_fallback_chain(const std::string& entity_type);

    // ── 字段级融合存储 ───────────────────────────────────────
    // 写入/更新某实体某源的字段值(写入 entity_fields 表)
    void put_entity_fields(const std::string& entity_id,
                            const std::string& source_id,
                            const std::map<std::string, json>& fields,
                            double quality_score = 0.5);

    // 读取实体已融合的字段(从 entity_fields 读取,按 is_primary 优先)
    std::map<std::string, FieldValue> get_entity_fields(const std::string& entity_id);

    // ── 核心:多源融合查询 ───────────────────────────────────
    // 查询实体完整信息,自动多源融合 + 降级
    // 上层工具优先调用这个
    EntityData get_entity(const std::string& entity_id,
                           const std::string& entity_type = "",
                           const std::vector<std::string>& required_fields = {},
                           bool force_refresh = false);

    // 按名称查找实体ID,找不到返回空
    std::string find_entity_by_name(const std::string& name,
                                     const std::string& entity_type = "");

    // 跨源实体搜索(从本地 entity_index + entity_fields 搜索)
    std::vector<EntityData> search_entities(const std::string& query,
                                             const std::string& entity_type = "",
                                             double min_quality = 0.0,
                                             int limit = 20);

    // ── 健康状态 ─────────────────────────────────────────────
    // 返回各数据源健康状态
    json get_source_health();

    // 按 source_type 分组的缓存统计: {source_type: {total, ok, failed, stale, key_sample[]}}
    json get_cache_summary(int key_sample_limit = 5);

    // 列出指定 source_type 下最近 N 条缓存 key (按 updated_at 降序)
    json list_cache(const std::string& source_type = "", int limit = 20);

    // 强制刷新某实体(可指定数据源)
    void force_refresh_entity(const std::string& entity_id,
                               const std::string& source_id = "");

    // 熔断器访问
    CircuitBreaker& breaker() { return breaker_; }

    // ═══════════════════════════════════════════════════════════
    //  GitHub 项目局部对象连续动态分析索引 (next2)
    // ═══════════════════════════════════════════════════════════

    // ── 文件时序变更记录 ─────────────────────────────────────
    // 写入单条文件变更记录(已存在则忽略,UNIQUE 幂等)
    void record_file_timeline(const std::string& repo_full_name,
                              const std::string& file_path,
                              const std::string& commit_hash,
                              const std::string& author_login,
                              int64_t commit_time,
                              int lines_add,
                              int lines_del,
                              int pr_number = 0,
                              const std::string& commit_message = "");

    // 批量写入一个 commit 的所有文件变更 + 自动更新 file_cooccurrence
    // files: [(file_path, lines_add, lines_del), ...]
    // 返回: 实际新增的记录数(已存在的跳过)
    int record_commit_files(const std::string& repo_full_name,
                             const std::string& commit_hash,
                             const std::string& author_login,
                             int64_t commit_time,
                             const std::vector<std::tuple<std::string, int, int>>& files,
                             int pr_number = 0,
                             const std::string& commit_message = "");

    // 查询文件时序变更记录(按时间升序)
    // prefix_match=true 时按 file_path 前缀匹配(目录查询:传入 "drivers/net" 可匹配
    //   "drivers/net/eth0.c" 等所有子文件),用于 target_path 为目录的场景
    std::vector<json> query_file_timeline(const std::string& repo_full_name,
                                           const std::string& file_path,
                                           int64_t since = 0,
                                           int64_t until = 0,
                                           int limit = 500,
                                           bool prefix_match = false);

    // ── 模块定义 ─────────────────────────────────────────────
    // 注册/更新模块定义,返回模块 id(失败返回 0)
    int64_t register_module_def(const std::string& repo_full_name,
                                 const std::string& module_name,
                                 const std::string& path_pattern,
                                 const std::string& tag = "",
                                 bool auto_generated = false);

    // 自动聚类模块(按顶层目录划分,如 src/* tests/* docs/*)
    // 返回: 生成的模块数
    int auto_cluster_modules(const std::string& repo_full_name,
                              const std::vector<std::string>& all_files);

    // 查询模块下的文件(通过 path_pattern glob 匹配,从 file_timeline 中找已记录文件)
    std::vector<std::string> query_module_files(const std::string& repo_full_name,
                                                 const std::string& module_name);

    // 列出仓库的所有模块定义
    std::vector<json> list_modules(const std::string& repo_full_name);

    // ── 模块-贡献者预聚合 ───────────────────────────────────
    // 触发预聚合:扫描 file_timeline,按窗口生成 module_contributor_agg
    // window_days: 90 / 180 / 365
    // 返回: 生成的聚合记录数
    int aggregate_module_contributors(const std::string& repo_full_name,
                                       int window_days = 365);

    // 查询模块贡献者排名(按 total_changes 降序)
    std::vector<json> query_module_contributors(const std::string& repo_full_name,
                                                 const std::string& module_name,
                                                 int window_days = 365,
                                                 int limit = 30);

    // ── 文件协同变更 ─────────────────────────────────────────
    // 查询与目标文件协同变更最多的其他文件
    std::vector<json> query_related_files(const std::string& repo_full_name,
                                           const std::string& file_path,
                                           int min_co_count = 2,
                                           int limit = 20);

    // ── 代码特征检索 ─────────────────────────────────────────
    // 按代码特征正则匹配 commit_message 或 file_path,返回文件列表
    // (注:简单 substring 匹配,不引入完整 regex 库依赖)
    std::vector<json> search_by_signature(const std::string& repo_full_name,
                                           const std::string& signature_pattern,
                                           int64_t since = 0,
                                           int limit = 100);

    // ── 模块时序统计 ─────────────────────────────────────────
    // 查询模块/文件的变更密度(按月统计改动频次)
    std::vector<json> query_change_density(const std::string& repo_full_name,
                                            const std::string& file_path,
                                            int64_t since = 0,
                                            int limit = 24);

    // 查询开发者在该仓库改动的所有模块(二级递进:开发者扩散)
    std::vector<json> query_developer_modules(const std::string& repo_full_name,
                                               const std::string& user_login,
                                               int window_days = 365,
                                               int limit = 30);

    // ── 子模块拆分时序切片(原语A) ────────────────────────────
    // 按一级子目录聚合变更密度曲线,实现热点迁徙观测
    // root_path 如 "drivers/usb",自动扫描 file_timeline 中所有前缀为
    //   "drivers/usb/<subdir>/" 的 file_path,按 subdir 分组返回月度统计
    std::vector<json> query_subdir_change_density(const std::string& repo_full_name,
                                                   const std::string& root_path,
                                                   int64_t since = 0,
                                                   int limit = 24);

    // ── 维护链路归因(原语B) ──────────────────────────────────
    // 区分开发提交 vs 合并提交(Merge commit),还原维护流水线
    // merge commit 识别规则:commit_message 以 "Merge" 开头
    // 返回 [{author_login, commit_type, commit_count, first_time, last_time, sample_messages}]
    std::vector<json> query_maintenance_attribution(const std::string& repo_full_name,
                                                     const std::string& path_prefix,
                                                     int64_t since = 0,
                                                     int limit = 50);

    // ═══════════════════════════════════════════════════════════
    //  定向知识雷达 — Focus 管理 (next.txt 定向蔓延)
    // ═══════════════════════════════════════════════════════════

    // ── Focus CRUD ──────────────────────────────────────────
    std::string create_focus(const std::string& name,
                             const std::string& description,
                             const std::vector<std::string>& seed_entity_ids,
                             const std::vector<std::string>& keywords,
                             const std::vector<std::string>& exclude_words = {},
                             int max_depth = 3,
                             double relevance_threshold = 0.55,
                             int max_nodes = 500);

    json get_focus(const std::string& focus_id);
    std::vector<json> list_focuses();
    bool update_focus(const std::string& focus_id, const json& patch);
    bool delete_focus(const std::string& focus_id, bool keep_entities = true);

    // ── Focus 成员 ───────────────────────────────────────────
    // 添加实体到 focus,自动计算 depth(基于已有成员的最小 depth+1)
    // 返回是否新增(vs 已存在)
    bool add_focus_member(const std::string& focus_id,
                          const std::string& entity_id,
                          int depth,
                          double relevance,
                          const std::string& sprawl_status = "active");

    std::vector<json> get_focus_members(const std::string& focus_id,
                                        const std::string& status = "",
                                        int limit = 100);

    bool update_member_status(const std::string& focus_id,
                              const std::string& entity_id,
                              const std::string& new_status,
                              double new_relevance = -1.0);

    // ── 属性级增量存储 (next.txt attributes 表) ──────────────
    // 写入一条属性,UNIQUE(entity_id, attr_key, attr_value, source)
    // 重复调用同值不同 source 会新增行(多源融合)
    int upsert_attribute(const std::string& entity_id,
                         const std::string& attr_key,
                         const std::string& attr_value_json,
                         const std::string& source,
                         double confidence = 0.5);

    // 读取某实体某 key 的所有属性(多源)
    std::vector<json> get_attributes(const std::string& entity_id,
                                      const std::string& attr_key = "");

    // 某实体某 key 的多源合并结果
    json get_merged_attribute(const std::string& entity_id, const std::string& attr_key);

    // ── 缺口检测与调度 (next.txt gaps 表) ───────────────────
    int upsert_gap(const std::string& focus_id,
                   const std::string& entity_id,
                   const std::string& missing_key,
                   double priority,
                   const std::string& reason = "",
                   const std::string& fetch_plan_json = "");

    std::vector<json> get_gaps(const std::string& focus_id,
                               double min_priority = 0.0,
                               int limit = 20);

    bool resolve_gap(int64_t gap_id);

    // ── 提取任务 (next.txt extraction_jobs 表) ────────────────
    int64_t create_extraction_job(const std::string& entity_id,
                                  const std::string& job_type,
                                  const std::string& prompt,
                                  const std::string& input_ref = "");

    bool update_extraction_job(int64_t job_id,
                               const std::string& status,
                               const std::string& result_json = "",
                               const std::string& error = "");

    // ── 跟踪计划 (next.txt track_schedules 表) ───────────────
    int create_track_schedule(const std::string& focus_id,
                              const std::string& entity_id,
                              const std::string& track_type,
                              int interval_hours);

    std::vector<json> get_due_tracks(int limit = 30);

    bool update_track_interval(int schedule_id, int new_interval_hours);

    // ── 蔓延统计 ─────────────────────────────────────────────
    json get_sprawl_stats(const std::string& focus_id = "");

private:
    CacheManager() = default;
    ~CacheManager() { shutdown(); }

    // ── 内部辅助 ─────────────────────────────────────────────
    bool exec_sql(const std::string& sql);
    int64_t now_sec();
    std::string sha256_hex(const std::string& input);
    std::string gen_entity_id(const std::string& entity_type,
                               const std::string& canonical_name);
    bool table_exists(const std::string& table);
    bool column_exists(const std::string& table, const std::string& column);
    void alter_add_column(const std::string& table, const std::string& column,
                           const std::string& type_def);

    // payload 大小决定 inline 或 blob;blob 写 cache_blobs 返回 id 字符串
    std::string store_payload(const std::string& payload);
    std::string load_payload(const std::string& payload_ref,
                              const std::string& payload_inline);

    // SQLite 句柄
    sqlite3* db_ = nullptr;
    CacheConfig cfg_;
    std::mutex mu_;  // 串行写,避免锁竞争
    bool inited_ = false;

    // 多源融合层
    CircuitBreaker breaker_;
    std::map<std::string, FetchEntityCallback> fetch_callbacks_;
    CleanerPipeline cleaner_;
};

} // namespace github_research
