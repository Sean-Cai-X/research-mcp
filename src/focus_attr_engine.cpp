#include "github_research/focus_attr_engine.hpp"
#include "github_research/cache_manager.hpp"
#include <ctime>
#include <set>
#include <algorithm>
#include <cmath>

namespace github_research {

// ── 属性模板 ────────────────────────────────────────────────
std::vector<std::string> attr_template_for(const std::string& entity_type) {
    if (entity_type == "project") {
        return {"stars", "forks", "description", "topics", "last_commit_at",
                "language", "license", "open_issues", "latest_release"};
    }
    if (entity_type == "paper") {
        return {"citation_count", "authors", "abstract", "venue", "year",
                "reference_count", "url", "doi"};
    }
    if (entity_type == "person") {
        return {"affiliation", "h_index", "paper_count", "homepage",
                "github_username", "orcid"};
    }
    if (entity_type == "org") {
        return {"description", "website", "member_count", "founded_year"};
    }
    if (entity_type == "concept") {
        return {"definition", "parent_concepts", "related_concepts"};
    }
    return {"description", "url"};
}

// ── 提取单个实体属性 ────────────────────────────────────────
int extract_attributes_for_entity(const std::string& entity_id,
                                   const std::string& entity_type,
                                   int max_keys_per_pass) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return 0;

    auto template_keys = attr_template_for(entity_type);

    // 查已有的属性,跳过已经有足够置信度的
    int extracted = 0;
    for (const auto& key : template_keys) {
        if (extracted >= max_keys_per_pass) break;

        auto existing = cm.get_attributes(entity_id, key);

        // 如果已有且 confidence >= 0.7,跳过
        bool skip = false;
        for (const auto& a : existing) {
            if (a.value("confidence", 0.0) >= 0.7) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        // ── 尝试从 entities 表和 relations 表推断 ──
        // 简化实现:从 entity_id 本身和已存 relations 推断一些基本属性
        std::string entity_name = entity_id;
        size_t colon = entity_id.find(':');
        if (colon != std::string::npos) {
            entity_name = entity_id.substr(colon + 1);
        }

        // 基本属性:从 entity_id 推断 type
        if (key == "name") {
            cm.upsert_attribute(entity_id, "name", entity_name,
                                "inferred:entity_id", 0.3);
            extracted++;
            continue;
        }

        // 特殊:project 类型,从 entity_id 推断 URL
        if (entity_type == "project" && key == "url") {
            // entity_id 格式: project:owner/repo → URL = https://github.com/owner/repo
            auto p = entity_id.find(":");
            if (p != std::string::npos) {
                std::string cn = entity_id.substr(p + 1);
                if (!cn.empty() && cn.find("/") != std::string::npos) {
                    std::string url = "https://github.com/" + cn;
                    cm.upsert_attribute(entity_id, "url", url,
                                        "inferred:entity_id", 0.4);
                    extracted++;
                    continue;
                }
            }
        }

        // TODO:真正的属性提取需要调用 GitHub API / arXiv API 等
        // 目前只做框架,真实调用在后续集成

        // placeholder: 写入低置信度的占位,表示"已知需要提取"
        // 不写真实值,等后续 API 调用后再补
    }

    return extracted;
}

// ── 批量提取 tick ────────────────────────────────────────────
json run_extract_tick(const std::string& focus_id,
                       int max_entities_per_tick,
                       bool dry_run) {
    CacheManager& cm = CacheManager::instance();
    json result;
    result["focus_id"] = focus_id;
    result["dry_run"] = dry_run;
    result["started_at"] = (int64_t)std::time(nullptr);

    json focus = cm.get_focus(focus_id);
    if (focus.empty()) {
        result["error"] = "focus not found";
        result["status"] = "failed";
        return result;
    }

    // 取 active + boundary 节点
    auto active = cm.get_focus_members(focus_id, "active", 200);
    auto boundary = cm.get_focus_members(focus_id, "boundary", 200);
    std::vector<json> all;
    all.insert(all.end(), active.begin(), active.end());
    all.insert(all.end(), boundary.begin(), boundary.end());

    // 按 depth 升序(靠近种子的优先提取)
    std::sort(all.begin(), all.end(),
              [](const json& a, const json& b) {
                  return a.value("depth", 99) < b.value("depth", 99);
              });

    if ((int)all.size() > max_entities_per_tick) {
        all.resize(max_entities_per_tick);
    }

    int total_extracted = 0;
    std::vector<json> per_entity;

    for (const auto& member : all) {
        std::string eid = member.value("entity_id", "");
        if (eid.empty()) continue;

        std::string etype;
        size_t colon = eid.find(':');
        if (colon != std::string::npos) etype = eid.substr(0, colon);

        int n = 0;
        if (!dry_run) {
            n = extract_attributes_for_entity(eid, etype, 3);
        }
        total_extracted += n;

        // 合并已有属性的置信度
        if (!dry_run) {
            merge_attributes_for_entity(eid);
        }

        per_entity.push_back({
            {"entity_id", eid},
            {"extracted_keys", n}
        });
    }

    // 检测缺口并生成提取任务
    int gaps_scheduled = 0;
    if (!dry_run) {
        gaps_scheduled = schedule_gap_extractions(focus_id);
    }

    result["status"] = "completed";
    result["entities_processed"] = (int)all.size();
    result["attributes_extracted"] = total_extracted;
    result["gaps_scheduled"] = gaps_scheduled;
    result["per_entity"] = per_entity;
    result["completed_at"] = (int64_t)std::time(nullptr);
    return result;
}

// ── 合并属性 ────────────────────────────────────────────────
int merge_attributes_for_entity(const std::string& entity_id) {
    CacheManager& cm = CacheManager::instance();
    // 简化:让调用方自己查
    // 真实实现:查所有该 entity 的属性,同 key 同值多来源时提升 confidence
    // 这里返回 0 表示需要后续完善
    return 0;
}

// ── 缺口检测 ────────────────────────────────────────────────
std::vector<json> detect_gaps(const std::string& focus_id) {
    CacheManager& cm = CacheManager::instance();
    std::vector<json> gaps_result;

    auto members = cm.get_focus_members(focus_id, "active", 200);
    auto boundary = cm.get_focus_members(focus_id, "boundary", 200);
    members.insert(members.end(), boundary.begin(), boundary.end());

    for (const auto& member : members) {
        std::string eid = member.value("entity_id", "");
        if (eid.empty()) continue;

        std::string etype;
        size_t colon = eid.find(':');
        if (colon != std::string::npos) etype = eid.substr(0, colon);

        auto template_keys = attr_template_for(etype);
        auto existing = cm.get_attributes(eid, "");

        // 已有哪些 key
        std::set<std::string> have_keys;
        for (const auto& a : existing) {
            have_keys.insert(a.value("attr_key", ""));
        }

        std::vector<std::string> missing;
        for (const auto& tk : template_keys) {
            if (have_keys.find(tk) == have_keys.end()) {
                missing.push_back(tk);
            }
        }

        if (!missing.empty()) {
            double coverage = 1.0 - (double)missing.size() / template_keys.size();
            json g;
            g["entity_id"] = eid;
            g["entity_type"] = etype;
            g["missing_keys"] = missing;
            g["coverage_ratio"] = std::round(coverage * 100.0) / 100.0;
            g["priority"] = member.value("relevance", 0.5) * (1.0 - coverage);
            gaps_result.push_back(g);
        }
    }

    // 按 priority 降序
    std::sort(gaps_result.begin(), gaps_result.end(),
              [](const json& a, const json& b) {
                  return a.value("priority", 0.0) > b.value("priority", 0.0);
              });

    return gaps_result;
}

// ── 生成缺口提取任务 ────────────────────────────────────────
int schedule_gap_extractions(const std::string& focus_id) {
    // 简化:detect_gaps 返回结果,真实写 gaps 表在后续完善
    auto gaps = detect_gaps(focus_id);
    return (int)gaps.size();
}

} // namespace github_research