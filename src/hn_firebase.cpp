#include "github_research/hn_firebase.hpp"
#include "github_research/curl_http_client.hpp"
#include "github_research/cache_manager.hpp"
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <memory>

namespace github_research {

namespace {

// Firebase item 详情缓存 TTL: HN 文章基本不变,score/comment_count 变化慢
// CacheManager::put 的 ttl 参数单位是小时
constexpr int kItemCacheTtlHours = 12;

// ID 列表缓存 TTL: top/new/best 变化较快,设 1 小时足够
// 5 分钟太短会导致频繁回源,且 Firebase API 响应很快
constexpr int kListCacheTtlHours = 1;

// "不存在"缓存 TTL:6 小时
constexpr int kNotFoundCacheTtlHours = 6;

// 单例 curl client
std::mutex g_curl_mutex;
std::unique_ptr<CurlHttpClient> g_curl_client;

CurlHttpClient& get_curl() {
    std::lock_guard<std::mutex> lk(g_curl_mutex);
    if (!g_curl_client) {
        std::string proxy;
        // 从环境变量读代理(与 mcp_server 初始化逻辑一致)
        if (const char* p = std::getenv("HTTPS_PROXY")) proxy = p;
        else if (const char* p = std::getenv("https_proxy")) proxy = p;
        else if (const char* p = std::getenv("HTTP_PROXY")) proxy = p;
        g_curl_client = std::make_unique<CurlHttpClient>("research-mcp-hn/1.0", 15);
        if (!proxy.empty()) {
            g_curl_client->set_proxy(proxy);
            std::cerr << "[hn-fb] proxy set: " << proxy << std::endl;
        }
        g_curl_client->initialize();
    }
    return *g_curl_client;
}

} // namespace

json HnFirebase::fetch_json(const std::string& endpoint) {
    try {
        CurlHttpClient& curl = get_curl();
        std::string url = std::string(kBase) + endpoint;
        HttpResponse resp = curl.get(url);
        if (resp.status_code != 200) {
            std::cerr << "[hn-fb] fetch " << endpoint
                      << " failed: HTTP " << resp.status_code << std::endl;
            return nullptr;
        }
        if (resp.body.empty()) return nullptr;
        return json::parse(resp.body);
    } catch (const std::exception& e) {
        std::cerr << "[hn-fb] fetch " << endpoint
                  << " exception: " << e.what() << std::endl;
        return nullptr;
    }
}

// 辅助:ID 列表端点(带缓存)
static json fetch_ids_list(const std::string& name) {
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "hn_fb_" + name;

    if (cm.is_ready() && cm.is_fresh("hn_fb_list", cache_key)) {
        auto ce = cm.get("hn_fb_list", cache_key);
        if (ce.has_value()) {
            try {
                json cached = json::parse(ce->payload);
                if (cached.is_array() && !cached.empty()) {
                    std::cerr << "[hn-fb] " << name << " cache hit, n=" << cached.size() << std::endl;
                    return cached;
                }
            } catch (...) {}
        }
    }

    json ids = HnFirebase::fetch_json("/" + name + ".json");
    if (ids.is_array() && !ids.empty() && cm.is_ready()) {
        cm.put("hn_fb_list", cache_key, ids.dump(), "json",
               kListCacheTtlHours, "", "ok", "");
    }
    return ids;
}

json HnFirebase::get_top_ids()    { return fetch_ids_list("topstories"); }
json HnFirebase::get_new_ids()    { return fetch_ids_list("newstories"); }
json HnFirebase::get_best_ids()   { return fetch_ids_list("beststories"); }
json HnFirebase::get_show_ids()   { return fetch_ids_list("showstories"); }
json HnFirebase::get_ask_ids()    { return fetch_ids_list("askstories"); }
json HnFirebase::get_job_ids()    { return fetch_ids_list("jobstories"); }

json HnFirebase::get_item(int id) {
    if (id <= 0) return nullptr;

    // 先查缓存
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = std::to_string(id);
    if (cm.is_ready() && cm.is_fresh("hn_fb_item", cache_key)) {
        auto ce = cm.get("hn_fb_item", cache_key);
        if (ce.has_value()) {
            try {
                json cached = json::parse(ce->payload);
                if (cached.is_object() && cached.contains("id")) {
                    return cached;
                }
                if (cached.is_null()) {
                    // 缓存了"不存在"
                    return nullptr;
                }
            } catch (...) {}
        }
    }

    // Firebase item 不存在时返回 null(HTTP 200 body="null")
    json item = fetch_json("/item/" + std::to_string(id) + ".json");
    if (item.is_null() || !item.is_object()) {
        // 也缓存"不存在"结果,TTL 短一些
        if (cm.is_ready()) {
            cm.put("hn_fb_item", cache_key, "null", "json",
                   kNotFoundCacheTtlHours, "", "not_found", "");
        }
        return nullptr;
    }

    // 存缓存
    if (cm.is_ready()) {
        cm.put("hn_fb_item", cache_key, item.dump(), "json",
               kItemCacheTtlHours, "", "ok", "");
    }
    return item;
}

std::map<int, json> HnFirebase::get_items_batch(const std::vector<int>& ids) {
    std::map<int, json> result;
    CacheManager& cm = CacheManager::instance();

    // 先批量查缓存,缓存 miss 的才去请求 Firebase
    std::vector<int> to_fetch;
    to_fetch.reserve(ids.size());

    if (cm.is_ready()) {
        for (int id : ids) {
            std::string cache_key = std::to_string(id);
            if (cm.is_fresh("hn_fb_item", cache_key)) {
                auto ce = cm.get("hn_fb_item", cache_key);
                if (ce.has_value()) {
                    try {
                        json cached = json::parse(ce->payload);
                        if (cached.is_object() && cached.contains("id")) {
                            result[id] = std::move(cached);
                            continue;
                        }
                        if (cached.is_null()) {
                            // 缓存了"不存在",跳过
                            continue;
                        }
                    } catch (...) {}
                }
            }
            to_fetch.push_back(id);
        }
    } else {
        to_fetch = ids;
    }

    if (!to_fetch.empty()) {
        std::cerr << "[hn-fb] batch fetch " << to_fetch.size()
                  << " items (cache hit " << (ids.size() - to_fetch.size()) << ")" << std::endl;
        // 串行请求(CurlHttpClient 不是线程安全的,但 HN item API 很快)
        for (int id : to_fetch) {
            json item = get_item(id);  // get_item 内部已处理缓存写入
            if (item.is_object() && item.contains("id")) {
                result[id] = std::move(item);
            }
        }
    }
    return result;
}

bool HnFirebase::is_available() {
    json ids = fetch_json("/topstories.json");
    return ids.is_array() && !ids.empty();
}

} // namespace github_research
