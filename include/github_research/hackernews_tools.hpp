#pragma once

// Hacker News MCP 工具集(7 个工具: 5 原始 + 2 分层渐进挖掘)
// 架构:通过独立 WebViewSession 访问 news.ycombinator.com 与 hn.algolia.com
// 使用 Navigate + ExecuteScript 模式,JS 注入提取 DOM
// 复用 webview_helpers 公共辅助函数,统一 MCP 返回格式
//
// 分层渐进挖掘(新增):
//   - hn_get_latest_index:      轻量索引,只拉首页元数据,无深度网络请求
//   - hn_fetch_detailed_story:  按 hn_id 深度抓取外部正文 + HN 评论树

#include <string>
#include <nlohmann/json.hpp>
#include "github_research/browser_session.hpp"

namespace github_research {

using json = nlohmann::json;

// 1. hn_get_topstories - 获取 HN 首页 Top Stories(原始文本)
json ToolHnGetTopStories(IBrowserSession& session, const json& args);

// 2. hn_get_new_stories - 获取 HN Newest 列表(原始文本)
json ToolHnGetNewStories(IBrowserSession& session, const json& args);

// 3. hn_get_best_stories - 获取 HN Best 列表(原始文本)
json ToolHnGetBestStories(IBrowserSession& session, const json& args);

// 4. hn_get_item - 获取单条 story 详情及评论树(原始文本)
json ToolHnGetItem(IBrowserSession& session, const json& args);

// 5. hn_search_by_keyword - 通过 Algolia HN 搜索接口按关键字检索(原始文本)
json ToolHnSearchByKeyword(IBrowserSession& session, const json& args);

// 6. hn_get_latest_index - 轻量索引(结构化,无深度请求)
// args: limit (int, default 30, max 100), source (string, default "front",
//       可选 "newest" / "best")
// 返回: [{hn_id, rank, title, external_url, score, created_min_ago, has_discussion}]
json ToolHnGetLatestIndex(IBrowserSession& session, const json& args);

// 7. hn_fetch_detailed_story - 按 hn_id 深度抓取(外部正文 + 评论树)
// args: hn_id (string/int, required),
//       fetch_external_article (bool, default true),
//       fetch_comments (bool, default true),
//       comment_max_depth (int, default 2, max 5),
//       max_comment_count (int, default 80, max 200),
//       text_max_chars (int, default 20000, 文章正文截断上限)
// 返回: {hn_id, title, source_url, article_plaintext, article_fetch_status,
//        discussion_comments:[{author,text,reply_level}], comment_count}
json ToolHnFetchDetailedStory(IBrowserSession& session, const json& args);

} // namespace github_research
