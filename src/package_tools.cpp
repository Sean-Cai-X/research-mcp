#include "github_research/package_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <string>
#include <mutex>
#include <memory>

namespace github_research {

namespace {

// ─── Curl HTTP client singleton ───
std::mutex g_pkg_curl_mutex;
std::unique_ptr<CurlHttpClient> g_pkg_curl;

CurlHttpClient& get_pkg_curl() {
    std::lock_guard<std::mutex> lk(g_pkg_curl_mutex);
    if (!g_pkg_curl) {
        g_pkg_curl = std::make_unique<CurlHttpClient>("research-mcp-pkg/1.0", 30);
        g_pkg_curl->initialize();
    }
    return *g_pkg_curl;
}

std::string read_string(const json& args, const std::string& key) {
    if (args.contains(key) && args[key].is_string())
        return args[key].get<std::string>();
    return "";
}

int read_count(const json& args, const std::string& key, int default_val) {
    if (args.contains(key) && args[key].is_number_integer()) {
        int n = args[key].get<int>();
        if (n < 1) return 1;
        if (n > 50) return 50;
        return n;
    }
    return default_val;
}

HttpResponse fetch_json(const std::string& url) {
    CurlHttpClient& curl = get_pkg_curl();
    std::map<std::string, std::string> hdrs;
    hdrs["User-Agent"] = "ResearchMCP/1.0";
    hdrs["Accept"] = "application/json";
    return curl.get(url, hdrs);
}

} // anonymous namespace

// ============================================================
// 1. pkg_search_npm - registry.npmjs.org/-/v1/search
// ============================================================
json ToolPkgSearchNpm(const json& args) {
    std::string query = read_string(args, "query");
    if (query.empty()) {
        return McpError("ERROR: [pkg] 'query' parameter is required");
    }
    int count = read_count(args, "count", 10);

    std::string url = "https://registry.npmjs.org/-/v1/search?text="
        + UrlEncodeComponent(query) + "&size=" + std::to_string(count);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pkg] npm API HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json packages = json::array();
        auto objects = data.value("objects", json::array());
        for (auto& obj : objects) {
            if (!obj.is_object()) continue;
            json pkg = obj.value("package", json::object());
            json item;
            item["name"] = pkg.value("name", "");
            item["version"] = pkg.value("version", "");
            item["description"] = pkg.value("description", "");
            // author can be string or object
            json author = pkg.value("author", json::object());
            if (author.is_string()) item["author"] = author.get<std::string>();
            else item["author"] = author.value("name", "");
            item["registry_url"] = "https://www.npmjs.com/package/" + item.value("name", "");
            packages.push_back(std::move(item));
        }
        json payload = {
            {"success", true},
            {"source", "npm_registry_api"},
            {"query", query},
            {"total_returned", packages.size()},
            {"packages", packages}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pkg] npm JSON parse failed: ") + e.what());
    }
}

// ============================================================
// 2. pkg_get_npm_detail - registry.npmjs.org/{name}
// ============================================================
json ToolPkgGetNpmDetail(const json& args) {
    std::string name = read_string(args, "name");
    if (name.empty()) {
        return McpError("ERROR: [pkg] 'name' parameter is required");
    }

    std::string url = "https://registry.npmjs.org/" + UrlEncodeComponent(name);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pkg] npm package not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        // Latest version info
        std::string latest = data.value("dist-tags", json::object()).value("latest", "");
        json versions = data.value("versions", json::object());
        json latestVer = latest.empty() ? json::object() : versions.value(latest, json::object());

        json payload = {
            {"success", true},
            {"registry", "npm"},
            {"name", data.value("name", name)},
            {"version", latest},
            {"description", data.value("description", "")},
            {"license", latestVer.value("license", data.value("license", ""))},
            {"homepage", data.value("homepage", "")},
            {"repository", data.value("repository", json::object()).value("url", "")},
            {"maintainers", data.value("maintainers", json::array())},
            {"dependencies", latestVer.value("dependencies", json::object())},
            {"devDependencies", latestVer.value("devDependencies", json::object())},
            {"registry_url", "https://www.npmjs.com/package/" + name}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pkg] npm parse failed: ") + e.what());
    }
}

// ============================================================
// 3. pkg_search_pypi - pypi.org search (HTML fallback with simple scrape)
// ============================================================
json ToolPkgSearchPypi(const json& args) {
    std::string query = read_string(args, "query");
    if (query.empty()) {
        return McpError("ERROR: [pkg] 'query' parameter is required");
    }
    int count = read_count(args, "count", 10);

    // pypi.org/search is HTML, no official search JSON API
    // We use pypi.org/search and extract package names from results
    std::string url = "https://pypi.org/search/?q=" + UrlEncodeComponent(query);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pkg] pypi search HTTP " + std::to_string(resp.status_code));
    }

    // Simple HTML scrape for package names: look for <a class="package-snippet">
    json packages = json::array();
    size_t pos = 0;
    int found = 0;
    while (found < count) {
        size_t snippet = resp.body.find("package-snippet", pos);
        if (snippet == std::string::npos) break;
        // Find href="/project/{name}/"
        size_t hrefStart = resp.body.rfind("href=\"", snippet);
        if (hrefStart == std::string::npos) { pos = snippet + 1; continue; }
        std::string href = resp.body.substr(hrefStart + 6, 200);
        size_t hrefEnd = href.find("\"");
        if (hrefEnd != std::string::npos) href = href.substr(0, hrefEnd);
        // href looks like /project/package-name/
        size_t pStart = href.find("/project/");
        if (pStart != std::string::npos) {
            std::string pkgName = href.substr(pStart + 9);
            if (!pkgName.empty() && pkgName.back() == '/') pkgName.pop_back();
            if (!pkgName.empty()) {
                json item;
                item["name"] = pkgName;
                item["project_url"] = "https://pypi.org/project/" + pkgName;
                packages.push_back(std::move(item));
                found++;
            }
        }
        pos = snippet + 1;
    }

    json payload = {
        {"success", true},
        {"source", "pypi_web_search"},
        {"query", query},
        {"total_returned", packages.size()},
        {"packages", packages}
    };
    return WrapMcpResult(payload);
}

// ============================================================
// 4. pkg_get_pypi_detail - pypi.org/pypi/{name}/json
// ============================================================
json ToolPkgGetPypiDetail(const json& args) {
    std::string name = read_string(args, "name");
    if (name.empty()) {
        return McpError("ERROR: [pkg] 'name' parameter is required");
    }

    std::string url = "https://pypi.org/pypi/" + UrlEncodeComponent(name) + "/json";
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pkg] pypi package not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        json info = data.value("info", json::object());
        json payload = {
            {"success", true},
            {"registry", "pypi"},
            {"name", info.value("name", name)},
            {"version", info.value("version", "")},
            {"description", info.value("summary", "")},
            {"homepage", info.value("home_page", "")},
            {"author", info.value("author", "")},
            {"license", info.value("license", "")},
            {"classifiers", info.value("classifiers", json::array())},
            {"project_urls", info.value("project_urls", json::object())},
            {"requires_python", info.value("requires_python", "")},
            {"project_url", "https://pypi.org/project/" + name}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pkg] pypi parse failed: ") + e.what());
    }
}

// ============================================================
// 5. ToolPkgFetchDetail - layered tool with cache + entity_mapper
// ============================================================
json ToolPkgFetchDetail(const json& args) {
    std::string registry = read_string(args, "registry");
    std::string name = read_string(args, "name");
    if (registry.empty()) registry = "npm";
    if (registry != "npm" && registry != "pypi") {
        return McpError("ERROR: [pkg] 'registry' must be 'npm' or 'pypi'");
    }
    if (name.empty()) {
        return McpError("ERROR: [pkg] 'name' parameter is required");
    }

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "pkg:" + registry + ":" + name;
    if (cm.is_ready()) {
        auto cached = cm.get("pkg", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("pkg", cache_key)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cp);
                }
            } catch (...) { cm.invalidate("pkg", cache_key); }
        }
    }

    json detail;
    if (registry == "npm") {
        detail = ToolPkgGetNpmDetail(args);
    } else {
        detail = ToolPkgGetPypiDetail(args);
    }

    // Extract payload from MCP wrapper
    json inner;
    if (detail.contains("content") && detail["content"].is_array() && !detail["content"].empty()) {
        auto& c = detail["content"][0];
        if (c.contains("text")) {
            try { inner = json::parse(c["text"].get<std::string>()); }
            catch (...) {}
        }
    }
    if (!inner.is_object()) {
        inner = {{"success", false}};
    }

    if (inner.value("success", false)) {
        if (cm.is_ready()) {
            cm.put("pkg", cache_key, inner.dump(), "json", 24, "", "ok", "");
        }
        if (cm.is_ready()) {
            std::string pkg_eid = cm.register_entity(
                "package", registry + ":" + name, {name}, {registry, "package"},
                {{"registry", registry}, {"name", name}}, name);
            cm.register_entity_source(pkg_eid, registry == "npm" ? "npm_registry" : "pypi_registry",
                                      name, {"name", "version", "description"}, 0.85);
            cm.record_metric(pkg_eid, "version_observed", 1.0, registry);
        }
    }
    return WrapMcpResult(inner);
}

} // namespace github_research
