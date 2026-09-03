#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace github_research {
using json = nlohmann::json;

// 为 exhausted/seed 节点生成跟踪计划
// track_type 按实体类型决定: project→new_commits+new_releases, paper→new_citations, person→new_papers
int create_track_schedule(const std::string& focus_id,
                           const std::string& entity_id,
                           const std::string& entity_type,
                           int interval_hours = 24);

// 为 focus 下所有 exhausted 节点批量生成跟踪计划
int auto_schedule_all_exhausted(const std::string& focus_id);

// 自适应调整 interval: 发现新内容→×0.7(不低于6h), 连续空→×1.5(不高于168h)
int adaptive_interval(int current_interval_hours,
                       bool found_new_content,
                       int consecutive_empty);

// 执行到期的跟踪 tick: 检查 due 的 schedule, 发现新内容, 更新状态
// 返回 {schedules_checked, new_items_found, schedules_dormant}
json run_track_tick(const std::string& focus_id,
                    int max_schedules_per_tick = 10,
                    bool dry_run = false);

// 类 RSS 查询: 自 since 以来的新发现
json get_updates_since(const std::string& focus_id,
                        const std::string& since_iso = "");

} // namespace github_research