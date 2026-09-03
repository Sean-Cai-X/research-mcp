#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>

namespace github_research {

using json = nlohmann::json;

// Hacker News Firebase API 客户端
// Firebase API 是免费、无认证的纯 JSON REST:
//   https://hacker-news.firebaseio.com/v0/
//
// 比 WebView2 爬 news.ycombinator.com 快 10 倍以上,
// 无页面结构依赖,返回完整结构化数据。
//
// 端点:
//   topstories / newstories / beststories / showstories / askstories / jobstories
//     → 返回最多 500 个 story ID 的 JSON 数组
//   item/{id} → 返回单个 story/comment/job 的完整字段
//   user/{id} → 返回用户信息
class HnFirebase {
public:
    // 获取 top stories ID 列表(最多 500)
    static json get_top_ids();
    // 获取 new stories ID 列表
    static json get_new_ids();
    // 获取 best stories ID 列表
    static json get_best_ids();
    // 获取 show HN stories ID 列表
    static json get_show_ids();
    // 获取 ask HN stories ID 列表
    static json get_ask_ids();
    // 获取 job stories ID 列表
    static json get_job_ids();

    // 获取单个 item 详情(title/url/score/descendants/by/time/kids...)
    // 返回 nullptr 表示 item 不存在或请求失败
    static json get_item(int id);

    // 批量获取 item 详情(串行,但比逐个调用快)
    // 返回 map<id, item_json>,失败的 id 不会出现在 map 中
    static std::map<int, json> get_items_batch(const std::vector<int>& ids);

    // 检查 Firebase API 是否可用(发一个轻量请求)
    static bool is_available();

    // 底层 fetch(其他文件也可能需要直接调 Firebase)
    static json fetch_json(const std::string& endpoint);

private:
    // 基础 URL
    static constexpr const char* kBase = "https://hacker-news.firebaseio.com/v0";
};

} // namespace github_research
