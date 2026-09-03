#include "github_research/focus_track_engine.hpp"
#include "github_research/cache_manager.hpp"
#include <ctime>
#include <algorithm>
#include <cmath>

namespace github_research {

int create_track_schedule(const std::string& focus_id,
                           const std::string& entity_id,
                           const std::string& entity_type,
                           int interval_hours) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return 0;

    // 根据实体类型选 track_type
    std::vector<std::string> track_types;
    if (entity_type == "project") {
        track_types = {"new_commits", "new_releases", "new_stars"};
    } else if (entity_type == "paper") {
        track_types = {"new_citations", "new_versions"};
    } else if (entity_type == "person") {
        track_types = {"new_papers", "new_projects"};
    } else {
        track_types = {"news"};
    }

    int created = 0;
    for (const auto& tt : track_types) {
        // 简化:调用 cache_manager 的 SQL 直接插入
        // 真实实现需要 CacheManager 提供 add_track_schedule 接口
        // 暂时通过 get_focus_members 检查实体是否属于 focus,然后写 SQL
        // 这里返回 1 表示"已请求创建",真实持久化在后续完善
        // 实际生产环境需要给 CacheManager 加一个 add_track_schedule 方法
        created++;
    }
    return created;
}

int auto_schedule_all_exhausted(const std::string& focus_id) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return 0;

    auto exhausted = cm.get_focus_members(focus_id, "exhausted", 200);
    auto seeds = cm.get_focus_members(focus_id, "seed", 200);

    int total = 0;
    for (const auto& m : exhausted) {
        std::string eid = m.value("entity_id", "");
        if (eid.empty()) continue;
        std::string etype;
        size_t colon = eid.find(':');
        if (colon != std::string::npos) etype = eid.substr(0, colon);
        total += create_track_schedule(focus_id, eid, etype);
    }
    // seed 节点也加入跟踪(持续跟踪"母节点")
    for (const auto& m : seeds) {
        std::string eid = m.value("entity_id", "");
        if (eid.empty()) continue;
        std::string etype;
        size_t colon = eid.find(':');
        if (colon != std::string::npos) etype = eid.substr(0, colon);
        total += create_track_schedule(focus_id, eid, etype);
    }
    return total;
}

int adaptive_interval(int current_interval_hours,
                       bool found_new_content,
                       int consecutive_empty) {
    if (found_new_content) {
        int new_interval = std::max(6, (int)(current_interval_hours * 0.7));
        return new_interval;
    }
    // 没发现新内容,且连续空 >= 3 次 → 拉长
    if (consecutive_empty >= 3) {
        return std::min(168, (int)(current_interval_hours * 1.5));
    }
    return current_interval_hours;
}

json run_track_tick(const std::string& focus_id,
                    int max_schedules_per_tick,
                    bool dry_run) {
    CacheManager& cm = CacheManager::instance();
    json result;
    result["focus_id"] = focus_id;
    result["status"] = "completed";
    result["started_at"] = (int64_t)std::time(nullptr);
    result["schedules_checked"] = 0;
    result["new_items_found"] = 0;
    result["schedules_dormant"] = 0;
    result["dry_run"] = dry_run;

    // 简化实现:先自动生成跟踪计划(如果还没有的话)
    // 真正的 "到期检查" 需要 track_schedules 表有数据
    auto members = cm.get_focus_members(focus_id, "", 2000);
    int exhausted_count = 0;
    int seed_count = 0;
    for (const auto& m : members) {
        std::string s = m.value("sprawl_status", "");
        if (s == "exhausted") exhausted_count++;
        if (s == "seed") seed_count++;
    }
    result["exhausted_nodes"] = exhausted_count;
    result["seed_nodes"] = seed_count;

    // 真实跟踪逻辑:调用 GitHub API / arXiv API 等查新内容
    // 简化版:只返回调度统计信息
    // 后续需要实现:
    //   - 对 project: github_list_commits(since) → 新 commits
    //   - 对 paper:    s2_get_citations_paginated → 新引用
    //   - 对 person:   arxiv_search(author) → 新论文

    result["completed_at"] = (int64_t)std::time(nullptr);
    return result;
}

json get_updates_since(const std::string& focus_id,
                        const std::string& since_iso) {
    CacheManager& cm = CacheManager::instance();
    json result;
    result["focus_id"] = focus_id;
    result["since"] = since_iso;

    // 简化:返回 focus 统计 + 最近更新的成员
    // 真实实现需要:
    //   1. 查询 entities 表中 updated_at >= since 的实体
    //   2. 查询 attributes 表中 extracted_at >= since 的属性
    //   3. 查询 relations 表中 created_at >= since 的关系
    //   4. 按 entity_id 聚合,标注"这一周 entity X 新增了属性 Y 和关系 Z"

    auto stats = cm.get_sprawl_stats(focus_id);
    result["stats"] = stats;
    result["note"] = "incremental update engine: pending API integration";
    return result;
}

} // namespace github_research