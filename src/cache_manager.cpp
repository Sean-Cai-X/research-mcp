#include "github_research/cache_manager.hpp"
#include <sqlite3.h>
#include <chrono>
#include <cstring>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <set>

// Windows CryptoAPI 用于 SHA256(不引入 openssl 依赖)
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace github_research {

// =============================================================
// 工具辅助(SQL 语句构造 + 绑定 + 执行)
// =============================================================
namespace {

std::string sql_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

// 轻量级 SQLite 语句封装(避免泄漏)
struct Stmt {
    sqlite3_stmt* p = nullptr;
    ~Stmt() { if (p) sqlite3_finalize(p); }
    explicit operator bool() const { return p != nullptr; }
};

bool prepare(sqlite3* db, Stmt& s, const std::string& sql) {
    int rc = sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &s.p, nullptr);
    return rc == SQLITE_OK;
}

} // anon

// =============================================================
// 单例
// =============================================================
CacheManager& CacheManager::instance() {
    static CacheManager inst;
    return inst;
}

// =============================================================
// 内部辅助
// =============================================================
int64_t CacheManager::now_sec() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string CacheManager::sha256_hex(const std::string& input) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }
    HCRYPTHASH hHash = 0;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    if (!CryptHashData(hHash, (const BYTE*)input.data(), (DWORD)input.size(), 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    DWORD hashLen = 0;
    DWORD sz = sizeof(hashLen);
    CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&hashLen, &sz, 0);
    std::vector<BYTE> hashBuf(hashLen);
    sz = hashLen;
    CryptGetHashParam(hHash, HP_HASHVAL, hashBuf.data(), &sz, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(hashLen * 2);
    for (auto b : hashBuf) {
        out += hex[(b >> 4) & 0xF];
        out += hex[b & 0xF];
    }
    return out;
#else
    return std::to_string(std::hash<std::string>{}(input));
#endif
}

std::string CacheManager::gen_entity_id(const std::string& entity_type,
                                          const std::string& canonical_name) {
    std::string raw = entity_type + ":" + canonical_name;
    return "ent_" + sha256_hex(raw).substr(0, 16);
}

bool CacheManager::table_exists(const std::string& table) {
    if (!db_) return false;
    std::string sql = "SELECT name FROM sqlite_master WHERE type='table' AND name='"
                    + sql_escape(table) + "';";
    Stmt s;
    if (!prepare(db_, s, sql)) return false;
    return sqlite3_step(s.p) == SQLITE_ROW;
}

bool CacheManager::column_exists(const std::string& table, const std::string& column) {
    if (!db_) return false;
    // PRAGMA table_info 返回列信息
    std::string sql = "PRAGMA table_info(" + table + ");";
    Stmt s;
    if (!prepare(db_, s, sql)) return false;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(s.p, 1);
        if (name && name == column) return true;
    }
    return false;
}

void CacheManager::alter_add_column(const std::string& table, const std::string& column,
                                      const std::string& type_def) {
    if (!db_) return;
    if (column_exists(table, column)) return;  // 幂等
    std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + column + " " + type_def + ";";
    exec_sql(sql);
}

bool CacheManager::exec_sql(const std::string& sql) {
    if (!db_) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[cache] SQL error: " << (err ? err : "?") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

std::string CacheManager::store_payload(const std::string& payload) {
    if ((int)payload.size() <= cfg_.max_blob_inline_chars) {
        return "";  // payload_ref 空 = 用 inline
    }
    std::string sql = "INSERT INTO cache_blobs(content,compressed,char_count,created_at) VALUES('"
                    + sql_escape(payload) + "',0," + std::to_string((int)payload.size()) + ","
                    + std::to_string(now_sec()) + ");";
    if (!exec_sql(sql)) return "";
    return std::to_string(sqlite3_last_insert_rowid(db_));
}

std::string CacheManager::load_payload(const std::string& payload_ref,
                                        const std::string& payload_inline) {
    if (payload_ref.empty()) return payload_inline;
    std::string sql = "SELECT content FROM cache_blobs WHERE id=" + payload_ref + " LIMIT 1;";
    Stmt s;
    if (!prepare(db_, s, sql)) return payload_inline;
    if (sqlite3_step(s.p) != SQLITE_ROW) return payload_inline;
    const char* txt = (const char*)sqlite3_column_text(s.p, 0);
    return txt ? txt : "";
}

// =============================================================
// init / shutdown
// =============================================================
bool CacheManager::init(const CacheConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (inited_) return true;
    cfg_ = cfg;
    if (!cfg_.enabled) {
        inited_ = true;
        return true;
    }

    // 确保 db 目录存在
    std::filesystem::path dbp(cfg_.db_path);
    if (dbp.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(dbp.parent_path(), ec);
    }

    int rc = sqlite3_open_v2(
        cfg_.db_path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (rc != SQLITE_OK || !db_) {
        std::cerr << "[cache] sqlite open failed: rc=" << rc << std::endl;
        return false;
    }

    // WAL 模式(支持并发读)
    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA synchronous=NORMAL;");

    // 建表 1: cache_entries
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS cache_entries (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    source_type     TEXT    NOT NULL,
    cache_key       TEXT    NOT NULL,
    entity_id       TEXT,
    payload_type    TEXT    NOT NULL,
    payload_ref     TEXT,
    payload_inline  TEXT,
    content_hash    TEXT    NOT NULL,
    fetched_at      INTEGER NOT NULL,
    expires_at      INTEGER,
    hit_count       INTEGER DEFAULT 0,
    last_accessed   INTEGER,
    fetch_status    TEXT DEFAULT 'ok',
    error_message   TEXT,
    UNIQUE(source_type, cache_key)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_cache_source_key ON cache_entries(source_type, cache_key);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_cache_entity     ON cache_entries(entity_id);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_cache_expires    ON cache_entries(expires_at);");

    // 建表 2: cache_blobs
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS cache_blobs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    content     TEXT    NOT NULL,
    compressed  INTEGER DEFAULT 0,
    char_count  INTEGER,
    created_at  INTEGER NOT NULL
);
    )");

    // 建表 3: entity_index
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS entity_index (
    entity_id       TEXT PRIMARY KEY,
    entity_type     TEXT NOT NULL,
    canonical_name  TEXT NOT NULL,
    aliases         TEXT,
    first_seen_at   INTEGER NOT NULL,
    last_seen_at    INTEGER NOT NULL,
    observation_count INTEGER DEFAULT 1,
    tags            TEXT,
    metadata        TEXT,
    description     TEXT
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_entity_type ON entity_index(entity_type);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_entity_name ON entity_index(canonical_name);");

    // 建表 4: relation_links
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS relation_links (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    source_entity   TEXT NOT NULL,
    target_entity   TEXT NOT NULL,
    relation_type   TEXT NOT NULL,
    weight          REAL DEFAULT 1.0,
    evidence_source TEXT NOT NULL,
    evidence_ref    TEXT,
    first_observed  INTEGER NOT NULL,
    last_observed   INTEGER NOT NULL,
    observation_count INTEGER DEFAULT 1,
    UNIQUE(source_entity, target_entity, relation_type)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_rel_source ON relation_links(source_entity);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_rel_target ON relation_links(target_entity);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_rel_type   ON relation_links(relation_type);");

    // 建表 5: time_series
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS time_series (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_id       TEXT NOT NULL,
    metric_name     TEXT NOT NULL,
    metric_value    REAL NOT NULL,
    measured_at     INTEGER NOT NULL,
    source_type     TEXT NOT NULL,
    delta_from_prev REAL,
    UNIQUE(entity_id, metric_name, measured_at)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ts_entity_metric ON time_series(entity_id, metric_name);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ts_time          ON time_series(measured_at);");

    // ═════════════════════════════════════════════════════════════
    //  多源融合 + 策略降级层 (next1.txt)
    // ═════════════════════════════════════════════════════════════

    // 建表 6: data_sources (数据源注册表)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS data_sources (
    source_id       TEXT PRIMARY KEY,
    source_type     TEXT NOT NULL,
    base_url        TEXT,
    reliability     REAL DEFAULT 0.8,
    avg_latency_ms  INTEGER,
    rate_limit_per_hour INTEGER,
    priority_weight REAL DEFAULT 1.0,
    enabled         INTEGER DEFAULT 1,
    last_failure_at INTEGER,
    consecutive_failures INTEGER DEFAULT 0,
    config_json     TEXT
);
    )");

    // 建表 7: entity_sources (实体-数据源映射)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS entity_sources (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_id       TEXT NOT NULL,
    source_id       TEXT NOT NULL,
    source_entity_key TEXT NOT NULL,
    fetch_status    TEXT DEFAULT 'pending',
    last_fetched_at INTEGER,
    next_retry_at   INTEGER,
    quality_score   REAL,
    coverage_score  REAL,
    fields_available TEXT,
    error_message   TEXT,
    UNIQUE(entity_id, source_id)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_es_entity ON entity_sources(entity_id);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_es_source ON entity_sources(source_id);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_es_status ON entity_sources(fetch_status);");

    // 建表 8: entity_fields (字段级融合表)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS entity_fields (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_id       TEXT NOT NULL,
    field_name      TEXT NOT NULL,
    field_value     TEXT,
    field_type      TEXT DEFAULT 'string',
    source_id       TEXT NOT NULL,
    quality_score   REAL DEFAULT 0.5,
    last_updated    INTEGER NOT NULL,
    is_primary      INTEGER DEFAULT 0,
    UNIQUE(entity_id, field_name, source_id)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ef_entity_field ON entity_fields(entity_id, field_name);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ef_primary ON entity_fields(entity_id, is_primary);");

    // 建表 9: fallback_policies (降级策略表)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS fallback_policies (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_type     TEXT NOT NULL,
    field_name      TEXT,
    priority_chain  TEXT NOT NULL,
    min_quality     REAL DEFAULT 0.3,
    allow_stale     INTEGER DEFAULT 1,
    max_stale_hours INTEGER DEFAULT 72,
    retry_strategy  TEXT DEFAULT 'exponential',
    UNIQUE(entity_type, field_name)
);
    )");

    // ALTER cache_entries: 新增 5 列(幂等,检查列是否存在)
    alter_add_column("cache_entries", "source_id",          "TEXT");
    alter_add_column("cache_entries", "quality_score",      "REAL DEFAULT 0.5");
    alter_add_column("cache_entries", "coverage_score",     "REAL DEFAULT 0.5");
    alter_add_column("cache_entries", "is_fallback_result", "INTEGER DEFAULT 0");
    alter_add_column("cache_entries", "fallback_chain_used","TEXT");

    // ═══════════════════════════════════════════════════════════
    //  GitHub 项目局部对象连续动态分析索引 (next2)
    //  - file_timeline:        文件时序变更记录
    //  - module_def:           模块定义(自动聚类 + 人工标注)
    //  - module_contributor_agg: 模块-贡献者预聚合
    //  - file_cooccurrence:    文件协同变更邻接表
    // ═══════════════════════════════════════════════════════════

    exec_sql(R"(
CREATE TABLE IF NOT EXISTS file_timeline (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_full_name  TEXT    NOT NULL,
    file_path       TEXT    NOT NULL,
    commit_hash     TEXT    NOT NULL,
    author_login    TEXT    NOT NULL,
    commit_time     INTEGER NOT NULL,
    lines_add       INTEGER DEFAULT 0,
    lines_del       INTEGER DEFAULT 0,
    pr_number       INTEGER,
    commit_message  TEXT,
    UNIQUE(repo_full_name, file_path, commit_hash)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ft_repo_file ON file_timeline(repo_full_name, file_path);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ft_repo_time ON file_timeline(repo_full_name, commit_time);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ft_author    ON file_timeline(author_login);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ft_commit    ON file_timeline(commit_hash);");

    exec_sql(R"(
CREATE TABLE IF NOT EXISTS module_def (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_full_name  TEXT    NOT NULL,
    module_name     TEXT    NOT NULL,
    path_pattern    TEXT    NOT NULL,
    tag             TEXT,
    auto_generated  INTEGER DEFAULT 0,
    UNIQUE(repo_full_name, module_name)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_md_repo ON module_def(repo_full_name);");

    exec_sql(R"(
CREATE TABLE IF NOT EXISTS module_contributor_agg (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_full_name      TEXT    NOT NULL,
    module_name         TEXT    NOT NULL,
    user_login          TEXT    NOT NULL,
    time_window_start   INTEGER NOT NULL,
    time_window_end     INTEGER NOT NULL,
    window_days         INTEGER NOT NULL,
    total_changes       INTEGER DEFAULT 0,
    total_lines_add     INTEGER DEFAULT 0,
    total_lines_del     INTEGER DEFAULT 0,
    first_commit_time   INTEGER,
    last_commit_time    INTEGER,
    UNIQUE(repo_full_name, module_name, user_login, window_days, time_window_start)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_mca_repo_module ON module_contributor_agg(repo_full_name, module_name);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_mca_user        ON module_contributor_agg(user_login);");

    exec_sql(R"(
CREATE TABLE IF NOT EXISTS file_cooccurrence (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_full_name      TEXT    NOT NULL,
    file_a              TEXT    NOT NULL,
    file_b              TEXT    NOT NULL,
    co_commit_count     INTEGER DEFAULT 1,
    last_co_commit_time INTEGER,
    UNIQUE(repo_full_name, file_a, file_b)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_fc_repo_a ON file_cooccurrence(repo_full_name, file_a);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_fc_repo_b ON file_cooccurrence(repo_full_name, file_b);");

    // ═══════════════════════════════════════════════════════════
    //  定向知识雷达 — Focus 管理层 (next.txt 定向蔓延)
    // ═══════════════════════════════════════════════════════════

    // 表 14: focuses (关注域)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS focuses (
    id              TEXT PRIMARY KEY,
    name            TEXT NOT NULL,
    description     TEXT,
    seed_entities   TEXT NOT NULL,
    keywords        TEXT NOT NULL,
    exclude_words   TEXT,
    allowed_rels    TEXT NOT NULL,
    allowed_sources TEXT,
    max_depth       INTEGER DEFAULT 3,
    relevance_threshold REAL DEFAULT 0.55,
    max_nodes       INTEGER DEFAULT 500,
    status          TEXT DEFAULT 'active',
    created_at      INTEGER NOT NULL,
    last_crawl_at   INTEGER
);
    )");

    // 表 15: focus_members (实体-关注域关联)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS focus_members (
    focus_id        TEXT NOT NULL REFERENCES focuses(id),
    entity_id       TEXT NOT NULL,
    depth           INTEGER NOT NULL,
    relevance       REAL NOT NULL,
    sprawl_status   TEXT NOT NULL,
    last_checked    INTEGER,
    check_count     INTEGER DEFAULT 0,
    PRIMARY KEY (focus_id, entity_id)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_fm_status   ON focus_members(focus_id, sprawl_status);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_fm_relevance ON focus_members(focus_id, relevance DESC);");

    // 表 16: attributes (属性级增量存储 — 蔓延核心)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS attributes (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_id       TEXT NOT NULL,
    attr_key        TEXT NOT NULL,
    attr_value      TEXT NOT NULL,
    source          TEXT NOT NULL,
    source_id       TEXT,
    confidence      REAL NOT NULL DEFAULT 0.5,
    verified        INTEGER DEFAULT 0,
    extracted_at    INTEGER NOT NULL,
    model           TEXT,
    UNIQUE(entity_id, attr_key, attr_value, source)
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_attr_entity_key ON attributes(entity_id, attr_key);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_attr_confidence  ON attributes(confidence);");

    // 表 17: gaps (缺口调度)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS gaps (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    focus_id        TEXT NOT NULL REFERENCES focuses(id),
    entity_id       TEXT NOT NULL,
    missing_key     TEXT NOT NULL,
    priority        REAL DEFAULT 0.5,
    reason          TEXT,
    fetch_plan      TEXT,
    created_at      INTEGER NOT NULL,
    resolved_at     INTEGER
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_gaps_unresolved ON gaps(resolved_at);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_gaps_priority  ON gaps(focus_id, priority DESC);");

    // 表 18: extraction_jobs (提取任务追踪)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS extraction_jobs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_id       TEXT,
    job_type        TEXT NOT NULL,
    target_key      TEXT,
    prompt          TEXT NOT NULL,
    input_ref       TEXT,
    status          TEXT DEFAULT 'pending',
    result          TEXT,
    attempts        INTEGER DEFAULT 0,
    created_at      INTEGER NOT NULL,
    completed_at    INTEGER
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_job_status ON extraction_jobs(status);");

    // 表 19: track_schedules (自适应跟踪)
    exec_sql(R"(
CREATE TABLE IF NOT EXISTS track_schedules (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    focus_id        TEXT NOT NULL REFERENCES focuses(id),
    entity_id       TEXT NOT NULL,
    track_type      TEXT NOT NULL,
    interval_hours  INTEGER NOT NULL,
    last_checked    INTEGER,
    last_result     TEXT,
    consecutive_empty INTEGER DEFAULT 0
);
    )");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_ts_due ON track_schedules(last_checked);");

    inited_ = true;
    return true;
}

void CacheManager::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    inited_ = false;
}

// =============================================================
// get
// =============================================================
std::optional<CacheEntry> CacheManager::get(const std::string& source_type,
                                             const std::string& cache_key) {
    if (!cfg_.enabled || !db_) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);

    std::string sql = R"(
SELECT id,source_type,cache_key,entity_id,payload_type,payload_ref,payload_inline,
       content_hash,fetched_at,expires_at,hit_count,last_accessed,fetch_status,error_message
FROM cache_entries
WHERE source_type=? AND cache_key=? LIMIT 1;
    )";
    Stmt s;
    if (!prepare(db_, s, sql)) return std::nullopt;
    sqlite3_bind_text(s.p, 1, source_type.c_str(), (int)source_type.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, cache_key.c_str(), (int)cache_key.size(), SQLITE_TRANSIENT);

    if (sqlite3_step(s.p) != SQLITE_ROW) return std::nullopt;

    CacheEntry e;
    e.id             = sqlite3_column_int64(s.p, 0);
    e.source_type    = (const char*)sqlite3_column_text(s.p, 1);
    e.cache_key      = (const char*)sqlite3_column_text(s.p, 2);
    const char* ei   = (const char*)sqlite3_column_text(s.p, 3);
    e.entity_id      = ei ? ei : "";
    e.payload_type   = (const char*)sqlite3_column_text(s.p, 4);
    const char* pref = (const char*)sqlite3_column_text(s.p, 5);
    const char* pinl = (const char*)sqlite3_column_text(s.p, 6);
    e.payload        = load_payload(pref ? pref : "", pinl ? pinl : "");
    e.content_hash   = (const char*)sqlite3_column_text(s.p, 7);
    e.fetched_at     = sqlite3_column_int64(s.p, 8);
    e.expires_at     = sqlite3_column_int64(s.p, 9);
    e.hit_count      = sqlite3_column_int64(s.p, 10);
    e.last_accessed  = sqlite3_column_int64(s.p, 11);
    const char* st   = (const char*)sqlite3_column_text(s.p, 12);
    e.fetch_status   = st ? st : "ok";
    const char* em   = (const char*)sqlite3_column_text(s.p, 13);
    e.error_message  = em ? em : "";

    // 先 UPDATE hit_count + last_accessed,然后重新读取(保证返回值是更新后的值)
    int64_t ts = now_sec();
    std::string upd = "UPDATE cache_entries SET hit_count=hit_count+1,last_accessed="
                    + std::to_string(ts) + " WHERE id=" + std::to_string(e.id) + ";";
    exec_sql(upd);
    // 重新读 hit_count / last_accessed
    {
        std::string sql2 = "SELECT hit_count,last_accessed FROM cache_entries WHERE id=? LIMIT 1;";
        Stmt s2;
        if (prepare(db_, s2, sql2)) {
            sqlite3_bind_int64(s2.p, 1, e.id);
            if (sqlite3_step(s2.p) == SQLITE_ROW) {
                e.hit_count     = sqlite3_column_int64(s2.p, 0);
                e.last_accessed = sqlite3_column_int64(s2.p, 1);
            }
        }
    }

    // 已过期(且不是失败态缓存) → 视为未命中(但记录了 hit 统计,帮助后续 LRU)
    if (e.expires_at > 0 && e.expires_at < ts && e.fetch_status == "ok") {
        return std::nullopt;
    }
    return e;
}

// =============================================================
// put
// =============================================================
void CacheManager::put(const std::string& source_type,
                       const std::string& cache_key,
                       const std::string& payload,
                       const std::string& payload_type,
                       int ttl_hours,
                       const std::string& entity_id,
                       const std::string& fetch_status,
                       const std::string& error_message) {
    if (!cfg_.enabled || !db_) return;
    std::lock_guard<std::mutex> lk(mu_);

    int64_t ts = now_sec();
    int64_t exp = 0;
    if (ttl_hours > 0) exp = ts + (int64_t)ttl_hours * 3600;
    // 失败态缓存:短 TTL(5 分钟),避免短时间内重复回源
    if (fetch_status != "ok" && exp > ts + 300) exp = ts + 300;

    std::string hash = sha256_hex(payload);
    if (hash.empty()) hash = "h" + std::to_string(payload.size());

    std::string ref = store_payload(payload);
    std::string inline_text = ref.empty() ? payload : "";

    // 使用 INSERT OR REPLACE 处理 UNIQUE 冲突
    std::string sql = "INSERT OR REPLACE INTO cache_entries("
        "source_type,cache_key,entity_id,payload_type,payload_ref,payload_inline,"
        "content_hash,fetched_at,expires_at,hit_count,last_accessed,fetch_status,error_message) VALUES(";
    sql += "'" + sql_escape(source_type) + "',";
    sql += "'" + sql_escape(cache_key) + "',";
    sql += entity_id.empty() ? "NULL," : ("'" + sql_escape(entity_id) + "',");
    sql += "'" + sql_escape(payload_type) + "',";
    sql += ref.empty() ? "NULL," : ("'" + sql_escape(ref) + "',");
    sql += inline_text.empty() ? "NULL," : ("'" + sql_escape(inline_text) + "',");
    sql += "'" + sql_escape(hash) + "',";
    sql += std::to_string(ts) + ",";
    sql += std::to_string(exp) + ",";
    sql += "COALESCE((SELECT hit_count FROM cache_entries WHERE source_type='"
         + sql_escape(source_type) + "' AND cache_key='" + sql_escape(cache_key) + "'),0),";
    sql += std::to_string(ts) + ",";
    sql += "'" + sql_escape(fetch_status) + "',";
    sql += error_message.empty() ? "NULL" : ("'" + sql_escape(error_message) + "'");
    sql += ");";

    exec_sql(sql);
}

// =============================================================
// is_fresh / invalidate
// =============================================================
bool CacheManager::is_fresh(const std::string& source_type,
                             const std::string& cache_key) {
    if (!cfg_.enabled || !db_) return false;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT expires_at,fetch_status FROM cache_entries "
                      "WHERE source_type=? AND cache_key=? LIMIT 1;";
    Stmt s;
    if (!prepare(db_, s, sql)) return false;
    sqlite3_bind_text(s.p, 1, source_type.c_str(), (int)source_type.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, cache_key.c_str(), (int)cache_key.size(), SQLITE_TRANSIENT);
    if (sqlite3_step(s.p) != SQLITE_ROW) return false;
    int64_t exp = sqlite3_column_int64(s.p, 0);
    const char* fs = (const char*)sqlite3_column_text(s.p, 1);
    int64_t ts = now_sec();
    if (exp == 0) return true;                       // 永不过期
    if (fs && std::strcmp(fs, "ok") != 0) return false;  // 失败态不算 fresh
    return exp > ts;
}

void CacheManager::invalidate(const std::string& source_type,
                               const std::string& cache_key) {
    if (!cfg_.enabled || !db_) return;
    std::lock_guard<std::mutex> lk(mu_);
    exec_sql("DELETE FROM cache_entries WHERE source_type='"
             + sql_escape(source_type) + "' AND cache_key='" + sql_escape(cache_key) + "';");
}

// =============================================================
// register_entity
// =============================================================
std::string CacheManager::register_entity(const std::string& entity_type,
                                            const std::string& canonical_name,
                                            const std::vector<std::string>& aliases,
                                            const std::vector<std::string>& tags,
                                            const json& metadata,
                                            const std::string& description) {
    if (!cfg_.enabled || !db_) return "";
    std::lock_guard<std::mutex> lk(mu_);

    std::string eid = gen_entity_id(entity_type, canonical_name);
    int64_t ts = now_sec();

    json aliases_j = aliases;
    json tags_j = tags;

    std::string aliases_txt = aliases.empty() ? "" : aliases_j.dump();
    std::string tags_txt = tags.empty() ? "" : tags_j.dump();
    std::string meta_txt = metadata.is_null() ? "" : metadata.dump();

    // 先检查是否存在
    std::string sql = "SELECT first_seen_at,observation_count FROM entity_index WHERE entity_id=? LIMIT 1;";
    Stmt s;
    if (prepare(db_, s, sql)) {
        sqlite3_bind_text(s.p, 1, eid.c_str(), (int)eid.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(s.p) == SQLITE_ROW) {
            int64_t fs = sqlite3_column_int64(s.p, 0);
            int64_t oc = sqlite3_column_int64(s.p, 1);
            // UPDATE last_seen + observation_count
            std::string upd = "UPDATE entity_index SET last_seen_at=" + std::to_string(ts)
                            + ",observation_count=" + std::to_string(oc + 1);
            if (!aliases_txt.empty())  upd += ",aliases='" + sql_escape(aliases_txt) + "'";
            if (!tags_txt.empty())     upd += ",tags='" + sql_escape(tags_txt) + "'";
            if (!meta_txt.empty())     upd += ",metadata='" + sql_escape(meta_txt) + "'";
            if (!description.empty())  upd += ",description='" + sql_escape(description) + "'";
            upd += " WHERE entity_id='" + sql_escape(eid) + "';";
            exec_sql(upd);
            return eid;
        }
    }

    // INSERT
    std::string ins = "INSERT INTO entity_index(entity_id,entity_type,canonical_name,aliases,"
                      "first_seen_at,last_seen_at,observation_count,tags,metadata,description) VALUES(";
    ins += "'" + sql_escape(eid) + "',";
    ins += "'" + sql_escape(entity_type) + "',";
    ins += "'" + sql_escape(canonical_name) + "',";
    ins += aliases_txt.empty() ? "NULL," : ("'" + sql_escape(aliases_txt) + "',");
    ins += std::to_string(ts) + ",";
    ins += std::to_string(ts) + ",";
    ins += "1,";
    ins += tags_txt.empty() ? "NULL," : ("'" + sql_escape(tags_txt) + "',");
    ins += meta_txt.empty() ? "NULL," : ("'" + sql_escape(meta_txt) + "',");
    ins += description.empty() ? "NULL" : ("'" + sql_escape(description) + "'");
    ins += ");";
    exec_sql(ins);
    return eid;
}

std::vector<std::string> CacheManager::find_entity(const std::string& name,
                                                    const std::string& entity_type) {
    std::vector<std::string> out;
    if (!cfg_.enabled || !db_ || name.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT entity_id FROM entity_index WHERE canonical_name LIKE ?";
    if (!entity_type.empty()) sql += " AND entity_type=?";
    sql += " LIMIT 50;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    std::string like = "%" + name + "%";
    sqlite3_bind_text(s.p, 1, like.c_str(), (int)like.size(), SQLITE_TRANSIENT);
    if (!entity_type.empty())
        sqlite3_bind_text(s.p, 2, entity_type.c_str(), (int)entity_type.size(), SQLITE_TRANSIENT);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        const char* v = (const char*)sqlite3_column_text(s.p, 0);
        if (v) out.emplace_back(v);
    }
    return out;
}

// =============================================================
// add_relation / query_relations / traverse_graph
// =============================================================
void CacheManager::add_relation(const std::string& source_entity,
                                 const std::string& target_entity,
                                 const std::string& relation_type,
                                 double weight,
                                 const std::string& evidence_source,
                                 const std::string& evidence_ref) {
    if (!cfg_.enabled || !db_) return;
    if (source_entity.empty() || target_entity.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);

    int64_t ts = now_sec();
    std::string es = evidence_source.empty() ? "unknown" : evidence_source;

    // 先查现有
    std::string sql = "SELECT first_observed,observation_count,weight FROM relation_links "
                      "WHERE source_entity=? AND target_entity=? AND relation_type=? LIMIT 1;";
    Stmt s;
    if (prepare(db_, s, sql)) {
        sqlite3_bind_text(s.p, 1, source_entity.c_str(), (int)source_entity.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(s.p, 2, target_entity.c_str(), (int)target_entity.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(s.p, 3, relation_type.c_str(), (int)relation_type.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(s.p) == SQLITE_ROW) {
            int64_t fs = sqlite3_column_int64(s.p, 0);
            int64_t oc = sqlite3_column_int64(s.p, 1);
            double  ow = sqlite3_column_double(s.p, 2);
            double  nw = (std::max)(ow, weight);
            std::string upd = "UPDATE relation_links SET last_observed=" + std::to_string(ts)
                            + ",observation_count=" + std::to_string(oc + 1)
                            + ",weight=" + std::to_string(nw);
            if (!evidence_ref.empty())
                upd += ",evidence_ref='" + sql_escape(evidence_ref) + "'";
            upd += " WHERE source_entity='" + sql_escape(source_entity)
                 + "' AND target_entity='" + sql_escape(target_entity)
                 + "' AND relation_type='" + sql_escape(relation_type) + "';";
            exec_sql(upd);
            return;
        }
    }

    std::string ins = "INSERT INTO relation_links(source_entity,target_entity,relation_type,"
                      "weight,evidence_source,evidence_ref,first_observed,last_observed,observation_count)"
                      " VALUES(";
    ins += "'" + sql_escape(source_entity) + "',";
    ins += "'" + sql_escape(target_entity) + "',";
    ins += "'" + sql_escape(relation_type) + "',";
    ins += std::to_string(weight) + ",";
    ins += "'" + sql_escape(es) + "',";
    ins += evidence_ref.empty() ? "NULL," : ("'" + sql_escape(evidence_ref) + "',");
    ins += std::to_string(ts) + ",";
    ins += std::to_string(ts) + ",";
    ins += "1);";
    exec_sql(ins);
}

std::vector<json> CacheManager::query_relations(const std::string& entity_id,
                                                  const std::string& direction,
                                                  const std::string& relation_type,
                                                  double min_weight,
                                                  int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || entity_id.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);

    auto push = [&](sqlite3_stmt* p) {
        json r;
        r["id"]              = sqlite3_column_int64(p, 0);
        r["source_entity"]   = (const char*)sqlite3_column_text(p, 1);
        r["target_entity"]   = (const char*)sqlite3_column_text(p, 2);
        r["relation_type"]   = (const char*)sqlite3_column_text(p, 3);
        r["weight"]          = sqlite3_column_double(p, 4);
        r["evidence_source"] = (const char*)sqlite3_column_text(p, 5);
        const char* er       = (const char*)sqlite3_column_text(p, 6);
        r["evidence_ref"]    = er ? er : "";
        r["first_observed"]  = sqlite3_column_int64(p, 7);
        r["last_observed"]   = sqlite3_column_int64(p, 8);
        r["observation_count"] = sqlite3_column_int64(p, 9);
        out.push_back(r);
    };

    std::string base = "SELECT id,source_entity,target_entity,relation_type,weight,"
                       "evidence_source,evidence_ref,first_observed,last_observed,observation_count"
                       " FROM relation_links WHERE weight>=" + std::to_string(min_weight);

    auto run = [&](const std::string& where, const std::string& bind1) {
        std::string sql = base + where;
        if (!relation_type.empty()) sql += " AND relation_type=?";
        sql += " ORDER BY weight DESC LIMIT " + std::to_string(limit) + ";";
        Stmt s;
        if (!prepare(db_, s, sql)) return;
        sqlite3_bind_text(s.p, 1, bind1.c_str(), (int)bind1.size(), SQLITE_TRANSIENT);
        if (!relation_type.empty())
            sqlite3_bind_text(s.p, 2, relation_type.c_str(), (int)relation_type.size(), SQLITE_TRANSIENT);
        while (sqlite3_step(s.p) == SQLITE_ROW) push(s.p);
    };

    if (direction == "out")  run(" AND source_entity=?", entity_id);
    else if (direction == "in") run(" AND target_entity=?", entity_id);
    else { // both
        run(" AND source_entity=?", entity_id);
        size_t cur = out.size();
        run(" AND target_entity=?", entity_id);
        std::inplace_merge(out.begin(), out.begin() + cur, out.end(),
            [](const json& a, const json& b){
                return a.value("weight", 0.0) > b.value("weight", 0.0);
            });
        if ((int)out.size() > limit) out.resize(limit);
    }
    return out;
}

json CacheManager::traverse_graph(const std::string& start_entity,
                                    int max_depth,
                                    double min_weight,
                                    int limit_per_level) {
    if (!cfg_.enabled || !db_ || start_entity.empty() || max_depth < 1) {
        return {{"levels", json::array()}, {"nodes", json::array()}, {"edges", json::array()}};
    }
    std::lock_guard<std::mutex> lk(mu_);

    std::set<std::string> visited;
    std::vector<std::vector<std::string>> levels;
    std::map<std::string, std::string> node_type;
    std::map<std::string, std::string> node_name;
    std::vector<json> edges;

    levels.push_back({start_entity});
    visited.insert(start_entity);

    // 查起始节点名字/类型
    {
        std::string sql = "SELECT entity_type,canonical_name FROM entity_index WHERE entity_id=? LIMIT 1;";
        Stmt s;
        if (prepare(db_, s, sql)) {
            sqlite3_bind_text(s.p, 1, start_entity.c_str(), (int)start_entity.size(), SQLITE_TRANSIENT);
            if (sqlite3_step(s.p) == SQLITE_ROW) {
                node_type[start_entity] = (const char*)sqlite3_column_text(s.p, 0);
                node_name[start_entity] = (const char*)sqlite3_column_text(s.p, 1);
            }
        }
    }

    for (int d = 0; d < max_depth; ++d) {
        std::vector<std::string> next_level;
        for (auto& cur : levels.back()) {
            // BFS 出边
            std::string sql = "SELECT target_entity,relation_type,weight FROM relation_links "
                              "WHERE source_entity=? AND weight>=" + std::to_string(min_weight)
                              + " ORDER BY weight DESC LIMIT " + std::to_string(limit_per_level) + ";";
            Stmt s;
            if (prepare(db_, s, sql)) {
                sqlite3_bind_text(s.p, 1, cur.c_str(), (int)cur.size(), SQLITE_TRANSIENT);
                while (sqlite3_step(s.p) == SQLITE_ROW) {
                    std::string tgt = (const char*)sqlite3_column_text(s.p, 0);
                    std::string rel = (const char*)sqlite3_column_text(s.p, 1);
                    double w = sqlite3_column_double(s.p, 2);
                    edges.push_back({{"from", cur}, {"to", tgt}, {"relation", rel}, {"weight", w}, {"level", d + 1}});
                    if (!visited.count(tgt)) {
                        visited.insert(tgt);
                        next_level.push_back(tgt);
                    }
                }
            }
        }
        if (next_level.empty()) break;
        if ((int)next_level.size() > limit_per_level) next_level.resize(limit_per_level);
        levels.push_back(next_level);

        // 拉节点 meta
        for (auto& nid : next_level) {
            std::string sql = "SELECT entity_type,canonical_name FROM entity_index WHERE entity_id=? LIMIT 1;";
            Stmt s;
            if (prepare(db_, s, sql)) {
                sqlite3_bind_text(s.p, 1, nid.c_str(), (int)nid.size(), SQLITE_TRANSIENT);
                if (sqlite3_step(s.p) == SQLITE_ROW) {
                    node_type[nid] = (const char*)sqlite3_column_text(s.p, 0);
                    node_name[nid] = (const char*)sqlite3_column_text(s.p, 1);
                }
            }
        }
    }

    json levels_j = json::array();
    for (auto& lv : levels) {
        json a = json::array();
        for (auto& x : lv) a.push_back(x);
        levels_j.push_back(a);
    }
    json nodes_j = json::array();
    for (auto& kv : node_name) {
        nodes_j.push_back({{"entity_id", kv.first},
                           {"name", kv.second},
                           {"type", node_type[kv.first]}});
    }
    json edges_j = json::array();
    for (auto& e : edges) edges_j.push_back(e);
    return {{"levels", levels_j}, {"nodes", nodes_j}, {"edges", edges_j}};
}

// =============================================================
// time_series
// =============================================================
void CacheManager::record_metric(const std::string& entity_id,
                                  const std::string& metric_name,
                                  double metric_value,
                                  const std::string& source_type) {
    if (!cfg_.enabled || !db_ || entity_id.empty() || metric_name.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    int64_t ts = now_sec();

    // 找上一次测量,计算 delta
    std::string sql = "SELECT metric_value FROM time_series WHERE entity_id=? AND metric_name=?"
                      " ORDER BY measured_at DESC LIMIT 1;";
    double prev = 0.0;
    bool has_prev = false;
    Stmt s;
    if (prepare(db_, s, sql)) {
        sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(s.p, 2, metric_name.c_str(), (int)metric_name.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(s.p) == SQLITE_ROW) {
            prev = sqlite3_column_double(s.p, 0);
            has_prev = true;
        }
    }
    double delta = has_prev ? (metric_value - prev) : 0.0;

    // 避免 UNIQUE(entity_id,metric_name,measured_at) 冲突:若冲突则 measured_at 递增重试
    int64_t attempt_ts = ts;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::string ins = "INSERT OR IGNORE INTO time_series(entity_id,metric_name,metric_value,"
                          "measured_at,source_type,delta_from_prev) VALUES(";
        ins += "'" + sql_escape(entity_id) + "',";
        ins += "'" + sql_escape(metric_name) + "',";
        ins += std::to_string(metric_value) + ",";
        ins += std::to_string(attempt_ts) + ",";
        ins += "'" + sql_escape(source_type) + "',";
        ins += has_prev ? std::to_string(delta) : "NULL";
        ins += ");";
        exec_sql(ins);
        int changes = sqlite3_changes(db_);
        if (changes > 0) break;
        attempt_ts += 1;
    }
}

std::vector<json> CacheManager::query_timeseries(const std::string& entity_id,
                                                   const std::string& metric_name,
                                                   int64_t start_time,
                                                   int64_t end_time) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || entity_id.empty() || metric_name.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);

    std::string sql = "SELECT measured_at,metric_value,source_type,delta_from_prev FROM time_series "
                      "WHERE entity_id=? AND metric_name=?";
    if (start_time > 0) sql += " AND measured_at>=" + std::to_string(start_time);
    if (end_time   > 0) sql += " AND measured_at<=" + std::to_string(end_time);
    sql += " ORDER BY measured_at ASC LIMIT 5000;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, metric_name.c_str(), (int)metric_name.size(), SQLITE_TRANSIENT);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["measured_at"]     = sqlite3_column_int64(s.p, 0);
        r["metric_value"]    = sqlite3_column_double(s.p, 1);
        r["source_type"]     = (const char*)sqlite3_column_text(s.p, 2);
        if (sqlite3_column_type(s.p, 3) != SQLITE_NULL)
            r["delta_from_prev"] = sqlite3_column_double(s.p, 3);
        out.push_back(r);
    }
    return out;
}

// =============================================================
// stats
// =============================================================
json CacheManager::stats() {
    if (!cfg_.enabled || !db_) {
        return {{"enabled", false}};
    }
    std::lock_guard<std::mutex> lk(mu_);
    auto cnt = [&](const std::string& table) -> int64_t {
        std::string sql = "SELECT COUNT(*) FROM " + table + ";";
        Stmt s;
        if (!prepare(db_, s, sql)) return 0;
        if (sqlite3_step(s.p) != SQLITE_ROW) return 0;
        return sqlite3_column_int64(s.p, 0);
    };
    auto one = [&](const std::string& sql) -> int64_t {
        Stmt s;
        if (!prepare(db_, s, sql)) return 0;
        if (sqlite3_step(s.p) != SQLITE_ROW) return 0;
        return sqlite3_column_int64(s.p, 0);
    };
    return {
        {"enabled", true},
        {"db_path", cfg_.db_path},
        {"cache_entries", cnt("cache_entries")},
        {"blobs",         cnt("cache_blobs")},
        {"hit_count_sum", one("SELECT COALESCE(SUM(hit_count),0) FROM cache_entries;")},
        {"entity_count",  cnt("entity_index")},
        {"relation_count",cnt("relation_links")},
        {"time_points",   cnt("time_series")}
    };
}

// ═════════════════════════════════════════════════════════════
//  多源融合 + 策略降级层 (next1.txt)
// ═════════════════════════════════════════════════════════════

// ── 数据源注册 ───────────────────────────────────────────────
void CacheManager::register_source(const std::string& source_id,
                                    const std::string& source_type,
                                    const std::string& base_url,
                                    double reliability,
                                    int avg_latency_ms,
                                    int rate_limit_per_hour,
                                    double priority_weight,
                                    const std::string& config_json) {
    if (!cfg_.enabled || !db_) return;
    std::lock_guard<std::mutex> lk(mu_);
    int64_t ts = now_sec();
    std::string sql = "INSERT OR REPLACE INTO data_sources("
        "source_id,source_type,base_url,reliability,avg_latency_ms,"
        "rate_limit_per_hour,priority_weight,enabled,config_json) VALUES(";
    sql += "'" + sql_escape(source_id) + "',";
    sql += "'" + sql_escape(source_type) + "',";
    sql += "'" + sql_escape(base_url) + "',";
    sql += std::to_string(reliability) + ",";
    sql += std::to_string(avg_latency_ms) + ",";
    sql += std::to_string(rate_limit_per_hour) + ",";
    sql += std::to_string(priority_weight) + ",";
    sql += "1,";
    sql += config_json.empty() ? "NULL" : ("'" + sql_escape(config_json) + "'");
    sql += ");";
    exec_sql(sql);
}

void CacheManager::register_source_fetch(const std::string& source_id,
                                          FetchEntityCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    fetch_callbacks_[source_id] = cb;
}

std::optional<DataSource> CacheManager::get_source(const std::string& source_id) {
    if (!cfg_.enabled || !db_) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT source_id,source_type,base_url,reliability,avg_latency_ms,"
                      "rate_limit_per_hour,priority_weight,enabled,last_failure_at,"
                      "consecutive_failures,config_json FROM data_sources WHERE source_id=? LIMIT 1;";
    Stmt s;
    if (!prepare(db_, s, sql)) return std::nullopt;
    sqlite3_bind_text(s.p, 1, source_id.c_str(), (int)source_id.size(), SQLITE_TRANSIENT);
    if (sqlite3_step(s.p) != SQLITE_ROW) return std::nullopt;
    DataSource ds;
    ds.source_id    = (const char*)sqlite3_column_text(s.p, 0);
    ds.source_type  = (const char*)sqlite3_column_text(s.p, 1);
    const char* bu  = (const char*)sqlite3_column_text(s.p, 2);
    ds.base_url     = bu ? bu : "";
    ds.reliability  = sqlite3_column_double(s.p, 3);
    ds.avg_latency_ms = sqlite3_column_int(s.p, 4);
    ds.rate_limit_per_hour = sqlite3_column_int(s.p, 5);
    ds.priority_weight = sqlite3_column_double(s.p, 6);
    ds.enabled      = sqlite3_column_int(s.p, 7) != 0;
    ds.last_failure_at = sqlite3_column_int64(s.p, 8);
    ds.consecutive_failures = sqlite3_column_int(s.p, 9);
    const char* cj  = (const char*)sqlite3_column_text(s.p, 10);
    ds.config_json  = cj ? cj : "";
    return ds;
}

void CacheManager::record_source_result(const std::string& source_id,
                                          bool success,
                                          int latency_ms) {
    if (!cfg_.enabled || !db_) return;
    int64_t ts = now_sec();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (success) {
            breaker_.record_success(source_id);
            // 更新 data_sources: 重置 consecutive_failures,更新 avg_latency_ms(滑动平均)
            std::string sql = "UPDATE data_sources SET consecutive_failures=0";
            if (latency_ms > 0) {
                sql += ",avg_latency_ms=" + std::to_string(latency_ms);
            }
            sql += " WHERE source_id='" + sql_escape(source_id) + "';";
            exec_sql(sql);
        } else {
            breaker_.record_failure(source_id, ts);
            std::string sql = "UPDATE data_sources SET consecutive_failures=consecutive_failures+1"
                            ",last_failure_at=" + std::to_string(ts) +
                            " WHERE source_id='" + sql_escape(source_id) + "';";
            exec_sql(sql);
        }
    }
}

// ── 实体-数据源映射 ─────────────────────────────────────────
void CacheManager::register_entity_source(const std::string& entity_id,
                                            const std::string& source_id,
                                            const std::string& source_entity_key,
                                            const std::vector<std::string>& fields_available,
                                            double quality_score) {
    if (!cfg_.enabled || !db_) return;
    std::lock_guard<std::mutex> lk(mu_);
    int64_t ts = now_sec();
    json fa = fields_available;
    std::string fa_txt = fields_available.empty() ? "" : fa.dump();
    std::string sql = "INSERT OR REPLACE INTO entity_sources("
        "entity_id,source_id,source_entity_key,fetch_status,last_fetched_at,"
        "quality_score,fields_available) VALUES(";
    sql += "'" + sql_escape(entity_id) + "',";
    sql += "'" + sql_escape(source_id) + "',";
    sql += "'" + sql_escape(source_entity_key) + "',";
    sql += "'pending',";
    sql += std::to_string(ts) + ",";
    sql += std::to_string(quality_score) + ",";
    sql += fa_txt.empty() ? "NULL" : ("'" + sql_escape(fa_txt) + "'");
    sql += ");";
    exec_sql(sql);
}

// ── 降级策略 ─────────────────────────────────────────────────
void CacheManager::set_fallback_policy(const std::string& entity_type,
                                         const std::vector<std::string>& priority_chain,
                                         const std::string& field_name,
                                         double min_quality,
                                         bool allow_stale,
                                         int max_stale_hours) {
    if (!cfg_.enabled || !db_) return;
    std::lock_guard<std::mutex> lk(mu_);
    // 先删除同 entity_type + field_name 的旧策略
    // (SQLite UNIQUE 中 NULL 视为不同,INSERT OR REPLACE 无法替换 NULL field_name 行)
    std::string del_sql = "DELETE FROM fallback_policies WHERE entity_type='"
                        + sql_escape(entity_type) + "'";
    if (field_name.empty()) {
        del_sql += " AND field_name IS NULL;";
    } else {
        del_sql += " AND field_name='" + sql_escape(field_name) + "';";
    }
    exec_sql(del_sql);

    json chain = priority_chain;
    std::string sql = "INSERT INTO fallback_policies("
        "entity_type,field_name,priority_chain,min_quality,allow_stale,max_stale_hours,"
        "retry_strategy) VALUES(";
    sql += "'" + sql_escape(entity_type) + "',";
    sql += field_name.empty() ? "NULL," : ("'" + sql_escape(field_name) + "',");
    sql += "'" + sql_escape(chain.dump()) + "',";
    sql += std::to_string(min_quality) + ",";
    sql += allow_stale ? "1," : "0,";
    sql += std::to_string(max_stale_hours) + ",";
    sql += "'exponential');";
    exec_sql(sql);
}

std::vector<std::string> CacheManager::get_fallback_chain(const std::string& entity_type) {
    std::vector<std::string> out;
    if (!cfg_.enabled || !db_) return out;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT priority_chain FROM fallback_policies "
                      "WHERE entity_type=? AND field_name IS NULL LIMIT 1;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, entity_type.c_str(), (int)entity_type.size(), SQLITE_TRANSIENT);
    if (sqlite3_step(s.p) != SQLITE_ROW) return out;
    const char* txt = (const char*)sqlite3_column_text(s.p, 0);
    if (!txt) return out;
    try {
        json chain = json::parse(txt);
        if (chain.is_array()) {
            for (auto& s2 : chain) out.push_back(s2.get<std::string>());
        }
    } catch (...) {}
    return out;
}

// ── 字段级融合存储 ───────────────────────────────────────────
void CacheManager::put_entity_fields(const std::string& entity_id,
                                       const std::string& source_id,
                                       const std::map<std::string, json>& fields,
                                       double quality_score) {
    if (!cfg_.enabled || !db_ || entity_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    int64_t ts = now_sec();

    for (auto& [name, val] : fields) {
        std::string val_str = val.dump();
        std::string ftype = val.is_string() ? "string"
                          : val.is_number() ? "number"
                          : val.is_boolean() ? "bool"
                          : val.is_array() ? "list" : "json";
        // 清除同实体同字段的 is_primary
        exec_sql("UPDATE entity_fields SET is_primary=0 WHERE entity_id='"
                 + sql_escape(entity_id) + "' AND field_name='" + sql_escape(name) + "';");
        // INSERT OR REPLACE
        std::string sql = "INSERT OR REPLACE INTO entity_fields("
            "entity_id,field_name,field_value,field_type,source_id,quality_score,"
            "last_updated,is_primary) VALUES(";
        sql += "'" + sql_escape(entity_id) + "',";
        sql += "'" + sql_escape(name) + "',";
        sql += "'" + sql_escape(val_str) + "',";
        sql += "'" + ftype + "',";
        sql += "'" + sql_escape(source_id) + "',";
        sql += std::to_string(quality_score) + ",";
        sql += std::to_string(ts) + ",";
        sql += "1);";
        exec_sql(sql);
    }

    // 更新 entity_sources 的 fetch_status
    exec_sql("UPDATE entity_sources SET fetch_status='fetched',last_fetched_at="
             + std::to_string(ts) + ",quality_score=" + std::to_string(quality_score)
             + " WHERE entity_id='" + sql_escape(entity_id)
             + "' AND source_id='" + sql_escape(source_id) + "';");
}

std::map<std::string, FieldValue> CacheManager::get_entity_fields(const std::string& entity_id) {
    std::map<std::string, FieldValue> out;
    if (!cfg_.enabled || !db_ || entity_id.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    // 优先取 is_primary=1 的,没有则取 quality_score 最高的
    std::string sql = "SELECT field_name,field_value,field_type,source_id,quality_score,"
                      "last_updated,is_primary FROM entity_fields WHERE entity_id=? "
                      "ORDER BY is_primary DESC,quality_score DESC,last_updated DESC;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        std::string name = (const char*)sqlite3_column_text(s.p, 0);
        if (out.count(name)) continue;  // 已有更高优先级的
        FieldValue fv;
        fv.field_name = name;
        const char* vtxt = (const char*)sqlite3_column_text(s.p, 1);
        if (vtxt) {
            try { fv.value = json::parse(vtxt); } catch(...) { fv.value = vtxt; }
        }
        const char* ft = (const char*)sqlite3_column_text(s.p, 2);
        fv.field_type = ft ? ft : "string";
        fv.source_id = (const char*)sqlite3_column_text(s.p, 3);
        fv.quality_score = sqlite3_column_double(s.p, 4);
        fv.last_updated = sqlite3_column_int64(s.p, 5);
        fv.is_primary = sqlite3_column_int(s.p, 6) != 0;
        out[name] = fv;
    }
    return out;
}

// ── 核心:多源融合查询 ───────────────────────────────────────
EntityData CacheManager::get_entity(const std::string& entity_id,
                                      const std::string& entity_type,
                                      const std::vector<std::string>& required_fields,
                                      bool force_refresh) {
    EntityData result;
    result.entity_id = entity_id;
    result.entity_type = entity_type;

    if (!cfg_.enabled || !db_ || entity_id.empty()) {
        result.errors.push_back("cache not ready or entity_id empty");
        return result;
    }

    int64_t ts = now_sec();

    // 1. 先查本地已融合字段(快速路径)
    if (!force_refresh) {
        result.fields = get_entity_fields(entity_id);
        if (!required_fields.empty()) {
            bool all_met = true;
            for (auto& rf : required_fields) {
                if (!result.fields.count(rf)) { all_met = false; break; }
            }
            if (all_met) {
                result.quality_score = SourceFusionEngine::calc_overall_quality(result.fields);
                result.coverage_score = SourceFusionEngine::calc_coverage(result.fields, entity_type);
                // 从 entity_index 取 canonical_name + entity_type
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    std::string sql = "SELECT canonical_name,entity_type FROM entity_index WHERE entity_id=? LIMIT 1;";
                    Stmt s;
                    if (prepare(db_, s, sql)) {
                        sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
                        if (sqlite3_step(s.p) == SQLITE_ROW) {
                            result.canonical_name = (const char*)sqlite3_column_text(s.p, 0);
                            if (result.entity_type.empty())
                                result.entity_type = (const char*)sqlite3_column_text(s.p, 1);
                        }
                    }
                }
                return result;
            }
        } else if (!result.fields.empty()) {
            result.quality_score = SourceFusionEngine::calc_overall_quality(result.fields);
            result.coverage_score = SourceFusionEngine::calc_coverage(result.fields, entity_type);
            // 从 entity_index 取 canonical_name + entity_type
            {
                std::lock_guard<std::mutex> lk(mu_);
                std::string sql = "SELECT canonical_name,entity_type FROM entity_index WHERE entity_id=? LIMIT 1;";
                Stmt s;
                if (prepare(db_, s, sql)) {
                    sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
                    if (sqlite3_step(s.p) == SQLITE_ROW) {
                        result.canonical_name = (const char*)sqlite3_column_text(s.p, 0);
                        if (result.entity_type.empty())
                            result.entity_type = (const char*)sqlite3_column_text(s.p, 1);
                    }
                }
            }
            return result;
        }
    }

    // 2. 获取降级链
    std::vector<std::string> chain = get_fallback_chain(entity_type);
    if (chain.empty()) {
        // 没有降级策略,尝试从 entity_sources 查已注册的源
        std::lock_guard<std::mutex> lk(mu_);
        std::string sql = "SELECT source_id,source_entity_key,quality_score FROM entity_sources "
                          "WHERE entity_id=? ORDER BY quality_score DESC;";
        Stmt s;
        if (prepare(db_, s, sql)) {
            sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
            while (sqlite3_step(s.p) == SQLITE_ROW) {
                chain.push_back((const char*)sqlite3_column_text(s.p, 0));
            }
        }
    }

    // 3. 按优先级依次尝试各数据源
    std::map<std::string, FieldValue> collected;
    if (!force_refresh) collected = result.fields;  // 保留已有字段

    for (auto& src_id : chain) {
        // 熔断器检查
        if (breaker_.is_open(src_id, ts)) {
            result.errors.push_back(src_id + ": circuit breaker open");
            continue;
        }

        // 查找该源的 entity_key
        std::string entity_key;
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::string sql = "SELECT source_entity_key FROM entity_sources "
                              "WHERE entity_id=? AND source_id=? LIMIT 1;";
            Stmt s;
            if (prepare(db_, s, sql)) {
                sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
                sqlite3_bind_text(s.p, 2, src_id.c_str(), (int)src_id.size(), SQLITE_TRANSIENT);
                if (sqlite3_step(s.p) == SQLITE_ROW) {
                    entity_key = (const char*)sqlite3_column_text(s.p, 0);
                }
            }
        }
        if (entity_key.empty()) continue;

        // 查找抓取回调
        FetchEntityCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = fetch_callbacks_.find(src_id);
            if (it == fetch_callbacks_.end()) continue;
            cb = it->second;
        }

        // 调用回调抓取
        double src_quality = 0.5;
        auto ds_opt = get_source(src_id);
        if (ds_opt) src_quality = ds_opt->reliability;

        try {
            auto incoming = cb(entity_key);
            if (incoming.empty()) {
                result.errors.push_back(src_id + ": empty result");
                record_source_result(src_id, false);
                continue;
            }

            // 清洗 + 融合
            SourceFusionEngine::merge_fields(collected, incoming, src_id, src_quality, ts);
            result.sources_used.push_back(src_id);
            record_source_result(src_id, true);

            // 持久化到 entity_fields
            put_entity_fields(entity_id, src_id, incoming, src_quality);

            // 检查是否已满足所需字段
            if (!required_fields.empty()) {
                bool all_met = true;
                for (auto& rf : required_fields) {
                    if (!collected.count(rf)) { all_met = false; break; }
                }
                if (all_met) break;
            }
        } catch (const std::exception& e) {
            result.errors.push_back(src_id + ": " + e.what());
            record_source_result(src_id, false);
            continue;
        }
    }

    // 4. 所有源都失败 / 字段不完整 → 尝试陈旧缓存降级
    if (collected.empty()) {
        // 尝试从 entity_fields 读旧数据
        auto stale = get_entity_fields(entity_id);
        if (!stale.empty()) {
            result.fields = stale;
            result.is_fallback_result = true;
            result.fallback_note = "所有源失败,返回陈旧缓存";
            result.quality_score = SourceFusionEngine::calc_overall_quality(stale);
            result.coverage_score = SourceFusionEngine::calc_coverage(stale, entity_type);
            return result;
        }
        result.errors.push_back("all sources failed, no stale cache available");
        return result;
    }

    // 5. 构建融合后的结果
    result.fields = collected;
    result.quality_score = SourceFusionEngine::calc_overall_quality(collected);
    result.coverage_score = SourceFusionEngine::calc_coverage(collected, entity_type);
    result.is_fallback_result = !result.errors.empty() || result.sources_used.size() > 1;

    // 从 entity_index 取 canonical_name
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string sql = "SELECT canonical_name FROM entity_index WHERE entity_id=? LIMIT 1;";
        Stmt s;
        if (prepare(db_, s, sql)) {
            sqlite3_bind_text(s.p, 1, entity_id.c_str(), (int)entity_id.size(), SQLITE_TRANSIENT);
            if (sqlite3_step(s.p) == SQLITE_ROW) {
                result.canonical_name = (const char*)sqlite3_column_text(s.p, 0);
            }
        }
    }

    return result;
}

// ── find_entity_by_name ─────────────────────────────────────
std::string CacheManager::find_entity_by_name(const std::string& name,
                                                const std::string& entity_type) {
    if (name.empty()) return "";
    auto ids = find_entity(name, entity_type);
    return ids.empty() ? "" : ids[0];
}

// ── search_entities ─────────────────────────────────────────
std::vector<EntityData> CacheManager::search_entities(const std::string& query,
                                                         const std::string& entity_type,
                                                         double min_quality,
                                                         int limit) {
    std::vector<EntityData> out;
    if (!cfg_.enabled || !db_ || query.empty()) return out;

    // 从 entity_index 模糊搜索
    auto ids = find_entity(query, entity_type);
    for (int i = 0; i < (int)ids.size() && (int)out.size() < limit; ++i) {
        EntityData ed;
        ed.entity_id = ids[i];
        ed.entity_type = entity_type;
        ed.fields = get_entity_fields(ids[i]);
        ed.quality_score = SourceFusionEngine::calc_overall_quality(ed.fields);
        if (ed.quality_score < min_quality) continue;
        ed.coverage_score = SourceFusionEngine::calc_coverage(ed.fields, entity_type);
        // 取 canonical_name
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::string sql = "SELECT canonical_name,entity_type FROM entity_index WHERE entity_id=? LIMIT 1;";
            Stmt s;
            if (prepare(db_, s, sql)) {
                sqlite3_bind_text(s.p, 1, ids[i].c_str(), (int)ids[i].size(), SQLITE_TRANSIENT);
                if (sqlite3_step(s.p) == SQLITE_ROW) {
                    ed.canonical_name = (const char*)sqlite3_column_text(s.p, 0);
                    if (ed.entity_type.empty())
                        ed.entity_type = (const char*)sqlite3_column_text(s.p, 1);
                }
            }
        }
        out.push_back(ed);
    }
    return out;
}

// ── get_source_health ───────────────────────────────────────
json CacheManager::get_source_health() {
    if (!cfg_.enabled || !db_) return {{"enabled", false}};
    std::lock_guard<std::mutex> lk(mu_);
    json arr = json::array();
    std::string sql = "SELECT source_id,source_type,reliability,avg_latency_ms,"
                      "enabled,consecutive_failures,last_failure_at FROM data_sources;";
    Stmt s;
    if (!prepare(db_, s, sql)) return arr;
    int64_t ts = now_sec();
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        std::string sid = (const char*)sqlite3_column_text(s.p, 0);
        json h = {
            {"source_id", sid},
            {"source_type", (const char*)sqlite3_column_text(s.p, 1)},
            {"reliability", sqlite3_column_double(s.p, 2)},
            {"avg_latency_ms", sqlite3_column_int(s.p, 3)},
            {"enabled", sqlite3_column_int(s.p, 4) != 0},
            {"consecutive_failures", sqlite3_column_int(s.p, 5)},
            {"last_failure_at", sqlite3_column_int64(s.p, 6)},
            {"breaker_state", breaker_.get_state(sid)}
        };
        arr.push_back(h);
    }
    return {{"sources", arr}, {"breakers", breaker_.dump_all()}};
}

json CacheManager::get_cache_summary(int key_sample_limit) {
    if (!cfg_.enabled || !db_) return {{"enabled", false}};
    std::lock_guard<std::mutex> lk(mu_);

    // 按 source_type 分组统计 total / ok / failed / stale
    std::string sql =
        "SELECT source_type, fetch_status, COUNT(*) as cnt "
        "FROM cache_entries "
        "GROUP BY source_type, fetch_status;";
    Stmt s;
    if (!prepare(db_, s, sql)) return json::object();

    json summary = json::object();

    while (sqlite3_step(s.p) == SQLITE_ROW) {
        std::string st = (const char*)sqlite3_column_text(s.p, 0);
        std::string fs = (const char*)sqlite3_column_text(s.p, 1);
        int64_t cnt = sqlite3_column_int64(s.p, 2);

        if (!summary.contains(st)) {
            summary[st] = {
                {"total", 0},
                {"ok", 0},
                {"failed", 0},
                {"partial", 0},
                {"rate_limited", 0},
                {"key_sample", json::array()}
            };
        }
        summary[st]["total"] = (int64_t)summary[st]["total"] + cnt;
        if (fs == "ok")       summary[st]["ok"]       = (int64_t)summary[st]["ok"] + cnt;
        else if (fs == "failed")    summary[st]["failed"]    = (int64_t)summary[st]["failed"] + cnt;
        else if (fs == "partial")   summary[st]["partial"]   = (int64_t)summary[st]["partial"] + cnt;
        else if (fs == "rate_limited") summary[st]["rate_limited"] = (int64_t)summary[st]["rate_limited"] + cnt;
    }

    // 取每个 source_type 的最近几个 key
    for (auto it = summary.begin(); it != summary.end(); ++it) {
        std::string st = it.key();
        std::string key_sql =
            "SELECT cache_key FROM cache_entries "
            "WHERE source_type='" + sql_escape(st) + "' "
            "ORDER BY updated_at DESC LIMIT " + std::to_string(key_sample_limit) + ";";
        Stmt ks;
        if (prepare(db_, ks, key_sql)) {
            while (sqlite3_step(ks.p) == SQLITE_ROW) {
                it.value()["key_sample"].push_back((const char*)sqlite3_column_text(ks.p, 0));
            }
        }
    }

    // 实体统计
    json entities_by_type = json::object();
    {
        std::string et_sql =
            "SELECT entity_type, COUNT(*) FROM entity_index GROUP BY entity_type;";
        Stmt es;
        if (prepare(db_, es, et_sql)) {
            while (sqlite3_step(es.p) == SQLITE_ROW) {
                entities_by_type[(const char*)sqlite3_column_text(es.p, 0)] =
                    sqlite3_column_int64(es.p, 1);
            }
        }
    }
    summary["_entities_by_type"] = entities_by_type;

    return summary;
}

json CacheManager::list_cache(const std::string& source_type, int limit) {
    if (!cfg_.enabled || !db_) return json::array();
    std::lock_guard<std::mutex> lk(mu_);
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;

    std::string sql =
        "SELECT source_type, cache_key, fetch_status, hit_count, "
        "payload_size, expires_at, updated_at "
        "FROM cache_entries ";
    if (!source_type.empty()) {
        sql += "WHERE source_type='" + sql_escape(source_type) + "' ";
    }
    sql += "ORDER BY updated_at DESC LIMIT " + std::to_string(limit) + ";";

    Stmt s;
    if (!prepare(db_, s, sql)) return json::array();
    json arr = json::array();
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        arr.push_back({
            {"source_type", (const char*)sqlite3_column_text(s.p, 0)},
            {"cache_key",   (const char*)sqlite3_column_text(s.p, 1)},
            {"fetch_status",(const char*)sqlite3_column_text(s.p, 2)},
            {"hit_count",   sqlite3_column_int(s.p, 3)},
            {"payload_size",sqlite3_column_int64(s.p, 4)},
            {"expires_at",  sqlite3_column_int64(s.p, 5)},
            {"updated_at",  sqlite3_column_int64(s.p, 6)}
        });
    }
    return arr;
}

// ── force_refresh_entity ────────────────────────────────────
void CacheManager::force_refresh_entity(const std::string& entity_id,
                                          const std::string& source_id) {
    if (!cfg_.enabled || !db_ || entity_id.empty()) return;

    // 清除该实体的 entity_fields(指定源则只清该源的)
    std::lock_guard<std::mutex> lk(mu_);
    if (source_id.empty()) {
        exec_sql("DELETE FROM entity_fields WHERE entity_id='" + sql_escape(entity_id) + "';");
    } else {
        exec_sql("DELETE FROM entity_fields WHERE entity_id='" + sql_escape(entity_id)
                 + "' AND source_id='" + sql_escape(source_id) + "';");
    }
}

// ═════════════════════════════════════════════════════════════
//  GitHub 项目局部对象连续动态分析索引 (next2)
// ═════════════════════════════════════════════════════════════

// ── 文件时序变更记录 ───────────────────────────────────────
void CacheManager::record_file_timeline(const std::string& repo_full_name,
                                          const std::string& file_path,
                                          const std::string& commit_hash,
                                          const std::string& author_login,
                                          int64_t commit_time,
                                          int lines_add,
                                          int lines_del,
                                          int pr_number,
                                          const std::string& commit_message) {
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || file_path.empty()
        || commit_hash.empty() || author_login.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "INSERT OR IGNORE INTO file_timeline("
        "repo_full_name,file_path,commit_hash,author_login,commit_time,"
        "lines_add,lines_del,pr_number,commit_message) VALUES(";
    sql += "'" + sql_escape(repo_full_name) + "',";
    sql += "'" + sql_escape(file_path) + "',";
    sql += "'" + sql_escape(commit_hash) + "',";
    sql += "'" + sql_escape(author_login) + "',";
    sql += std::to_string(commit_time) + ",";
    sql += std::to_string(lines_add) + ",";
    sql += std::to_string(lines_del) + ",";
    sql += pr_number > 0 ? std::to_string(pr_number) : "NULL";
    sql += ",";
    sql += commit_message.empty() ? "NULL" : ("'" + sql_escape(commit_message) + "'");
    sql += ");";
    exec_sql(sql);
}

int CacheManager::record_commit_files(const std::string& repo_full_name,
                                        const std::string& commit_hash,
                                        const std::string& author_login,
                                        int64_t commit_time,
                                        const std::vector<std::tuple<std::string, int, int>>& files,
                                        int pr_number,
                                        const std::string& commit_message) {
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || commit_hash.empty()
        || author_login.empty()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    int inserted = 0;
    for (auto& [path, add, del] : files) {
        if (path.empty()) continue;
        std::string sql = "INSERT OR IGNORE INTO file_timeline("
            "repo_full_name,file_path,commit_hash,author_login,commit_time,"
            "lines_add,lines_del,pr_number,commit_message) VALUES(";
        sql += "'" + sql_escape(repo_full_name) + "',";
        sql += "'" + sql_escape(path) + "',";
        sql += "'" + sql_escape(commit_hash) + "',";
        sql += "'" + sql_escape(author_login) + "',";
        sql += std::to_string(commit_time) + ",";
        sql += std::to_string(add) + ",";
        sql += std::to_string(del) + ",";
        sql += pr_number > 0 ? std::to_string(pr_number) : "NULL";
        sql += ",";
        sql += commit_message.empty() ? "NULL" : ("'" + sql_escape(commit_message) + "'");
        sql += ");";
        if (exec_sql(sql)) {
            int changes = sqlite3_changes(db_);
            inserted += changes;
        }
    }

    // 更新 file_cooccurrence: 同一 commit 内的文件两两配对
    int n = (int)files.size();
    if (n >= 2) {
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                std::string a = std::get<0>(files[i]);
                std::string b = std::get<0>(files[j]);
                if (a.empty() || b.empty() || a == b) continue;
                // 规范化:确保 file_a < file_b(避免重复存储无向边)
                if (a > b) std::swap(a, b);
                std::string sql = "INSERT INTO file_cooccurrence("
                    "repo_full_name,file_a,file_b,co_commit_count,last_co_commit_time) VALUES(";
                sql += "'" + sql_escape(repo_full_name) + "',";
                sql += "'" + sql_escape(a) + "',";
                sql += "'" + sql_escape(b) + "',";
                sql += "1," + std::to_string(commit_time) + ") ";
                sql += "ON CONFLICT(repo_full_name,file_a,file_b) DO UPDATE SET "
                       "co_commit_count=co_commit_count+1, "
                       "last_co_commit_time=" + std::to_string(commit_time) + ";";
                exec_sql(sql);
            }
        }
    }
    return inserted;
}

std::vector<json> CacheManager::query_file_timeline(const std::string& repo_full_name,
                                                      const std::string& file_path,
                                                      int64_t since,
                                                      int64_t until,
                                                      int limit,
                                                      bool prefix_match) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || file_path.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    // prefix_match=true:用 LIKE 前缀匹配目录下所有子文件
    // SQL: WHERE file_path LIKE 'drivers/net/%' (注意尾部 / 避免匹配 drivers/network)
    // 注意:SELECT 包含 file_path 字段(前缀匹配时每条记录可能属于不同文件,需要知道具体文件名)
    std::string sql = "SELECT file_path,commit_hash,author_login,commit_time,lines_add,lines_del,"
                      "pr_number,commit_message FROM file_timeline "
                      "WHERE repo_full_name=? AND ";
    if (prefix_match) {
        sql += "file_path LIKE ?";
    } else {
        sql += "file_path=?";
    }
    if (since > 0)  sql += " AND commit_time>=" + std::to_string(since);
    if (until > 0)  sql += " AND commit_time<=" + std::to_string(until);
    sql += " ORDER BY commit_time ASC LIMIT " + std::to_string(limit) + ";";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    if (prefix_match) {
        // 前缀匹配:drivers/net -> drivers/net/%(加 / 避免误匹配 drivers/network)
        std::string pattern = file_path;
        if (pattern.back() != '/') pattern += "/";
        pattern += "%";
        sqlite3_bind_text(s.p, 2, pattern.c_str(), (int)pattern.size(), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_text(s.p, 2, file_path.c_str(), (int)file_path.size(), SQLITE_TRANSIENT);
    }
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["file_path"]      = (const char*)sqlite3_column_text(s.p, 0);
        r["commit_hash"]    = (const char*)sqlite3_column_text(s.p, 1);
        r["author_login"]   = (const char*)sqlite3_column_text(s.p, 2);
        r["commit_time"]    = sqlite3_column_int64(s.p, 3);
        r["lines_add"]      = sqlite3_column_int(s.p, 4);
        r["lines_del"]      = sqlite3_column_int(s.p, 5);
        if (sqlite3_column_type(s.p, 6) != SQLITE_NULL)
            r["pr_number"]  = sqlite3_column_int(s.p, 6);
        if (sqlite3_column_type(s.p, 7) != SQLITE_NULL)
            r["commit_message"] = (const char*)sqlite3_column_text(s.p, 7);
        out.push_back(r);
    }
    return out;
}

// ── 模块定义 ───────────────────────────────────────────────
int64_t CacheManager::register_module_def(const std::string& repo_full_name,
                                            const std::string& module_name,
                                            const std::string& path_pattern,
                                            const std::string& tag,
                                            bool auto_generated) {
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || module_name.empty()
        || path_pattern.empty()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    // 先 DELETE 同名模块,再 INSERT(简化 upsert)
    std::string del = "DELETE FROM module_def WHERE repo_full_name='"
                    + sql_escape(repo_full_name) + "' AND module_name='"
                    + sql_escape(module_name) + "';";
    exec_sql(del);
    std::string sql = "INSERT INTO module_def("
        "repo_full_name,module_name,path_pattern,tag,auto_generated) VALUES(";
    sql += "'" + sql_escape(repo_full_name) + "',";
    sql += "'" + sql_escape(module_name) + "',";
    sql += "'" + sql_escape(path_pattern) + "',";
    sql += tag.empty() ? "NULL," : ("'" + sql_escape(tag) + "',");
    sql += auto_generated ? "1" : "0";
    sql += ");";
    if (!exec_sql(sql)) return 0;
    return sqlite3_last_insert_rowid(db_);
}

// 简易 glob 匹配: 支持 * 通配(如 "src/*" "src/net/*")
// 不引入外部 regex 库,用 std::string 实现
static bool glob_match(const std::string& pattern, const std::string& path) {
    if (pattern.empty()) return false;
    // "*": 匹配任意字符(含 /)
    if (pattern == "*") return true;
    // 无通配符: 前缀匹配(如 "src" 匹配 "src/foo.cpp" 和 "src/net/bar.cpp")
    if (pattern.find('*') == std::string::npos) {
        if (path == pattern) return true;
        return path.rfind(pattern + "/", 0) == 0;
    }
    // 拆分 pattern 为 prefix + "*"
    size_t star = pattern.find('*');
    std::string prefix = pattern.substr(0, star);
    std::string suffix = pattern.substr(star + 1);
    if (!prefix.empty() && path.rfind(prefix, 0) != 0) return false;
    if (!suffix.empty() && path.size() >= suffix.size()
        && path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    return true;
}

int CacheManager::auto_cluster_modules(const std::string& repo_full_name,
                                         const std::vector<std::string>& all_files) {
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || all_files.empty()) return 0;
    // 提取顶层目录作为模块
    std::set<std::string> top_dirs;
    for (auto& f : all_files) {
        size_t pos = f.find('/');
        if (pos == std::string::npos) {
            top_dirs.insert("(root)");  // 根目录下的散落文件
        } else {
            top_dirs.insert(f.substr(0, pos));
        }
    }
    int count = 0;
    for (auto& dir : top_dirs) {
        std::string pattern = (dir == "(root)") ? "*" : (dir + "/*");
        if (register_module_def(repo_full_name, dir, pattern, "", true) > 0) ++count;
    }
    return count;
}

std::vector<std::string> CacheManager::query_module_files(const std::string& repo_full_name,
                                                            const std::string& module_name) {
    std::vector<std::string> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || module_name.empty()) return out;
    // 1. 取模块的 path_pattern
    std::string pattern;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string sql = "SELECT path_pattern FROM module_def "
                          "WHERE repo_full_name=? AND module_name=? LIMIT 1;";
        Stmt s;
        if (!prepare(db_, s, sql)) return out;
        sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(s.p, 2, module_name.c_str(), (int)module_name.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(s.p) != SQLITE_ROW) return out;
        const char* txt = (const char*)sqlite3_column_text(s.p, 0);
        if (!txt) return out;
        pattern = txt;
    }
    // 2. 从 file_timeline 中取该仓库所有文件,用 glob 匹配
    std::set<std::string> matched;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string sql = "SELECT DISTINCT file_path FROM file_timeline WHERE repo_full_name=?;";
        Stmt s;
        if (!prepare(db_, s, sql)) return out;
        sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
        while (sqlite3_step(s.p) == SQLITE_ROW) {
            const char* path = (const char*)sqlite3_column_text(s.p, 0);
            if (path && glob_match(pattern, path)) matched.insert(path);
        }
    }
    for (auto& p : matched) out.push_back(p);
    return out;
}

std::vector<json> CacheManager::list_modules(const std::string& repo_full_name) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT module_name,path_pattern,tag,auto_generated FROM module_def "
                      "WHERE repo_full_name=? ORDER BY module_name;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json m;
        m["module_name"]    = (const char*)sqlite3_column_text(s.p, 0);
        m["path_pattern"]   = (const char*)sqlite3_column_text(s.p, 1);
        if (sqlite3_column_type(s.p, 2) != SQLITE_NULL)
            m["tag"]        = (const char*)sqlite3_column_text(s.p, 2);
        m["auto_generated"] = sqlite3_column_int(s.p, 3) != 0;
        out.push_back(m);
    }
    return out;
}

// ── 模块-贡献者预聚合 ─────────────────────────────────────
int CacheManager::aggregate_module_contributors(const std::string& repo_full_name,
                                                  int window_days) {
    if (!cfg_.enabled || !db_ || repo_full_name.empty()) return 0;
    int64_t now = now_sec();
    int64_t window_start = now - (int64_t)window_days * 86400;
    int64_t window_end = now;

    // 取该仓库所有模块定义
    struct ModDef { std::string name; std::string pattern; };
    std::vector<ModDef> modules;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string sql = "SELECT module_name,path_pattern FROM module_def WHERE repo_full_name=?;";
        Stmt s;
        if (!prepare(db_, s, sql)) return 0;
        sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
        while (sqlite3_step(s.p) == SQLITE_ROW) {
            ModDef m;
            m.name = (const char*)sqlite3_column_text(s.p, 0);
            m.pattern = (const char*)sqlite3_column_text(s.p, 1);
            modules.push_back(m);
        }
    }
    if (modules.empty()) return 0;

    // 先清除该仓库该窗口的旧聚合数据
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string del = "DELETE FROM module_contributor_agg WHERE repo_full_name='"
                        + sql_escape(repo_full_name) + "' AND window_days="
                        + std::to_string(window_days) + ";";
        exec_sql(del);
    }

    int total = 0;
    // 对每个模块,扫描 file_timeline 中匹配的文件,按 user 聚合
    for (auto& mod : modules) {
        // 取该模块所有文件
        std::set<std::string> mod_files;
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::string sql = "SELECT DISTINCT file_path FROM file_timeline WHERE repo_full_name=?;";
            Stmt s;
            if (!prepare(db_, s, sql)) continue;
            sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
            while (sqlite3_step(s.p) == SQLITE_ROW) {
                const char* path = (const char*)sqlite3_column_text(s.p, 0);
                if (path && glob_match(mod.pattern, path)) mod_files.insert(path);
            }
        }
        if (mod_files.empty()) continue;

        // 对每个文件,扫描时间窗口内的提交,按 user 累加
        std::map<std::string, std::tuple<int, int, int, int64_t, int64_t>> user_stats;
        // user -> (changes, lines_add, lines_del, first_ts, last_ts)
        for (auto& path : mod_files) {
            std::lock_guard<std::mutex> lk(mu_);
            std::string sql = "SELECT author_login,lines_add,lines_del,commit_time FROM file_timeline "
                              "WHERE repo_full_name=? AND file_path=? AND commit_time>=?;";
            Stmt s;
            if (!prepare(db_, s, sql)) continue;
            sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
            sqlite3_bind_text(s.p, 2, path.c_str(), (int)path.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int64(s.p, 3, window_start);
            while (sqlite3_step(s.p) == SQLITE_ROW) {
                std::string user = (const char*)sqlite3_column_text(s.p, 0);
                int add = sqlite3_column_int(s.p, 1);
                int del = sqlite3_column_int(s.p, 2);
                int64_t ts = sqlite3_column_int64(s.p, 3);
                auto& st = user_stats[user];
                std::get<0>(st) += 1;
                std::get<1>(st) += add;
                std::get<2>(st) += del;
                if (std::get<3>(st) == 0 || ts < std::get<3>(st)) std::get<3>(st) = ts;
                if (ts > std::get<4>(st)) std::get<4>(st) = ts;
            }
        }

        // 写入 module_contributor_agg
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [user, st] : user_stats) {
            std::string sql = "INSERT OR REPLACE INTO module_contributor_agg("
                "repo_full_name,module_name,user_login,time_window_start,time_window_end,"
                "window_days,total_changes,total_lines_add,total_lines_del,"
                "first_commit_time,last_commit_time) VALUES(";
            sql += "'" + sql_escape(repo_full_name) + "',";
            sql += "'" + sql_escape(mod.name) + "',";
            sql += "'" + sql_escape(user) + "',";
            sql += std::to_string(window_start) + ",";
            sql += std::to_string(window_end) + ",";
            sql += std::to_string(window_days) + ",";
            sql += std::to_string(std::get<0>(st)) + ",";
            sql += std::to_string(std::get<1>(st)) + ",";
            sql += std::to_string(std::get<2>(st)) + ",";
            sql += std::to_string(std::get<3>(st)) + ",";
            sql += std::to_string(std::get<4>(st));
            sql += ");";
            if (exec_sql(sql)) ++total;
        }
    }
    return total;
}

std::vector<json> CacheManager::query_module_contributors(const std::string& repo_full_name,
                                                            const std::string& module_name,
                                                            int window_days,
                                                            int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || module_name.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT user_login,total_changes,total_lines_add,total_lines_del,"
                      "first_commit_time,last_commit_time FROM module_contributor_agg "
                      "WHERE repo_full_name=? AND module_name=? AND window_days=? "
                      "ORDER BY total_changes DESC LIMIT ?;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, module_name.c_str(), (int)module_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(s.p, 3, window_days);
    sqlite3_bind_int(s.p, 4, limit);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["user_login"]        = (const char*)sqlite3_column_text(s.p, 0);
        r["total_changes"]     = sqlite3_column_int(s.p, 1);
        r["total_lines_add"]   = sqlite3_column_int(s.p, 2);
        r["total_lines_del"]   = sqlite3_column_int(s.p, 3);
        r["first_commit_time"] = sqlite3_column_int64(s.p, 4);
        r["last_commit_time"]  = sqlite3_column_int64(s.p, 5);
        out.push_back(r);
    }
    return out;
}

// ── 文件协同变更 ───────────────────────────────────────────
std::vector<json> CacheManager::query_related_files(const std::string& repo_full_name,
                                                      const std::string& file_path,
                                                      int min_co_count,
                                                      int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || file_path.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    // file_cooccurrence 中 file_a < file_b,需要双向查
    std::string sql = "SELECT CASE WHEN file_a=? THEN file_b ELSE file_a END AS related_file, "
                      "co_commit_count, last_co_commit_time FROM file_cooccurrence "
                      "WHERE repo_full_name=? AND (file_a=? OR file_b=?) "
                      "AND co_commit_count>=? ORDER BY co_commit_count DESC LIMIT ?;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, file_path.c_str(), (int)file_path.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 3, file_path.c_str(), (int)file_path.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 4, file_path.c_str(), (int)file_path.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(s.p, 5, min_co_count);
    sqlite3_bind_int(s.p, 6, limit);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["related_file"]         = (const char*)sqlite3_column_text(s.p, 0);
        r["co_commit_count"]      = sqlite3_column_int(s.p, 1);
        r["last_co_commit_time"]  = sqlite3_column_int64(s.p, 2);
        out.push_back(r);
    }
    return out;
}

// ── 代码特征检索 ───────────────────────────────────────────
std::vector<json> CacheManager::search_by_signature(const std::string& repo_full_name,
                                                      const std::string& signature_pattern,
                                                      int64_t since,
                                                      int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || signature_pattern.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    // 简单 LIKE 匹配: signature_pattern 作为子串匹配 file_path 或 commit_message
    std::string sql = "SELECT DISTINCT file_path, author_login, commit_time, commit_message "
                      "FROM file_timeline WHERE repo_full_name=? "
                      "AND (file_path LIKE ? OR commit_message LIKE ?)";
    if (since > 0) sql += " AND commit_time>=" + std::to_string(since);
    sql += " ORDER BY commit_time DESC LIMIT ?;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    std::string like_pat = "%" + signature_pattern + "%";
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, like_pat.c_str(), (int)like_pat.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 3, like_pat.c_str(), (int)like_pat.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(s.p, 4, limit);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["file_path"]     = (const char*)sqlite3_column_text(s.p, 0);
        r["author_login"]  = (const char*)sqlite3_column_text(s.p, 1);
        r["commit_time"]   = sqlite3_column_int64(s.p, 2);
        if (sqlite3_column_type(s.p, 3) != SQLITE_NULL)
            r["commit_message"] = (const char*)sqlite3_column_text(s.p, 3);
        out.push_back(r);
    }
    return out;
}

// ── 模块时序统计 ───────────────────────────────────────────
std::vector<json> CacheManager::query_change_density(const std::string& repo_full_name,
                                                       const std::string& file_path,
                                                       int64_t since,
                                                       int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || file_path.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    // 按月聚合: strftime('%Y-%m', commit_time, 'unixepoch')
    std::string sql = "SELECT strftime('%Y-%m', commit_time, 'unixepoch') AS month, "
                      "COUNT(*) AS changes, SUM(lines_add) AS lines_added, SUM(lines_del) AS lines_deleted, "
                      "COUNT(DISTINCT author_login) AS authors "
                      "FROM file_timeline WHERE repo_full_name=? AND file_path=?";
    if (since > 0) sql += " AND commit_time>=" + std::to_string(since);
    sql += " GROUP BY month ORDER BY month DESC LIMIT ?;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, file_path.c_str(), (int)file_path.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(s.p, 3, limit);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["month"]    = (const char*)sqlite3_column_text(s.p, 0);
        r["changes"]  = sqlite3_column_int(s.p, 1);
        r["lines_add"] = sqlite3_column_int(s.p, 2);
        r["lines_del"] = sqlite3_column_int(s.p, 3);
        r["authors"]  = sqlite3_column_int(s.p, 4);
        out.push_back(r);
    }
    return out;
}

std::vector<json> CacheManager::query_developer_modules(const std::string& repo_full_name,
                                                          const std::string& user_login,
                                                          int window_days,
                                                          int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || user_login.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT module_name,total_changes,total_lines_add,total_lines_del,"
                      "first_commit_time,last_commit_time FROM module_contributor_agg "
                      "WHERE repo_full_name=? AND user_login=? AND window_days=? "
                      "ORDER BY total_changes DESC LIMIT ?;";
    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, user_login.c_str(), (int)user_login.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(s.p, 3, window_days);
    sqlite3_bind_int(s.p, 4, limit);
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        json r;
        r["module_name"]        = (const char*)sqlite3_column_text(s.p, 0);
        r["total_changes"]      = sqlite3_column_int(s.p, 1);
        r["total_lines_add"]    = sqlite3_column_int(s.p, 2);
        r["total_lines_del"]    = sqlite3_column_int(s.p, 3);
        r["first_commit_time"]  = sqlite3_column_int64(s.p, 4);
        r["last_commit_time"]   = sqlite3_column_int64(s.p, 5);
        out.push_back(r);
    }
    return out;
}

// ── 原语A:子模块拆分时序切片 ────────────────────────────────
// 按一级子目录聚合变更密度曲线
// SQL:从 file_timeline 中提取 root_path 下的一级子目录,按 subdir + month 分组统计
std::vector<json> CacheManager::query_subdir_change_density(const std::string& repo_full_name,
                                                             const std::string& root_path,
                                                             int64_t since,
                                                             int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || root_path.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);

    // 构建 LIKE 前缀:root_path + "/" + "%"
    std::string prefix = root_path;
    if (prefix.back() != '/') prefix += "/";
    prefix += "%";
    // root_path 的长度(含尾部 /),用于 substr 提取一级子目录名
    int root_len = (int)(root_path.back() == '/' ? root_path.length() : root_path.length() + 1);

    // 提取一级子目录名:
    //   file_path = "drivers/usb/typec/ucsi/ucsi.c"
    //   root_path = "drivers/usb"
    //   substr(file_path, root_len+1) = "typec/ucsi/ucsi.c"
    //   一级子目录 = substr 到第一个 / 或结尾 = "typec"
    // SQLite instr() 返回第一个匹配位置(1-based),找不到返回 0
    std::string sql =
        "SELECT "
        "  CASE "
        "    WHEN instr(substr(file_path, ?2), '/') > 0 "
        "    THEN substr(file_path, ?2, instr(substr(file_path, ?2), '/') - 1) "
        "    ELSE substr(file_path, ?2) "
        "  END AS subdir, "
        "  strftime('%Y-%m', commit_time, 'unixepoch') AS month, "
        "  COUNT(*) AS changes, "
        "  COUNT(DISTINCT author_login) AS authors, "
        "  SUM(lines_add) AS lines_add, "
        "  SUM(lines_del) AS lines_del "
        "FROM file_timeline "
        "WHERE repo_full_name=?1 AND file_path LIKE ?3 ";
    if (since > 0) sql += " AND commit_time>=" + std::to_string(since);
    sql += " GROUP BY subdir, month ORDER BY subdir ASC, month ASC LIMIT ?4;";

    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int (s.p, 2, root_len + 1);  // substr 起始位置(1-based)
    sqlite3_bind_text(s.p, 3, prefix.c_str(), (int)prefix.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int (s.p, 4, limit * 50);    // 每个 subdir 多个月份,放宽限制

    // 按 subdir 分组组装
    std::map<std::string, json> subdir_map;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        std::string subdir = (const char*)sqlite3_column_text(s.p, 0);
        if (subdir.empty()) continue;
        json month_entry;
        month_entry["month"]      = (const char*)sqlite3_column_text(s.p, 1);
        month_entry["changes"]    = sqlite3_column_int(s.p, 2);
        month_entry["authors"]    = sqlite3_column_int(s.p, 3);
        month_entry["lines_add"]  = sqlite3_column_int(s.p, 4);
        month_entry["lines_del"]  = sqlite3_column_int(s.p, 5);

        auto it = subdir_map.find(subdir);
        if (it == subdir_map.end()) {
            json sd;
            sd["subdir"]         = subdir;
            sd["full_path"]      = root_path + "/" + subdir;
            sd["monthly_density"] = json::array({month_entry});
            sd["total_changes"]  = month_entry["changes"].get<int>();
            subdir_map[subdir]   = sd;
        } else {
            it->second["monthly_density"].push_back(month_entry);
            it->second["total_changes"] = it->second["total_changes"].get<int>() +
                                          month_entry["changes"].get<int>();
        }
    }

    // 按总变更数降序排列
    for (auto& kv : subdir_map) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a.value("total_changes", 0) > b.value("total_changes", 0);
    });

    return out;
}

// ── 原语B:维护链路归因 ─────────────────────────────────────
// 区分开发提交 vs 合并提交,按作者聚合统计
std::vector<json> CacheManager::query_maintenance_attribution(const std::string& repo_full_name,
                                                               const std::string& path_prefix,
                                                               int64_t since,
                                                               int limit) {
    std::vector<json> out;
    if (!cfg_.enabled || !db_ || repo_full_name.empty() || path_prefix.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);

    // 构建 LIKE 前缀
    std::string prefix = path_prefix;
    if (prefix.back() != '/') prefix += "/";
    prefix += "%";

    // 按作者 + commit_type(merge/dev) 聚合
    // merge commit 识别:commit_message 以 "Merge" 开头
    std::string sql =
        "SELECT author_login, "
        "  CASE WHEN commit_message LIKE 'Merge%' THEN 'merge' ELSE 'dev' END AS commit_type, "
        "  COUNT(*) AS commit_count, "
        "  MIN(commit_time) AS first_time, "
        "  MAX(commit_time) AS last_time, "
        "  SUM(lines_add) AS total_lines_add, "
        "  SUM(lines_del) AS total_lines_del "
        "FROM file_timeline "
        "WHERE repo_full_name=?1 AND file_path LIKE ?2 ";
    if (since > 0) sql += " AND commit_time>=" + std::to_string(since);
    sql += " GROUP BY author_login, commit_type "
           "ORDER BY commit_count DESC LIMIT ?3;";

    Stmt s;
    if (!prepare(db_, s, sql)) return out;
    sqlite3_bind_text(s.p, 1, repo_full_name.c_str(), (int)repo_full_name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(s.p, 2, prefix.c_str(), (int)prefix.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int (s.p, 3, limit);

    // 按作者分组,每个作者下有 merge/dev 两种类型
    std::map<std::string, json> author_map;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        std::string author = (const char*)sqlite3_column_text(s.p, 0);
        if (author.empty()) continue;
        std::string ctype  = (const char*)sqlite3_column_text(s.p, 1);
        int cnt            = sqlite3_column_int(s.p, 2);
        int64_t first_t    = sqlite3_column_int64(s.p, 3);
        int64_t last_t     = sqlite3_column_int64(s.p, 4);
        int lines_add      = sqlite3_column_int(s.p, 5);
        int lines_del      = sqlite3_column_int(s.p, 6);

        auto it = author_map.find(author);
        if (it == author_map.end()) {
            json a;
            a["author_login"]  = author;
            a["dev_commits"]   = 0;
            a["merge_commits"] = 0;
            a["total_commits"] = 0;
            a["total_lines_add"] = 0;
            a["total_lines_del"] = 0;
            a["first_commit_time"] = first_t;
            a["last_commit_time"]  = last_t;
            author_map[author] = a;
            it = author_map.find(author);
        }
        if (ctype == "merge") {
            it->second["merge_commits"] = it->second["merge_commits"].get<int>() + cnt;
        } else {
            it->second["dev_commits"] = it->second["dev_commits"].get<int>() + cnt;
        }
        it->second["total_commits"] = it->second["total_commits"].get<int>() + cnt;
        it->second["total_lines_add"] = it->second["total_lines_add"].get<int>() + lines_add;
        it->second["total_lines_del"] = it->second["total_lines_del"].get<int>() + lines_del;
        if (first_t < it->second["first_commit_time"].get<int64_t>())
            it->second["first_commit_time"] = first_t;
        if (last_t > it->second["last_commit_time"].get<int64_t>())
            it->second["last_commit_time"] = last_t;
    }

    // 计算每个作者的 merge 占比并输出
    for (auto& kv : author_map) {
        json& a = kv.second;
        int total = a["total_commits"].get<int>();
        int merge = a["merge_commits"].get<int>();
        a["merge_ratio"] = total > 0 ? (double)merge / total : 0.0;
        // 维护者角色判定:merge_commits > 0 标记为 maintainer
        a["role"] = merge > 0 ? "maintainer" : "developer";
        out.push_back(a);
    }
    // 按总提交数降序
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a.value("total_commits", 0) > b.value("total_commits", 0);
    });

    return out;
}

// ═══════════════════════════════════════════════════════════════════════
//  定向知识雷达 — Focus 管理层实现
// ═══════════════════════════════════════════════════════════════════════

static std::string now_iso() {
    auto t = static_cast<int64_t>(std::time(nullptr));
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm_utc{};
    gmtime_s(&tm_utc, &tt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

static int64_t now_sec() {
    return static_cast<int64_t>(std::time(nullptr));
}

static std::string join_strings(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += sep;
        out += parts[i];
    }
    return out;
}

static std::string gen_focus_id() {
    // 类似 "f_3fa1b2"
    static const char hex[] = "0123456789abcdef";
    std::string id = "f_";
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t h = static_cast<uint64_t>(now);
    for (int i = 0; i < 6; ++i) {
        id.push_back(hex[h & 0xF]);
        h >>= 4;
    }
    return id;
}

std::string CacheManager::create_focus(
    const std::string& name,
    const std::string& description,
    const std::vector<std::string>& seed_entity_ids,
    const std::vector<std::string>& keywords,
    const std::vector<std::string>& exclude_words,
    int max_depth,
    double relevance_threshold,
    int max_nodes) {

    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return "";

    std::string id = gen_focus_id();
    auto now = now_sec();

    json seed_json = seed_entity_ids;
    json kw_json = keywords;
    json ex_json = exclude_words;
    json rel_json = {"cites","author_of","depends_on","extends","competes_with","used_by","derived_from","evaluated_on"};

    std::string sql = "INSERT INTO focuses (id, name, description, seed_entities, keywords, exclude_words, allowed_rels, allowed_sources, max_depth, relevance_threshold, max_nodes, status, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return "";
    auto bind_text = [&](int i, const std::string& s) {
        sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT);
    };
    bind_text(1, id);
    bind_text(2, name);
    bind_text(3, description);
    bind_text(4, seed_json.dump());
    bind_text(5, kw_json.dump());
    bind_text(6, ex_json.dump());
    bind_text(7, rel_json.dump());
    sqlite3_bind_null(stmt, 8);          // allowed_sources 暂空
    sqlite3_bind_int(stmt, 9, max_depth);
    sqlite3_bind_double(stmt, 10, relevance_threshold);
    sqlite3_bind_int(stmt, 11, max_nodes);
    bind_text(12, "active");
    sqlite3_bind_int64(stmt, 13, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // 种子实体标记为 seed(depth=0, relevance=1.0, sprawl_status=seed)
    for (const auto& eid : seed_entity_ids) {
        sql = "INSERT OR IGNORE INTO focus_members (focus_id, entity_id, depth, relevance, sprawl_status) VALUES (?,?,?,?,?)";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            bind_text(1, id);
            bind_text(2, eid);
            sqlite3_bind_int(stmt, 3, 0);
            sqlite3_bind_double(stmt, 4, 1.0);
            bind_text(5, "seed");
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return id;
}

json CacheManager::get_focus(const std::string& focus_id) {
    std::lock_guard<std::mutex> lk(mu_);
    json result;
    std::string sql = "SELECT id, name, description, seed_entities, keywords, exclude_words, "
                      "allowed_rels, allowed_sources, max_depth, relevance_threshold, max_nodes, "
                      "status, created_at, last_crawl_at FROM focuses WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, focus_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result["id"] = (const char*)sqlite3_column_text(stmt, 0);
            result["name"] = (const char*)sqlite3_column_text(stmt, 1);
            const char* desc = (const char*)sqlite3_column_text(stmt, 2);
            result["description"] = desc ? desc : "";
            result["seed_entities"] = json::parse((const char*)sqlite3_column_text(stmt, 3));
            result["keywords"] = json::parse((const char*)sqlite3_column_text(stmt, 4));
            result["exclude_words"] = json::parse((const char*)sqlite3_column_text(stmt, 5));
            result["allowed_rels"] = json::parse((const char*)sqlite3_column_text(stmt, 6));
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
                result["allowed_sources"] = json::parse((const char*)sqlite3_column_text(stmt, 7));
            } else {
                result["allowed_sources"] = json::array();
            }
            result["max_depth"] = sqlite3_column_int(stmt, 8);
            result["relevance_threshold"] = sqlite3_column_double(stmt, 9);
            result["max_nodes"] = sqlite3_column_int(stmt, 10);
            const char* status = (const char*)sqlite3_column_text(stmt, 11);
            result["status"] = status ? status : "active";
            result["created_at"] = sqlite3_column_int64(stmt, 12);
            result["last_crawl_at"] = sqlite3_column_int64(stmt, 13);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

std::vector<json> CacheManager::list_focuses() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<json> out;
    std::string sql = "SELECT f.id, f.name, f.status, f.max_depth, f.relevance_threshold, f.max_nodes, COUNT(fm.entity_id) as node_count "
                      "FROM focuses f LEFT JOIN focus_members fm ON fm.focus_id=f.id GROUP BY f.id ORDER BY f.created_at DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json row;
            row["id"] = (const char*)sqlite3_column_text(stmt, 0);
            row["name"] = (const char*)sqlite3_column_text(stmt, 1);
            row["status"] = (const char*)sqlite3_column_text(stmt, 2);
            row["max_depth"] = sqlite3_column_int(stmt, 3);
            row["relevance_threshold"] = sqlite3_column_double(stmt, 4);
            row["max_nodes"] = sqlite3_column_int(stmt, 5);
            row["node_count"] = sqlite3_column_int(stmt, 6);
            out.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

bool CacheManager::update_focus(const std::string& focus_id, const json& patch) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return false;
    std::vector<std::string> sets;
    std::vector<std::string> vals;
    if (patch.contains("name") && patch["name"].is_string()) {
        sets.push_back("name=?");
        vals.push_back(patch["name"]);
    }
    if (patch.contains("description") && patch["description"].is_string()) {
        sets.push_back("description=?");
        vals.push_back(patch["description"]);
    }
    if (patch.contains("max_depth") && patch["max_depth"].is_number_integer()) {
        sets.push_back("max_depth=?");
        vals.push_back(std::to_string(patch["max_depth"].get<int>()));
    }
    if (patch.contains("relevance_threshold") && patch["relevance_threshold"].is_number()) {
        sets.push_back("relevance_threshold=?");
        vals.push_back(std::to_string(patch["relevance_threshold"].get<double>()));
    }
    if (patch.contains("status") && patch["status"].is_string()) {
        sets.push_back("status=?");
        vals.push_back(patch["status"]);
    }
    if (sets.empty()) return true;

    std::string sql = "UPDATE focuses SET " + join_strings(sets, ",") + " WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    for (size_t i = 0; i < vals.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), vals[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, static_cast<int>(vals.size() + 1), focus_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool CacheManager::delete_focus(const std::string& focus_id, bool keep_entities) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return false;
    // 删除 focus 和其成员(实体数据在 entities/attributes 表中保留)
    exec_sql("DELETE FROM focus_members WHERE focus_id='" + focus_id + "';");
    exec_sql("DELETE FROM gaps WHERE focus_id='" + focus_id + "';");
    exec_sql("DELETE FROM track_schedules WHERE focus_id='" + focus_id + "';");
    exec_sql("DELETE FROM focuses WHERE id='" + focus_id + "';");
    return true;
}

bool CacheManager::add_focus_member(const std::string& focus_id,
                                     const std::string& entity_id,
                                     int depth,
                                     double relevance,
                                     const std::string& sprawl_status) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return false;
    std::string sql = "INSERT OR IGNORE INTO focus_members (focus_id, entity_id, depth, relevance, sprawl_status) VALUES (?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
    bind_t(1, focus_id);
    bind_t(2, entity_id);
    sqlite3_bind_int(stmt, 3, depth);
    sqlite3_bind_double(stmt, 4, relevance);
    bind_t(5, sprawl_status);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

std::vector<json> CacheManager::get_focus_members(const std::string& focus_id,
                                                   const std::string& status,
                                                   int limit) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<json> out;
    std::string sql = "SELECT fm.entity_id, fm.depth, fm.relevance, fm.sprawl_status, fm.check_count, fm.last_checked, "
                      "ei.entity_type, ei.canonical_name "
                      "FROM focus_members fm LEFT JOIN entity_index ei ON ei.entity_id=fm.entity_id "
                      "WHERE fm.focus_id=?";
    if (!status.empty()) sql += " AND fm.sprawl_status='" + status + "'";
    sql += " ORDER BY fm.relevance DESC LIMIT " + std::to_string(limit);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, focus_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json row;
            row["entity_id"] = (const char*)sqlite3_column_text(stmt, 0);
            row["depth"] = sqlite3_column_int(stmt, 1);
            row["relevance"] = sqlite3_column_double(stmt, 2);
            row["sprawl_status"] = (const char*)sqlite3_column_text(stmt, 3);
            row["check_count"] = sqlite3_column_int(stmt, 4);
            row["last_checked"] = sqlite3_column_int64(stmt, 5);
            const char* et = (const char*)sqlite3_column_text(stmt, 6);
            row["entity_type"] = et ? et : "";
            const char* cn = (const char*)sqlite3_column_text(stmt, 7);
            row["canonical_name"] = cn ? cn : "";
            out.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

bool CacheManager::update_member_status(const std::string& focus_id,
                                         const std::string& entity_id,
                                         const std::string& new_status,
                                         double new_relevance) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql;
    sqlite3_stmt* stmt = nullptr;
    if (new_relevance >= 0) {
        sql = "UPDATE focus_members SET sprawl_status=?, relevance=?, last_checked=? WHERE focus_id=? AND entity_id=?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
        auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        bind_t(1, new_status);
        sqlite3_bind_double(stmt, 2, new_relevance);
        sqlite3_bind_int64(stmt, 3, now_sec());
        bind_t(4, focus_id);
        bind_t(5, entity_id);
    } else {
        sql = "UPDATE focus_members SET sprawl_status=?, last_checked=? WHERE focus_id=? AND entity_id=?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
        auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        bind_t(1, new_status);
        sqlite3_bind_int64(stmt, 2, now_sec());
        bind_t(3, focus_id);
        bind_t(4, entity_id);
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ── attributes ─────────────────────────────────────────────
int CacheManager::upsert_attribute(const std::string& entity_id,
                                    const std::string& attr_key,
                                    const std::string& attr_value_json,
                                    const std::string& source,
                                    double confidence) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return 0;
    int64_t now = now_sec();
    std::string sql = "INSERT OR IGNORE INTO attributes (entity_id, attr_key, attr_value, source, confidence, extracted_at) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
    bind_t(1, entity_id);
    bind_t(2, attr_key);
    bind_t(3, attr_value_json);
    bind_t(4, source);
    sqlite3_bind_double(stmt, 5, confidence);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes;
}

std::vector<json> CacheManager::get_attributes(const std::string& entity_id, const std::string& attr_key) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<json> out;
    std::string sql = "SELECT entity_id, attr_key, attr_value, source, confidence, verified, extracted_at, model FROM attributes WHERE entity_id=?";
    if (!attr_key.empty()) sql += " AND attr_key='" + attr_key + "'";
    sql += " ORDER BY confidence DESC, verified DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, entity_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json row;
            row["entity_id"] = (const char*)sqlite3_column_text(stmt, 0);
            row["attr_key"] = (const char*)sqlite3_column_text(stmt, 1);
            row["attr_value"] = (const char*)sqlite3_column_text(stmt, 2);
            row["source"] = (const char*)sqlite3_column_text(stmt, 3);
            row["confidence"] = sqlite3_column_double(stmt, 4);
            row["verified"] = sqlite3_column_int(stmt, 5);
            row["extracted_at"] = sqlite3_column_int64(stmt, 6);
            const char* m = (const char*)sqlite3_column_text(stmt, 7);
            row["model"] = m ? m : "";
            out.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

json CacheManager::get_merged_attribute(const std::string& entity_id, const std::string& attr_key) {
    auto attrs = get_attributes(entity_id, attr_key);
    if (attrs.empty()) return json();
    // 多源合并:相同值合并,不同值按最高 confidence 取
    std::map<std::string, double> value_scores;
    std::map<std::string, std::vector<std::string>> value_sources;
    for (const auto& a : attrs) {
        std::string v = a.value("attr_value", "");
        double c = a.value("confidence", 0.5);
        value_scores[v] += c;
        value_sources[v].push_back(a.value("source", ""));
    }
    std::string best_value;
    double best_score = -1;
    for (auto& kv : value_scores) {
        if (kv.second > best_score) {
            best_score = kv.second;
            best_value = kv.first;
        }
    }
    json result;
    result["value"] = best_value;
    result["confidence"] = best_score / value_scores.size();
    result["source_count"] = attrs.size();
    result["sources"] = value_sources[best_value];
    return result;
}

// ── gaps ─────────────────────────────────────────────────
int CacheManager::upsert_gap(const std::string& focus_id,
                               const std::string& entity_id,
                               const std::string& missing_key,
                               double priority,
                               const std::string& reason,
                               const std::string& fetch_plan_json) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return 0;
    // 先看是否已有未解决的同缺口
    std::string check_sql = "SELECT id FROM gaps WHERE focus_id=? AND entity_id=? AND missing_key=? AND resolved_at IS NULL";
    sqlite3_stmt* stmt = nullptr;
    int64_t existing = 0;
    if (sqlite3_prepare_v2(db_, check_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        bind_t(1, focus_id); bind_t(2, entity_id); bind_t(3, missing_key);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            existing = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (existing > 0) {
        // 更新优先级(取更高的)
        std::string upd = "UPDATE gaps SET priority=MAX(priority, ?) WHERE id=?";
        if (sqlite3_prepare_v2(db_, upd.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_double(stmt, 1, priority);
            sqlite3_bind_int64(stmt, 2, existing);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return static_cast<int>(existing);
    }
    int64_t now = now_sec();
    std::string ins = "INSERT INTO gaps (focus_id, entity_id, missing_key, priority, reason, fetch_plan, created_at) VALUES (?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, ins.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        bind_t(1, focus_id); bind_t(2, entity_id); bind_t(3, missing_key);
        sqlite3_bind_double(stmt, 4, priority);
        bind_t(5, reason); bind_t(6, fetch_plan_json);
        sqlite3_bind_int64(stmt, 7, now);
        sqlite3_step(stmt);
        int64_t id = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(stmt);
        return static_cast<int>(id);
    }
    return 0;
}

std::vector<json> CacheManager::get_gaps(const std::string& focus_id, double min_priority, int limit) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<json> out;
    std::string sql = "SELECT id, entity_id, missing_key, priority, reason, created_at FROM gaps WHERE focus_id=? AND resolved_at IS NULL AND priority>=? ORDER BY priority DESC LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, focus_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, min_priority);
        sqlite3_bind_int(stmt, 3, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json row;
            row["id"] = sqlite3_column_int64(stmt, 0);
            row["entity_id"] = (const char*)sqlite3_column_text(stmt, 1);
            row["missing_key"] = (const char*)sqlite3_column_text(stmt, 2);
            row["priority"] = sqlite3_column_double(stmt, 3);
            const char* r = (const char*)sqlite3_column_text(stmt, 4);
            row["reason"] = r ? r : "";
            row["created_at"] = sqlite3_column_int64(stmt, 5);
            out.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

bool CacheManager::resolve_gap(int64_t gap_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return false;
    std::string sql = "UPDATE gaps SET resolved_at=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, now_sec());
    sqlite3_bind_int64(stmt, 2, gap_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ── extraction_jobs ────────────────────────────────────────
int64_t CacheManager::create_extraction_job(const std::string& entity_id,
                                              const std::string& job_type,
                                              const std::string& prompt,
                                              const std::string& input_ref) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return 0;
    std::string sql = "INSERT INTO extraction_jobs (entity_id, job_type, prompt, input_ref, created_at) VALUES (?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
    bind_t(1, entity_id); bind_t(2, job_type); bind_t(3, prompt); bind_t(4, input_ref);
    sqlite3_bind_int64(stmt, 5, now_sec());
    sqlite3_step(stmt);
    int64_t id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return id;
}

bool CacheManager::update_extraction_job(int64_t job_id,
                                           const std::string& status,
                                           const std::string& result_json,
                                           const std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return false;
    std::string sql;
    sqlite3_stmt* stmt = nullptr;
    if (status == "done" || status == "failed" || status == "empty") {
        sql = "UPDATE extraction_jobs SET status=?, result=?, completed_at=? WHERE id=?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
        auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        bind_t(1, status); bind_t(2, result_json);
        sqlite3_bind_int64(stmt, 3, now_sec());
        sqlite3_bind_int64(stmt, 4, job_id);
    } else {
        sql = "UPDATE extraction_jobs SET status=? WHERE id=?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
        auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
        bind_t(1, status);
        sqlite3_bind_int64(stmt, 2, job_id);
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ── track_schedules ────────────────────────────────────────
int CacheManager::create_track_schedule(const std::string& focus_id,
                                         const std::string& entity_id,
                                         const std::string& track_type,
                                         int interval_hours) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return 0;
    std::string sql = "INSERT INTO track_schedules (focus_id, entity_id, track_type, interval_hours) VALUES (?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    auto bind_t = [&](int i, const std::string& s) { sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT); };
    bind_t(1, focus_id); bind_t(2, entity_id); bind_t(3, track_type);
    sqlite3_bind_int(stmt, 4, interval_hours);
    sqlite3_step(stmt);
    int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    sqlite3_finalize(stmt);
    return id;
}

std::vector<json> CacheManager::get_due_tracks(int limit) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<json> out;
    int64_t now = now_sec();
    std::string sql = "SELECT id, focus_id, entity_id, track_type, interval_hours, consecutive_empty FROM track_schedules WHERE last_checked IS NULL OR (? - last_checked) >= interval_hours*3600 ORDER BY consecutive_empty ASC, interval_hours ASC LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_int(stmt, 2, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json row;
            row["id"] = sqlite3_column_int(stmt, 0);
            row["focus_id"] = (const char*)sqlite3_column_text(stmt, 1);
            row["entity_id"] = (const char*)sqlite3_column_text(stmt, 2);
            row["track_type"] = (const char*)sqlite3_column_text(stmt, 3);
            row["interval_hours"] = sqlite3_column_int(stmt, 4);
            row["consecutive_empty"] = sqlite3_column_int(stmt, 5);
            out.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

bool CacheManager::update_track_interval(int schedule_id, int new_interval_hours) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inited_) return false;
    std::string sql = "UPDATE track_schedules SET interval_hours=?, last_checked=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, new_interval_hours);
    sqlite3_bind_int64(stmt, 2, now_sec());
    sqlite3_bind_int(stmt, 3, schedule_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ── 蔓延统计 ────────────────────────────────────────────────
json CacheManager::get_sprawl_stats(const std::string& focus_id) {
    std::lock_guard<std::mutex> lk(mu_);
    json stats;
    // 总 entities
    int64_t n_entities = 0;
    if (focus_id.empty()) {
        std::string sql = "SELECT COUNT(*) FROM entity_index";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            n_entities = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    } else {
        std::string sql = "SELECT COUNT(*) FROM focus_members WHERE focus_id=?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, focus_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) n_entities = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    stats["entities"] = n_entities;

    // attributes 数量
    int64_t n_attr = 0;
    {
        std::string sql = "SELECT COUNT(*) FROM attributes";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            n_attr = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    stats["attributes"] = n_attr;

    // relations 数量
    int64_t n_rel = 0;
    {
        std::string sql = "SELECT COUNT(*) FROM relation_links";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            n_rel = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    stats["relations"] = n_rel;

    // gaps 数量
    int64_t n_gaps = 0;
    {
        std::string sql = "SELECT COUNT(*) FROM gaps WHERE resolved_at IS NULL";
        if (!focus_id.empty()) sql += " AND focus_id='" + focus_id + "'";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            n_gaps = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    stats["open_gaps"] = n_gaps;

    // active focuses 数量
    int64_t n_focuses = 0;
    {
        std::string sql = "SELECT COUNT(*) FROM focuses WHERE status='active'";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            n_focuses = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    stats["active_focuses"] = n_focuses;

    return stats;
}

} // namespace github_research
