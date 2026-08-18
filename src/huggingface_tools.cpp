#include "github_research/huggingface_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include <iostream>
#include <string>
#include <regex>
#include <cctype>
#include <sstream>
#include <vector>

namespace github_research {

namespace {

// HF 工具统一日志前缀
constexpr const char* kLogPrefix = "[hf]";

// 默认提取数量上限(保留用于参数校验)
constexpr int kDefaultCount = 10;
constexpr int kMaxCount = 50;

// --------- 小工具 ---------
// 字符串前后空白修剪
static std::string trim_str(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return s.substr(a, b - a);
}

// 解析短数字: "10.8k" -> 10800, "2.73M" -> 2730000, "415k" -> 415000, "30B" -> 30000000000
// 额外支持千分位逗号形式: "415,039" -> 415039
static bool parse_short_number(const std::string& raw, double& out) {
    static const std::string kDotSepUtf8 = "\xe2\x80\xa2"; // "•"
    std::string s = trim_str(raw);
    if (s.empty()) return false;
    // 去掉所有 ',' (千分位分隔符)
    {
        std::string tmp;
        tmp.reserve(s.size());
        for (char c : s) if (c != ',') tmp.push_back(c);
        s = std::move(tmp);
    }
    // 去掉结尾可能的 "•"(UTF-8 3 字节) / "|" / 空白
    while (!s.empty()) {
        char c = s.back();
        if (c == '|' || std::isspace(static_cast<unsigned char>(c))) {
            s.pop_back();
            continue;
        }
        if (s.size() >= kDotSepUtf8.size() &&
            s.compare(s.size() - kDotSepUtf8.size(), kDotSepUtf8.size(), kDotSepUtf8) == 0) {
            s.resize(s.size() - kDotSepUtf8.size());
            continue;
        }
        break;
    }
    if (s.empty()) return false;
    double mul = 1.0;
    char last = s.back();
    if (last == 'k' || last == 'K') { mul = 1e3; s.pop_back(); }
    else if (last == 'M' || last == 'm') { mul = 1e6; s.pop_back(); }
    else if (last == 'B' || last == 'b') { mul = 1e9; s.pop_back(); }
    else if (last == 'T' || last == 't') { mul = 1e12; s.pop_back(); }
    try {
        size_t pos = 0;
        double v = std::stod(s, &pos);
        // 允许后面还有空格
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) return false;
        out = v * mul;
        return true;
    } catch (...) { return false; }
}
static inline double parse_short_number_or(const std::string& raw, double fallback) {
    double v;
    return parse_short_number(raw, v) ? v : fallback;
}

// 行是否看起来像 "org/repo" 开头的模型行(用来作为扫描入口锚点)
static bool starts_with_model_id(const std::string& line, size_t& slash_pos) {
    // 在 line 中搜索 "A/B" 模式:前面和 '/' 都是 token 字符
    std::regex pat(R"((?:^|\s)([A-Za-z0-9][A-Za-z0-9._-]*\/[A-Za-z0-9][A-Za-z0-9._-]*))");
    std::smatch m;
    if (std::regex_search(line, m, pat)) {
        // 只接受第一个 match 在开头附近
        // 返回 true,slash_pos 没意义
        slash_pos = static_cast<size_t>(m.position(1));
        return true;
    }
    return false;
}

// --------- 规范化 1: HuggingFace 列表(Trending/Search Models)页面 innerText -> models[] ---------
// HF 列表页 innerText 格式示例(每行一条模型):
//   Qwen/Qwen3.8-27B
//   Image-Text-to-Text
//   •
//   28B
//   •
//   Updated 4 days ago
//   •
//   415k
//   •
//   •
//   10.8k
// (每个模型的字段之间用 "•" 分隔)
// 策略: 逐行扫描,遇到 model_id(org/repo) 开头的行开始一条新记录,然后把后面的字段(直到下一条 model_id 之前)逐段填入
static json NormalizeHfModelList(const json& raw, const std::string& pageUrl, int limitCount) {
    json payload;
    payload["success"] = true;
    payload["url"] = raw.value("url", pageUrl);
    payload["title"] = raw.value("title", "");
    payload["page_source"] = "hf_model_list";

    if (!raw.is_object() || !raw.contains("text") || !raw["text"].is_string()) {
        payload["success"] = false;
        payload["models"] = json::array();
        payload["raw_text"] = raw.value("text", "");
        return payload;
    }

    std::string pageText = raw["text"].get<std::string>();
    payload["raw_text"] = pageText;

    // 按换行切分
    std::vector<std::string> lines;
    {
        size_t i = 0;
        while (i < pageText.size()) {
            size_t j = i;
            while (j < pageText.size() && pageText[j] != '\n') ++j;
            lines.push_back(pageText.substr(i, j - i));
            i = j + 1;
        }
    }

    // 预扫描:把 "•" 作为单独字段分隔符,并且把空行剔除
    static const std::string kDotSep = "\xe2\x80\xa2"; // UTF-8: "•"
    std::vector<std::string> tokens;
    for (auto& l : lines) {
        std::string t = trim_str(l);
        if (t.empty()) continue;
        // 把包含多个 "•" 的行按 "•" 拆成多段
        size_t start = 0;
        while (start <= t.size()) {
            size_t pos = t.find(kDotSep, start);
            if (pos == std::string::npos) {
                std::string sub = trim_str(t.substr(start));
                if (!sub.empty()) tokens.push_back(sub);
                break;
            } else {
                std::string sub = trim_str(t.substr(start, pos - start));
                if (!sub.empty()) tokens.push_back(sub);
                tokens.push_back("•");  // 保留分隔符作为占位
                start = pos + kDotSep.size();
            }
        }
    }

    json models = json::array();
    int n = static_cast<int>(tokens.size());
    // model_id 正则: org/repo (两个路径段,都不含空格)
    std::regex model_id_pat(R"(^[A-Za-z0-9][A-Za-z0-9._-]*\/[A-Za-z0-9][A-Za-z0-9._-]*$)");
    int i = 0;
    while (i < n) {
        if (std::regex_match(tokens[i], model_id_pat)) {
            // 开始一条模型
            json cur;
            cur["model_id"] = tokens[i];
            ++i;
            // 跳过 0 个或多个 "•"
            while (i < n && tokens[i] == "•") ++i;
            // 下一个字段可能是 pipeline_tag(多词空格分隔的短语)或 "<digits>B"（参数量，此时 pipeline_tag 缺失）
            std::string field1;
            while (i < n && tokens[i] != "•") {
                if (!field1.empty()) field1 += " ";
                field1 += tokens[i];
                ++i;
            }
            // 判断 field1 是不是参数量(如 "28B"、"2.4T")而不是 pipeline_tag
            bool is_params = false;
            double params_val = 0.0;
            if (!field1.empty()) {
                // 如果末尾是 B/T/M/K 且前面是数字,当作 params
                std::regex param_pat(R"(^\d+(\.\d+)?[kKmMbBtT]$)");
                if (std::regex_match(field1, param_pat)) {
                    is_params = parse_short_number(field1, params_val);
                }
            }
            if (!is_params && !field1.empty()) {
                cur["pipeline_tag"] = field1;
            } else if (is_params) {
                cur["params"] = params_val;
                cur["params_display"] = field1;
            }
            while (i < n && tokens[i] == "•") ++i;
            // 下一个:params(如果上面是 pipeline_tag) / Updated 文本(如果上面已经是 params 了)
            std::string field2;
            while (i < n && tokens[i] != "•") {
                if (!field2.empty()) field2 += " ";
                field2 += tokens[i];
                ++i;
            }
            if (!field2.empty() && !cur.contains("params")) {
                // 判断是否参数量
                std::regex param_pat(R"(^\d+(\.\d+)?[kKmMbBtT]$)");
                double vv = 0.0;
                if (std::regex_match(field2, param_pat) && parse_short_number(field2, vv)) {
                    cur["params"] = vv;
                    cur["params_display"] = field2;
                } else {
                    // 可能是 Updated 文本(极少数情况下 pipeline_tag 缺失)
                    if (field2.find("Updated") != std::string::npos || field2.find("about") != std::string::npos) {
                        cur["updated"] = field2;
                    } else {
                        cur["_extra"] = field2;
                    }
                }
            } else if (!field2.empty()) {
                // 已经有 params 了,这里应该是 Updated
                cur["updated"] = field2;
            }
            // 继续按 "•" 顺序填入: updated / downloads / likes (中间可能有空格,即 "• (空白) • likes")
            std::vector<std::string> ordered;
            while (i < n) {
                // 跳到下一个非 "•"
                while (i < n && tokens[i] == "•") ++i;
                if (i >= n) break;
                // 看下一条 token
                std::string next_tok = tokens[i];
                // 如果 next_tok 本身是下一条 model_id 就 break
                if (std::regex_match(next_tok, model_id_pat)) break;
                // 否则连续收集到下一个 "•" 之间;
                // 注意:收集过程中一旦遇到下一个 model_id(即使中间还没遇到 "•"),也要立即停止,
                // 因为这是相邻模型行的边界(它们之间可能没有 • 分隔符)
                std::string chunk;
                bool hit_next_model = false;
                while (i < n && tokens[i] != "•") {
                    if (std::regex_match(tokens[i], model_id_pat)) {
                        hit_next_model = true;
                        break;
                    }
                    if (!chunk.empty()) chunk += " ";
                    chunk += tokens[i];
                    ++i;
                }
                if (!chunk.empty()) ordered.push_back(chunk);
                if (hit_next_model) break;
            }
            // ordered 的典型内容依次是:
            //   [0] Updated 4 days ago
            //   [1] 415k           (downloads)
            //   [2] 10.8k          (likes)
            // 但 downloads / likes 可能因为 UI 上有个 logo 没有文字而产生空位置(即 tokens 里的 "• •" 连续分隔符
            // 已经在上面循环里跳过了)
            for (auto& chunk : ordered) {
                if (chunk.empty()) continue;
                if (chunk.find("Updated") != std::string::npos || chunk.find("about") != std::string::npos || chunk.find("hours ago") != std::string::npos || chunk.find("days ago") != std::string::npos) {
                    cur["updated"] = chunk;
                } else {
                    // 解析为数字; downloads 和 likes 按先后顺序填(第一个是 downloads)
                    double num = 0.0;
                    bool ok = parse_short_number(chunk, num);
                    if (!cur.contains("downloads")) {
                        if (ok) cur["downloads"] = num;
                        cur["downloads_display"] = chunk;
                    } else if (!cur.contains("likes")) {
                        if (ok) cur["likes"] = num;
                        cur["likes_display"] = chunk;
                    } else if (!cur.contains("extra_metric")) {
                        if (ok) cur["extra_metric"] = num;
                        cur["extra_metric_display"] = chunk;
                    }
                }
            }

            models.push_back(cur);
            if (static_cast<int>(models.size()) >= limitCount) break;
        } else {
            ++i;
        }
    }

    payload["models"] = models;
    payload["count"] = static_cast<int>(models.size());
    return payload;
}

// --------- 规范化 2: HuggingFace 详情页 innerText -> model_info ---------
// 2026-08 HuggingFace 详情页 innerText 的典型布局（含导航噪声）:
//   L30~L32: 面包屑（Qwen / Qwen3.8-27B）
//   L33~L34: "like"  ↓  "10.8k" (标签/下一行就是数值)
//   L44~L45: "License:" ↓ "apache-2.0"  (带冒号!)
//   L783:    "Downloads last month"
//   L785~L790: (下载量在 L783 之后的非空行)
//   L787:    "28B params"（独立一行: "<数量> params"）
//   L838:    "Updated 5 days ago" (或 "Published · 3 days ago" / "Published about 14 hours ago")
//   L241:    "Number of Parameters: 27B" (README 中可能出现一行描述)
static json NormalizeHfModelInfo(const json& raw, const std::string& modelId, const std::string& pageUrl) {
    json payload;
    payload["success"] = true;
    payload["model_id"] = modelId;
    payload["url"] = raw.value("url", pageUrl);
    payload["title"] = raw.value("title", "");
    payload["page_source"] = "hf_model_detail";

    if (!raw.is_object() || !raw.contains("text") || !raw["text"].is_string()) {
        payload["success"] = false;
        payload["raw_text"] = raw.value("text", "");
        return payload;
    }
    std::string pageText = raw["text"].get<std::string>();
    payload["raw_text"] = pageText;

    std::istringstream iss(pageText);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) lines.push_back(trim_str(line));
    const int N = static_cast<int>(lines.size());

    auto next_nonempty = [&](int idx) -> int {
        while (idx < N && lines[idx].empty()) ++idx;
        return idx;
    };

    std::regex param_pat(R"(^\d+(\.\d+)?[kKmMbBtT]$)");
    std::smatch sm;

    for (int i = 0; i < N; ++i) {
        const std::string& L = lines[i];
        if (L.empty()) continue;

        // 1) Likes: 上一行是 "like" -> 下一行就是 likes 数值
        if (L == "like") {
            int j = next_nonempty(i + 1);
            if (j < N) {
                double v;
                if (parse_short_number(lines[j], v)) payload["likes"] = v;
                payload["likes_display"] = lines[j];
            }
            continue;
        }

        // 2) License: 本行是 "License:" -> 下一行是 license 名
        if (L == "License:") {
            int j = next_nonempty(i + 1);
            if (j < N && !lines[j].empty()) {
                payload["license"] = lines[j];
            }
            continue;
        }

        // 3) Downloads: 本行以 "Downloads" 开头("Downloads last month" 或 "Downloads")
        if (L.size() >= 9 && L.compare(0, 9, "Downloads") == 0) {
            // 往后最多看 5 行找第一个数字形式 (可能带千分位逗号,或有 k/M/B/T 后缀,或纯整数)
            for (int j = i + 1; j < N && j <= i + 5; ++j) {
                const std::string& J = lines[j];
                if (J.empty()) continue;
                double v;
                if (parse_short_number(J, v)) {
                    // 若本行同时匹配 "<数字> params" 形式,那是 params 行而不是 downloads,跳过
                    std::regex param_line_pat(R"(^\d+(\.\d+)?[kKmMbBtT]?\s*params$)");
                    if (std::regex_match(J, sm, param_line_pat)) continue;
                    payload["downloads_last_month"] = v;
                    payload["downloads_last_month_display"] = J;
                    break;
                }
            }
            continue;
        }

        // 4) Params 模式 A: "<数值> params" (例如 "28B params")
        {
            std::regex params_line_a_pat(R"(^(\d+(?:\.\d+)?[kKmMbBtT])\s*params$)");
            if (std::regex_match(L, sm, params_line_a_pat)) {
                std::string num = sm[1].str();
                double v;
                if (parse_short_number(num, v)) payload["params"] = v;
                payload["params_display"] = num;
                continue;
            }
        }
        // 5) Params 模式 B: "Number of Parameters: <数值>"（可选）
        if (L.find("Number of Parameters") != std::string::npos || L.find("parameters:") != std::string::npos) {
            // 提取 ":" 后面的内容
            size_t c = L.find(':');
            if (c != std::string::npos) {
                std::string right = trim_str(L.substr(c + 1));
                // right 可能是 "27B · ..." -> 取首 token
                size_t sp = right.find(' ');
                std::string first = (sp == std::string::npos) ? right : right.substr(0, sp);
                double v;
                if (parse_short_number(first, v)) {
                    if (!payload.contains("params")) payload["params"] = v;
                    if (!payload.contains("params_display")) payload["params_display"] = first;
                }
            }
            continue;
        }

        // 6) Published / Updated 行（长度 < 80 避免命中 README 段落词）
        if (L.size() < 80 &&
            (L.find("Updated") != std::string::npos || L.find("Published") != std::string::npos)) {
            if (!payload.contains("published_at")) {
                payload["published_at"] = L;
            } else {
                payload["last_modified_at"] = L;
            }
            continue;
        }
    }

    return payload;
}

// --------- 规范化 3: Datasets / Spaces 列表页 (简单复用 ModelList 的行级策略,字段叫 dataset_id / space_id) ---------
static json NormalizeHfGenericList(const json& raw, const std::string& pageUrl,
                                   const std::string& list_key, const std::string& id_key, int limitCount) {
    // 先借用模型列表的解析,把 model_id 替换成 id_key
    json tmp = NormalizeHfModelList(raw, pageUrl, limitCount);
    json out;
    out["success"] = tmp.value("success", true);
    out["url"] = tmp.value("url", pageUrl);
    out["title"] = tmp.value("title", "");
    out["raw_text"] = tmp.value("raw_text", "");
    json arr = json::array();
    if (tmp.contains("models") && tmp["models"].is_array()) {
        for (auto& m : tmp["models"]) {
            json item = m;
            if (item.contains("model_id")) {
                item[id_key] = item["model_id"];
                item.erase("model_id");
            }
            arr.push_back(item);
        }
    }
    out[list_key] = arr;
    out["count"] = static_cast<int>(arr.size());
    return out;
}

} // anonymous namespace

// ============================================================
// 1. ToolHfSearchModels
// ============================================================
json ToolHfSearchModels(WebViewSession& session, const json& args) {
    std::string query;
    std::string task;
    bool hasTask = false;
    int count = kDefaultCount;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("task") && args["task"].is_string()) {
        task = args["task"].get<std::string>();
        hasTask = !task.empty();
    }
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (query.empty()) {
        return McpError("'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://huggingface.co/models?search=" + encoded;
    if (hasTask) {
        url += "&pipeline_tag=" + UrlEncodeComponent(task);
    }

    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " models search page fetch failed");
    }
    json payload = NormalizeHfModelList(raw, url, count);
    payload["query"] = query;
    if (hasTask) payload["task_filter"] = task;
    return WrapMcpResult(payload);
}

// ============================================================
// 2. ToolHfGetModelInfo
// ============================================================
json ToolHfGetModelInfo(WebViewSession& session, const json& args) {
    std::string modelId;
    if (args.contains("model_id") && args["model_id"].is_string())
        modelId = args["model_id"].get<std::string>();

    if (modelId.empty()) {
        return McpError("'model_id' parameter is required");
    }
    // 清洗:去掉前导斜杠
    if (!modelId.empty() && modelId[0] == '/') modelId = modelId.substr(1);

    std::string url = "https://huggingface.co/" + modelId;
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " model page fetch failed: " + modelId);
    }
    json payload = NormalizeHfModelInfo(raw, modelId, url);
    return WrapMcpResult(payload);
}

// ============================================================
// 3. ToolHfGetModelReadme
// ============================================================
json ToolHfGetModelReadme(WebViewSession& session, const json& args) {
    std::string modelId;
    if (args.contains("model_id") && args["model_id"].is_string())
        modelId = args["model_id"].get<std::string>();

    if (modelId.empty()) {
        return McpError("'model_id' parameter is required");
    }
    if (!modelId.empty() && modelId[0] == '/') modelId = modelId.substr(1);

    std::string url = "https://huggingface.co/" + modelId;
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " model page fetch failed: " + modelId);
    }
    // README 主要输出 raw_text / title / url,同时带上 model_info 规范化的少量结构化字段
    json info = NormalizeHfModelInfo(raw, modelId, url);
    info["page_source"] = "hf_model_readme";
    return WrapMcpResult(info);
}

// ============================================================
// 4. ToolHfSearchDatasets
// ============================================================
json ToolHfSearchDatasets(WebViewSession& session, const json& args) {
    std::string query;
    int count = kDefaultCount;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (query.empty()) {
        return McpError("'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://huggingface.co/datasets?search=" + encoded;

    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " datasets search page fetch failed");
    }
    json payload = NormalizeHfGenericList(raw, url, "datasets", "dataset_id", count);
    payload["query"] = query;
    return WrapMcpResult(payload);
}

// ============================================================
// 5. ToolHfGetDatasetInfo
// ============================================================
json ToolHfGetDatasetInfo(WebViewSession& session, const json& args) {
    std::string datasetId;
    if (args.contains("dataset_id") && args["dataset_id"].is_string())
        datasetId = args["dataset_id"].get<std::string>();

    if (datasetId.empty()) {
        return McpError("'dataset_id' parameter is required");
    }
    if (!datasetId.empty() && datasetId[0] == '/') datasetId = datasetId.substr(1);

    std::string url = "https://huggingface.co/datasets/" + datasetId;
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " dataset page fetch failed: " + datasetId);
    }
    // Dataset info 页面也按 model info 的模式做简单行抽取,字段名换成 dataset_id
    json payload = NormalizeHfModelInfo(raw, datasetId, url);
    payload.erase("model_id");
    payload["dataset_id"] = datasetId;
    payload["page_source"] = "hf_dataset_detail";
    return WrapMcpResult(payload);
}

// ============================================================
// 6. ToolHfGetTrendingModels
// ============================================================
json ToolHfGetTrendingModels(WebViewSession& session, const json& args) {
    int count = kDefaultCount;
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string url = "https://huggingface.co/models?sort=trending";
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " trending models page fetch failed");
    }
    json payload = NormalizeHfModelList(raw, url, count);
    payload["sort"] = "trending";
    return WrapMcpResult(payload);
}

// ============================================================
// 7. ToolHfSearchSpaces
// ============================================================
json ToolHfSearchSpaces(WebViewSession& session, const json& args) {
    std::string query;
    int count = kDefaultCount;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (query.empty()) {
        return McpError("'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://huggingface.co/spaces?search=" + encoded;

    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " spaces search page fetch failed");
    }
    json payload = NormalizeHfGenericList(raw, url, "spaces", "space_id", count);
    payload["query"] = query;
    return WrapMcpResult(payload);
}

// ============================================================
// 8. ToolHfFetchModelDetail - 分层工具: 缓存 + entity_mapper
//    args: model_id (string), fetch_readme (bool, default false)
//    cache_key: hf:model:{model_id}, TTL=12h
//    entity: model 实体 + derived_from(base_model) 关系 + downloads 时间快照
// ============================================================
json ToolHfFetchModelDetail(WebViewSession& session, const json& args) {
    std::string modelId;
    if (args.contains("model_id") && args["model_id"].is_string()) {
        modelId = args["model_id"].get<std::string>();
    }
    if (modelId.empty()) {
        return McpError("'model_id' parameter is required");
    }
    if (!modelId.empty() && modelId[0] == '/') modelId = modelId.substr(1);

    // ── 缓存查询 ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "hf:model:" + modelId;
    if (cm.is_ready()) {
        auto cached = cm.get("hf", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("hf", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("hf", cache_key);
            }
        }
    }

    std::string url = "https://huggingface.co/" + modelId;
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                      kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        if (cm.is_ready()) {
            cm.put("hf", cache_key, "", "json", 1, "", "failed", "hf model page fetch failed");
        }
        return McpError(std::string("ERROR: [hf] failed to fetch model=") + modelId);
    }

    std::string pageText, pageTitle;
    if (raw.is_object()) {
        if (raw.contains("text") && raw["text"].is_string()) {
            pageText = raw["text"].get<std::string>();
        }
        if (raw.contains("title") && raw["title"].is_string()) {
            pageTitle = raw["title"].get<std::string>();
        }
    }
    if (pageText.size() > 50000) pageText = pageText.substr(0, 50000);

    std::string title = pageTitle;
    {
        size_t pos = title.find(" | Hugging Face");
        if (pos != std::string::npos) title = title.substr(0, pos);
    }

    json payload = {
        {"success", true},
        {"model_id", modelId},
        {"title", title},
        {"page_url", url},
        {"page_title", pageTitle},
        {"raw_text", pageText}
    };

    if (cm.is_ready()) {
        cm.put("hf", cache_key, payload.dump(), "json", 12, "", "ok", "");
    }

    // entity_mapper: model 实体
    if (cm.is_ready() && !modelId.empty()) {
        std::string model_eid = cm.register_entity(
            "model",
            "hf:" + modelId,  // canonical_name,带 hf: 前缀
            {title, modelId},  // aliases
            {"huggingface"},   // tags
            {{"model_id", modelId},
             {"page_url", url}},
            title
        );
        cm.register_entity_source(model_eid, "hf_web", modelId,
                                  {"title", "downloads", "likes", "pipeline_tag"}, 0.85);
        // 时间快照: 记录被观测一次(避免硬解析 downloads 数值)
        cm.record_metric(model_eid, "hf_observed", 1.0, "hf");
    }

    return WrapMcpResult(payload);
}

// ============================================================
// 9. ToolHfFetchDatasetDetail - 分层工具: 缓存 + entity_mapper
//    args: dataset_id (string)
//    cache_key: hf:dataset:{dataset_id}, TTL=24h
//    entity: dataset 实体
// ============================================================
json ToolHfFetchDatasetDetail(WebViewSession& session, const json& args) {
    std::string datasetId;
    if (args.contains("dataset_id") && args["dataset_id"].is_string()) {
        datasetId = args["dataset_id"].get<std::string>();
    }
    if (datasetId.empty()) {
        return McpError("'dataset_id' parameter is required");
    }
    if (!datasetId.empty() && datasetId[0] == '/') datasetId = datasetId.substr(1);

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "hf:dataset:" + datasetId;
    if (cm.is_ready()) {
        auto cached = cm.get("hf", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("hf", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("hf", cache_key);
            }
        }
    }

    std::string url = "https://huggingface.co/datasets/" + datasetId;
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                      kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        if (cm.is_ready()) {
            cm.put("hf", cache_key, "", "json", 1, "", "failed", "hf dataset page fetch failed");
        }
        return McpError(std::string("ERROR: [hf] failed to fetch dataset=") + datasetId);
    }

    std::string pageText, pageTitle;
    if (raw.is_object()) {
        if (raw.contains("text") && raw["text"].is_string()) {
            pageText = raw["text"].get<std::string>();
        }
        if (raw.contains("title") && raw["title"].is_string()) {
            pageTitle = raw["title"].get<std::string>();
        }
    }
    if (pageText.size() > 50000) pageText = pageText.substr(0, 50000);

    std::string title = pageTitle;
    {
        size_t pos = title.find(" | Hugging Face");
        if (pos != std::string::npos) title = title.substr(0, pos);
    }

    json payload = {
        {"success", true},
        {"dataset_id", datasetId},
        {"title", title},
        {"page_url", url},
        {"page_title", pageTitle},
        {"raw_text", pageText}
    };

    if (cm.is_ready()) {
        cm.put("hf", cache_key, payload.dump(), "json", 24, "", "ok", "");
    }

    if (cm.is_ready() && !datasetId.empty()) {
        std::string ds_eid = cm.register_entity(
            "dataset",
            "hf:" + datasetId,
            {title, datasetId},
            {"huggingface"},
            {{"dataset_id", datasetId},
             {"page_url", url}},
            title
        );
        cm.register_entity_source(ds_eid, "hf_web", datasetId,
                                  {"title", "downloads", "likes"}, 0.85);
        cm.record_metric(ds_eid, "hf_dataset_observed", 1.0, "hf");
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
