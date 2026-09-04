#pragma once

// DeepDive 工具: 基于图谱的次级链接发现 + 批量网页抓取 + 综合分析上下文
//
// 工作流:
//   1. SEED DISCOVERY: 根据 seed_query 发现研究起点(hn_id / 实体名 / 关键词)
//      - 如果是 "hn:XXXX" 或纯数字 -> 走 HN 深度抓取(正文 + 评论树)
//      - 否则 -> 走 CacheManager find/search 找实体 + get_entity
//   2. GRAPH TRAVERSE: 从起点实体 2 跳 BFS,遍历所有关联实体
//   3. URL AGGREGATION: 从 HN 正文/评论文本 + 各实体 metadata 中提取 URL
//      归一化、去重、过滤低价值、按 出现频次*关系权重 排序
//   4. SECONDARY FETCH: 逐个 WebView Navigate 抓网页正文(带缓存 TTL=72h)
//   5. COMPOSE: 组装 {seed_summary, graph_trace, secondary_pages[]} 大包返回
//
// 工具名: research_deep_dive
//   参数: seed_query (string, 必选),
//         mode (string, 可选): "auto"(默认) | "hn" | "general"
//           - "auto": 有 WebView session 就走增强,没有就降级 general
//           - "hn":   强制 HN 模式,需要 HN WebView session
//           - "general": 通用模式,不依赖 HN,纯实体图谱遍历 + (可选)外部网页抓取
//         max_secondary_links (int, 默认 5, 最大 15),
//         page_text_max_chars (int, 默认 15000, 最大 50000),
//         graph_max_depth (int, 默认 2, 最大 3),
//         force_refresh (bool, 默认 false)
//
// 实现文件: src/research_deep_dive.cpp

#include <string>
#include <nlohmann/json.hpp>
#include "webview_session.hpp"

namespace github_research {

using json = nlohmann::json;

// research_deep_dive 主入口
// web_session: 用于页面抓取的 WebView session,可为 null
//   - null + mode=general → 跳过外部网页抓取,只用实体图谱 + 关键词扩展
//   - 非 null → 可做次级网页抓取
//   - mode=hn → 必须传入指向 HN 的 WebView session
json ToolResearchDeepDive(WebViewSession* web_session, const json& args);

} // namespace github_research
