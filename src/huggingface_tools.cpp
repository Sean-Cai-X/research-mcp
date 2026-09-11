#include "github_research/huggingface_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <string>
#include <mutex>
#include <memory>

namespace github_research {

namespace {

constexpr const char* kLogPrefix = "[hf]";
constexpr int kDefaultCount = 10;
constexpr int kMaxCount = 50;

// ─── Curl HTTP client singleton ───
std::mutex g_hf_curl_mutex;
std::unique_ptr<CurlHttpClient> g_hf_curl;

CurlHttpClient& get_hf_curl() {
    std::lock_guard<std::mutex> lk(g_hf_curl_mutex);
    if (!g_hf_curl) {
        g_hf_curl = std::make_unique<CurlHttpClient>("research-mcp-hf/1.0", 30);
        g_hf_curl->initialize();
    }
    return *g_hf_curl;
}

HttpResponse fetch_json(const std::string& url) {
    CurlHttpClient& curl = get_hf_curl();
    std::map<std::string, std::string> hdrs;
    hdrs["User-Agent"] = "ResearchMCP/1.0";
    hdrs["Accept"] = "application/json";
    return curl.get(url, hdrs);
}

} // anonymous namespace

// ============================================================
// 1. hf_search_models - huggingface.co/api/models?search={q}&limit={n}
// ============================================================
json ToolHfSearchModels(const json& args) {
    std::string query;
    std::string task;
    bool hasTask = false;
    int count = kDefaultCount;

    if (args.contains("query") && args["query"].is_string()) query = args["query"].get<std::string>();
    if (args.contains("task") && args["task"].is_string()) {
        task = args["task"].get<std::string>();
        hasTask = !task.empty();
    }
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (query.empty()) return McpError("ERROR: 'query' parameter is required");
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string url = std::string("https://huggingface.co/api/models?search=")
        + UrlEncodeComponent(query) + "&limit=" + std::to_string(count);
    if (hasTask) url += "&pipeline_tag=" + UrlEncodeComponent(task);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json arr = json::parse(resp.body);
        if (!arr.is_array()) return McpError("ERROR: [hf] unexpected response format");
        json models = json::array();
        for (auto& m : arr) {
            if (!m.is_object()) continue;
            json item;
            item["model_id"] = m.value("modelId", m.value("id", ""));
            item["downloads"] = m.value("downloads", 0);
            item["likes"] = m.value("likes", 0);
            item["pipeline_tag"] = m.value("pipeline_tag", m.value("pipelineTag", ""));
            item["tags"] = m.value("tags", json::array());
            models.push_back(std::move(item));
        }
        json payload = {
            {"success", true},
            {"source", "hf_api"},
            {"query", query},
            {"count", models.size()},
            {"models", models}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] parse failed: ") + e.what());
    }
}

// ============================================================
// 2. hf_get_model_info - huggingface.co/api/models/{model_id}
// ============================================================
json ToolHfGetModelInfo(const json& args) {
    std::string modelId;
    if (args.contains("model_id") && args["model_id"].is_string())
        modelId = args["model_id"].get<std::string>();
    if (modelId.empty()) return McpError("ERROR: 'model_id' parameter is required");
    if (!modelId.empty() && modelId[0] == '/') modelId = modelId.substr(1);

    std::string url = "https://huggingface.co/api/models/" + UrlEncodeComponent(modelId);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [hf] model not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        json payload = {
            {"success", true},
            {"model_id", data.value("modelId", modelId)},
            {"downloads", data.value("downloads", 0)},
            {"likes", data.value("likes", 0)},
            {"pipeline_tag", data.value("pipeline_tag", data.value("pipelineTag", ""))},
            {"tags", data.value("tags", json::array())},
            {"library_name", data.value("library_name", "")},
            {"created_at", data.value("createdAt", "")},
            {"last_modified", data.value("lastModified", "")},
            {"siblings", data.value("siblings", json::array())},
            {"card_data", data.value("cardData", json::object())}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] parse failed: ") + e.what());
    }
}

// ============================================================
// 3. hf_get_model_readme - README.md raw content via API
// ============================================================
json ToolHfGetModelReadme(const json& args) {
    std::string modelId;
    if (args.contains("model_id") && args["model_id"].is_string())
        modelId = args["model_id"].get<std::string>();
    if (modelId.empty()) return McpError("ERROR: 'model_id' parameter is required");
    if (!modelId.empty() && modelId[0] == '/') modelId = modelId.substr(1);

    // README.md content from HuggingFace API endpoint
    std::string url = "https://huggingface.co/api/models/" + UrlEncodeComponent(modelId);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [hf] model not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        // Get tags array
        json tags = data.value("tags", json::array());
        // cardData might have documentation
        json cardData = data.value("cardData", json::object());
        json payload = {
            {"success", true},
            {"model_id", modelId},
            {"description", data.value("description", "")},
            {"tags", tags},
            {"pipeline_tag", data.value("pipeline_tag", data.value("pipelineTag", ""))},
            {"card_data", cardData},
            {"source", "hf_api_model_info"}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] readme parse failed: ") + e.what());
    }
}

// ============================================================
// 4. hf_search_datasets - huggingface.co/api/datasets?search={q}
// ============================================================
json ToolHfSearchDatasets(const json& args) {
    std::string query;
    int count = kDefaultCount;
    if (args.contains("query") && args["query"].is_string()) query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (query.empty()) return McpError("ERROR: 'query' parameter is required");
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string url = std::string("https://huggingface.co/api/datasets?search=")
        + UrlEncodeComponent(query) + "&limit=" + std::to_string(count);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [hf] datasets search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json arr = json::parse(resp.body);
        json datasets = json::array();
        for (auto& d : arr) {
            if (!d.is_object()) continue;
            json item;
            item["dataset_id"] = d.value("id", "");
            item["downloads"] = d.value("downloads", 0);
            item["tags"] = d.value("tags", json::array());
            datasets.push_back(std::move(item));
        }
        json payload = {
            {"success", true},
            {"source", "hf_api"},
            {"query", query},
            {"count", datasets.size()},
            {"datasets", datasets}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] datasets parse failed: ") + e.what());
    }
}

// ============================================================
// 5. hf_get_dataset_info - huggingface.co/api/datasets/{dataset_id}
// ============================================================
json ToolHfGetDatasetInfo(const json& args) {
    std::string datasetId;
    if (args.contains("dataset_id") && args["dataset_id"].is_string())
        datasetId = args["dataset_id"].get<std::string>();
    if (datasetId.empty()) return McpError("ERROR: 'dataset_id' parameter is required");
    if (!datasetId.empty() && datasetId[0] == '/') datasetId = datasetId.substr(1);

    std::string url = "https://huggingface.co/api/datasets/" + UrlEncodeComponent(datasetId);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [hf] dataset not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        json payload = {
            {"success", true},
            {"dataset_id", data.value("id", datasetId)},
            {"description", data.value("description", "")},
            {"downloads", data.value("downloads", 0)},
            {"tags", data.value("tags", json::array())},
            {"card_data", data.value("cardData", json::object())}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] dataset parse failed: ") + e.what());
    }
}

// ============================================================
// 6. hf_get_trending_models - trending via /api/models?sort=trending
// ============================================================
json ToolHfGetTrendingModels(const json& args) {
    int count = kDefaultCount;
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string url = "https://huggingface.co/api/models?sort=trending&limit=" + std::to_string(count);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [hf] trending HTTP " + std::to_string(resp.status_code));
    }
    try {
        json arr = json::parse(resp.body);
        json models = json::array();
        for (auto& m : arr) {
            if (!m.is_object()) continue;
            json item;
            item["model_id"] = m.value("modelId", m.value("id", ""));
            item["downloads"] = m.value("downloads", 0);
            item["likes"] = m.value("likes", 0);
            item["pipeline_tag"] = m.value("pipeline_tag", m.value("pipelineTag", ""));
            models.push_back(std::move(item));
        }
        json payload = {
            {"success", true},
            {"source", "hf_api_trending"},
            {"count", models.size()},
            {"models", models}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] trending parse failed: ") + e.what());
    }
}

// ============================================================
// 7. hf_search_spaces - huggingface.co/api/spaces?search={q}
// ============================================================
json ToolHfSearchSpaces(const json& args) {
    std::string query;
    int count = kDefaultCount;
    if (args.contains("query") && args["query"].is_string()) query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (query.empty()) return McpError("ERROR: 'query' parameter is required");
    if (count < 1) count = 1;
    if (count > kMaxCount) count = kMaxCount;

    std::string url = std::string("https://huggingface.co/api/spaces?search=")
        + UrlEncodeComponent(query) + "&limit=" + std::to_string(count);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [hf] spaces search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json arr = json::parse(resp.body);
        json spaces = json::array();
        for (auto& s : arr) {
            if (!s.is_object()) continue;
            json item;
            item["space_id"] = s.value("id", "");
            item["sdk"] = s.value("sdk", "");
            item["tags"] = s.value("tags", json::array());
            spaces.push_back(std::move(item));
        }
        json payload = {
            {"success", true},
            {"source", "hf_api"},
            {"query", query},
            {"count", spaces.size()},
            {"spaces", spaces}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [hf] spaces parse failed: ") + e.what());
    }
}

// ============================================================
// 8. ToolHfFetchModelDetail - layered with cache + entity_mapper
// ============================================================
json ToolHfFetchModelDetail(const json& args) {
    std::string modelId;
    if (args.contains("model_id") && args["model_id"].is_string())
        modelId = args["model_id"].get<std::string>();
    if (modelId.empty()) return McpError("ERROR: 'model_id' parameter is required");
    if (!modelId.empty() && modelId[0] == '/') modelId = modelId.substr(1);

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "hf:model:" + modelId;
    if (cm.is_ready()) {
        auto cached = cm.get("hf", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("hf", cache_key)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cp);
                }
            } catch (...) { cm.invalidate("hf", cache_key); }
        }
    }

    json rawArgs = json::object();
    rawArgs["model_id"] = modelId;
    json detail = ToolHfGetModelInfo(rawArgs);

    json inner;
    if (detail.contains("content") && detail["content"].is_array() && !detail["content"].empty()) {
        auto& c = detail["content"][0];
        if (c.contains("text")) {
            try { inner = json::parse(c["text"].get<std::string>()); }
            catch (...) {}
        }
    }
    if (!inner.is_object()) inner = {{"success", false}};

    if (inner.value("success", false)) {
        if (cm.is_ready()) cm.put("hf", cache_key, inner.dump(), "json", 12, "", "ok", "");
        if (cm.is_ready()) {
            std::string eid = cm.register_entity(
                "model", "hf:" + modelId, {modelId}, {"huggingface"},
                {{"model_id", modelId}}, modelId);
            cm.register_entity_source(eid, "hf_api", modelId, {"model_id", "downloads"}, 0.9);
            cm.record_metric(eid, "hf_observed", 1.0, "hf");
        }
    }
    return WrapMcpResult(inner);
}

// ============================================================
// 9. ToolHfFetchDatasetDetail - layered with cache
// ============================================================
json ToolHfFetchDatasetDetail(const json& args) {
    std::string datasetId;
    if (args.contains("dataset_id") && args["dataset_id"].is_string())
        datasetId = args["dataset_id"].get<std::string>();
    if (datasetId.empty()) return McpError("ERROR: 'dataset_id' parameter is required");
    if (!datasetId.empty() && datasetId[0] == '/') datasetId = datasetId.substr(1);

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "hf:dataset:" + datasetId;
    if (cm.is_ready()) {
        auto cached = cm.get("hf", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("hf", cache_key)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cp);
                }
            } catch (...) { cm.invalidate("hf", cache_key); }
        }
    }

    json rawArgs = json::object();
    rawArgs["dataset_id"] = datasetId;
    json detail = ToolHfGetDatasetInfo(rawArgs);

    json inner;
    if (detail.contains("content") && detail["content"].is_array() && !detail["content"].empty()) {
        auto& c = detail["content"][0];
        if (c.contains("text")) {
            try { inner = json::parse(c["text"].get<std::string>()); }
            catch (...) {}
        }
    }
    if (!inner.is_object()) inner = {{"success", false}};

    if (inner.value("success", false)) {
        if (cm.is_ready()) cm.put("hf", cache_key, inner.dump(), "json", 24, "", "ok", "");
        if (cm.is_ready()) {
            std::string eid = cm.register_entity(
                "dataset", "hf:" + datasetId, {datasetId}, {"huggingface"},
                {{"dataset_id", datasetId}}, datasetId);
            cm.register_entity_source(eid, "hf_api", datasetId, {"dataset_id", "downloads"}, 0.9);
            cm.record_metric(eid, "hf_dataset_observed", 1.0, "hf");
        }
    }
    return WrapMcpResult(inner);
}

} // namespace github_research
