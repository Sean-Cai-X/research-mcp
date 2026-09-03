#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace github_research {
using json = nlohmann::json;

// 统一搜索接口:Bing 主力 + Tavily 备选 + DuckDuckGo 兜底
// API key 通过环境变量读取:BING_SEARCH_KEY, TAVILY_API_KEY
// 国内直连:Bing 走 Azure 国内端点;Tavily 需代理

struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
    std::string content;       // advanced 模式可能有正文
    std::string source_engine; // bing | tavily | duckduckgo
    double score = 0.0;
};

// 综合搜索:按优先级尝试多个引擎,去重后返回
// focus_id 用于限流和缓存
std::vector<WebSearchResult> web_search(
    const std::string& query,
    const std::string& focus_id = "",
    int max_results = 10,
    const std::string& freshness = "", // "" | "Day" | "Week" | "Month" | "Year"
    const std::string& mkt = "en-US"  // "en-US" | "zh-CN"
);

// 单个引擎调用(公开便于测试)
std::vector<WebSearchResult> bing_search(const std::string& query,
                                          int count = 10,
                                          const std::string& freshness = "",
                                          const std::string& mkt = "en-US");

std::vector<WebSearchResult> tavily_search(const std::string& query,
                                            int max_results = 8);

} // namespace github_research