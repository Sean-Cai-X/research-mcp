#include "github_research/focus_web_search.hpp"
#include "github_research/curl_http_client.hpp"
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>

namespace github_research {

std::vector<WebSearchResult> bing_search(const std::string& query,
                                          int count,
                                          const std::string& freshness,
                                          const std::string& mkt) {
    std::vector<WebSearchResult> results;
    const char* api_key = std::getenv("BING_SEARCH_KEY");
    if (!api_key || std::string(api_key).empty()) {
        return results;
    }

    std::string url = "https://api.bing.microsoft.com/v7.0/search"
                      "?q=" + query +
                      "&count=" + std::to_string(count) +
                      "&mkt=" + mkt;
    if (!freshness.empty()) {
        url += "&freshness=" + freshness;
    }

    std::map<std::string, std::string> headers;
    headers["Ocp-Apim-Subscription-Key"] = api_key;

    CurlHttpClient curl("ResearchMCP/1.0", 30);
    auto resp = curl.get(url, headers);

    if (resp.status_code != 200) {
        return results;
    }

    try {
        json j = json::parse(resp.body);
        if (!j.contains("webPages") || !j["webPages"].contains("value")) {
            return results;
        }
        for (const auto& r : j["webPages"]["value"]) {
            WebSearchResult sr;
            sr.title = r.value("name", "");
            sr.url = r.value("url", "");
            sr.snippet = r.value("snippet", "");
            sr.source_engine = "bing";
            sr.score = 0.8;
            if (!sr.url.empty() && !sr.title.empty()) {
                results.push_back(std::move(sr));
            }
        }
    } catch (...) {}
    return results;
}

std::vector<WebSearchResult> tavily_search(const std::string& query,
                                            int max_results) {
    std::vector<WebSearchResult> results;
    const char* api_key = std::getenv("TAVILY_API_KEY");
    if (!api_key || std::string(api_key).empty()) {
        return results;
    }

    // Tavily 用 POST,需要找 CurlHttpClient 的 post 方法
    // 简化:只支持 Bing,Tavily POST 后续完善
    return results;
}

std::vector<WebSearchResult> web_search(
    const std::string& query,
    const std::string& focus_id,
    int max_results,
    const std::string& freshness,
    const std::string& mkt) {

    std::vector<WebSearchResult> merged;
    std::set<std::string> seen_urls;

    // 优先级 1: Bing
    auto bing = bing_search(query, max_results, freshness, mkt);
    for (auto& r : bing) {
        if (seen_urls.insert(r.url).second) merged.push_back(std::move(r));
    }

    // 优先级 2: Tavily
    if ((int)merged.size() < max_results) {
        auto tavily = tavily_search(query, max_results);
        for (auto& r : tavily) {
            if (seen_urls.insert(r.url).second) merged.push_back(std::move(r));
        }
    }

    if ((int)merged.size() > max_results) {
        merged.resize(max_results);
    }
    return merged;
}

} // namespace github_research