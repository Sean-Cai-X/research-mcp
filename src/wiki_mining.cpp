#include "github_research/wiki_mining.hpp"
#include "github_research/string_utils.hpp"

// cppjieba — 通过 CMake FetchContent / third_party junction 引入
// 词典路径由 CMake 通过 CPPJIEBA_DICT_PATH 编译宏注入 (utf8 dict 目录)
#if __has_include("cppjieba/Jieba.hpp")
  #include "cppjieba/Jieba.hpp"
  #define RESEARCH_MCP_HAS_CPPJIEBA 1
#else
  #define RESEARCH_MCP_HAS_CPPJIEBA 0
#endif
#include "github_research/string_utils.hpp"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <set>
#include <regex>

namespace github_research {

// =============================================================
// 查询预处理层 —— 分词 + 结巴容错匹配
// 嵌在触发层之后、挖掘路径之前, 所有搜索路径的统一入口
// =============================================================
namespace query_preprocessor {

// —— Jieba lazy singleton (header-only INTERFACE, 词典路径编译时注入) ——
#if RESEARCH_MCP_HAS_CPPJIEBA
static cppjieba::Jieba& jiebaInstance() {
    static cppjieba::Jieba jb(
        CPPJIEBA_DICT_PATH "/jieba.dict.utf8",
        CPPJIEBA_DICT_PATH "/hmm_model.utf8",
        CPPJIEBA_DICT_PATH "/user.dict.utf8",
        CPPJIEBA_DICT_PATH "/idf.utf8",
        CPPJIEBA_DICT_PATH "/stop_words.utf8"
    );
    return jb;
}
static std::vector<std::string> tryJiebaSegment(const std::string& input) {
    std::vector<std::string> words;
    try { jiebaInstance().Cut(input, words, true); } catch (...) { words.clear(); }
    return words;
}
#endif

// 规则分词 (fallback): 空格/连字符/下划线 + 中英文边界分离
static std::vector<std::string> rule_tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
    };
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == ' ' || c == '-' || c == '_' || c == '/' || c == '\\') {
            flush();
            continue;
        }
        // 中英文边界: 中文字符 (UTF-8 3字节) 前后切分
        // 简化: 检测 ASCII ↔ 非 ASCII 边界
        bool is_ascii = (static_cast<unsigned char>(c) < 0x80);
        if (i > 0) {
            char prev = input[i-1];
            bool prev_ascii = (static_cast<unsigned char>(prev) < 0x80);
            if (is_ascii != prev_ascii) flush();
        }
        cur += c;
    }
    flush();
    return tokens;
}

// 常见技术后缀剥离
static const char* kStripSuffixes[] = {
    "优化", "算法", "技术", "方法", "实现", "机制",
    "优化器", "策略", "框架", "架构", "模型", "系统",
    "optimization", "algorithm", "technique", "method",
    "implementation", "mechanism", "optimizer", "strategy",
    "framework", "architecture", "model", "system"
};

static std::string stripSuffix(const std::string& token) {
    std::string lower = to_lower(token);
    for (auto* suf : kStripSuffixes) {
        std::string s = suf;
        auto pos = lower.rfind(s);
        if (pos != std::string::npos && pos + s.size() == lower.size() &&
            pos > 0 && token.size() > pos) {
            return token.substr(0, pos);
        }
    }
    return token;
}

// 结巴容错: 大小写 + 符号归一, 生成一组形态变体
static std::vector<std::string> jieba_normalize(const std::string& term) {
    std::vector<std::string> result;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        for (auto& e : result) if (e == s) return;
        result.push_back(s);
    };

    add(term);
    std::string lower = to_lower(term);
    add(lower);

    // 符号归一: 所有非字母数字 → 空格
    std::string sym_norm;
    for (char c : term) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (isalnum(uc) || uc >= 0x80) sym_norm += c;
        else sym_norm += ' ';
    }
    std::string no_space;
    for (char c : sym_norm) if (c != ' ') no_space += c;
    if (!no_space.empty()) add(no_space);

    std::string underscore;
    for (char c : sym_norm) {
        if (c == ' ') underscore += '_';
        else underscore += c;
    }
    add(underscore);

    // 缩写提取 (首字母大写)
    std::string acronym;
    bool next_upper = true;
    for (char c : sym_norm) {
        if (c == ' ') { next_upper = true; continue; }
        if (next_upper && (isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80)) {
            acronym += static_cast<char>(toupper(static_cast<unsigned char>(c)));
            next_upper = false;
        }
    }
    if (acronym.size() >= 2) add(acronym);

    return result;
}

// 分级查询组合生成
// 输出: {完整原术语, 2-3 核心词组合, 单核心词} × 结巴变体
// =============================================================
// 编辑距离容错 (Levenshtein, 纯 ASCII)
// 阈值自适应: len<5 不容忍, 5-8 允许 1, >8 允许 2
// 只对全 ASCII 串, 中文靠 Jieba 就够了
// =============================================================
static int levenshteinDistance(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if (a.empty()) return (int)b.size();
    if (b.empty()) return (int)a.size();

    // 只支持 len(a) < len(b)
    if (a.size() > b.size()) return levenshteinDistance(b, a);

    std::vector<int> prev((int)b.size() + 1);
    std::vector<int> curr((int)b.size() + 1);
    for (int j = 0; j <= (int)b.size(); ++j) prev[j] = j;

    for (int i = 1; i <= (int)a.size(); ++i) {
        curr[0] = i;
        for (int j = 1; j <= (int)b.size(); ++j) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j-1] + 1, prev[j-1] + cost});
        }
        prev.swap(curr);
    }
    return prev[(int)b.size()];
}

static bool isPureAscii(const std::string& s) {
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80) return false;
        if (!isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_') return false;
    }
    return !s.empty();
}

static int maxEditDistance(const std::string& term) {
    int len = (int)term.size();
    if (len < 5) return 0;
    if (len <= 8) return 1;
    return 2;
}

// 单字符 substitution 候选: 对每个位置尝试 a-z (小写) + 大小写翻转
static std::vector<std::string> generate1Substitution(const std::string& term) {
    std::vector<std::string> out;
    out.reserve(term.size() * 27);
    std::string lower = to_lower(term);

    for (size_t i = 0; i < lower.size(); ++i) {
        if (!isalpha(static_cast<unsigned char>(lower[i]))) continue;
        for (char c = 'a'; c <= 'z'; ++c) {
            if (c == lower[i]) continue;
            std::string variant = lower;
            variant[i] = c;
            out.push_back(variant);
        }
    }
    return out;
}

// 高频技术同义词表 (4 核心域, 硬编码 + 可扩展)
// key 小写 → value 候选串 (小写后生成 jieba_normalize 形态)
static std::vector<std::string> lookupSynonyms(const std::string& term) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kSyn = {
        {"kernel",        {"core", "os kernel", "os core", "monolithic", "microkernel"}},
        {"kernel space",  {"kernelmode", "ring 0", "supervisor mode"}},
        {"ring 0",        {"ring0", "kernel mode", "supervisor"}},
        {"system call",   {"syscall", "sys call", "trap"}},
        {"trap",          {"exception", "interrupt", "syscall"}},
        {"interrupt",     {"irq", "exception", "trap"}},
        {"context switch",{"context switching", "task switch", "thread switch"}},
        {"virtual memory",{"vm", "virtual storage", "address translation"}},
        {"vm",            {"virtual memory", "vm system"}},
        {"page table",    {"translation table", "tlb table"}},
        {"tlb",           {"translation lookaside buffer"}},
        {"malloc",        {"memory allocator", "heap alloc", "allocation"}},
        {"heap",          {"dynamic memory", "heap memory"}},
        {"stack",         {"stack memory", "call stack"}},
        {"mmu",           {"memory management unit", "address translation"}},
        {"memory leak",   {"leak", "resource leak"}},
        {"socket",        {"network socket", "berkeley socket"}},
        {"tcp",           {"transmission control protocol"}},
        {"udp",           {"user datagram protocol"}},
        {"ip",            {"internet protocol"}},
        {"load balancing",{"load balancer", "lb", "traffic distribution"}},
        {"compiler",      {"transpiler", "code generator"}},
        {"llvm",          {"low level vm", "bytecode", "ir"}},
        {"hash",          {"hashing", "digest", "checksum"}},
        {"encryption",    {"crypto", "cipher", "cryptography"}},
        {"public key",    {"asymmetric", "pubkey", "rsa", "ecdsa"}},
        {"mutex",         {"mutual exclusion", "lock", "spinlock"}},
        {"race condition",{"data race", "thread race"}},
        {"deadlock",      {"lock inversion", "circular wait"}},
    };
    std::vector<std::string> out;
    std::string lower = to_lower(term);
    for (auto& entry : kSyn) {
        if (lower.find(entry.first) != std::string::npos) {
            for (auto& v : entry.second) out.push_back(v);
        }
    }
    return out;
}


static std::vector<std::string> buildQueryQueue(const std::string& raw_query,
                                                 const std::string& context_focus) {
    std::vector<std::string> queue;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        for (auto& e : queue) if (e == s) return;
        queue.push_back(s);
    };

    // 0. 上下文 focus 关键词注入 (如果有)
    std::vector<std::string> focus_tokens;
    if (!context_focus.empty()) {
        focus_tokens = rule_tokenize(context_focus);
    }

    // 1. 完整原术语 (先加)
    for (auto& v : jieba_normalize(raw_query)) add(v);

    // 2. 分词: 优先 cppjieba 真结巴, 失败回退规则分词
    std::vector<std::string> tokens;
#if RESEARCH_MCP_HAS_CPPJIEBA
    auto jieba_tokens = tryJiebaSegment(raw_query);
    // Jieba 结果 ≥ 规则分词结果更细 → 用 Jieba
    if (!jieba_tokens.empty() && jieba_tokens.size() >= 2) {
        tokens = jieba_tokens;
    } else {
        tokens = rule_tokenize(raw_query);
    }
#else
    tokens = rule_tokenize(raw_query);
#endif
    std::vector<std::string> core_tokens;
    for (auto& t : tokens) {
        auto stripped = stripSuffix(t);
        if (stripped.size() >= 2) core_tokens.push_back(stripped);
    }
    // 去重
    {
        std::set<std::string> seen;
        std::vector<std::string> dedup;
        for (auto& t : core_tokens) {
            if (seen.insert(to_lower(t)).second) dedup.push_back(t);
        }
        core_tokens = dedup;
    }

    // 3. 2-3 核心词组合
    if (core_tokens.size() >= 2 && core_tokens.size() <= 4) {
        // 两两组合
        for (size_t i = 0; i < core_tokens.size(); ++i) {
            for (size_t j = i + 1; j < core_tokens.size(); ++j) {
                std::string combo = core_tokens[i] + " " + core_tokens[j];
                for (auto& v : jieba_normalize(combo)) add(v);
            }
        }
    }
    // focus + 核心词组合
    for (auto& ft : focus_tokens) {
        for (auto& ct : core_tokens) {
            std::string combo = ft + " " + ct;
            for (auto& v : jieba_normalize(combo)) add(v);
        }
    }

    // 4. 单个核心词 (最后, 最宽泛)
    for (auto& t : core_tokens) {
        for (auto& v : jieba_normalize(t)) add(v);
    }

    // 5. 编辑距离容错 + 同义词变体 (宁准勿滥, 追加到末尾最低优先级)
    std::set<std::string> queue_set(queue.begin(), queue.end());
    for (auto& q : queue) {
        // 5a. Levenshtein 1-substitution 候选 (纯 ASCII, len>=5)
        if (isPureAscii(q) && maxEditDistance(q) >= 1) {
            auto fuzzy = generate1Substitution(q);
            for (auto& f : fuzzy) {
                if (f.size() == q.size() && queue_set.insert(f).second) {
                    queue.push_back(f);
                }
            }
        }
        // 5b. 同义词变体 (高频技术词硬编码表命中才加)
        auto syns = lookupSynonyms(q);
        for (auto& s : syns) {
            if (queue_set.insert(s).second) queue.push_back(s);
        }
        // 5c. 同义词再走一次 jieba_normalize (大小写/符号归一)
        for (auto& s : syns) {
            for (auto& v : jieba_normalize(s)) {
                if (queue_set.insert(v).second) queue.push_back(v);
            }
        }
    }

    return queue;
}

} // namespace query_preprocessor

// =============================================================
// WikiMiningPipeline 实现
// =============================================================
WikiMiningPipeline::WikiMiningPipeline(DataSourceRegistry& registry, CacheManager& cache)
    : registry_(registry), cache_(cache) {}

// =============================================================
// 主入口 (嵌入分词 + 结巴容错预处理 + 路径调度 + 自优化闭环)
// =============================================================
json WikiMiningPipeline::mine(const std::string& query,
                              const std::string& context_focus,
                              const MiningConfig& config) {
    json result;
    result["query"] = query;
    result["context_focus"] = context_focus;
    result["mined_resources"] = json::array();
    result["paths_executed"] = json::array();
    result["confidence_scores"] = json::array();
    result["query_queue"] = json::array();
    result["_source"] = "wiki_mining_pipeline";

    if (!shouldMine(query)) {
        result["skipped_reason"] = "query_filtered_or_failed_before";
        return result;
    }
    if (isInFailCache(query)) {
        result["skipped_reason"] = "cached_failure";
        return result;
    }

    // —— 入口预处理: Jieba 分词 + 结巴容错 + 编辑距离 + 同义词变体 ——
    auto query_queue = query_preprocessor::buildQueryQueue(query, context_focus);
    for (auto& q : query_queue) result["query_queue"].push_back(q);

    std::vector<MinedEntity> all_entities;
    bool high_conf_hit = false;

    // 对队列中每个查询依次尝试挖掘路径 (高优先级先试)
    for (auto& q : query_queue) {
        if (high_conf_hit) break;

        // 路径 1: 分类树向下遍历
        if (config.enable_category_tree) {
            auto hits = pathCategoryTree(q, config);
            for (auto& h : hits) result["paths_executed"].push_back("category_tree:" + q);
            for (auto& h : hits) if (h.confidence >= 0.9) high_conf_hit = true;
            all_entities.insert(all_entities.end(), hits.begin(), hits.end());
        }
        if (high_conf_hit) break;

        // 路径 2: 别名/重定向挖掘
        if (config.enable_alias_redirect) {
            auto hits = pathAliasRedirect(q, config);
            for (auto& h : hits) result["paths_executed"].push_back("alias_redirect:" + q);
            for (auto& h : hits) if (h.confidence >= 0.9) high_conf_hit = true;
            all_entities.insert(all_entities.end(), hits.begin(), hits.end());
        }
        if (high_conf_hit) break;

        // 路径 3: 全文上下文定位 (宽泛搜索)
        if (config.enable_fulltext_scan) {
            auto hits = pathFulltextScan(q, config);
            for (auto& h : hits) result["paths_executed"].push_back("fulltext_scan:" + q);
            for (auto& h : hits) if (h.confidence >= 0.9) high_conf_hit = true;
            all_entities.insert(all_entities.end(), hits.begin(), hits.end());
        }

        // 路径 4: 跨源反向映射 (最慢, 最后)
        if (!high_conf_hit && config.enable_cross_source_map) {
            auto hits = pathCrossSourceMap(q, config);
            for (auto& h : hits) result["paths_executed"].push_back("cross_source:" + q);
            for (auto& h : hits) if (h.confidence >= 0.9) high_conf_hit = true;
            all_entities.insert(all_entities.end(), hits.begin(), hits.end());
        }
    }

    if (all_entities.empty()) {
        recordFail(query, "no_hit_in_any_path");
        result["skipped_reason"] = "all_paths_failed";
        return result;
    }

    // 去重 (canonical_uri)
    std::set<std::string> seen_uri;
    std::vector<MinedEntity> deduped;
    for (auto& e : all_entities) {
        if (!e.canonical_uri.empty() && seen_uri.insert(e.canonical_uri).second) {
            deduped.push_back(std::move(e));
        } else if (e.canonical_uri.empty()) {
            deduped.push_back(std::move(e));
        }
    }

    // —— 自优化闭环 ——
    // 1) 候选实体: 每被新 mine 命中一次就累加 hit_count, ≥3 自动升正式
    // 2) 正式实体: 如果本轮 mine 找到同源更准的 Wiki 条目 (confidence + canonical_uri 更优) → 替换
    for (auto& e : deduped) {
        // 读候选缓存看 hit_count
        std::string cache_key = e.canonical_uri.empty() ? e.canonical_name : e.canonical_uri;
        auto cached = cache_.get(MINED_CACHE_TYPE, cache_key);
        int hit_count = 1;
        if (cached.has_value()) {
            try {
                json cj = json::parse(cached->payload);
                if (cj.contains("hit_count")) hit_count = cj["hit_count"].get<int>() + 1;
                // 同源更准条目: 已有 is_formal 但旧置信度 < 新 → 升级
                if (cj.value("is_formal", false) && cj.value("confidence", 0.0) < e.confidence) {
                    e.is_formal = true;
                }
            } catch (...) {}
        }
        if (!e.is_formal && hit_count >= 3) {
            e.is_formal = true;  // 自动升级!
            e.confidence = std::max(e.confidence, 0.85);
            result["auto_promoted"].push_back(e.canonical_uri);
        }
        // 把 hit_count 塞到 metadata 里留给 persist
    }

    // 落库
    persist(deduped);

    // 输出为 resources 数组
    for (auto& e : deduped) {
        result["mined_resources"].push_back(toResourceJson(e));
        result["confidence_scores"].push_back({{
            {"canonical_uri", e.canonical_uri},
            {"confidence", e.confidence},
            {"source_path", e.source_path}
        }});
    }

    return result;
}

// =============================================================
// 触发判断
// =============================================================
bool WikiMiningPipeline::shouldMine(const std::string& query) {
    if (query.size() < 3) return false;
    static const char* stop[] = {
        "the","and","for","with","from","that","this","what","how","why","when",
        "where","is","are","a","an","of","to","in","on","at","by","help","test",
        "demo","example","sample"
    };
    std::string lower = to_lower(query);
    for (auto* s : stop) if (lower == s) return false;
    static const char* ban[] = {
        "TODO","FIXME","xxx","tbd","placeholder","undefined","null","nil","none"
    };
    for (auto* b : ban) if (lower == b) return false;
    return true;
}

bool WikiMiningPipeline::isInFailCache(const std::string& query) {
    return cache_.get(FAIL_CACHE_TYPE, query).has_value();
}

void WikiMiningPipeline::recordFail(const std::string& query, const std::string& reason) {
    json payload = {{"query", query}, {"reason", reason}};
    cache_.put(FAIL_CACHE_TYPE, query, payload.dump(), "json", 24, "", "fail", "");
}

// =============================================================
// 路径 1: 分类树向下遍历
// =============================================================
std::vector<MinedEntity> WikiMiningPipeline::pathCategoryTree(
    const std::string& query, const MiningConfig& config) {
    std::vector<MinedEntity> results;
    auto categories = guessParentCategories(query);
    if (categories.empty()) return results;

    auto* kiwix = registry_.get("kiwix_local");
    if (!kiwix || !kiwix->healthCheck()) return results;

    std::string lower_query = to_lower(query);

    for (auto& cat_keyword : categories) {
        if ((int)results.size() >= config.max_hits_per_path) break;

        SearchQuery sq;
        sq.query = cat_keyword;
        sq.max_results = 5;
        auto cat_results = kiwix->search(sq);

        for (auto& cat_sr : cat_results) {
            if ((int)results.size() >= config.max_hits_per_path) break;

            std::string lower_title = to_lower(cat_sr.title);
            bool looks_like_category =
                lower_title.find("category") != std::string::npos ||
                lower_title.find("分类") != std::string::npos ||
                lower_title.find("index") != std::string::npos ||
                cat_sr.title.back() == '/';
            if (!looks_like_category && cat_results.size() > 2) continue;

            auto children = kiwix->expand(cat_sr.canonical_uri, "",
                                           config.category_scan_depth);
            for (auto& child_uri : children) {
                if ((int)results.size() >= config.max_hits_per_path) break;
                std::string child_lower = to_lower(child_uri);
                if (child_lower.find(lower_query) != std::string::npos) {
                    results.push_back(calibrate(query, child_uri, child_uri,
                        std::string(), "category_tree", 0.85));
                }
            }
        }
    }
    return results;
}

// =============================================================
// 路径 2: 别名与重定向挖掘
// =============================================================
std::vector<MinedEntity> WikiMiningPipeline::pathAliasRedirect(
    const std::string& query, const MiningConfig& config) {
    std::vector<MinedEntity> results;
    auto* kiwix = registry_.get("kiwix_local");
    std::string lower_query = to_lower(query);

    auto variants = generateVariants(query, config.alias_variant_limit);

    if (!kiwix || !kiwix->healthCheck()) {
        DataSourceRegistry::MultiSearchOptions opts{config.max_hits_per_path, false};
        for (auto& variant : variants) {
            if ((int)results.size() >= config.max_hits_per_path) break;
            SearchQuery sq;
            sq.query = variant;
            sq.max_results = 5;
            auto hits = registry_.multiSearch(sq, opts);
            for (auto& hit : hits) {
                results.push_back(calibrate(query, hit.title, hit.canonical_uri,
                    hit.snippet, "alias_redirect", 0.65));
            }
        }
        return results;
    }

    for (auto& variant : variants) {
        if ((int)results.size() >= config.max_hits_per_path) break;
        if (variant == query) continue;

        SearchQuery sq;
        sq.query = variant;
        sq.max_results = 5;
        auto hits = kiwix->search(sq);

        for (auto& hit : hits) {
            if ((int)results.size() >= config.max_hits_per_path) break;
            std::string hit_lower_title = to_lower(hit.title);
            std::string hit_lower_uri = to_lower(hit.canonical_uri);

            bool variant_in_title = hit_lower_title.find(to_lower(variant)) != std::string::npos;
            bool query_in_title = hit_lower_title.find(lower_query) != std::string::npos;

            double conf = 0.5;
            if (query_in_title) conf = 0.95;
            else if (variant_in_title) conf = 0.70;
            else if (hit_lower_uri.find(lower_query) != std::string::npos) conf = 0.75;

            results.push_back(calibrate(query, hit.title, hit.canonical_uri,
                hit.snippet, "alias_redirect", conf));
        }
    }
    return results;
}

// =============================================================
// 路径 3: 全文上下文定位 (Kiwix Xapian 全文检索)
// =============================================================
std::vector<MinedEntity> WikiMiningPipeline::pathFulltextScan(
    const std::string& query, const MiningConfig& config) {
    std::vector<MinedEntity> results;
    auto* kiwix = registry_.get("kiwix_local");

    SearchQuery sq;
    sq.query = query;
    sq.max_results = config.max_hits_per_path;
    auto hits = kiwix ? kiwix->search(sq)
                      : registry_.multiSearch(sq, DataSourceRegistry::MultiSearchOptions{config.max_hits_per_path, false});

    std::string lower_query = to_lower(query);
    for (auto& hit : hits) {
        if ((int)results.size() >= config.max_hits_per_path) break;
        std::string hit_lower_title = to_lower(hit.title);
        std::string hit_lower_uri = to_lower(hit.canonical_uri);

        double conf = 0.55;
        if (hit_lower_title.find(lower_query) != std::string::npos) conf = 0.80;
        else if (hit_lower_uri.find(lower_query) != std::string::npos) conf = 0.75;
        else if (!hit.snippet.empty() && to_lower(hit.snippet).find(lower_query) != std::string::npos) conf = 0.60;

        results.push_back(calibrate(query, hit.title, hit.canonical_uri,
            hit.snippet, "fulltext_scan", conf));
    }
    return results;
}

// =============================================================
// 路径 4: 跨源反向映射 (github → hn → arxiv)
// =============================================================
std::vector<MinedEntity> WikiMiningPipeline::pathCrossSourceMap(
    const std::string& query, const MiningConfig& config) {
    std::vector<MinedEntity> results;
    if ((int)query.size() < 3) return results;

    static const char* kCrossSources[] = {"github_repo", "hn", "arxiv", nullptr};
    auto* kiwix = registry_.get("kiwix_local");

    for (auto** sp = kCrossSources; *sp; ++sp) {
        auto* src = registry_.get(*sp);
        if (!src || !src->healthCheck()) continue;

        SearchQuery sq;
        sq.query = query;
        sq.max_results = 3;
        auto hits = src->search(sq);

        for (auto& hit : hits) {
            if ((int)results.size() >= config.max_hits_per_path) break;
            auto related_cats = guessParentCategories(hit.title + " " + hit.snippet);
            if (related_cats.empty()) continue;

            for (auto& cat : related_cats) {
                if ((int)results.size() >= config.max_hits_per_path) break;
                if (kiwix && kiwix->healthCheck()) {
                    SearchQuery sq_cat;
                    sq_cat.query = cat;
                    sq_cat.max_results = 2;
                    auto cat_hits = kiwix->search(sq_cat);
                    for (auto& ch : cat_hits) {
                        results.push_back(calibrate(query, ch.title, ch.canonical_uri,
                            hit.snippet, "cross_source_map", 0.55));
                    }
                }
            }
        }
        if (!results.empty()) break;
    }
    return results;
}

// =============================================================
// 变体生成
// =============================================================
std::vector<std::string> WikiMiningPipeline::generateVariants(
    const std::string& query, int limit) {
    std::vector<std::string> v;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        for (auto& e : v) if (e == s) return;
        v.push_back(s);
    };
    add(query);
    if ((int)v.size() >= limit) return v;
    add(to_lower(query));
    if ((int)v.size() >= limit) return v;

    std::string upper;
    for (char c : query) upper += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    add(upper);
    if ((int)v.size() >= limit) return v;

    std::string cleaned;
    for (char c : query) {
        if (c == ' ' || c == '_' || c == '-') cleaned += ' ';
        else cleaned += c;
    }
    std::string no_space, with_underscore;
    for (char c : cleaned) {
        if (c != ' ') { no_space += c; with_underscore += c; }
        else with_underscore += '_';
    }
    add(no_space);
    if ((int)v.size() >= limit) return v;
    add(with_underscore);
    if ((int)v.size() >= limit) return v;

    std::string acronym;
    bool next_upper = true;
    for (char c : cleaned) {
        if (c == ' ') { next_upper = true; continue; }
        if (next_upper) {
            acronym += static_cast<char>(toupper(static_cast<unsigned char>(c)));
            next_upper = false;
        }
    }
    if (acronym.size() >= 2 && (int)v.size() < limit) add(acronym);
    return v;
}

// =============================================================
// 上位分类预判 (38 条规则)
// =============================================================
std::vector<std::string> WikiMiningPipeline::guessParentCategories(
    const std::string& query) {
    std::vector<std::string> cats;
    std::string lower = to_lower(query);

    static const std::pair<const char*, const char*> rules[] = {
        {"memory","Memory management"},{"malloc","Memory management"},
        {"free","Memory management"},{"page","Memory management"},
        {"virtual","Virtual memory"},{"kernel","Linux kernel"},
        {"syscall","Linux kernel"},{"process","Process management"},
        {"thread","Process management"},{"sched","Process management"},
        {"network","Computer networking"},{"socket","Computer networking"},
        {"tcp","Computer networking"},{"ip","Computer networking"},
        {"driver","Device drivers"},{"fs","File systems"},
        {"file system","File systems"},{"inode","File systems"},
        {"compiler","Compilers"},{"parser","Compilers"},
        {"llvm","Compilers"},{"gpu","Graphics processing"},
        {"shader","Graphics processing"},{"crypto","Cryptography"},
        {"encrypt","Cryptography"},{"hash","Cryptography"},
        {"algorithm","Algorithms"},{"data structure","Data structures"},
        {"database","Database management"},{"sql","Database management"},
        {"distributed","Distributed systems"},{"consensus","Distributed systems"},
        {"microkernel","Operating systems"},{"monolithic","Operating systems"},
        {"posix","Operating systems"},{"api","Application programming"},
        {"c library","C standard library"},{"stdlib","C standard library"},
    };
    for (auto& [keyword, cat] : rules) {
        if (lower.find(keyword) != std::string::npos) {
            bool dup = false;
            for (auto& e : cats) if (e == cat) { dup = true; break; }
            if (!dup) cats.push_back(cat);
        }
    }
    return cats;
}

// =============================================================
// 结构化校准
// =============================================================
MinedEntity WikiMiningPipeline::calibrate(
    const std::string& query,
    const std::string& hit_title,
    const std::string& hit_uri,
    const std::string& hit_content,
    const std::string& source_path,
    double base_confidence) {

    MinedEntity e;
    e.canonical_name = hit_title.empty() ? query : hit_title;
    e.canonical_uri = hit_uri;
    e.source_path = source_path;
    e.confidence = base_confidence;

    if (!hit_content.empty()) {
        size_t end = std::min(hit_content.size(), (size_t)200);
        e.summary = hit_content.substr(0, end);
        if (end < hit_content.size()) e.summary += "...";
    }

    std::string lower = to_lower(hit_title + " " + hit_uri);
    if (lower.find("kernel") != std::string::npos) e.domain_tag = "kernel";
    else if (lower.find("memory") != std::string::npos) e.domain_tag = "memory";
    else if (lower.find("network") != std::string::npos || lower.find("socket") != std::string::npos) e.domain_tag = "networking";
    else if (lower.find("file") != std::string::npos) e.domain_tag = "filesystem";
    else if (lower.find("crypto") != std::string::npos || lower.find("crypt") != std::string::npos) e.domain_tag = "crypto";
    else if (lower.find("compiler") != std::string::npos) e.domain_tag = "compiler";
    else e.domain_tag = "general";

    e.aliases.push_back(query);
    if (!hit_title.empty() && hit_title != query) e.aliases.push_back(hit_title);
    e.entity_id = std::string(ENTITY_TYPE) + "::" + e.canonical_name;
    e.is_formal = (e.confidence >= 0.8);
    return e;
}

// =============================================================
// 落库
// =============================================================
void WikiMiningPipeline::persist(const std::vector<MinedEntity>& entities) {
    for (auto& e : entities) {
        json metadata = {
            {"mined_by", "wiki_mining_pipeline"},
            {"source_path", e.source_path},
            {"confidence", e.confidence},
            {"original_query", e.aliases.empty() ? "" : e.aliases[0]},
            {"canonical_uri", e.canonical_uri},
            {"domain_tag", e.domain_tag}
        };

        if (e.is_formal) {
            cache_.register_entity(ENTITY_TYPE, e.canonical_name, e.aliases,
                {e.domain_tag}, metadata, e.summary);
        }

        // 候选缓存: 读 hit_count, +1 写回
        std::string cache_key = e.canonical_uri.empty() ? e.canonical_name : e.canonical_uri;
        json cache_payload = {
            {"entity_id", e.entity_id},
            {"canonical_name", e.canonical_name},
            {"aliases", e.aliases},
            {"domain_tag", e.domain_tag},
            {"summary", e.summary},
            {"canonical_uri", e.canonical_uri},
            {"source_path", e.source_path},
            {"confidence", e.confidence},
            {"is_formal", e.is_formal},
            {"hit_count", 1}
        };
        auto existing = cache_.get(MINED_CACHE_TYPE, cache_key);
        if (existing.has_value()) {
            try {
                cache_payload = json::parse(existing->payload);
                cache_payload["hit_count"] = cache_payload.value("hit_count", 0) + 1;
                cache_payload["confidence"] = std::max(cache_payload.value("confidence", 0.0), e.confidence);
                if (e.is_formal) cache_payload["is_formal"] = true;
            } catch (...) {}
        }

        int ttl = e.is_formal ? 720 : 168;  // 30d / 7d
        cache_.put(MINED_CACHE_TYPE, cache_key, cache_payload.dump(),
            "json", ttl, "", "wiki_mined", "");
    }
}

json WikiMiningPipeline::toResourceJson(const MinedEntity& e) {
    return {
        {"canonical_uri", e.canonical_uri},
        {"title", e.canonical_name},
        {"resource_kind", "wiki_mined_entity"},
        {"source_id", "wiki_mining_pipeline"},
        {"is_local", true},
        {"snippet", e.summary},
        {"aliases", e.aliases},
        {"domain_tag", e.domain_tag},
        {"entity_id", e.entity_id},
        {"confidence", e.confidence},
        {"is_formal", e.is_formal},
        {"mined_source_path", e.source_path},
        {"_source", "wiki_mining_pipeline"}
    };
}

} // namespace github_research
