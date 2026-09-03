#include "github_research/focus_engine.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/string_utils.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

namespace github_research {
namespace focus_engine {

constexpr double kW_kw        = 0.4;
constexpr double kW_rel       = 0.3;
constexpr double kW_src       = 0.2;
constexpr double kW_other     = 0.1;
constexpr double kDepthDecay  = 0.85;
constexpr int    kExhaustedRounds = 3;

double relation_type_weight(const std::string& rel_type) {
    if (rel_type == "cites" || rel_type == "cited_by") return 0.9;
    if (rel_type == "author_of" || rel_type == "authored_by") return 0.8;
    if (rel_type == "depends_on" || rel_type == "depended_by" ||
        rel_type == "derived_from") return 0.7;
    if (rel_type == "extends" || rel_type == "fork_of") return 0.65;
    if (rel_type == "mentions" || rel_type == "mentioned_by") return 0.5;
    if (rel_type == "competes_with") return 0.5;
    if (rel_type == "used_by" || rel_type == "used_in") return 0.6;
    return 0.4;
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return r;
}

int score_keyword_overlap_multi(const json& candidate,
                                const std::vector<std::string>& keywords) {
    if (keywords.empty()) return 0;
    std::string text;
    if (candidate.contains("canonical_name") &&
        candidate["canonical_name"].is_string()) {
        text += candidate["canonical_name"].get<std::string>() + " ";
    }
    if (candidate.contains("name") && candidate["name"].is_string()) {
        text += candidate["name"].get<std::string>() + " ";
    }
    if (candidate.contains("alias") && candidate["alias"].is_array()) {
        for (const auto& a : candidate["alias"]) {
            if (a.is_string()) text += a.get<std::string>() + " ";
        }
    }
    if (candidate.contains("description") &&
        candidate["description"].is_string()) {
        text += candidate["description"].get<std::string>() + " ";
    }
    if (candidate.contains("topics") && candidate["topics"].is_array()) {
        for (const auto& t : candidate["topics"]) {
            if (t.is_string()) text += t.get<std::string>() + " ";
        }
    }
    std::string lt = to_lower(text);
    int s = 0;
    for (const auto& kw : keywords) {
        if (lt.find(to_lower(kw)) != std::string::npos) s += 3;
    }
    return s;
}

double compute_relevance(const json& focus,
                          const json& entity_candidate,
                          const std::string& relation_type,
                          int depth,
                          const std::string& source_id) {
    std::vector<std::string> keywords;
    if (focus.contains("keywords") && focus["keywords"].is_array()) {
        for (const auto& k : focus["keywords"]) {
            if (k.is_string()) keywords.push_back(k.get<std::string>());
        }
    }
    int raw_kw = score_keyword_overlap_multi(entity_candidate, keywords);
    double kw_norm = std::min(1.0, raw_kw / 15.0);
    double rw = relation_type_weight(relation_type);
    if (relation_type.empty()) rw = 0.5;
    double src_trust = 0.7;
    if (!source_id.empty()) {
        CacheManager& cm = CacheManager::instance();
        auto ds = cm.get_source(source_id);
        if (ds) src_trust = ds->reliability;
    }
    double base = kW_kw * kw_norm + kW_rel * rw + kW_src * src_trust;
    double decay = std::pow(kDepthDecay, std::max(0, depth));
    double score = base * decay;
    return std::max(0.0, std::min(1.0, score));
}

ClassifyResult classify_member(double relevance,
                                int depth,
                                const json& focus,
                                int existing_node_count) {
    ClassifyResult r;
    double threshold = focus.value("relevance_threshold", 0.55);
    int max_depth = focus.value("max_depth", 3);
    int max_nodes = focus.value("max_nodes", 500);
    if (existing_node_count >= max_nodes) {
        r.status = "boundary";
        r.relevance = relevance;
        r.reason = "max_nodes reached(" + std::to_string(max_nodes) + ")";
        return r;
    }
    if (relevance >= threshold && depth < max_depth) {
        r.status = "active";
        r.relevance = relevance;
        r.reason = "relevance>=threshold && depth<max_depth";
    } else if (relevance >= threshold && depth >= max_depth) {
        r.status = "boundary";
        r.relevance = relevance;
        r.reason = "relevance>=threshold but depth>=max_depth";
    } else if (relevance >= threshold * 0.5) {
        r.status = "boundary";
        r.relevance = relevance;
        r.reason = "relevance in [threshold*0.5, threshold), boundary";
    } else {
        r.status = "pruned";
        r.relevance = relevance;
        r.reason = "relevance < threshold*0.5, pruned";
    }
    return r;
}

std::vector<json> discover_neighbors(const std::string& entity_id,
                                      const std::string& entity_type,
                                      const std::string& canonical_name) {
    std::vector<json> candidates;
    CacheManager& cm = CacheManager::instance();
    std::vector<std::string> out_types, in_types;
    if (entity_type == "project") {
        out_types = {"depends_on", "derived_from", "fork_of"};
        in_types  = {"depended_by", "forked_by", "used_by"};
    } else if (entity_type == "paper") {
        out_types = {"cites", "mentions"};
        in_types  = {"cited_by", "mentioned_by", "authored_by"};
    } else if (entity_type == "person") {
        out_types = {"authored_by", "contributed"};
        in_types  = {"author_of", "mentions"};
    } else {
        out_types = {"mentions"};
        in_types  = {"mentioned_by"};
    }
    for (const auto& rt : out_types) {
        auto rels = cm.query_relations(entity_id, "out", rt, 0.0, 20);
        for (const auto& r : rels) {
            json cand;
            cand["entity_id"] = r.value("target_entity", "");
            cand["relation_type"] = rt;
            cand["source_id"] = r.value("source", "");
            candidates.push_back(cand);
        }
    }
    for (const auto& rt : in_types) {
        auto rels = cm.query_relations(entity_id, "in", rt, 0.0, 20);
        for (const auto& r : rels) {
            json cand;
            cand["entity_id"] = r.value("source_entity", "");
            cand["relation_type"] = rt;
            cand["source_id"] = r.value("source", "");
            candidates.push_back(cand);
        }
    }
    return candidates;
}

json run_sprawl_tick(const std::string& focus_id,
                      int max_nodes_per_tick,
                      bool dry_run) {
    CacheManager& cm = CacheManager::instance();
    json result;
    result["focus_id"] = focus_id;
    result["dry_run"] = dry_run;
    result["started_at"] = (int64_t)std::time(nullptr);

    json focus = cm.get_focus(focus_id);
    if (focus.empty()) {
        result["error"] = "focus not found: " + focus_id;
        result["status"] = "failed";
        return result;
    }
    result["focus_name"] = focus.value("name", "");
    std::string status = focus.value("status", "active");
    if (status != "active") {
        result["status"] = "skipped";
        result["reason"] = "focus status is '" + status + "', not active";
        return result;
    }

    int total_new = 0, total_boundary = 0, total_pruned = 0;
    int nodes_processed = 0;
    std::vector<json> processed_nodes;

    auto active_members = cm.get_focus_members(focus_id, "active", 200);
    if (active_members.empty()) {
        auto seed_members = cm.get_focus_members(focus_id, "seed", 50);
        if (seed_members.empty()) {
            result["status"] = "skipped";
            result["reason"] = "no active or seed members";
            return result;
        }
        active_members = seed_members;
    }

    std::sort(active_members.begin(), active_members.end(),
              [](const json& a, const json& b) {
                  double ra = a.value("relevance", 0.0);
                  double rb = b.value("relevance", 0.0);
                  int ca = a.value("check_count", 0);
                  int cb = b.value("check_count", 0);
                  if (std::abs(ra - rb) > 0.001) return ra > rb;
                  return ca < cb;
              });

    if ((int)active_members.size() > max_nodes_per_tick) {
        active_members.resize(max_nodes_per_tick);
    }

    auto all_members = cm.get_focus_members(focus_id, "", 2000);
    int existing_count = (int)all_members.size();

    for (const auto& member : active_members) {
        std::string entity_id = member.value("entity_id", "");
        int cur_depth = member.value("depth", 0);
        if (entity_id.empty()) continue;
        nodes_processed++;

        std::string entity_type, canonical_name;
        size_t colon = entity_id.find(':');
        if (colon != std::string::npos) {
            entity_type = entity_id.substr(0, colon);
            canonical_name = entity_id.substr(colon + 1);
        }

        auto neighbors = discover_neighbors(entity_id, entity_type, canonical_name);
        int new_this_node = 0;

        for (auto& cand : neighbors) {
            std::string cand_id = cand.value("entity_id", "");
            if (cand_id.empty()) continue;
            std::string rel_type = cand.value("relation_type", "");
            std::string src_id = cand.value("source_id", "");

            json cand_entity;
            cand_entity["canonical_name"] = cand_id;

            double relevance = compute_relevance(
                focus, cand_entity, rel_type, cur_depth + 1, src_id);

            auto cls = classify_member(
                relevance, cur_depth + 1, focus, existing_count + total_new);

            if (!dry_run) {
                cm.add_focus_member(focus_id, cand_id,
                                     cur_depth + 1, cls.relevance,
                                     cls.status);
            }

            if (cls.status == "active") {
                total_new++;
                new_this_node++;
            } else if (cls.status == "boundary") {
                total_boundary++;
            } else {
                total_pruned++;
            }
        }

        json node_info = {
            {"entity_id", entity_id},
            {"entity_type", entity_type},
            {"depth", cur_depth},
            {"discovered_neighbors", (int)neighbors.size()},
            {"new_active", new_this_node}
        };
        processed_nodes.push_back(node_info);
    }

    if (!dry_run) {
        cm.update_focus(focus_id, {{"last_crawl_at", (int64_t)std::time(nullptr)}});
    }

    result["status"] = "completed";
    result["nodes_processed"] = nodes_processed;
    result["new_active"] = total_new;
    result["new_boundary"] = total_boundary;
    result["new_pruned"] = total_pruned;
    result["dry_run"] = dry_run;
    result["nodes"] = processed_nodes;
    result["completed_at"] = (int64_t)std::time(nullptr);
    return result;
}

json estimate_relevance(const std::string& focus_id,
                         const json& entity_candidate,
                         const std::string& relation_type,
                         int depth) {
    CacheManager& cm = CacheManager::instance();
    json focus = cm.get_focus(focus_id);
    if (focus.empty()) {
        return {{"error", "focus not found"}, {"focus_id", focus_id}};
    }
    double relevance = compute_relevance(
        focus, entity_candidate, relation_type, depth, "");
    auto cls = classify_member(relevance, depth, focus, 0);
    json result;
    result["focus_id"] = focus_id;
    result["entity"] = entity_candidate;
    result["relation_type"] = relation_type;
    result["depth"] = depth;
    result["relevance"] = relevance;
    result["classify"] = {
        {"status", cls.status},
        {"reason", cls.reason}
    };
    result["focus_keywords"] = focus.value("keywords", json::array());
    result["relevance_threshold"] = focus.value("relevance_threshold", 0.55);
    result["max_depth"] = focus.value("max_depth", 3);
    return result;
}

} // namespace focus_engine
} // namespace github_research