#include "github_research/query_preprocessor.hpp"
#include "github_research/string_utils.hpp"

// cppjieba — 可选依赖, 通过编译宏 RESEARCH_MCP_HAS_CPPJIEBA 开关
#if __has_include("cppjieba/Jieba.hpp")
  #include "cppjieba/Jieba.hpp"
  #ifndef RESEARCH_MCP_HAS_CPPJIEBA
    #define RESEARCH_MCP_HAS_CPPJIEBA 1
  #endif
#else
  #ifndef RESEARCH_MCP_HAS_CPPJIEBA
    #define RESEARCH_MCP_HAS_CPPJIEBA 0
  #endif
#endif

#include <algorithm>
#include <cctype>
#include <set>

namespace github_research {

// =============================================================
// Jieba lazy singleton (词典路径编译时注入)
// =============================================================
#if RESEARCH_MCP_HAS_CPPJIEBA && defined(CPPJIEBA_DICT_PATH)
namespace {
cppjieba::Jieba& jiebaInstance() {
    static cppjieba::Jieba jb(
        CPPJIEBA_DICT_PATH "/jieba.dict.utf8",
        CPPJIEBA_DICT_PATH "/hmm_model.utf8",
        CPPJIEBA_DICT_PATH "/user.dict.utf8",
        CPPJIEBA_DICT_PATH "/idf.utf8",
        CPPJIEBA_DICT_PATH "/stop_words.utf8"
    );
    return jb;
}
} // anon
static std::vector<std::string> tryJiebaSegment(const std::string& input) {
    std::vector<std::string> words;
    try { jiebaInstance().Cut(input, words, true); } catch (...) { words.clear(); }
    return words;
}
#endif

// =============================================================
// 规则分词: 空格/连字符/下划线 + 中英文边界分离
// =============================================================
std::vector<std::string> rule_tokenize(const std::string& input) {
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

// =============================================================
// 结巴容错: 大小写 + 符号归一, 生成一组形态变体
// =============================================================
std::vector<std::string> jieba_normalize(const std::string& term) {
    std::vector<std::string> result;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        for (auto& e : result) if (e == s) return;
        result.push_back(s);
    };

    add(term);
    std::string lower = to_lower(term);
    add(lower);

    // 符号归一: 所有非字母数字 + 非中文 → 空格
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

// =============================================================
// 编辑距离容错 (Levenshtein, 纯 ASCII)
// 风险控制: maxEditDistance 永远 ≤ 1 字符 (用户硬约束)
//   len < 5  → 0 (不容忍)
//   5 ≤ len  → 1 (最多 1 个字符错)
// 原来 len > 8 允许 2 的逻辑已收窄
// =============================================================
int levenshteinDistance(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if (a.empty()) return (int)b.size();
    if (b.empty()) return (int)a.size();
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

int maxEditDistance(const std::string& term) {
    int len = (int)term.size();
    if (len < 5) return 0;
    return 1;  // 永远 ≤ 1, 风险控制硬约束
}

static std::vector<std::string> generate1Substitution(const std::string& term) {
    std::vector<std::string> out;
    out.reserve(term.size() * 26);
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

// =============================================================
// 高频技术同义词表 (4 核心域)
// key 小写 → value 候选串
// =============================================================
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

// =============================================================
// 分级查询组合生成 — 唯一对外入口
//
// 输出按优先级排列 (高→低):
//   "original"     完整原术语 (jieba_normalize 形态)
//   "token_combo"  2-3 核心词组合 + jieba_normalize
//   "core_token"   单个核心词 (最宽泛)
//   "fuzzy_lev1"   Levenshtein 1-substitution 候选 (风险最高)
//   "synonym"      同义词变体 (技术表硬命中)
//
// 注意: 本函数只负责生成, 不负责调用外部 API.
//       调用方 (arxiv_search / s2_search / wiki_mining) 负责:
//         1. 按队列顺序依次调用
//         2. 对 fuzzy/synonym 来源的变体命中 → confidence - 0.1 + _source.match_type = "fuzzy"
//         3. 总开销预算 (远程 API 建议额外查询 ≤ 3 次)
// =============================================================
std::vector<VariantSource> buildQueryQueue(const std::string& raw_query,
                                            const std::string& context_focus) {
    std::vector<VariantSource> queue;
    std::set<std::string> seen;

    auto add = [&](const std::string& s, const std::string& tag) {
        if (s.empty()) return;
        if (!seen.insert(s).second) return;
        queue.push_back({s, tag});
    };

    // —— 上下文 focus 分词 (如果有) ——
    std::vector<std::string> focus_tokens;
    if (!context_focus.empty()) {
        focus_tokens = rule_tokenize(context_focus);
    }

    // —— 1. 完整原术语 (最高优先级) ——
    for (auto& v : jieba_normalize(raw_query)) add(v, "original");

    // —— 2. 分词 ——
    std::vector<std::string> tokens;
#if RESEARCH_MCP_HAS_CPPJIEBA && defined(CPPJIEBA_DICT_PATH)
    auto jieba_tokens = tryJiebaSegment(raw_query);
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
    {
        std::set<std::string> dedup_seen;
        std::vector<std::string> dedup;
        for (auto& t : core_tokens) {
            if (dedup_seen.insert(to_lower(t)).second) dedup.push_back(t);
        }
        core_tokens = dedup;
    }

    // —— 3. 2-3 核心词组合 ——
    if (core_tokens.size() >= 2 && core_tokens.size() <= 4) {
        for (size_t i = 0; i < core_tokens.size(); ++i) {
            for (size_t j = i + 1; j < core_tokens.size(); ++j) {
                std::string combo = core_tokens[i] + " " + core_tokens[j];
                for (auto& v : jieba_normalize(combo)) add(v, "token_combo");
            }
        }
    }
    // focus + 核心词交叉
    for (auto& ft : focus_tokens) {
        for (auto& ct : core_tokens) {
            std::string combo = ft + " " + ct;
            for (auto& v : jieba_normalize(combo)) add(v, "token_combo");
        }
    }

    // —— 4. 单个核心词 ——
    for (auto& t : core_tokens) {
        for (auto& v : jieba_normalize(t)) add(v, "core_token");
    }

    // —— 5. 编辑距离容错 + 同义词 (最低优先级, fuzzy 来源) ——
    std::set<std::string> queue_set;
    for (auto& qs : queue) queue_set.insert(qs.variant);

    for (auto& qs : queue) {
        const std::string& q = qs.variant;

        // 5a. Levenshtein 1-substitution (纯 ASCII, len>=5)
        if (isPureAscii(q) && maxEditDistance(q) >= 1) {
            auto fuzzy = generate1Substitution(q);
            for (auto& f : fuzzy) {
                if (f.size() == q.size() && queue_set.insert(f).second) {
                    queue.push_back({f, "fuzzy_lev1"});
                }
            }
        }

        // 5b. 同义词变体
        auto syns = lookupSynonyms(q);
        for (auto& s : syns) {
            if (queue_set.insert(s).second) queue.push_back({s, "synonym"});
        }
        for (auto& s : syns) {
            for (auto& v : jieba_normalize(s)) {
                if (queue_set.insert(v).second) queue.push_back({v, "synonym"});
            }
        }
    }

    return queue;
}

// =============================================================
// fuzzy 来源判定 —— 调用方用于风险控制 (降置信度 + 溯源)
// =============================================================
bool isFuzzySource(const std::string& source_tag) {
    return source_tag == "fuzzy_lev1" || source_tag == "synonym";
}

} // namespace github_research
