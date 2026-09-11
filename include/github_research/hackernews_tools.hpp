#pragma once

// Hacker News MCP 工具集(7 个工具)
// 架构:纯 HTTP REST,零浏览器依赖
//   - Firebase API: 结构化 item/stories (https://hacker-news.firebaseio.com/v0/)
//   - Algolia API:   关键字搜索         (https://hn.algolia.com/api/v1/search)
//   - CurlHttpClient: 抓外部文章 HTML,简单 text 提取
//
// 所有工具启动即可用,不需要 --hn-profile <DIR>

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

// 1. hn_get_topstories
json ToolHnGetTopStories(const json& args);

// 2. hn_get_new_stories
json ToolHnGetNewStories(const json& args);

// 3. hn_get_best_stories
json ToolHnGetBestStories(const json& args);

// 4. hn_get_item - 纯 Firebase,返回完整结构化 item + comments 递归
json ToolHnGetItem(const json& args);

// 5. hn_search_by_keyword - Algolia REST API
json ToolHnSearchByKeyword(const json& args);

// 6. hn_get_latest_index - 轻量索引(同 top/new/best,结构化)
json ToolHnGetLatestIndex(const json& args);

// 7. hn_fetch_detailed_story - 深度抓取:Firebase item + comments + 外部文章 curl 抓取
json ToolHnFetchDetailedStory(const json& args);

} // namespace github_research
